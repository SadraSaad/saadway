/*
 * main.c — saadwayd entrypoint, single poll() event loop, child registry,
 * activation/switch engine and boot recovery (T004/T014/T040)
 *
 * Constitution Principle II: ONE uloop poll loop; every timer below has
 * an explicit constant from the D2 table:
 *   t_probe : watchdog probe   → probe_interval   (default 30 s)
 *   t_stats : counters         → stats_interval   (default 1 s)
 *   t_tick  : 1 s housekeeping → activation deadlines, cooldown timer,
 *                                signal flags, deadlines re-check
 * Every outbound network operation is either poll-driven in-process
 * (health.c) or a supervised child with a hard budget (tunnel daemons,
 * system(3)-bounded local control ops only).
 *
 * Activation engine (T014, SC-001 ≤10 s): profile.activate issues a
 * deferred ubus reply; the engine verifies the L3 interface appears
 * (Principle III) and replies success/error within the switch-report
 * budget, while the Failover machine handles any subsequent cycle work.
 * FR-014 ordering: containment lands BEFORE any re-open; the tunnel
 * route is applied only after the interface is verified present.
 */
#define _GNU_SOURCE
#include "saadway.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <libubox/blobmsg.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

#define SW_DNSFILES_MAX 20

static struct blob_buf bb;

/* ------------------------------------------------------------------ */
/* child registry (tunnel children, dnsmasq instance; reaper honors)    */
/* ------------------------------------------------------------------ */

#define SW_CHILD_MAX 16

struct child {
	struct uloop_process up;        /* uloop auto-waits on SIGCHLD */
	pid_t pid;
	int type;
	void *arg;
	bool in_use;
};

static struct child children[SW_CHILD_MAX];
static volatile sig_atomic_t g_hup, g_term;

/* container_of for the registry entries */
static struct child *child_of(struct uloop_process *up)
{
	return (struct child *)((char *)up - offsetof(struct child, up));
}

void sw_child_track(pid_t pid, int type, void *arg)
{
	struct child *chip = NULL;

	for (int i = 0; i < SW_CHILD_MAX; i++)
		if (!children[i].in_use) {
			chip = &children[i];
			break;
		}
	if (!chip) {
		sw_err("child registry full; child %d untracked", (int)pid);
		return;
	}
	chip->pid = pid;
	chip->type = type;
	chip->arg = arg;
	chip->in_use = true;
	memset(&chip->up, 0, sizeof(chip->up));
	chip->up.pid = pid;
	chip->up.cb = sw_child_notify;
	uloop_process_add(&chip->up);
}

bool sw_child_take(pid_t pid, int *type, void **arg)
{
	for (int i = 0; i < SW_CHILD_MAX; i++) {
		if (children[i].in_use && children[i].pid == pid) {
			if (type)
				*type = children[i].type;
			if (arg)
				*arg = children[i].arg;
			uloop_process_delete(&children[i].up);
			children[i].in_use = false;
			return true;
		}
	}
	if (type)
		*type = -1;
	if (arg)
		*arg = NULL;
	return false;
}

pid_t sw_child_find(int type, void *arg)
{
	for (int i = 0; i < SW_CHILD_MAX; i++)
		if (children[i].in_use && children[i].type == type &&
		    children[i].arg == arg)
			return children[i].pid;
	return 0;
}

/* fail-safe teardown: every tracked tunnel child gets a bounded kill */
void sw_tunnel_cancel_children(struct sw_ctx *c)
{
	(void)c;
	for (int i = 0; i < SW_CHILD_MAX; i++) {
		if (!children[i].in_use || children[i].type != SW_CHILD_TUNNEL)
			continue;
		kill(children[i].pid, SIGTERM);
	}
}

