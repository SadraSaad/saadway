#!/bin/sh
# dashboard-credential-audit.sh — host-side audit of the dashboard credential
# (T073, Phase 22).
#
# The dashboard ships with the default credential 'admin' / 'admin'. The
# credential must therefore satisfy a set of invariants that a plain
# "username = admin" change would silently break:
#
#   A  the rpcd login mapping references the SHADOW row, never a value:
#      username='admin' + password='$p$admin' (upstream resolves '$p$<name>'
#      with getspnam(<name>)), the package is committed (a batch alone writes
#      only a /tmp delta), and the ACL grant stays least-privilege
#   B  no plaintext credential material in any shipped UCI-writing file
#   C  the SPA publishes no password: the username is pre-filled, the
#      password input carries no value (the SPA is served to every LAN
#      client — a pre-filled password would publish the credential)
#   D  the provisioning applies the password over STDIN, never as an
#      argument (argv is readable through /proc/<pid>/cmdline), probes for a
#      password setter and fails closed when none exists
#   E  the legacy 'saadway-admin' login is gone from the shipped tree — it
#      may only survive in the provisioning script as the cleanup reference
#
# Comments are stripped before every check: a helper name or a path in a
# comment is documentation, not a credential.
#
# Usage: tests/dashboard-credential-audit.sh [tree-root]
#   tree-root  directory holding saadway/ and luci-app-saadway/
#              (default: the repository root) — the negative test points it
#              at a scratch tree with the pre-change files
#
# Exit: 0 = clean, 1 = violation(s), 2 = setup error.
#
# The negative fixture tests/fixtures/50-saadway-rpcd.prefix is a verbatim
# snapshot of the PRE-CHANGE login script; the audit must FAIL against it.

set -u

REPO=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
TREE=${1:-$REPO}

RPCD="$TREE/saadway/files/uci-defaults/50-saadway-rpcd"
PROVISION="$TREE/saadway/files/saadway-provision"
INDEX="$TREE/saadway/files/htdocs/saadway/index.html"
ACL="$TREE/saadway/files/rpc-acl.json"

die() {
	echo "dashboard-credential-audit: $1" >&2
	exit 2
}

for f in "$RPCD" "$PROVISION" "$INDEX" "$ACL"; do
	[ -f "$f" ] || die "missing '$f'"
done

failures=0
report_pass() { printf '  ok    %s\n' "$1"; }
report_fail() {
	printf '  FAIL  %s\n' "$1" >&2
	failures=$((failures + 1))
}

# strip shell comments (# to end of line)
strip_sh_comments() { sed 's/#.*$//' "$1"; }
# strip HTML comments
strip_html_comments() { sed 's/<!--.*-->//' "$1"; }

# grep a tree with the comments stripped PER FILE (the comment marker must be
# removed before the file:line prefix is added, otherwise an emptied comment
# line still looks like a hit)
grep_tree_nocomment() {
	_root=$1
	_pattern=$2
	find "$_root" -type f 2>/dev/null | sort | while IFS= read -r _f; do
		strip_sh_comments "$_f" |
			grep -n "$_pattern" 2>/dev/null | sed "s|^|$_f:|"
	done
}

DEFAULT_USER='admin'
DEFAULT_PASSWORD='admin'

# ---------------------------------------------------------------------
# A — the rpcd login mapping
# ---------------------------------------------------------------------
echo "check A — rpcd login mapping ($RPCD)"
rpcd_code=$(strip_sh_comments "$RPCD")

if printf '%s\n' "$rpcd_code" | grep -q "username='${DEFAULT_USER}'"; then
	report_pass "username='${DEFAULT_USER}'"
else
	report_fail "the login section does not set username='${DEFAULT_USER}'"
fi

pw_lines=$(printf '%s\n' "$rpcd_code" | grep -c 'password=' || true)
if [ "$pw_lines" -eq 0 ]; then
	report_fail "the login section sets no password option"
elif printf '%s\n' "$rpcd_code" | grep -q "password='\$p\$${DEFAULT_USER}'"; then
	report_pass "password='\$p\$${DEFAULT_USER}' (shadow reference, not a value)"
else
	report_fail "the password option is not the \$p\$ shadow reference of '${DEFAULT_USER}'"
fi

if printf '%s\n' "$rpcd_code" | grep -q 'commit rpcd'; then
	report_pass "the rpcd package is committed (the mapping survives a reboot)"
else
	report_fail "no 'commit rpcd' — uci batch alone only writes the /tmp delta"
fi

if printf '%s\n' "$rpcd_code" | grep -q "read='saadway'" &&
	printf '%s\n' "$rpcd_code" | grep -q "write='saadway'"; then
	report_pass "the ACL grant stays least-privilege ('saadway')"
