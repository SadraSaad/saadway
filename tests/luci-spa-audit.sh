#!/bin/sh
# luci-spa-audit.sh — host-side audit of the LuCI rendering fix and the
# standalone-SPA auth bypass (T080, Phase 24).
#
# Two defect clusters are guarded here, both of them regression-prone:
#
#   A  the LuCI view is loadable and bound to the right UCI coordinates:
#      it declares the built-in 'view' module, and its form.Map carries the
#      config the service state actually lives in ('saadway') plus the
#      section that holds it ('service') — a form.Map binds to exactly ONE
#      uci config (openwrt/luci form.js → CBIMap), so a wrong binding
#      renders an empty section ("This section contains no values yet")
#   B  the app's ACL group grants the uci scope in the upstream shape: the
#      scope's "function" is literally 'read'/'write', never a section name
#      (openwrt/rpcd uci.c:324-349), and the ubus scope covers the saadway
#      object
#   C  the standalone SPA is tokenless by design and carries no credential
#      material: no login markup, no session.login, no session token, and
#      every /ubus request carries rpcd's default (unauthenticated) session
#      id (openwrt/rpcd include/rpcd/session.h:35 → RPC_DEFAULT_SESSION_ID)
#   D  the dashboard's ubus authorization is the access group named exactly
#      'unauthenticated' (rpcd session.c:944-946 grants ONLY that group to
#      the default session) and its blast radius stays inside the 'saadway'
#      ubus object — no other ubus object and no uci scope
#   E  the first-boot path provisions NO well-known default password and
#      still commits the mapping and reloads rpcd (rpcd binds the default
#      session's ACLs once at startup, session.c:1396-1400)
#
# The audit is static and offline: it never executes the provisioning helper
# and never needs a device.
#
# Usage: tests/luci-spa-audit.sh [tree-root]
#   tree-root  directory holding saadway/ and luci-app-saadway/
#              (default: the repository root) — the negative test points it
#              at a scratch tree with the pre-T079 files
#
# Exit: 0 = clean, 1 = violation(s), 2 = setup error.

set -u

REPO=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
TREE=${1:-$REPO}

VIEW="$TREE/luci-app-saadway/htdocs/luci-static/resources/view/saadway/dashboard.js"
LUCI_ACL="$TREE/luci-app-saadway/root/usr/share/rpcd/acl.d/luci-app-saadway.json"
RPC_ACL="$TREE/saadway/files/rpc-acl.json"
API="$TREE/saadway/files/htdocs/saadway/js/api.js"
APP="$TREE/saadway/files/htdocs/saadway/js/app.js"
INDEX="$TREE/saadway/files/htdocs/saadway/index.html"
RPCD="$TREE/saadway/files/uci-defaults/50-saadway-rpcd"

ZERO_SESSION="00000000000000000000000000000000"

die() {
	echo "luci-spa-audit: $1" >&2
	exit 2
}

for f in "$VIEW" "$LUCI_ACL" "$RPC_ACL" "$API" "$APP" "$INDEX" "$RPCD"; do
	[ -f "$f" ] || die "missing '$f'"
done

failures=0
report_pass() { printf '  ok    %s\n' "$1"; }
report_fail() {
	printf '  FAIL  %s\n' "$1" >&2
	failures=$((failures + 1))
}

# strip shell comments
strip_sh() { sed 's/#.*$//' "$1"; }

# strip /* ... */ blocks (a C/JS block comment) — a path or an API name
# mentioned in a comment is documentation, not code. Line comments are left
# alone: a '//' inside a string literal ("http://…") would truncate the line.
strip_block_comments() {
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
			if (p == 0) { out = out line; line = ""; break }
			out = out substr(line, 1, p - 1)
			line = substr(line, p + 2)
			inblock = 1
		}
		print out
	}' "$1"
}

# ---------------------------------------------------------------------
# A — the LuCI view loads and is bound to the service's own coordinates
# ---------------------------------------------------------------------
echo "check A — the LuCI view is loadable and correctly bound ($VIEW)"
view_code=$(strip_block_comments "$VIEW")

# the requires are the leading "'require …" statements; the built-in 'view'
# module must come first (openwrt/luci luci.js:2243 → view: View)
first_require=$(printf '%s\n' "$view_code" | grep -o "^'require [a-z]*';" | head -1)
if [ "$first_require" = "'require view';" ]; then
	report_pass "the view requires the built-in 'view' module first"
else
	report_fail "the first require is '${first_require:-none}' — 'require view' must come first (view.extend would be undefined)"
fi

if printf '%s\n' "$view_code" | grep -q "new form.Map('saadway'"; then
	report_pass "the form.Map is bound to the 'saadway' config"
else
	report_fail "the form.Map is not bound to the 'saadway' config — a form.Map binds to exactly ONE uci config"
fi

if printf '%s\n' "$view_code" | grep -q "'service'"; then
	report_pass "the form section is the service's 'service' section"
else
	report_fail "the form does not bind the 'service' section"
fi

if printf '%s\n' "$view_code" | grep -q "'/saadway/'"; then
	report_pass "the Open Dashboard link points at /saadway/"
else
	report_fail "the Open Dashboard link is missing"
fi

# ---------------------------------------------------------------------
# B — the app's ACL group: the uci scope in the upstream shape
# ---------------------------------------------------------------------
echo "check B — the LuCI ACL group grants the uci scope correctly ($LUCI_ACL)"
luci_acl=$(cat "$LUCI_ACL")

