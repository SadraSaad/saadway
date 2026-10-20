/* test-stubs.c — unit-test harness process boundary (T042)
 *
 * Replaces the parts of main.c (activation engine + child registry) that
 * cannot exist in a host unit test, so the failover state machine can be
 * driven deterministically: every spawn attempt FAILS (-1), every registry
 * query reports "no child", and the tunnel-stop wrappers are no-ops that
 * log. This is documented as a boundary shim, not a behavioral fake: the
 * unit under test (failover.c) receives exactly the spawn-failed verdict
 * it is required to roll on. See unit/README.md.
 */
#define _GNU_SOURCE
#include "saadway.h"

#include <stdlib.h>
#include <stdio.h>

struct sw_ctx *global_ctx;   /* owned by main.c; unit harness uses NULL */

int sw_activate_profile(struct sw_ctx *c, struct sw_profile *p,
			struct ubus_request_data *req)
{
	(void)c;
	(void)req;
	sw_warn("unit: activate '%s' — spawn stubbed to fail", p->name);
	return -1;
}

pid_t sw_child_find(int type, void *arg)
{
	(void)type; (void)arg;
	return 0;                    /* no child tracked in unit scope */
}

void sw_child_track(pid_t pid, int type, void *arg)
{
	(void)pid; (void)type; (void)arg;
}

bool sw_child_take(pid_t pid, int *type, void **arg)
{
	(void)pid;
	if (type) *type = -1;
	if (arg)  *arg = NULL;
	return false;
}

void sw_child_quit_tunnel(void *arg)
{
	(void)arg;
}

void sw_tunnel_cancel_children(struct sw_ctx *c)
{
	(void)c;
}

void sw_deactivate_all(struct sw_ctx *c, bool apply_policy)
{
	(void)apply_policy;
	if (!c)
		return;
	c->active = NULL;
	c->tun_if[0] = 0;
	c->st.active_profile[0] = 0;
}

void sw_reload(void)
{
}

void sw_tick(struct uloop_timeout *t)
{
	(void)t;
}

void sw_child_notify(struct uloop_process *up, int ret)
{
	(void)up; (void)ret;
}

void sw_dnsfile_check(void)
{
}