/* ------------------------------------------------------------------ */
/* activation engine (T014)                                             */
/* ------------------------------------------------------------------ */

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* deferred reply plumbing (profile.activate) */
static void act_reply_free(struct sw_ctx *c, bool with_error,
			   const char *detail)
{
	if (!c->act.dreq)
		return;
	if (with_error) {
		blob_buf_init(&bb, 0);
		blobmsg_add_string(&bb, "detail", detail ? detail :
				   "activation failed");
		ubus_send_reply(c->ubus, &c->act.dreq->hdr, bb.head);
		blob_buf_free(&bb);
		ubus_complete_deferred_request(c->ubus, c->act.dreq,
					       UBUS_STATUS_UNKNOWN_ERROR);
	}
	free(c->act.dreq);
	c->act.dreq = NULL;
}

/* success path: the L3 interface was verified → atomic open + UP */
static int iface_exists(struct sw_ctx *c, const char *ifname)
{
	char path[64];

	snprintf(path, sizeof(path), "/sys/class/net/%s", ifname);
	struct stat st;

	return stat(path, &st) == 0;
}

static void verified_up(struct sw_ctx *c)
{
	struct sw_profile *p = c->act.prof;

	if (!c->act.pending || !p || !iface_exists(c, p->ifname))
		return;

	long elapsed = now_ms() - c->act.started;

	c->st.conn_status = SW_CONN_UP;
	snprintf(c->tun_if, sizeof(c->tun_if), "%s", p->ifname);

	/* FR-014 ordering: the re-open happens ONLY after the interface
	 * is verified up, inside one atomic render+apply.              */
	sw_policy_apply(c, c->st.kill_switch, c->tun_if, true);
	sw_failover_probe_result(c, true);       /* up + counters reset   */
	sw_uci_persist_state(c);
	sw_info("activation '%s' verified (iface=%s, %ld ms)",
		p->name, p->ifname, elapsed);

	if (c->act.dreq) {
		blob_buf_init(&bb, 0);
		blobmsg_add_string(&bb, "switched_to", p->name);
		blobmsg_add_u32(&bb, "elapsed_ms", (uint32_t)elapsed);
		ubus_send_reply(c->ubus, &c->act.dreq->hdr, bb.head);
		blob_buf_free(&bb);
		ubus_complete_deferred_request(c->ubus, c->act.dreq,
					       UBUS_STATUS_OK);
		free(c->act.dreq);
		c->act.dreq = NULL;
	}
	c->act.pending = false;
	c->act.prof = NULL;
}

int sw_activate_profile(struct sw_ctx *c, struct sw_profile *p,
			struct ubus_request_data *req)
{
	int rc;

	if (!p)
		return -1;

	/* single-active invariant: stop the previous tunnel first        */
	if (c->active) {
		sw_info("switching: tearing down '%s' (containment first)",
			c->st.active_profile);
		sw_policy_apply(c, true, NULL, false);   /* FR-014       */
		sw_tunnel_cancel_children(c);
		if (c->active->kind == SW_KIND_OPENVPN)
			sw_ovpn_stop(c, c->active, true);
		else
			sw_ikev2_stop(c, c->active, true);
		c->active = NULL;
		c->tun_if[0] = 0;
	} else {
		/* cold start: the desired pre-tunnel baseline (direct when
		 * the kill switch is off, containment otherwise)          */
		sw_policy_apply_now(c);
	}

	/* deferred reply for the switch report (SC-001 ≤10 s)            */
	free(c->act.dreq);
	c->act.dreq = NULL;
	if (req && c->ubus) {
		if (ubus_defer_request(c->ubus, req, &c->act.dreq) != 0) {
			c->act.dreq = NULL;
			return -1;
		}
	}

	c->st.conn_status = SW_CONN_CONNECTING;
	snprintf(c->st.active_profile, sizeof(c->st.active_profile), "%s",
		 p->name);
	sw_uci_persist_state(c);

	c->act.pending = true;
	c->act.prof = p;
	c->act.started = now_ms();
	c->act.deadline = time(NULL) + SW_T_SWITCH_REPORT;   /* reply cap */