else
	report_fail "the least-privilege ACL grant is missing"
fi

if printf '%s\n' "$rpcd_code" | grep -q "read='\*'\|write='\*'"; then
	report_fail "the login section grants a wildcard ACL"
else
	report_pass "no wildcard ACL grant"
fi

# ---------------------------------------------------------------------
# B — no plaintext credential material in the shipped UCI-writing files
# ---------------------------------------------------------------------
echo "check B — no plaintext credential material in the shipped tree"
bad_pw=$(grep_tree_nocomment "$TREE/saadway/files" "option password\|password=" |
	grep -vF "'\$p\$" | grep -v ':[[:space:]]*$')
bad_pw="$bad_pw$(grep_tree_nocomment "$TREE/luci-app-saadway" "option password\|password=" |
	grep -vF "'\$p\$" | grep -v ':[[:space:]]*$')"
if [ -z "$bad_pw" ]; then
	report_pass "every password reference is a \$p\$ shadow reference"
else
	report_fail "a password value is not a \$p\$ reference:"
	printf '%s\n' "$bad_pw" | sed 's/^/        /' >&2
fi

if grep -rq "\"password\"" "$ACL" 2>/dev/null; then
	report_fail "the ACL file carries a password field"
else
	report_pass "the ACL file carries no credential field"
fi

# ---------------------------------------------------------------------
# C — the SPA publishes no password
# ---------------------------------------------------------------------
echo "check C — the SPA page source carries no password ($INDEX)"
index_html=$(strip_html_comments "$INDEX")

user_line=$(printf '%s\n' "$index_html" | grep 'id="login-user"')
pass_line=$(printf '%s\n' "$index_html" | grep 'id="login-pass"')

if printf '%s\n' "$user_line" | grep -q "value=\"${DEFAULT_USER}\""; then
	report_pass "the username input is pre-filled with '${DEFAULT_USER}'"
else
	report_fail "the username input is not pre-filled with '${DEFAULT_USER}'"
fi

if [ -z "$pass_line" ]; then
	report_fail "the password input (id=\"login-pass\") is missing"
elif printf '%s\n' "$pass_line" | grep -q 'value='; then
	report_fail "the password input carries a value — the credential would be published in the page source"
elif printf '%s\n' "$pass_line" | grep -q "$DEFAULT_PASSWORD"; then
	report_fail "the password input carries the default password"
else
	report_pass "the password input carries no value"
fi

# ---------------------------------------------------------------------
# D — the provisioning applies the password over stdin
# ---------------------------------------------------------------------
echo "check D — the provisioning never passes the password as an argument ($PROVISION)"
prov_code=$(strip_sh_comments "$PROVISION")

leak=$(printf '%s\n' "$prov_code" |
	grep -E '(chpasswd|passwd|cryptpw)' | grep "$DEFAULT_PASSWORD")
if [ -z "$leak" ]; then
	report_pass "the default password never appears in a password-setting command line"
else
	report_fail "the default password appears next to a password setter (argv is world-readable):"
	printf '%s\n' "$leak" | sed 's/^/        /' >&2
fi

if printf '%s\n' "$prov_code" | grep -q 'command -v chpasswd'; then
	report_pass "the script probes for a password setter"
else
	report_fail "the script does not probe for a password setter"
fi

if printf '%s\n' "$prov_code" | grep -qE 'exit 1|fail "'; then
	report_pass "the script fails closed when the password cannot be applied"
else
	report_fail "the script has no fail-closed path for the password step"
fi

# ---------------------------------------------------------------------
# E — the legacy login is gone from the shipped tree
# ---------------------------------------------------------------------
echo "check E — the legacy 'saadway-admin' login is retired"
legacy_hits=$(grep_tree_nocomment "$TREE/saadway/files" 'saadway-admin' |
	grep -v "^$PROVISION:" | grep -v ':[[:space:]]*$')
legacy_hits="$legacy_hits$(grep_tree_nocomment "$TREE/luci-app-saadway" 'saadway-admin' |
	grep -v ':[[:space:]]*$')"
if [ -z "$legacy_hits" ]; then
	report_pass "the legacy name survives only in the provisioning cleanup path"
else
	report_fail "the legacy login name is still referenced:"
	printf '%s\n' "$legacy_hits" | sed 's/^/        /' >&2
fi

# ---------------------------------------------------------------------
if [ "$failures" -ne 0 ]; then
	echo "dashboard-credential-audit: FAIL — $failures check(s) failed" >&2
	exit 1
fi
echo "dashboard-credential-audit: OK — the dashboard credential contract holds"
exit 0
