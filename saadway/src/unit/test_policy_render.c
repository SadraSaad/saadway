/* test_policy_render.c — atomic policy ruleset renderer (T042)
 *
 * Pins the contract of sw_policy_render() (policy.c):
 *   - the rendered text REPLACES the whole `table inet saadway` inside one
 *     transaction (leading `delete table inet saadway`, Principle IV);
 *   - containment order (FR-014, T018): bypass classes first, then the
 *     tunnel-class drop rule — with a verified tun_if, tunnel-permitted
 *     egress stays allowed for ESTABLISHED flows only;
 *   - kill switch on WITHOUT a verified interface ⇒ total tunnel-class
 *     blackhole (fail-closed, constitution);
 *   - forced-tunnel DNS redirect (T020) to :5353 with device-direct
 *     exemptions (T026);
 *   - never banned tooling references (Gate 1/2 spot: no iptables/ipset/fw3).
 */
#include "saadway.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdlib.h>
#include <string.h>

static struct sw_ctx test;

static void seed_ctx(struct sw_ctx *c)
{
	static struct sw_profile p1, p2;
	static struct sw_domain_rule d1;
	static struct sw_device_rule v1, v2;

	memset(c, 0, sizeof(*c));
	memset(&p1, 0, sizeof(p1));
	memset(&p2, 0, sizeof(p2));
	memset(&d1, 0, sizeof(d1));
	memset(&v1, 0, sizeof(v1));
	memset(&v2, 0, sizeof(v2));

	snprintf(p1.name, sizeof(p1.name), "a");
	p2.next = NULL;
	p1.next = &p2;
	snprintf(p2.name, sizeof(p2.name), "b");

	c->st.profiles = &p1;
	snprintf(d1.domain, sizeof(d1.domain),
		 "bank.example.ir");
	d1.next = NULL;
	c->st.domains = &d1;

	v1.match_type = SW_MTYPE_IP;
	snprintf(v1.match_value, sizeof(v1.match_value), "192.168.1.23");
	v1.ptype = SW_POLICY_DIRECT;
	v1.next = &v2;
	v2.match_type = SW_MTYPE_MAC;
	snprintf(v2.match_value, sizeof(v2.match_value),
		 "AA:BB:CC:DD:EE:FF");
	v2.ptype = SW_POLICY_TUNNELED;
	v2.next = NULL;
	c->st.devices = &v1;

	c->lan_devices = "br-lan";
	c->st.kill_switch = true;
}

static void test_render_full_transaction_keys(void **state)
{
	char *out = NULL;
	size_t len = 0;

	(void)state;
	seed_ctx(&test);
	assert_return_code(sw_policy_render(&test, true, "tun_redirect1",
					   false, &out, &len), 0);
	assert_non_null(out);

	/* single-transaction open: whole replace in the same file */
	assert_non_null(strstr(out, "delete table inet saadway"));
	/* table skeleton */
	assert_non_null(strstr(out, "table inet saadway {"));
	assert_non_null(strstr(out, "chain mark_prerouting"));
	assert_non_null(strstr(out, "chain mark_rules"));
	assert_non_null(strstr(out, "chain dns_redirect"));
	assert_non_null(strstr(out, "chain contain"));
	/* tunnel-default catch-all marking (Q1-A stage 4) */
	assert_non_null(strstr(out, "meta mark set 0x08"));

	free(out);
}

static void test_render_contained_with_if(void **state)
{
	char *out = NULL;
	size_t len = 0;
	const char *c_if = "xfrm_0_0";

	(void)state;
	seed_ctx(&test);
	assert_return_code(sw_policy_render(&test, true, c_if, false, &out,
					   &len), 0);

	/* FR-014/A principles: while CONTAINED and a tunnel exists, marked
	 * egress may leave ONLY via that tunnel; even on the tunnel, only
	 * not-new state (the new flows stay dropped until verification). */
	assert_non_null(strstr(out,
		"oifname != \"xfrm_0_0\" drop"));
	assert_non_null(strstr(out,
		"oifname \"xfrm_0_0\" ct state new drop"));
	/* bypass marks accepted BEFORE the tunnel rules */
	assert_non_null(strstr(out, "0x01 accept\n"));
	assert_non_null(strstr(out, "0x02 accept\n"));
	assert_non_null(strstr(out, "0x04 accept\n"));
	free(out);
}

static void test_render_cold_catch_all_drop(void **state)
{
	char *out = NULL;
	size_t len = 0;

	(void)state;
	seed_ctx(&test);
	/* no verified tun_if → total drop of tunnel class (fail closed) */
	assert_return_code(sw_policy_render(&test, true, "", false, &out,
					   &len), 0);
	assert_non_null(strstr(out, "meta mark & 0x0f == 0x08 drop"));
	assert_null(strstr(out, "oifname"));
	free(out);
}