	rc = p->kind == SW_KIND_OPENVPN ? sw_ovpn_start(c, p) :
					  sw_ikev2_start(c, p);
	if (rc != 0) {
		/* synchronous spawn failure: reply + roll immediately    */
		if (c->act.dreq) {
			blob_buf_init(&bb, 0);
			blobmsg_add_string(&bb, "detail",
					   "spawn failed (see log)");
			ubus_send_reply(c->ubus, &c->act.dreq->hdr, bb.head);
			blob_buf_free(&bb);
			ubus_complete_deferred_request(c->ubus, c->act.dreq,
					UBUS_STATUS_UNKNOWN_ERROR);
			free(c->act.dreq);
			c->act.dreq = NULL;
		}
		c->act.pending = false;
		c->act.prof = NULL;
		return -1;
	}
	return 0;
}

void sw_deactivate_all(struct sw_ctx *c, bool apply_policy)
{
	sw_tunnel_cancel_children(c);
	if (c->active) {
		if (c->active->kind == SW_KIND_OPENVPN)
			sw_ovpn_stop(c, c->active, true);
		else
			sw_ikev2_stop(c, c->active, true);
	}
	c->active = NULL;
	c->tun_if[0] = 0;
	c->st.active_profile[0] = 0;
	if (apply_policy) {
		if (c->st.kill_switch) {
			sw_policy_apply(c, true, NULL, false);
			c->st.conn_status = SW_CONN_CONTAINED;
		} else {
			sw_policy_apply_direct(c);
			c->st.conn_status = SW_CONN_DOWN;
		}
	} else {
		c->st.conn_status = SW_CONN_DOWN;
	}
	sw_uci_persist_state(c);
}

/* ------------------------------------------------------------------ */
/* child reaper dispatch                                                */
/* ------------------------------------------------------------------ */

void sw_child_notify(struct uloop_process *up, int ret)
{
	struct child *chip = child_of(up);
	struct sw_ctx *c = global_ctx;
	pid_t pid = chip->pid;
	int type = chip->type;
	void *arg = chip->arg;

	sw_child_take(pid, &type, &arg);    /* de-register                 */

	if (!c || c->terminating)
		return;

	if (type == SW_CHILD_DNSMASQ) {
		/* supervised resolver instance: respawn quickly          */
		sw_warn("dnsmasq instance exited (pid %d) — respawning",
			(int)pid);
		sw_dns_reload_conf(c);
		return;
	}
	if (type == SW_CHILD_TUNNEL) {
		struct sw_profile *p = (struct sw_profile *)arg;

		if (p && p->kind == SW_KIND_IKEV2 &&
		    WIFEXITED(ret) && WEXITSTATUS(ret) == 0) {
			/* IKEv2 initiate verdict: SA established           */
			verified_up(c);
			return;
		}
		/* nonzero / signal → treat as failed attempt             */
		sw_warn("tunnel child '%s' exited rc=%d",
			p ? p->name : "?", WIFEXITED(ret) ?
			WEXITSTATUS(ret) : -WTERMSIG(ret));
		if (c->act.pending) {
			act_reply_free(c, true,
				       "tunnel process exited before "
				       "establishing");
			c->act.pending = false;
			c->act.prof = NULL;
		}
		sw_failover_child_died(c);
		return;
	}
	/* SW_CHILD_PING etc.: nothing to do (bespoke probes exit)        */
}

/* ------------------------------------------------------------------ */
/* housekeeping tick (1 s) + watchdog probe + counters                  */
/* ------------------------------------------------------------------ */

static void probe_cb(struct sw_ctx *c, bool ok)
{
	sw_failover_probe_result(c, ok);
}

static void watchdog_probe(struct uloop_timeout *t)
{
	struct sw_ctx *c = global_ctx;

	uloop_timeout_set(t, c->st.probe_interval * 1000);
	if (!c || !c->st.enabled || c->terminating)
		return;
	if (c->st.conn_status != SW_CONN_UP)
		return;
	if (sw_health_is_running())
		return;                       /* dashboard call in flight  */
	if (!c->tun_if[0])
		return;
	sw_health_run(c, probe_cb);
}

