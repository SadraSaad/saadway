#!/bin/sh
# uci-defaults-audit.sh — host-side regression audit for the first-boot
# uci-defaults script (T067, Phase 20).
#
# The script under audit writes the uhttpd listener addresses. uhttpd
# resolves the listener host with getaddrinfo(), so a value that is not a
# bare host:port ("192.168.5.1/24:80") makes uhttpd abort the bind with
# "getaddrinfo(): Name does not resolve" — the dashboard and the /ubus
# transport behind it become unreachable.
#
# The audit runs the script under a stub PATH (recording uci/ubus/jsonfilter)
# and asserts:
#   A  the ubus path yields a BARE address (netifd reports objects, so the
#      leaf must be selected — the JSON text of the object is not an address)
#   B  a CIDR address from the UCI fallback is stripped ("192.168.5.1/24" ->
#      "192.168.5.1:80") — the observed production failure
#   C  an unusable address writes NOTHING (the stock configuration stays)
#   D  the wildcards are deleted BEFORE the LAN address is added (a bare
#      add_list would leave uhttpd listening on every interface)
#   E  the change is committed (uci batch alone only writes a /tmp delta)
#   F  the script always exits 0 (it is sourced by /etc/init.d/boot in a
#      subshell — a non-zero exit would abort the boot sequence)
#
# Usage: tests/uci-defaults-audit.sh [script]
#   default: saadway/files/uci-defaults/40-saadway-uhttpd
#
# Exit: 0 = every case passes, 1 = at least one case failed, 2 = usage error.
#
# The negative fixture tests/fixtures/40-saadway-uhttpd.prefix is a verbatim
# snapshot of the PRE-FIX script; the audit must FAIL against it.

set -u

REPO=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
TARGET=${1:-"$REPO/saadway/files/uci-defaults/40-saadway-uhttpd"}

[ -f "$TARGET" ] || {
	echo "uci-defaults-audit: no such script: $TARGET" >&2
	exit 2
}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

LOG="$TMP/uci.log"
failures=0

report_pass() { printf '  ok    %s\n' "$1"; }
report_fail() {
	printf '  FAIL  %s\n' "$1" >&2
	failures=$((failures + 1))
}

# ---------------------------------------------------------------------
# stubs
# ---------------------------------------------------------------------
mkdir -p "$TMP/bin" "$TMP/ubin"

cat > "$TMP/bin/uci" <<'STUB'
#!/bin/sh
# stub uci: records every invocation and answers the queries the script makes
log=$SAADWAY_AUDIT_LOG
{
	printf 'CMD:'
	for a in "$@"; do printf ' %s' "$a"; done
	printf '\n'
} >>"$log"
case " $* " in
*" batch "*)
	while IFS= read -r line; do printf 'BATCH: %s\n' "$line" >>"$log"; done
	exit 0
	;;
*" commit "*)
	exit 0
	;;
*" get "*)
	key=""
	for a in "$@"; do key=$a; done
	case "$key" in
	network.lan.ipaddr)
		[ -n "${SAADWAY_AUDIT_IPADDR-}" ] || exit 1
		printf '%s\n' "$SAADWAY_AUDIT_IPADDR"
		exit 0
		;;
	uhttpd.main)
		exit 0
		;;
	esac
	exit 1
	;;
esac
exit 0
STUB

cat > "$TMP/bin/jsonfilter" <<'STUB'
#!/bin/sh
# stub jsonfilter: mirrors the upstream jsonpath semantics it stands in for —
# a matched STRING is printed raw, a matched OBJECT is printed as its JSON
# text (upstream jsonpath main.c).
expr=""
while [ $# -gt 0 ]; do
	case "$1" in
	-e)
		expr=$2
		shift 2
		;;
	*) shift ;;
	esac
done
data=$(cat)
case "$expr" in
*'[0].address'*)
	printf '%s\n' "$(printf '%s' "$data" |
		sed -n 's/.*"address"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')"
	exit 0
	;;
*'[0]'*)
	printf '%s\n' "$(printf '%s' "$data" |
		sed -n 's/.*\({"address"[^}]*}\).*/\1/p')"
	exit 0
	;;
esac
exit 1
STUB

cat > "$TMP/ubin/ubus" <<'STUB'
#!/bin/sh
# stub ubus: the netifd status shape — each address is an OBJECT with an
# "address" and a separate "mask" (upstream netifd ubus.c).
printf '%s\n' '{"interface":"lan","up":true,"device":"br-lan","ipv4-address":[{"address":"192.168.5.1","mask":24}]}'
exit 0
STUB

