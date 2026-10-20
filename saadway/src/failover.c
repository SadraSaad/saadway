/*
 * failover.c — watchdog/failover state machine (T015)
 *
 * data-model.md state machine:
 *   (up) ──probe-fail×N──▶ (contained) ──next-profile──▶ (connecting)
 *                                  │ cycle exhausted
 *   (cooldown) ──timer──▶ (connecting, new cycle)
 *   (down) — service disabled
 *
 * Decisions ratified by the plan:
 *   Q3-A: full coverage of every enabled profile (budget of
 *         failover_attempts failed tries each) → 300 s cool-down → full
 *         repeat cycle ( FR-026; no infinite switch loop).
 *   Q2-B: kill switch off ⇒ during cool-down LAN traffic egresses DIRECT
 *         (policy DIRECT mode); kill switch on ⇒ hard containment
 *         persists through the whole cool-down.
 *   FR-014: during any switch the containment-first ordering guarantees
 *         zero leaking packets — contain BEFORE re-opening the next
 *         candidate.
 *
 * Runtime attempt counters live on struct sw_profile (in-RAM, not UCI);
 * the counter of the currently active profile is MIRRORED into
 * ServiceState.run_attempts which is persisted to UCI.
 */
#define _GNU_SOURCE
#include "saadway.h"

#include <limits.h>

/* static forward decl: cycle-exhaustion entry point */
static void enter_cooldown(struct sw_ctx *c);

/* success resets nothing here; the UP transition is owned by
 * sw_failover_probe_result(c, true) */
static void persist(struct sw_ctx *c)
{
	if (sw_uci_persist_state(c) != SW_UCI_OK)
		sw_warn("failover: cannot persist service state to UCI");
}

static void status_set(struct sw_ctx *c, int st, const char *why)
{
	if (c->st.conn_status != st) {
		sw_info("connection_status %d -> %d (%s)", c->st.conn_status,
			st, why);
		c->st.conn_status = st;
	}
}

/* enabled profile with the lowest order whose attempts are still inside
 * the budget, strictly after `after_order` (wrap allowed once) */
static struct sw_profile *pick_next(struct sw_ctx *c, int after_order)
{
	int thr = c->st.failover_attempts;
	struct sw_profile *best = NULL;

	for (struct sw_profile *p = c->st.profiles; p; p = p->next) {
		if (!p->enabled || p->attempts >= thr)
			continue;
		if (p->order <= after_order)
			continue;
		if (!best || p->order < best->order)
			best = p;
	}
	if (best)
		return best;               /* same sweep, later position  */

	/* wrap: full cycle restarts from the lowest-order candidate */
	for (struct sw_profile *p = c->st.profiles; p; p = p->next) {
		if (!p->enabled || p->attempts >= thr)
			continue;
		if (!best || p->order < best->order)
			best = p;
	}
	return best;                       /* NULL ⇒ exhausted or empty  */
}

/* stop the active tunnel child per kind (no policy side effects) */
static void stop_active(struct sw_ctx *c, struct sw_profile *p)
{
	if (p->kind == SW_KIND_OPENVPN)
		sw_ovpn_stop(c, p, true);
	else
		sw_ikev2_stop(c, p, true);
}

static void start_cycle_at(struct sw_ctx *c, struct sw_profile *cand)
{
	c->st.conn_status = SW_CONN_CONNECTING;
	snprintf(c->st.active_profile, sizeof(c->st.active_profile), "%s",
		 cand->name);
	c->st.run_attempts = cand->attempts;
	persist(c);
	if (sw_activate_profile(c, cand, NULL) != 0) {
		/* synchronous spawn failure: count and roll to the next */
		sw_err("failover: activate '%s' failed synchronously",
		       cand->name);
		cand->attempts++;
		if (c->st.active_profile[0] &&
		    !strcmp(c->st.active_profile, cand->name)) {
			c->st.run_attempts = cand->attempts;
			persist(c);
		}
		/* evaluate the remaining cycle directly */
		struct sw_profile *nx = pick_next(c, cand->order);
		if (nx)
			start_cycle_at(c, nx);
		else
			enter_cooldown(c);
	}
}

/* FR-026: full cycle exhausted → cool-down, then the machine re-arms
 * itself (sw_failover_cooldown_check from sw_tick) */
static void enter_cooldown(struct sw_ctx *c)
{
	c->st.cycle_exhausted = true;
	c->fo.cooldown_active = true;
	c->fo.cooldown_deadline = time(NULL) + c->st.failover_cooldown;
	status_set(c, SW_CONN_COOLDOWN, "cycle exhausted");
	if (c->st.kill_switch)
		sw_policy_apply(c, true, NULL, false);
	else
		sw_policy_apply_direct(c);   /* Q2-B: direct permitted now */
	persist(c);
	sw_warn("failover: full cycle exhausted → cooldown %ds (ks=%d)",
		c->st.failover_cooldown, c->st.kill_switch);
}

/* Account one failed attempt on the doomed profile and IF its budget ran
 * out, roll the machine to the next candidate (static, single-active). */