static void stats_tick(struct uloop_timeout *t)
{
	struct sw_ctx *c = global_ctx;

	uloop_timeout_set(t, c->st.stats_interval * 1000);
	if (c && !c->terminating)
		sw_counters_tick(c);
}

void sw_tick(struct uloop_timeout *t)
{
	struct sw_ctx *c = global_ctx;
	long now;

	uloop_timeout_set(t, 1000);
	if (!c || c->terminating)
		return;

	/* signals */
	if (g_hup) {
		g_hup = 0;
		sw_reload();
	}
	if (g_term) {
		g_term = 0;
		uloop_end();
		return;
	}

	/* failover cooldown timer */
	sw_failover_cooldown_check(c);

	/* dnsmasq hook-file watcher (mtime-guarded; no reload churn)      */
	sw_dnsfile_check();

	/* activation deadline check */
	if (c->act.pending) {
		now = time(NULL);
		if (now >= c->act.deadline) {
			struct sw_profile *p = c->act.prof;
			bool iface = p && iface_exists(c, p->ifname);

			if (iface)
				verified_up(c);
			else {
				act_reply_free(c, true,
					       "activation timeout: no L3 "
					       "interface appeared");
				c->act.pending = false;
				c->act.prof = NULL;
				sw_failover_child_died(c);
			}
		}
	}
}

void sw_reload(void)
{
	struct sw_ctx *c = global_ctx;

	sw_info("SIGHUP: reloading configuration");
	if (sw_uci_load(c) != SW_UCI_OK)
		sw_err("reload: UCI load failed; keeping last known state");
	sw_policy_apply_now(c);
	sw_dns_reload_conf(c);
	sw_util_mkdirs(SW_RUNSTATE_DIR, 0700);
}

/* dns hook file watcher: tunnel-advertised DNS may change → re-render.
 * Guarded by an mtime+size cache so the per-second tick only notifies on
 * an ACTUAL hook-file change (no dnsmasq churn). */
void sw_dnsfile_check(void)
{
	static char seen_names[20][64];
	static long seen_mtime[20];
	static long seen_size[20];
	int changed = 0;
	int nseen = 0;
	DIR *d = opendir(SW_RUNSTATE_DIR);

	if (!d)
		return;
	/* cheap state-dir sweep: only openvpn-*.dns hook files             */
	struct dirent *de;

	while ((de = readdir(d)) != NULL &&
	       nseen < (int)(sizeof(seen_names) / sizeof(seen_names[0]))) {
		const char *dot = strrchr(de->d_name, '.');

		if (!dot || strcmp(dot, ".dns") ||
		    strncmp(de->d_name, "openvpn-", 8))
			continue;

		char path[SW_PATH_MAX];
		struct stat st;

		snprintf(path, sizeof(path), SW_RUNSTATE_DIR "/%s",
			 de->d_name);
		if (stat(path, &st) != 0)
			continue;
		if ((size_t)snprintf(seen_names[nseen],
				     sizeof(seen_names[0]), "%s",
				     de->d_name) >= sizeof(seen_names[0]))
			continue;

		/* changed vs cached? */
		bool known = false;
		for (int i = 0;
		     i < (int)(sizeof(seen_mtime) / sizeof(seen_mtime[0]));
		     i++)
			if (seen_names[i][0] &&
			    !strcmp(seen_names[i], de->d_name)) {
				known = true;
				if (seen_mtime[i] != (long)st.st_mtime ||
				    seen_size[i] != (long)st.st_size) {
					changed = 1;
					seen_mtime[i] = (long)st.st_mtime;
					seen_size[i] = (long)st.st_size;
				}
				break;
			}
		if (!known) {
			changed = 1;
			seen_mtime[nseen] = (long)st.st_mtime;
			seen_size[nseen] = (long)st.st_size;
		}
		nseen++;
	}
	closedir(d);
	if (changed)
		sw_dns_notify_tunnel_dns_changed();
}

