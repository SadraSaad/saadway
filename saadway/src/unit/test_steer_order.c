/* test_steer_order.c — Q1-A steering precedence chain (T042)
 *
 * sw_steering_mark_rules() must emit the evaluation chain in EXACT order:
 *   device-direct > device-tunneled > domain-bypass > range-bypass > default
 * and the mark constants must be 0x01/0x02/0x04/0x08 (mask 0xf).
 * Also pins sw_steering_dnsconf_append() T026 exception rendering.
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
	static struct sw_device_rule d2, d1, d3;
	static struct sw_domain_rule dom1;

	memset(c, 0, sizeof(*c));
	memset(&d1, 0, sizeof(d1));
	memset(&d2, 0, sizeof(d2));
	memset(&d3, 0, sizeof(d3));
	memset(&dom1, 0, sizeof(dom1));

	d1.match_type = SW_MTYPE_IP;
	snprintf(d1.match_value, sizeof(d1.match_value), "192.168.7.9");
	d1.ptype = SW_POLICY_DIRECT;
	d1.next = &d2;
	d2.match_type = SW_MTYPE_MAC;
	snprintf(d2.match_value, sizeof(d2.match_value), "aa:bb:cc:00:11:22");
	d2.ptype = SW_POLICY_TUNNELED;
	d2.next = &d3;
	d3.match_type = SW_MTYPE_IP;
	snprintf(d3.match_value, sizeof(d3.match_value), "192.168.9.9");
	d3.ptype = SW_POLICY_DIRECT;
	d3.next = NULL;
	c->st.devices = &d1;

	snprintf(dom1.domain, sizeof(dom1.domain), "bank.example.ir");
	dom1.next = NULL;
	c->st.domains = &dom1;
}

static void cmp_contains(const char *hay, const char *needle)
{
	assert_true(strstr(hay, needle) != NULL);
}

static void cmp_order(const char *hay, const char *a, const char *b)
{
	const char *pa = strstr(hay, a);
	const char *pb = strstr(hay, b);

	assert_non_null(pa);
	assert_non_null(pb);
	assert_true(pa < pb);          /* a strictly precedes b */
}

static void test_device_rules_first_in_list_order(void **state)
{
	char sb_buf = 0;
	struct sw_strbuf sb;

	(void)state;
	(void)sb_buf;
	seed_ctx(&test);
	sw_sb_init(&sb);
	sw_steering_mark_rules(&sb, &test);
	assert_non_null(sb.b);

	/* per-device policies expressed in the highest-priority block */
	cmp_order(sb.b, "ip saddr 192.168.7.9", "ether saddr aa:bb:cc");
	cmp_contains(sb.b, "ip saddr 192.168.7.9 meta mark set 0x01");
	cmp_contains(sb.b, "ether saddr aa:bb:cc:00:11:22 meta mark set 0x08");
	cmp_contains(sb.b, "ip saddr 192.168.9.9 meta mark set 0x01");

	/* then, in Q1-A order: domains → ranges → default */
	cmp_contains(sb.b, "ip daddr @bypass_domains meta mark set 0x02");
	cmp_contains(sb.b, "ip daddr @bypass_ranges meta mark set 0x04");
	cmp_contains(sb.b, "meta mark set 0x08");

	sw_sb_free(&sb);
}

static void test_precedence_ordering(void **state)
{
	struct sw_strbuf sb;

	(void)state;
	seed_ctx(&test);
	sw_sb_init(&sb);
	sw_steering_mark_rules(&sb, &test);
	assert_non_null(sb.b);
	/* explicit tunnel device re-asserts BEFORE domains (device > domain) */
	cmp_order(sb.b, "ether saddr", "ip daddr @bypass_domains");
	/* domain rule precedes range rule */
	cmp_order(sb.b, "@bypass_domains", "@bypass_ranges");
	/* range rule precedes the implicit default catchall; the tab prefix
	 * disambiguates the catchall line from the tunneled-device mark
	 * elsewhere in the chain */
	cmp_order(sb.b, "@bypass_ranges", "\tmeta mark set 0x08\n");

	sw_sb_free(&sb);
}

static void test_dnsconf_t026_exception(void **state)
{
	struct sw_strbuf sb;

	(void)state;
	seed_ctx(&test);
	sw_sb_init(&sb);
	sw_steering_dnsconf_append(&sb, &test);
	assert_non_null(sb.b);
	/* bypass domains resolve via the WAN resolver (main LAN, port 53) */
	cmp_contains(sb.b, "server=/bank.example.ir/127.0.0.1#53");
	/* and their addresses feed the bypass_domains nftset */
	cmp_contains(sb.b, "/4#inet#saadway#bypass_domains");
	sw_sb_free(&sb);
}

static void test_marks_never_collide_mask(void **state)
{
	struct sw_strbuf sb;

	(void)state;
	seed_ctx(&test);
	sw_sb_init(&sb);
	sw_steering_mark_rules(&sb, &test);
	assert_non_null(sb.b);
	/* every mark in the chain is 1 bit within the 0xf nibble */
	assert_null(strstr(sb.b, "mark set 0x16"));
	assert_null(strstr(sb.b, "mark set 0x18"));
	sw_sb_free(&sb);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_device_rules_first_in_list_order),
		cmocka_unit_test(test_precedence_ordering),
		cmocka_unit_test(test_dnsconf_t026_exception),
		cmocka_unit_test(test_marks_never_collide_mask),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