static void
account_failure(struct sw_ctx *c, struct sw_profile *p, const char *why)
{
	/* 1) fail-closed containment FIRST (FR-014 / Principle IV) */
	status_set(c, SW_CONN_CONTAINED, why);
	sw_policy_apply(c, true, c->tun_if[0] ? c->tun_if : NULL, false);

	/* 2) stop the old child before touching the routing decision  */
	if (p) {
		stop_active(c, p);
		p->attempts++;
		if (c->st.active_profile[0] &&
		    !strcmp(c->st.active_profile, p->name))
			c->st.run_attempts = p->attempts;
	}
	persist(c);

	/* 3) single-active invariant: the machine owns c->active now    */
	struct sw_profile *cand = pick_next(c, p ? p->order : INT_MIN);
	c->active = NULL;
	c->tun_if[0] = 0;
	c->st.active_profile[0] = 0;

	if (!cand) {
		enter_cooldown(c);
		return;
	}
	start_cycle_at(c, cand);
}

/* ------------------------------------------------------------------ */
/* public API                                                           */
/* ------------------------------------------------------------------ */

void sw_failover_init(struct sw_ctx *c)
{
	c->fo.miss_count = 0;
	c->fo.child_died = false;
	c->fo.cooldown_active = false;
	c->fo.cooldown_deadline = 0;
	/* fresh boot: stale UCI attempts mirror irrelevant — reset for a
	 * clean cycle start (activation resume owned by main.c T040) */
	for (struct sw_profile *p = c->st.profiles; p; p = p->next)
		p->attempts = 0;
	c->st.run_attempts = 0;
}

/*
 * watchdog probe verdict (main.c timer, every probe_interval seconds).
 *   - while UP: N misses advance failover (FR-008)
 *   - while CONNECTING: ok verdict completes activation
 *   - other states: no tunnel to judge
 */
void sw_failover_probe_result(struct sw_ctx *c, bool ok)
{
	if (c->terminating || !c->st.enabled)
		return;

	if (c->st.conn_status == SW_CONN_CONNECTING) {
		if (!ok)
			return;    /* activation deadline governs instead */
		c->fo.miss_count = 0;
		status_set(c, SW_CONN_UP, "tunnel verified");
		for (struct sw_profile *p = c->st.profiles; p; p = p->next)
			p->attempts = 0;
		c->st.run_attempts = 0;
		c->fo.child_died = false;
		persist(c);
		return;
	}
	if (c->st.conn_status != SW_CONN_UP)
		return;            /* contained/cooldown/down: no tunnel */

	if (ok) {
		if (c->fo.miss_count)
			sw_info("watchdog: probe recovered (misses=%d)",
				c->fo.miss_count);
		c->fo.miss_count = 0;
		return;
	}
	c->fo.miss_count++;
	sw_warn("watchdog: probe miss %d/%d on '%s'", c->fo.miss_count,
		c->st.failover_attempts, c->st.active_profile);
	if (c->fo.miss_count >= c->st.failover_attempts) {
		struct sw_profile *p = sw_uci_profile_find(
			c, c->st.active_profile);
		c->fo.miss_count = 0;
		account_failure(c, p, "probe misses exhausted");
	}
}

/*
 * tunnel child reaper verdict (main.c): a tracked tunnel child did NOT
 * establish/open (nonzero exit) — one failed attempt on the active
 * profile, then roll.  (An exit-0 verdict for IKEv2 initiate is routed to
 * sw_failover_probe_result(c, true) by the reaper instead.)
 */
void sw_failover_child_died(struct sw_ctx *c)
{
	if (c->terminating || !c->st.enabled)
		return;
	if (c->st.conn_status == SW_CONN_COOLDOWN ||
	    c->st.conn_status == SW_CONN_DOWN)
		return;
	c->fo.child_died = true;
	struct sw_profile *p = sw_uci_profile_find(c, c->st.active_profile);
	account_failure(c, p, c->active ? c->active->name : "tunnel child");
}

/* per-second tick (main.c sw_tick): cooldown re-arm timer */
void sw_failover_cooldown_check(struct sw_ctx *c)
{
	if (!c->fo.cooldown_active)
		return;
	long now = time(NULL);
	if (now < c->fo.cooldown_deadline)
		return;

	c->fo.cooldown_active = false;
	c->st.cycle_exhausted = false;
	for (struct sw_profile *p = c->st.profiles; p; p = p->next)
		p->attempts = 0;
	c->st.run_attempts = 0;
	persist(c);
	sw_info("failover: cooldown elapsed — cycle re-armed");

	struct sw_profile *cand = pick_next(c, INT_MIN);
	if (cand) {
		sw_policy_apply(c, false, NULL, false);
		start_cycle_at(c, cand);
	} else {
		/* nothing enabled at all: park the machine */
		status_set(c, SW_CONN_DOWN, "no enabled profile");
		if (c->st.kill_switch)
			sw_policy_apply(c, true, NULL, false);
		else
			sw_policy_apply_direct(c);
	}
}

/* manual reset (deactivate / service disable): clears machine state;
 * the CALLER owns the policy determinations (sw_deactivate_all) */
void sw_failover_reset(struct sw_ctx *c)
{
	c->fo.miss_count = 0;
	c->fo.child_died = false;
	c->fo.cooldown_active = false;
	c->fo.cooldown_deadline = 0;
	for (struct sw_profile *p = c->st.profiles; p; p = p->next)
		p->attempts = 0;
	c->st.run_attempts = 0;
	c->st.cycle_exhausted = false;
	c->active = NULL;
	c->tun_if[0] = 0;
	c->st.active_profile[0] = 0;
	status_set(c, SW_CONN_DOWN, "reset");
	persist(c);
}