chmod +x "$TMP/bin/uci" "$TMP/bin/jsonfilter" "$TMP/ubin/ubus"

# run_script <with-ubus yes|no> <canned network.lan.ipaddr>
# leaves the recorded invocations in $LOG and the exit code in $RC
run_script() {
	: >"$LOG"
	if [ "$1" = yes ]; then
		PATH="$TMP/ubin:$TMP/bin:/usr/bin:/bin"
	else
		PATH="$TMP/bin:/usr/bin:/bin"
	fi
	SAADWAY_AUDIT_LOG="$LOG" SAADWAY_AUDIT_IPADDR="$2" \
		/bin/sh "$TARGET" >/dev/null 2>&1
	RC=$?
}

batch_has() { grep -q "^BATCH: $1\$" "$LOG"; }
cmd_has() { grep -q "^CMD: $1\$" "$LOG"; }

# ---------------------------------------------------------------------
# case A — the ubus path must yield a bare address
# ---------------------------------------------------------------------
echo "case A — ubus path yields a bare address"
run_script yes ""
if batch_has "add_list uhttpd.main.listen_http='192.168.5.1:80'"; then
	report_pass "listen_http is 192.168.5.1:80"
else
	report_fail "listen_http is not 192.168.5.1:80 (netifd reports objects — the leaf must be selected)"
fi
if grep -q '"address"' "$LOG"; then
	report_fail "the JSON text of the address object leaked into the configuration"
else
	report_pass "no JSON object text in the configuration"
fi

# ---------------------------------------------------------------------
# case B — a CIDR address from the UCI fallback must be stripped
# ---------------------------------------------------------------------
echo "case B — CIDR address is stripped"
run_script no "192.168.5.1/24"
if batch_has "add_list uhttpd.main.listen_http='192.168.5.1:80'"; then
	report_pass "the CIDR mask is stripped"
else
	report_fail "the CIDR mask was not stripped"
fi
if grep -q '/24' "$LOG"; then
	report_fail "a CIDR mask reached the configuration"
else
	report_pass "no CIDR mask in the configuration"
fi

# ---------------------------------------------------------------------
# case C — an unusable address must not write anything
# ---------------------------------------------------------------------
echo "case C — an unusable address writes nothing"
run_script no "not-an-ip"
if grep -q '^BATCH:' "$LOG"; then
	report_fail "the script wrote a configuration from an unusable address"
else
	report_pass "the stock configuration is left untouched"
fi

# ---------------------------------------------------------------------
# case D — the wildcards are deleted before the LAN address is added
# ---------------------------------------------------------------------
echo "case D — wildcards are deleted before the LAN listener is added"
run_script no "192.168.5.1/24"
del_line=$(grep -n '^BATCH: delete uhttpd.main.listen_http$' "$LOG" | head -1 | cut -d: -f1)
add_line=$(grep -n "^BATCH: add_list uhttpd.main.listen_http=" "$LOG" | head -1 | cut -d: -f1)
if [ -n "$del_line" ] && [ -n "$add_line" ] && [ "$del_line" -lt "$add_line" ]; then
	report_pass "listen_http is deleted before it is re-added"
else
	report_fail "listen_http is not deleted before it is re-added (a bare add_list leaves 0.0.0.0:80 and [::]:80 in place)"
fi

# ---------------------------------------------------------------------
# case E — the change is committed
# ---------------------------------------------------------------------
echo "case E — the change is committed"
run_script no "192.168.5.1/24"
if cmd_has "-q commit uhttpd"; then
	report_pass "uci commit uhttpd is issued"
else
	report_fail "no uci commit uhttpd (a batch alone only writes a /tmp delta)"
fi

# ---------------------------------------------------------------------
# case F — the script always exits 0
# ---------------------------------------------------------------------
echo "case F — the script exits 0"
for ipaddr in "192.168.5.1/24" "not-an-ip" ""; do
	run_script no "$ipaddr"
	if [ "$RC" -ne 0 ]; then
		report_fail "exit code $RC for ipaddr='$ipaddr' (must be 0)"
	else
		report_pass "exit 0 for ipaddr='$ipaddr'"
	fi
done
run_script yes ""
if [ "$RC" -ne 0 ]; then
	report_fail "exit code $RC on the ubus path (must be 0)"
else
	report_pass "exit 0 on the ubus path"
fi

# ---------------------------------------------------------------------
if [ "$failures" -ne 0 ]; then
	echo "uci-defaults-audit: FAIL — $failures check(s) failed for $TARGET" >&2
	exit 1
fi
echo "uci-defaults-audit: OK — $TARGET satisfies the uhttpd listener contract"
exit 0
