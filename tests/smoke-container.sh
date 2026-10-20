#!/bin/sh
# smoke-container.sh — timeout-hardening gate (T037, Gate 3, FR-018)
#
# Run ON a real OpenWrt 25.12 device with the SaadWay service active.
#
# Scenario: block the health-check targets (ICMP to 1.1.1.1, HTTP-204
# GET to cp.cloudflare.com) for traffic LEAVING the router (the daemon's
# in-process probes, health.c, are bound to the tunnel interface) using an
# independent nft observer/blocker table — the daemon-owned `table inet
# saadway` is never modified. Then prove:
#   1. mon.health_test still RESOLVES within the ≤5 s master budget
#      (SC-003) and reports "timeout" states instead of hanging;
#   2. the daemon/API stay responsive throughout (mgmt.status answers
#      in real time while the health test is inside its window).
# Exits 0 on pass; 1 on any gate violation.

set -u

BLK_TABLE=saadway_smoketest

fail() { echo "FAIL: $1" >&2; exit 1; }
cleanup() { nft delete table inet "$BLK_TABLE" 2>/dev/null; }

command -v nft    >/dev/null || fail "nft not present"
command -v ubus   >/dev/null || fail "ubus not present"
command -v jsonfilter >/dev/null || fail "jsonfilter not present"
command -v timeout >/dev/null || fail "coreutils timeout not present"

# resolve the dashboard's current tunnel interface for the blocker rules
TUNIF=$(ubus call saadway mgmt.status | jsonfilter -e '@.current_if' 2>/dev/null)
[ -n "$TUNIF" ] && [ "$TUNIF" != "-" ] || \
	fail "no active/tunneled interface (activate a profile first)"

# upstream IPs: fixed ICMP target + the HTTP-204 host resolved at run start
CFIP=$(nslookup cp.cloudflare.com 2>/dev/null | awk '/^Address/ {print $NF; exit}')
[ -n "${CFIP:-}" ] || fail "cannot resolve cp.cloudflare.com for the blocker"

trap 'cleanup' EXIT

# ---- install blocker (own atomic transaction, independent namespace) -------
nft -f - <<EOF || fail "cannot install blocker table inet $BLK_TABLE"
table inet $BLK_TABLE {
	chain blk {
		type filter hook output priority filter + 1; policy accept;
		oifname "$TUNIF" ip daddr 1.1.1.1 drop
		oifname "$TUNIF" ip daddr $CFIP drop
	}
}
EOF
echo "blocker installed: ICMP(1.1.1.1) and HTTP($CFIP) dropped on $TUNIF"

# ---- 1. health test must self-resolve with timeouts ----------------------
START=$(date +%s)
HC=$(timeout "${SW_HTEST_BUDGET:-8}" ubus call saadway mon.health_test '{}')
RC=$?
ELAPSED=$(( $(date +%s) - START ))

case $RC in
	0) ;;
	124) fail "mon.health_test hung beyond the outer budget (${ELAPSED}s) — async deadline violated" ;;
	*) fail "mon.health_test ubus call failed (rc=$RC)" ;;
esac

if [ $ELAPSED -gt 5 ]; then
	fail "mon.health_test took ${ELAPSED}s — exceeds the SC-003 5 s budget"
fi
echo "health_test resolved in ${ELAPSED}s (budget ok)"

echo "$HC" | grep -q 'timeout' || fail "health payload lacks explicit timeout states"
echo "== health payload (dead targets) =="
echo "$HC"

# ---- 2. daemon stayed responsive during the health window ----------------
START=$(date +%s)
MS=$(timeout 3 ubus call saadway mgmt.status 2>&1)
MSRC=$?
ELAPSED_MS=$(( $(date +%s) - START ))

[ "$MSRC" = "0" ] || fail "mgmt.status failed while health test was pending (rc=$MSRC)"
[ "$ELAPSED_MS" -le 1 ] || fail "mgmt.status took ${ELAPSED_MS}s while a health test ran"

ST=$(echo "$MS" | jsonfilter -e '@.connection_status' 2>/dev/null)
case "$ST" in
	up|connecting|contained|cooldown|down)
		echo "mgmt.status ok (${ELAPSED_MS}s) — connection_status=$ST" ;;
	*) fail "unexpected connection_status: '$ST'" ;;
esac

# ---- second round: nested concurrency — a streaming poll during htest ----
( timeout 3 ubus call saadway mon.stats '{}' >/dev/null ) &
STATS_PID=$!
timeout 8 ubus call saadway mon.health_test '{}' >/dev/null || \
	fail "second health_test failed under concurrent monitor load"
kill "$STATS_PID" 2>/dev/null && wait "$STATS_PID" 2>/dev/null
echo "concurrent mon.stats + mon.health_test both answered"

echo "PASS: probes return timeout within budget; daemon stayed responsive"
exit 0
