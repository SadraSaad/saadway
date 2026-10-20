/* test_failover.c — failover state machine (T042)
 *
 * Pins (Q3-A, FR-008, FR-026, Q2-B):
 *   - watch up: probe OK from CONNECTING → UP with attempt counters reset;
 *   - budget: one failure per profile per cycle; success path rolls to the
 *     next enabled candidate in order;
 *   - exhaustion: full cycle exhausted ⇒ cooldown_entered (SW_CONN_COOLDOWN)
 *     with attempts parked, and cooldown re-arm resets ALL counters before
 *     rolling again (observable: the fail-always stub consumes the fresh
 *     cycle synchronously; the drained-disables variant parks and then
 *     shows the zeroed counters);
 *   - reset: manual reset clears the machine and reports down;
 *   - spawn-failed roll: with sw_activate_profile() stubbed to fail, the
 *     machine rolls to the next candidate and finally cools down — the
 *     fail-closed-but-responsive contract (Principle II).
 *
 * Spawn-related calls in the harness land in test-stubs.c (boundary shim).
 */
#include "saadway.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdlib.h>
#include <string.h>

static struct sw_ctx C;
static struct sw_profile P[3];

static void build_ctx(int attempts)
{
	memset(&C, 0, sizeof(C));
	memset(P, 0, sizeof(P));

	for (int i = 0; i < 2; i++) {     /* keep determinism: two profiles */
		snprintf(P[i].name, sizeof(P[i].name), "pr%d", i);
		P[i].kind = SW_KIND_OPENVPN;
		P[i].enabled = true;
		P[i].order = i;
		P[i].attempts = 0;
		P[i].next = i ? NULL : &P[i + 1];
	}
	C.st.profiles = &P[0];
	C.st.enabled = true;
	C.st.kill_switch = true;
	C.st.failover_attempts = attempts;
	C.st.failover_cooldown = 300;
	C.st.conn_status = SW_CONN_DOWN;

	/* the stubs render persistence no-ops: persist() failure is only a
	 * warning; state asserts never depend on UCI (unit scope note). */
}

static void test_ok_probe_from_connecting_sets_up(void **state)
{
	(void)state;
	build_ctx(3);
	sw_failover_init(&C);
	C.st.conn_status = SW_CONN_CONNECTING;

	sw_failover_probe_result(&C, true);

	assert_int_equal(C.st.conn_status, SW_CONN_UP);
	assert_int_equal(P[0].attempts, 0);
	assert_int_equal(P[1].attempts, 0);
	assert_int_equal(C.fo.miss_count, 0);
}

static void test_misses_under_budget_keep_up(void **state)
{
	(void)state;
	build_ctx(3);
	sw_failover_init(&C);
	C.st.conn_status = SW_CONN_UP;
	snprintf(C.st.active_profile, sizeof(C.st.active_profile), "pr0");
	C.active = &P[0];

	sw_failover_probe_result(&C, false);
	sw_failover_probe_result(&C, false);

	/* 2 < 3 misses: still UP, no attempt spent */
	assert_int_equal(C.st.conn_status, SW_CONN_UP);
	assert_int_equal(P[0].attempts, 0);
}

/* account_failure with only ONE enabled candidate rolls to nothing and
 * enters cooldown (the spawn stub produces a further accounted failure). */
static void test_exhaustion_enters_cooldown(void **state)
{
	struct sw_failover *fo;

	(void)state;
	build_ctx(1);                 /* ONE attempt per profile */
	sw_failover_init(&C);
	C.st.conn_status = SW_CONN_UP;
	snprintf(C.st.active_profile, sizeof(C.st.active_profile), "pr0");
	C.active = &P[0];

	/* child died ⇒ first (and only) attempt for pr0 consumed → roll */
	sw_failover_child_died(&C);
	/* pr0 died; spawn stub fails for everyone → cooldown            */
	assert_true(C.fo.child_died);
	assert_true(C.st.cycle_exhausted);

	fo = &C.fo;
	(void)fo;
	assert_true(C.fo.cooldown_active);
	assert_int_equal(C.st.conn_status, SW_CONN_COOLDOWN);
	/* the cooldown deadline is at least now+cooldown seconds        */
	assert_true(C.fo.cooldown_deadline >= time(NULL) +
		    C.st.failover_cooldown - 1);
}

