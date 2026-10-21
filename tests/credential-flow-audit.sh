#!/bin/sh
# credential-flow-audit.sh — host-side audit of the dashboard credential
# flow (T078, Phase 23).
#
# The dashboard's login credential is a security-sensitive path, and every
# rule below is a rule the code MUST keep. The audit is static and offline:
# it never executes the provisioning helper (that would touch /etc/passwd
# and /etc/shadow) and it never needs a device.
#
#   A  the login mapping is a REFERENCE ('$p$<user>'), never a literal hash
#      in a world-readable UCI file
#   B  the password is PIPED to the setter - never an argument (argv is
#      readable through /proc/<pid>/cmdline)
#   C  the first-boot path provisions NO well-known default password (T079:
#      the standalone dashboard needs no credential, so a default password
#      would be a liability with no consumer), the credential mapping is
#      refreshed only when the account exists, and rpcd is still reloaded
#   D  the credential-change path creates the account BEFORE it updates the
#      mapping (a mapping must never reference a missing account)
#   E  the credential method is granted to a dedicated root-only ACL group
#      and is NOT part of the dashboard's own group
#   F  the daemon exposes the method once and keeps NO credential store
#
# Usage: tests/credential-flow-audit.sh [tree-root]
#   tree-root  directory holding saadway/ and luci-app-saadway/
#              (default: the repository root) — the negative test points it
#              at a scratch tree with the pre-change files
#
# Exit: 0 = clean, 1 = violation(s), 2 = setup error.

set -u

REPO=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
TREE=${1:-$REPO}

RPCD="$TREE/saadway/files/uci-defaults/50-saadway-rpcd"
PROVISION="$TREE/saadway/files/saadway-provision"
RPC_ACL="$TREE/saadway/files/rpc-acl.json"
CRED_ACL="$TREE/luci-app-saadway/root/usr/share/rpcd/acl.d/saadway-credentials.json"
UBUS_SRC="$TREE/saadway/src/ubus.c"
CREDS_SRC="$TREE/saadway/src/creds.c"

die() {
	echo "credential-flow-audit: $1" >&2
	exit 2
}

for f in "$RPCD" "$PROVISION" "$RPC_ACL"; do
	[ -f "$f" ] || die "missing '$f'"
done

failures=0
report_pass() { printf '  ok    %s\n' "$1"; }
report_fail() {
	printf '  FAIL  %s\n' "$1" >&2
	failures=$((failures + 1))
}

# strip shell comments
strip_comments() { sed 's/#.*$//' "$1"; }

# strip C comments (block and line) — a path mentioned in a comment is
# documentation, not code
strip_c_comments() {
	awk '
	BEGIN { inblock = 0 }
	{
		line = $0
		out = ""
		while (1) {
			if (inblock) {
				p = index(line, "*/")
				if (p == 0) { line = ""; break }
				line = substr(line, p + 2)
				inblock = 0
				continue
			}
			p = index(line, "/*")
			q = index(line, "//")
			if (q > 0 && (p == 0 || q < p)) {
				out = out substr(line, 1, q - 1)
				line = ""
				break
			}
			if (p == 0) { out = out line; line = ""; break }
			out = out substr(line, 1, p - 1)
			line = substr(line, p + 2)
			inblock = 1
		}
		print out
	}' "$1"
}

# ---------------------------------------------------------------------
# A — the login mapping is a reference, not a value
# ---------------------------------------------------------------------
echo "check A — the login mapping is a '\$p\$<user>' reference"
rpcd=$(strip_comments "$RPCD")

if printf '%s\n' "$rpcd" | grep -q "password='\\\$p\\\$"; then
	report_pass "the password option is a \$p\$ reference"
else
	report_fail "the password option is not a \$p\$ reference"
fi

if printf '%s\n' "$rpcd" | grep -q "saadway_login"; then
	report_pass "the login section is saadway_login (contracts/uci.md)"
else
	report_fail "the login section is not saadway_login"
fi

if printf '%s\n' "$rpcd" | grep -q 'delete rpcd.saadway_admin'; then
	report_pass "the pre-T072 login section is removed"
else
	report_fail "the pre-T072 login section (saadway_admin) is not removed"
fi

if printf '%s\n' "$rpcd" | grep -q 'commit rpcd'; then
	report_pass "the rpcd package is committed (the mapping survives a reboot)"
else
	report_fail "no 'commit rpcd' — a batch alone only writes the /tmp delta"
fi

# ---------------------------------------------------------------------
# B — the password is piped, never an argument
# ---------------------------------------------------------------------
echo "check B — the password is piped to the setter"
prov=$(strip_comments "$PROVISION")
rpcd=$(strip_comments "$RPCD")

leak=$(printf '%s\n' "$prov" | grep -E '(chpasswd|passwd|cryptpw)' |
	grep -E "(DEFAULT_PASSWORD|'admin'|\"admin\")")
if [ -z "$leak" ]; then
	report_pass "no password literal next to a password setter"
else
	report_fail "a password literal appears next to a password setter:"
	printf '%s\n' "$leak" | sed 's/^/        /' >&2
fi

if printf '%s\n' "$prov" | grep -q 'chpasswd' &&
	printf '%s\n' "$prov" | grep -q '|'; then
	report_pass "the setter is fed over a pipe"
else
	report_fail "the setter is not fed over a pipe"
fi

# ---------------------------------------------------------------------
# C — the default is initialized only when the account is absent
# ---------------------------------------------------------------------
echo "check C — the first-boot path provisions no well-known default password"
# T079: the dashboard needs no credential (spec FR-020) — the first-boot
# path MUST NOT ship a default password any more.
if grep -q 'provision init' "$RPCD"; then
	report_fail "the first-boot script provisions the default credential — a well-known password with no consumer"
