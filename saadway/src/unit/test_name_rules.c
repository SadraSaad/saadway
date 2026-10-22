/* test_name_rules.c — the profile-name rule (T104)
 *
 * Pins the contract of sw_valid_name() (saadway/src/uci-state.c): the
 * rule the daemon enforces on every profile name, and the rule the
 * dashboard mirrors client-side (js/app.js) before it sends a request.
 *
 * The rule is a POSIX-safety rule, NOT a Latin-only one:
 *   - non-empty, at most 64 BYTES;
 *   - no leading '.';
 *   - no byte below 0x21 (so no space and no control character);
 *   - no '"', no '\'' and no 0x7f;
 *   - PERSIAN / UTF-8 NAMES ARE VALID — the dashboard is Persian-first
 *     (FR-027); the Persian cases below are part of the contract and
 *     must not be "fixed" into rejections.
 */
#include "saadway.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>

static void test_accepts_plain_names(void **state)
{
	(void)state;
	assert_true(sw_valid_name("prov-a"));
	assert_true(sw_valid_name("UPPER_ok"));
	assert_true(sw_valid_name("123"));
	assert_true(sw_valid_name("a"));
	assert_true(sw_valid_name("provider-a-ovpn"));
}

static void test_accepts_persian_names(void **state)
{
	(void)state;
	/* FR-027: the dashboard is Persian-first — a Persian profile name
	 * is valid and must keep working. */
	assert_true(sw_valid_name("پروفایل"));
	assert_true(sw_valid_name("پروفایل-۱"));
	assert_true(sw_valid_name("تست-۲۰۲۶"));
}

static void test_rejects_empty_and_too_long(void **state)
{
	char longname[70];

	(void)state;
	assert_false(sw_valid_name(""));
	assert_false(sw_valid_name(NULL));

	memset(longname, 'a', 64);
	longname[64] = 0;
	assert_true(sw_valid_name(longname));	/* exactly 64 bytes is fine */

	memset(longname, 'a', 65);
	longname[65] = 0;
	assert_false(sw_valid_name(longname));	/* 65 bytes is too long */
}

static void test_rejects_dot_and_spaces(void **state)
{
	(void)state;
	assert_false(sw_valid_name(".hidden"));
	assert_false(sw_valid_name(" leading"));
	assert_false(sw_valid_name("trailing "));
	assert_false(sw_valid_name("in ner"));
	assert_false(sw_valid_name("tab\there"));
}

static void test_rejects_quotes_and_control_chars(void **state)
{
	char del[3];

	(void)state;
	assert_false(sw_valid_name("a\"b"));
	assert_false(sw_valid_name("a'b"));

	del[0] = 'a';
	del[1] = 0x7f;
	del[2] = 0;
	assert_false(sw_valid_name(del));
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_accepts_plain_names),
		cmocka_unit_test(test_accepts_persian_names),
		cmocka_unit_test(test_rejects_empty_and_too_long),
		cmocka_unit_test(test_rejects_dot_and_spaces),
		cmocka_unit_test(test_rejects_quotes_and_control_chars),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
