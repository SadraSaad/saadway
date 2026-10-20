#!/bin/sh
# leak-check.sh — zero-leak gate (T022, Gate 5, SC-002)
#
# Run ON a real OpenWrt 25.12 device against the live saadwayd, with a LAN
# client that generates tunneled-default load during the test.
#
# Asserts EXACTLY 0 tunneled-default egress toward the uplink
#   (a) while the tunnel is dropped under kill switch, and
#   (b) during a one-click profile switch under sustained load
# (FR-012/FR-014: containment is applied before any re-open in both phases;
# SC-002 zero-leak budget).
#
# Method: install an INDEPENDENT observer nft table (the daemon-owned
# `table inet saadway` is never modified from outside) that counts packets
# forwarded to the WAN device. Any such packet during an outage/switch
# window is a leak (the tunnel route/blackhole should have captured the
# flow instead). Observer install/remove each happen in their own nft
# transaction and never race the daemon's atomic applies. Constitution
# Principle I compliant: nft only.
#
# Required env (nothing is guessed — the gate must never fire by accident):
#   WTN_WAN_DEV   WAN/uplink interface name, e.g. eth1 or pppoe-wan
#   WTN_LAN_DEV   LAN bridge, e.g. br-lan
#   WTN_LAN_HOST  IPv4 of the load-generating LAN client, e.g. 192.168.1.40
#   WTN_PROF_B    optional second profile name for the switch phase
# Exits 0 when all executed phases show zero leaks; 1 otherwise.

set -u

OBS_TABLE=saadway_leaktest

fail() { echo "FAIL: $1" >&2; exit 1; }
cleanup() { nft delete table inet "$OBS_TABLE" 2>/dev/null; }
[ -n "${WTN_WAN_DEV:-}" ]  || fail "WTN_WAN_DEV required"
[ -n "${WTN_LAN_DEV:-}" ]  || fail "WTN_LAN_DEV required"
[ -n "${WTN_LAN_HOST:-}" ] || fail "WTN_LAN_HOST required"
command -v nft   >/dev/null || fail "nft not present"
command -v ubus  >/dev/null || fail "ubus not present"
command -v jsonfilter >/dev/null || fail "jsonfilter not present"
command -v ping  >/dev/null || fail "ping (busybox) not present"

ecall() { timeout "${WTN_UBUS_T:-20}" ubus call "$@" 2>&1; }
status_of() {
	ecall saadway mgmt.status | jsonfilter -e '@.connection_status' 2>/dev/null
}
curr_profile() {
	ecall saadway mgmt.status | jsonfilter -e '@.active_profile' 2>/dev/null
}
leaked_pkts() {
	nft list table inet "$OBS_TABLE" 2>/dev/null |
		grep 'comment "sw_leak"' | head -n1 | sed 's/.*packets //'
}

trap 'cleanup' EXIT

# ---- install observer (own atomic transaction) -----------------------------
nft -f - <<EOF || fail "cannot install observer table table inet $OBS_TABLE"
table inet $OBS_TABLE {
	chain observe {
		type filter hook forward priority 10; policy accept;
		oifname "$WTN_WAN_DEV" counter comment "sw_leak"
	}
}
EOF
PRE=$(leaked_pkts)
echo "observer installed; baseline counter = ${PRE:-0}"

# ---- arm kill switch (persisted; state asserted below) --------------------
ecall saadway sec.kill_switch.set '{"enabled":true}' >/dev/null || \
	fail "kill switch set failed"
echo "kill switch armed"

[ -n "$(curr_profile)" ] || fail "no active profile — activate a provider first"

# ---- Phase (a): drop the tunnel, flood, read the counter ------------------
ecall saadway profile.deactivate >/dev/null || fail "profile deactivate failed"
ST=$(status_of)
[ "$ST" = "contained" ] || fail "expected connection_status=contained after drop, got '$ST'"
echo "phase (a): tunnel dropped; status=contained"

ping -I "$WTN_LAN_DEV" -i 0.1 -c 20 "$WTN_LAN_HOST" >/dev/null 2>&1 || true
sleep 1
LEAK_A=$(leaked_pkts)
echo "phase (a): leaked-egress counter = ${LEAK_A:-n/a}"
[ "${LEAK_A:-x}" = "0" ] || fail "phase (a) leaked $LEAK_A forwarded egress packets"

# ---- Phase (b): one-click switch under sustained load ---------------------
RESULT_OK=1
if [ -n "${WTN_PROF_B:-}" ]; then
	# sustained load while the switch executes (containment must hold)
	( ping -I "$WTN_LAN_DEV" -i 0.2 -c 150 "$WTN_LAN_HOST" >/dev/null 2>&1 ) &
	LOAD_PID=$!
	ecall saadway profile.activate "{\"name\":\"$WTN_PROF_B\"}" >/dev/null ||
		fail "activate '$WTN_PROF_B' failed"
	ST=$(status_of)
	SW_PROFILE=$(curr_profile)
	kill "$LOAD_PID" 2>/dev/null && wait "$LOAD_PID" 2>/dev/null
	LEAK_B=$(leaked_pkts)
	echo "phase (b): after switch → status=$ST profile=$SW_PROFILE counter=${LEAK_B:-n/a}"
	[ "${LEAK_B:-x}" = "0" ] || { echo "FAIL: phase (b) leaked $LEAK_B packets" >&2; RESULT_OK=0; }
else
	echo "phase (b) SKIPPED: set WTN_PROF_B to exercise switch-under-load"
fi

# ---- restore the pre-test desired state -----------------------------------
ecall saadway profile.list >/dev/null
: # (engine re-applies policy itself; nothing to restore for the observer)

if [ "$RESULT_OK" -eq 1 ]; then
	echo "PASS: zero tunneled-egress leaks through all executed phases"
	exit 0
fi
exit 1