else
	report_pass "the first-boot script provisions no default credential"
fi

# the helper may keep the opt-in 'init' mode, but it must stay guarded by an
# existence check (it must never overwrite an administrator's change)
if printf '%s\n' "$prov" | grep -q '^	init)'; then
	init_block=$(printf '%s\n' "$prov" | sed -n '/^	init)/,/^	[a-z-]*)/p')
	if printf '%s\n' "$init_block" | grep -q 'user_exists' &&
		printf '%s\n' "$init_block" | grep -q 'set_password'; then
		report_pass "the helper's opt-in 'init' mode applies the default only behind an existence check"
	else
		report_fail "the helper's 'init' mode does not guard the default behind an existence check"
	fi
else
	report_pass "the helper has no 'init' mode at all"
fi

# the credential mapping is refreshed only when the account exists — an
# operator-set credential must survive an upgrade
if printf '%s\n' "$rpcd" | grep -q '/etc/passwd'; then
	report_pass "the credential mapping is refreshed only when the account exists"
else
	report_fail "the mapping refresh is unconditional — an operator-set credential would be clobbered"
fi

# the commit and the rpcd reload must stay: rpcd binds the default session's
# ACLs once at startup (openwrt/rpcd session.c:1396-1400)
if printf '%s\n' "$rpcd" | grep -q 'commit rpcd'; then
	report_pass "the rpcd package is committed"
else
	report_fail "no 'commit rpcd' — the mapping would be lost on the next reboot"
fi

if printf '%s\n' "$rpcd" | grep -q 'init.d/rpcd'; then
	report_pass "rpcd is reloaded (the default session's ACLs are granted at startup)"
else
	report_fail "rpcd is not reloaded — the dashboard would stay permission-denied until the next reboot"
fi

# ---------------------------------------------------------------------
# D — the account is created before the mapping is updated
# ---------------------------------------------------------------------
echo "check D — the account is created before the mapping is updated"
if printf '%s\n' "$prov" | grep -q 'set-credentials)'; then
	report_pass "the helper has a 'set-credentials' mode"
else
	report_fail "the helper has no 'set-credentials' mode"
fi

set_block=$(printf '%s\n' "$prov" | sed -n '/^	set-credentials)/,/^	[a-z-]*)/p')
add_line=$(printf '%s\n' "$set_block" | grep -n 'do_add' | head -1 | cut -d: -f1)
map_line=$(printf '%s\n' "$set_block" | grep -n 'update_login_mapping' | head -1 | cut -d: -f1)
if [ -n "$add_line" ] && [ -n "$map_line" ] && [ "$add_line" -lt "$map_line" ]; then
	report_pass "the account is created before the mapping is updated"
else
	report_fail "the mapping is updated before/without creating the account"
fi

# ---------------------------------------------------------------------
# E — the credential method is root-only
# ---------------------------------------------------------------------
echo "check E — the credential method is granted to a dedicated group"
if [ -f "$CRED_ACL" ]; then
	report_pass "the saadway-credentials ACL file exists"
	if grep -q '"mgmt.credentials.set"' "$CRED_ACL"; then
		report_pass "the group grants mgmt.credentials.set"
	else
		report_fail "the group does not grant mgmt.credentials.set"
	fi
	methods=$(grep -o '"mgmt\.[a-z._]*"' "$CRED_ACL" | sort -u | wc -l)
	if [ "$methods" -eq 1 ]; then
		report_pass "the group grants exactly one method (least privilege)"
	else
		report_fail "the group grants $methods methods (it must grant exactly one)"
	fi
else
	report_fail "the saadway-credentials ACL file is missing"
fi

if grep -q 'mgmt.credentials.set' "$RPC_ACL"; then
	report_fail "the dashboard's own 'saadway' ACL group grants mgmt.credentials.set — the dashboard could re-key its own login"
else
	report_pass "the dashboard's 'saadway' group does not grant mgmt.credentials.set"
fi

# ---------------------------------------------------------------------
# F — the daemon exposes the method once and stores no credential
# ---------------------------------------------------------------------
echo "check F — the daemon side"
if [ -f "$UBUS_SRC" ]; then
	n=$(grep -c '"mgmt.credentials.set"' "$UBUS_SRC")
	if [ "$n" -eq 1 ]; then
		report_pass "mgmt.credentials.set is registered exactly once"
	else
		report_fail "mgmt.credentials.set is registered $n times (expected 1)"
	fi
else
	report_fail "the daemon source is missing"
fi

if [ -f "$CREDS_SRC" ]; then
	creds_code=$(strip_c_comments "$CREDS_SRC")
	if printf '%s\n' "$creds_code" | grep -qE '/etc/shadow|cryptpw|crypt\('; then
		report_fail "the daemon touches the shadow file or computes a hash — the platform's passwd must do both"
	else
		report_pass "the daemon neither hashes nor writes /etc/shadow"
	fi
	if printf '%s\n' "$creds_code" | grep 'execl(' | grep -q 'password'; then
		report_fail "the password is passed to the helper as an argument (argv is world-readable)"
	else
		report_pass "the password reaches the helper over stdin"
	fi
else
	report_fail "the credential module (saadway/src/creds.c) is missing"
fi

# ---------------------------------------------------------------------
if [ "$failures" -ne 0 ]; then
	echo "credential-flow-audit: FAIL — $failures check(s) failed" >&2
	exit 1
fi
echo "credential-flow-audit: OK — the dashboard credential flow honours the contract"
exit 0