/* ------------------------------------------------------------------ */
/* boot / shutdown (T040)                                               */
/* ------------------------------------------------------------------ */

static void on_signal(int sig)
{
	if (sig == SIGHUP)
		g_hup = 1;
	else
		g_term = 1;
}

static void boot_recovery(struct sw_ctx *c)
{
	/* T040 ordering per contracts/uci.md: containment/state BEFORE the
	 * tunnel; steering rides inside the same policy application      */
	sw_util_mkdirs(SW_BASE_DIR, 0700);
	sw_util_mkdirs(SW_RUNSTATE_DIR, 0700);

	if (c->st.kill_switch)
		sw_policy_apply(c, true, NULL, false);   /* leak-safe      */
	else
		sw_policy_apply_now(c);                  /* direct base    */

	sw_dns_reload_conf(c);   /* tunnel resolver instance               */

	if (!c->st.enabled) {
		sw_info("service disabled by config — waiting");
		return;
	}
	struct sw_profile *p = sw_uci_profile_find(c, c->st.active_profile);

	/* FR-024 / SC-006: reboot returns to the last desired state with
	 * zero manual steps                                              */
	if (p && p->enabled)
		sw_activate_profile(c, p, NULL);
	else if (c->st.active_profile[0])
		sw_warn("boot: stale active_profile '%s' not resumed",
			c->st.active_profile);
}

int main(void)
{
	struct sw_ctx *c = calloc(1, sizeof(*c));
	static struct sigaction sa;

	if (!c)
		return 1;
	global_ctx = c;

	/* dirs the daemon owns */
	sw_util_mkdirs(SW_BASE_DIR, 0700);
	sw_util_mkdirs(SW_RUNSTATE_DIR, 0700);
	sw_util_mkdirs(SW_SWANCTL_DIR, 0700);

	sw_log_init();

	if (sw_uci_load(c) != SW_UCI_OK) {
		sw_err("UCI load failed — aborting (fail closed)");
		return 1;
	}
	sw_failover_init(c);
	sw_steering_init(c);

	/* signals before the loop: SIGHUP → sw_reload, TERM/INT → exit   */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	/* ubus wiring: single client context, object registered          */
	c->ubus = ubus_connect(SW_UBUS_SOCK);
	if (!c->ubus) {
		sw_err("cannot connect ubus at %s: %s", SW_UBUS_SOCK,
		       strerror(errno));
		return 1;
	}
	uloop_init();
	ubus_add_uloop(c->ubus);
	if (sw_ubus_init(c) != 0) {
		sw_err("ubus object registration failed");
		return 1;
	}

	/* boot recovery BEFORE the management surface answers status calls
	 * (the ordering inspires containment-first boot; T040)           */
	boot_recovery(c);

	/* timers */
	c->t_probe.cb = watchdog_probe;
	c->t_stats.cb = stats_tick;
	c->t_tick.cb = sw_tick;
	uloop_timeout_set(&c->t_probe, c->st.probe_interval * 1000);
	uloop_timeout_set(&c->t_stats, c->st.stats_interval * 1000);
	uloop_timeout_set(&c->t_tick, 1000);

	sw_info("saadwayd started (enabled=%d kill_switch=%d)",
		c->st.enabled, c->st.kill_switch);
	{
		struct sw_profile *cnt = c->st.profiles;
		int np = 0;

		while (cnt) {
			np++;
			cnt = cnt->next;
		}
		sw_info("saadwayd active with %d profile(s)", np);
	}

	uloop_run();

	/* orderly shutdown */
	c->terminating = true;
	sw_tunnel_cancel_children(c);
	sw_dns_stop();
	sw_log_shutdown();
	uloop_done();
	ubus_shutdown(c->ubus);
	return 0;
}