if printf '%s\n' "$luci_acl" | grep -q '"uci"'; then
	report_pass "the group carries a uci scope"
else
	report_fail "the group carries no uci scope — uci.get would be denied (code 6)"
fi

# the uci scope's entries are config names (the "function" is the read/write
# permission bucket, never a section name — rpcd uci.c:324-349)
uci_scope=$(printf '%s\n' "$luci_acl" | sed -n '/"uci"/,/}/p')
if printf '%s\n' "$uci_scope" | grep -q '"saadway"'; then
	report_pass "the uci scope covers the 'saadway' config"
else
	report_fail "the uci scope does not cover the 'saadway' config"
fi

if printf '%s\n' "$uci_scope" | grep -q '"service"'; then
	report_fail "the uci scope names a SECTION ('service') — the upstream shape is the CONFIG name with the read/write permission"
else
	report_pass "the uci scope names the config, not a section (upstream shape)"
fi

if printf '%s\n' "$luci_acl" | grep -q '"saadway"[[:space:]]*:[[:space:]]*\[[[:space:]]*"\*"'; then
	report_pass "the ubus scope covers the saadway object"
else
	report_fail "the ubus scope does not cover the saadway object"
fi

# ---------------------------------------------------------------------
# C — the SPA is tokenless and carries no credential material
# ---------------------------------------------------------------------
echo "check C — the SPA is tokenless and publishes no credential ($API, $APP, $INDEX)"
api_code=$(strip_block_comments "$API")
app_code=$(strip_block_comments "$APP")
index_html=$(sed 's/<!--[^>]*-->//g' "$INDEX")

for marker in 'id="login-form"' 'id="login-user"' 'id="login-pass"' 'id="btn-logout"'; do
	if printf '%s\n' "$index_html" | grep -q "$marker"; then
		report_fail "the SPA still ships '$marker' — the login gate is gone (spec FR-020)"
	else
		report_pass "the SPA ships no '$marker'"
	fi
done

if printf '%s\n' "$app_code" | grep -q 'SwApi\.login\|SwApi\.logout\|is_logged_in\|session_lost'; then
	report_fail "the SPA still calls the session lifecycle"
else
	report_pass "the SPA calls no session lifecycle"
fi

if printf '%s\n' "$api_code" | grep -q 'session.*login\|ubus_rpc_session'; then
	report_fail "the transport still performs a session login / keeps a session token"
else
	report_pass "the transport performs no session login and keeps no token"
fi

if printf '%s\n' "$api_code" | grep -q "$ZERO_SESSION"; then
	report_pass "the transport sends rpcd's default (unauthenticated) session id"
else
	report_fail "the transport does not send the all-zero default session id"
fi

# every /ubus request must carry that id as params[0]
if printf '%s\n' "$api_code" | grep -q 'params: \[DEFAULT_SESSION_ID'; then
	report_pass "params[0] is the default session id"
else
	report_fail "params[0] is not the default session id"
fi

# ---------------------------------------------------------------------
# D — the dashboard's authorization group
# ---------------------------------------------------------------------
echo "check D — the dashboard's access group ($RPC_ACL)"
rpc_acl=$(cat "$RPC_ACL")

if printf '%s\n' "$rpc_acl" | grep -q '"unauthenticated"'; then
	report_pass "the access group 'unauthenticated' exists"
else
	report_fail "the access group 'unauthenticated' is missing — a tokenless call would be denied (rpcd session.c:944-946)"
fi

# its blast radius: the 'saadway' ubus object only — no other ubus object.
# The object names are the keys of the "ubus" table; the method names live
# in its arrays (they are not "key": pairs).
unauth_block=$(printf '%s\n' "$rpc_acl" | sed -n '/"unauthenticated"/,$p')
unauth_ubus=$(printf '%s\n' "$unauth_block" | sed -n '/"ubus"/,$p')
objects=$(printf '%s\n' "$unauth_ubus" | grep -o '"[a-z_]*"[[:space:]]*:[[:space:]]*\[' |
	sed 's/[[:space:]]*:[[:space:]]*\[$//' | tr -d '"' | sort -u)
if [ "$objects" = "saadway" ]; then
	report_pass "the group reaches exactly one ubus object: saadway"
else
	report_fail "the group's ubus objects are '${objects:-none}' — it must reach the saadway object only"
fi

if printf '%s\n' "$unauth_block" | grep -q '"uci"'; then
	report_fail "the group carries a uci scope — the dashboard must not reach the configuration store"
else
	report_pass "the group carries no uci scope"
fi

# ---------------------------------------------------------------------
# E — the first-boot path: no default password, but the reload stays
# ---------------------------------------------------------------------
echo "check E — the first-boot path provisions no default password ($RPCD)"
rpcd=$(strip_sh "$RPCD")

if printf '%s\n' "$rpcd" | grep -q 'provision init'; then
	report_fail "the first-boot script provisions the default credential — a well-known password with no consumer"
else
	report_pass "the first-boot script provisions no default credential"
fi

if printf '%s\n' "$rpcd" | grep -q '/etc/passwd'; then
	report_pass "the credential mapping is refreshed only when the account exists"
else
	report_fail "the mapping refresh is unconditional — an operator-set credential would be clobbered"
fi

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
if [ "$failures" -ne 0 ]; then
	echo "luci-spa-audit: FAIL — $failures check(s) failed" >&2
	exit 1
fi
echo "luci-spa-audit: OK — the LuCI view renders and the SPA stays tokenless"
exit 0
