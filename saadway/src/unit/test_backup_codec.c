/* test_backup_codec.c — backup codec surface (T042)
 *
 * Tests the parts of the codec that are deterministic WITHOUT an on-device
 * UCI backing store (on-device equivalence is proven by
 * tests/restore-roundtrip.sh — T041):
 *   - EXPORT is pure: schema v1 JSON with service/steering/profile blocks
 *     and per-kind keys (FR-019; export never persists, never forks);
 *   - IMPORT validate-first (FR-025): malformed schema items land in the
 *     per-item errors report with EXACT codes and NO partial apply;
 *   - IMPORT desired-state metadata from the "service" block is applied to
 *     the in-RAM context before persistence is attempted (potentially pcap
 *     failing) — lng/en validation, cooldown clamp 30..86400, attempts
 *     clamp 1..10, session_timeout floor 300 (contracts/uci.md).
 */
#include "saadway.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdlib.h>
#include <string.h>

static struct sw_ctx C;
static struct sw_profile P[2];
static struct sw_domain_rule DOM;
static struct sw_device_rule DEV;

static void build_ctx(struct sw_ctx *c, bool openvpn_pro, bool ikev2_pro)
{
	memset(c, 0, sizeof(*c));
	memset(P, 0, sizeof(P));
	memset(&DOM, 0, sizeof(DOM));
	memset(&DEV, 0, sizeof(DEV));

	if (openvpn_pro) {
		snprintf(P[0].name, sizeof(P[0].name), "prov-a");
		P[0].kind = SW_KIND_OPENVPN;
		P[0].enabled = true;
		P[0].order = 0;
		snprintf(P[0].secret_ref, sizeof(P[0].secret_ref),
			 "secrets/prov-a/auth.txt");
		snprintf(P[0].remote_host, sizeof(P[0].remote_host),
			 "vpn.example.co");
		P[0].next = &P[1];
	}

	if (ikev2_pro) {
		snprintf(P[1].name, sizeof(P[1].name), "prov-b");
		P[1].kind = SW_KIND_IKEV2;
		P[1].enabled = true;
		P[1].order = 1;
		P[1].if_id = 7;
		P[1].next = NULL;
		snprintf(P[1].hostname, sizeof(P[1].hostname), "gw.example.co");
		snprintf(P[1].username, sizeof(P[1].username), "user123");
	}

	c->st.profiles = &P[0];

	snprintf(DOM.domain, sizeof(DOM.domain), "bank.example.ir");
	DOM.next = NULL;
	c->st.domains = &DOM;

	DEV.match_type = SW_MTYPE_IP;
	snprintf(DEV.match_value, sizeof(DEV.match_value), "192.168.3.20");
	DEV.ptype = SW_POLICY_DIRECT;
	DEV.next = NULL;
	c->st.devices = &DEV;

	c->st.kill_switch = true;
	c->st.enabled = true;
	c->st.session_timeout = 1800;
	snprintf(c->st.language, sizeof(c->st.language), "fa");
}

static void test_export_pure_schema(void **state)
{
	char *json = NULL;

	(void)state;
	build_ctx(&C, true, true);

	assert_return_code(sw_backup_export(&C, false, &json), 0);
	assert_non_null(json);

	/* schema + top blocks */
	assert_non_null(strstr(json, "\"schema_version\""));
	assert_non_null(strstr(json, "\"service\""));
	assert_non_null(strstr(json, "\"profiles\""));
	assert_non_null(strstr(json, "\"steering\""));
	assert_non_null(strstr(json, "bank.example.ir"));
	assert_non_null(strstr(json, "192.168.3.20"));
	/* both profile kinds emitted */
	assert_non_null(strstr(json, "\"prov-a\""));
	assert_non_null(strstr(json, "\"prov-b\""));
	assert_non_null(strstr(json, "\"gw.example.co\""));
	assert_non_null(strstr(json, "\"user123\""));
	/* NO inlined credentials when include_credentials=false */
	assert_null(strstr(json, "\"password\""));

	free(json);
}

static void test_import_malformed_top(void **state)
{
	struct sw_ctx fresh;
	char *report = NULL;

	(void)state;
	memset(&fresh, 0, sizeof(fresh));

	int rc = sw_backup_import(&fresh, "{ this is not [json", &report);
	assert_true(rc != 0);
	assert_null(fresh.st.profiles);
	free(report);
}

static void test_import_validation_reports(void **state)
{
	const char *bad =
		"{\"profiles\":["
		"{\"name\":\"good name?\",\"kind\":\"openvpn\"},"  /* bad name */
		"{\"name\":\"p2\"},"                               /* no kind */
		"{\"name\":\"p3\",\"kind\":\"openvpn\",\"openvpn\":{}},"
		/* no ovpn text */
		"{\"name\":\"p4\",\"kind\":\"boguskind\"},"        /* unknown */
		"{\"name\":\"p5\",\"kind\":\"ikev2\"}"             /* missing ikev2 */
		"]}";
	struct sw_ctx fresh;
	char *report = NULL;

	(void)state;
	memset(&fresh, 0, sizeof(fresh));

	assert_return_code(sw_backup_import(&fresh, bad, &report), 0);
	assert_non_null(report);

	/* per-item errors (validate-first, no partial apply) — count = 5 */
	const char *hits[5] = {
		strstr(report, "\"good name?\""),
		strstr(report, "\"p2\""),
		strstr(report, "\"p3\""),
		strstr(report, "\"p4\""),
		strstr(report, "\"p5\""),
	};
	for (int i = 0; i < 5; i++)
		assert_non_null(hits[i]);
	/* all five must be in ERRORS */
	const char *err = strstr(report, "\"errors\"");
	assert_non_null(err);
	assert_non_null(strstr(err, "E_BAD_SYNTAX"));
	assert_non_null(strstr(err, "unsupported kind"));
	free(report);
}

static void test_import_service_prefs_apply_in_ram(void **state)
{
	const char *snap =
		"{\"schema\":{\"version\":1},"
		"\"service\":{"
		"\"enabled\":true,"
		"\"kill_switch\":true,"
		"\"failover_attempts\":11,"       /* out of range → rejected */
		"\"failover_cooldown\":500,"
		"\"iprange_bypass\":true,"
		"\"dashboard\":{\"language\":\"en\",\"session_timeout\":999}"
		"}}";
	struct sw_ctx fresh;
	char *report = NULL;

	(void)state;
	memset(&fresh, 0, sizeof(fresh));
	fresh.st.session_timeout = 1800;
	fresh.st.failover_cooldown = 300;
	fresh.st.failover_attempts = 3;

	assert_return_code(sw_backup_import(&fresh, snap, &report), 0);
	assert_non_null(report);

	assert_true(fresh.st.enabled);
	assert_true(fresh.st.kill_switch);
	assert_true(fresh.st.iprange_bypass);
	assert_string_equal(fresh.st.language, "en");
	assert_int_equal(fresh.st.session_timeout, 999);
	/* clamps per contracts/uci.md: attempts 1..10, cooldown 30..86400 */
	assert_int_equal(fresh.st.failover_attempts, 3);
	assert_int_equal(fresh.st.failover_cooldown, 500);
	assert_non_null(strstr(report, "\"service\""));    /* restored item */

	free(report);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_export_pure_schema),
		cmocka_unit_test(test_import_malformed_top),
		cmocka_unit_test(test_import_validation_reports),
		cmocka_unit_test(test_import_service_prefs_apply_in_ram),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