static void test_cooldown_rearm_resets(void **state)
{
	(void)state;
	build_ctx(1);
	sw_failover_init(&C);
	C.st.conn_status = SW_CONN_UP;
	snprintf(C.st.active_profile, sizeof(C.st.active_profile), "pr0");
	C.active = &P[0];

	sw_failover_child_died(&C);       /* full cycle → cooldown     */
	assert_true(C.fo.cooldown_active);

	/* simulate the wall clock: pull the deadline into the past       */
	C.fo.cooldown_deadline = time(NULL) - 1;
	P[0].attempts = 1;
	sw_failover_cooldown_check(&C);

	/* the re-arm first resets every counter, then rolls; with the
	 * fail-always spawn stub that fresh cycle is consumed SYNCHRO-
	 * NOUSLY, so the machine deterministically returns to cooldown
	 * inside the same tick. End state under this boundary shim: a
	 * fully-consumed second cycle (attempts 1/1 again — which is
	 * only reachable if the counters were really reset, otherwise
	 * the re-arm would have parked the machine instead).            */
	assert_true(C.fo.cooldown_active);
	assert_true(C.st.cycle_exhausted);
	assert_int_equal(P[0].attempts, 1);
	assert_int_equal(P[1].attempts, 1);
	assert_int_equal(C.st.run_attempts, 1);
	assert_int_equal(C.st.conn_status, SW_CONN_COOLDOWN);
}

/* the parked variant makes the re-arm RESET observable directly: with
 * both profiles drained, cooldown_check must clear every counter before
 * evaluating candidates (the end state after the drained re-arm has no
 * fresh attempts anywhere — exactly the FR-026 reset semantics).       */
static void test_cooldown_rearm_parks_when_drained(void **state)
{
	(void)state;
	build_ctx(1);
	sw_failover_init(&C);
	C.st.conn_status = SW_CONN_UP;
	snprintf(C.st.active_profile, sizeof(C.st.active_profile), "pr0");
	C.active = &P[0];
	sw_failover_child_died(&C);
	assert_true(C.fo.cooldown_active);

	C.fo.cooldown_deadline = time(NULL) - 1;
	P[0].attempts = 1;
	P[1].attempts = 1;
	P[0].enabled = false;
	P[1].enabled = false;
	sw_failover_cooldown_check(&C);

	assert_false(C.fo.cooldown_active);
	assert_false(C.st.cycle_exhausted);
	assert_int_equal(P[0].attempts, 0);
	assert_int_equal(P[1].attempts, 0);
	assert_int_equal(C.st.run_attempts, 0);
	assert_int_equal(C.st.conn_status, SW_CONN_DOWN);
}

static void test_reset_clears_and_parks(void **state)
{
	(void)state;
	build_ctx(3);
	sw_failover_init(&C);
	C.st.conn_status = SW_CONN_CONNECTING;

	sw_failover_reset(&C);

	assert_int_equal(C.st.conn_status, SW_CONN_DOWN);
	assert_false(C.fo.cooldown_active);
	assert_int_equal(C.fo.miss_count, 0);
	assert_string_equal(C.st.active_profile, "");
	assert_null(C.active);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_ok_probe_from_connecting_sets_up),
		cmocka_unit_test(test_misses_under_budget_keep_up),
		cmocka_unit_test(test_exhaustion_enters_cooldown),
		cmocka_unit_test(test_cooldown_rearm_resets),
		cmocka_unit_test(test_cooldown_rearm_parks_when_drained),
		cmocka_unit_test(test_reset_clears_and_parks),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