static void test_render_dns_redirect_and_exemptions(void **state)
{
	char *out = NULL;
	size_t len = 0;

	(void)state;
	seed_ctx(&test);
	assert_return_code(sw_policy_render(&test, false, "tun_redirect1",
					   false, &out, &len), 0);
	/* all dport-53 from marked clients → tunnel resolver */
	assert_non_null(strstr(out, "udp dport 53 redirect to :5353"));
	assert_non_null(strstr(out, "tcp dport 53 redirect to :5353"));
	/* device-direct exemption (T026) */
	assert_non_null(strstr(out, "ip saddr 192.168.1.23 udp dport 53 return"));
	free(out);
}

static void test_render_no_banned_tools(void **state)
{
	char *out = NULL;
	size_t len = 0;

	(void)state;
	seed_ctx(&test);
	assert_return_code(sw_policy_render(&test, true, "tun_x", false, &out,
					   &len), 0);
	assert_null(strstr(out, "iptable"));
	assert_null(strstr(out, "ipset"));
	assert_null(strstr(out, "fw3"));
	free(out);
}

/* ------------------------------------------------------------------ */
/* T103: the per-device sets must be VALID nftables.                    */
/*                                                                    */
/* The renderer used to emit BOTH sets whenever either kind of rule   */
/* existed, with a ";" terminator and a shared separator counter:     */
/*   - a single IP rule produced "elements = { ; }" for dev_mac,      */
/*   - a MAC rule that was not the first rule produced a leading ", ".*/
/* Both are syntax errors: nft rejects the WHOLE table, the          */
/* delete+create transaction fails, the create-only retry fails with  */
/* the same content, and the daemon falls back to the emergency       */
/* containment (the tunnel-class egress blackholed).                   */
/* ------------------------------------------------------------------ */

/* the rendered table must never contain a malformed element list */
static void assert_no_malformed_sets(const char *out)
{
	assert_null(strstr(out, "elements = { ; }"));
	assert_null(strstr(out, "elements = { ,"));
	assert_null(strstr(out, "; } }"));
}

static void test_render_device_sets_both_kinds(void **state)
{
	char *out = NULL;
	size_t len = 0;

	(void)state;
	seed_ctx(&test);
	assert_return_code(sw_policy_render(&test, true, "tun_x", false, &out,
					   &len), 0);
	/* the IP rule and the MAC rule (lowercased) - comma syntax */
	assert_non_null(strstr(out,
		"set dev_ip { type ipv4_addr; flags interval; elements = { 192.168.1.23 } }"));
	assert_non_null(strstr(out,
		"set dev_mac { type ether_addr; elements = { aa:bb:cc:dd:ee:ff } }"));
	/* no leading separator before the MAC element */
	assert_null(strstr(out, "elements = { ,"));
	assert_no_malformed_sets(out);
	free(out);
}

static void test_render_device_sets_ip_only(void **state)
{
	char *out = NULL;
	size_t len = 0;

	(void)state;
	seed_ctx(&test);
	/* keep only the IP rule */
	test.st.devices->next = NULL;

	assert_return_code(sw_policy_render(&test, true, "tun_x", false, &out,
					   &len), 0);
	assert_non_null(strstr(out,
		"set dev_ip { type ipv4_addr; flags interval; elements = { 192.168.1.23 } }"));
	/* the empty set must NOT be emitted at all */
	assert_null(strstr(out, "set dev_mac"));
	assert_no_malformed_sets(out);
	free(out);
}

static void test_render_device_sets_mac_only(void **state)
{
	char *out = NULL;
	size_t len = 0;

	(void)state;
	seed_ctx(&test);
	/* keep only the MAC rule */
	test.st.devices = test.st.devices->next;

	assert_return_code(sw_policy_render(&test, true, "tun_x", false, &out,
					   &len), 0);
	assert_non_null(strstr(out,
		"set dev_mac { type ether_addr; elements = { aa:bb:cc:dd:ee:ff } }"));
	assert_null(strstr(out, "set dev_ip"));
	assert_no_malformed_sets(out);
	free(out);
}

static void test_render_device_sets_none(void **state)
{
	char *out = NULL;
	size_t len = 0;

	(void)state;
	seed_ctx(&test);
	test.st.devices = NULL;

	assert_return_code(sw_policy_render(&test, true, "tun_x", false, &out,
					   &len), 0);
	assert_null(strstr(out, "set dev_ip"));
	assert_null(strstr(out, "set dev_mac"));
	assert_no_malformed_sets(out);
	free(out);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_render_full_transaction_keys),
		cmocka_unit_test(test_render_contained_with_if),
		cmocka_unit_test(test_render_cold_catch_all_drop),
		cmocka_unit_test(test_render_dns_redirect_and_exemptions),
		cmocka_unit_test(test_render_no_banned_tools),
		cmocka_unit_test(test_render_device_sets_both_kinds),
		cmocka_unit_test(test_render_device_sets_ip_only),
		cmocka_unit_test(test_render_device_sets_mac_only),
		cmocka_unit_test(test_render_device_sets_none),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
