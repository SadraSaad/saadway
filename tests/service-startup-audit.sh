#!/bin/sh
# service-startup-audit.sh — host-side service-startup audit (T070, Phase 21).
#
# Two defects kept the SaadWay service from starting on a stock OpenWrt
# 25.12 device:
#
#   1. the daemon connected with ubus_connect("/var/run/ubus.sock") — the
#      pre-21.02 socket location; the stock socket lives in
#      /var/run/ubus/ubus.sock, so the connect failed with ENOENT and the
#      daemon exited before registering its ubus object. libubus resolves
#      the socket itself when the caller passes NULL.
#   2. the init script called procd_add_restart_trigger, which upstream does
#      not export at all — rc.common's shell reported
#      "procd_add_restart_trigger: not found" on every start.
#
# The audit is offline and checks:
#   A  every procd_* function called by the init script is in the upstream
#      exported helper set (tests/fixtures/procd-exported-helpers.txt)
#   B  the daemon sources carry no hardcoded ubus socket path and no
#      SW_UBUS_SOCK reference — the socket must be resolved by libubus
#
# Comments are stripped before both checks: a helper or a path mentioned in
# a comment is documentation, not a call.
#
# Usage: tests/service-startup-audit.sh [init-script] [daemon-src-dir]
#
# Exit: 0 = clean, 1 = violation(s), 2 = setup error.

set -u

REPO=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
INIT=${1:-"$REPO/saadway/files/saadway.init"}
SRC=${2:-"$REPO/saadway/src"}
FIXTURE="$REPO/tests/fixtures/procd-exported-helpers.txt"

die() {
	echo "service-startup-audit: $1" >&2
	exit 2
}

[ -f "$INIT" ] || die "no init script at '$INIT'"
[ -d "$SRC" ] || die "no daemon source dir at '$SRC'"
[ -f "$FIXTURE" ] || die "missing upstream helper fixture '$FIXTURE'"

failures=0
report_pass() { printf '  ok    %s\n' "$1"; }
report_fail() {
	printf '  FAIL  %s\n' "$1" >&2
	failures=$((failures + 1))
}

# ---------------------------------------------------------------------
# strip C comments (block and line) — POSIX awk, no toolchain needed
# ---------------------------------------------------------------------
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
# A — the init script may only call upstream-exported procd helpers
# ---------------------------------------------------------------------
echo "check A — procd helper API surface ($INIT)"

called=$(sed 's/#.*$//' "$INIT" | grep -oE '\bprocd_[a-z_]+' | sort -u)
if [ -z "$called" ]; then
	report_fail "no procd_* call found in the init script (parse error?)"
else
	for fn in $called; do
		if grep -qx "$fn" "$FIXTURE"; then
			report_pass "$fn is exported upstream"
		else
			report_fail "$fn is NOT exported upstream (upstream exports $(grep -vc '^#' "$FIXTURE") helpers — see tests/fixtures/procd-exported-helpers.txt)"
		fi
	done
fi

# ---------------------------------------------------------------------
# B — no hardcoded ubus socket path in the daemon sources
# ---------------------------------------------------------------------
echo "check B — no hardcoded ubus socket path ($SRC)"

sockets=$(grep -l '' "$SRC"/*.c "$SRC"/*.h 2>/dev/null | tr '\n' ' ')
[ -n "$sockets" ] || die "no daemon sources found in '$SRC'"

for f in $sockets; do
	code=$(strip_c_comments "$f")
	if printf '%s\n' "$code" | grep -qE '/var/run/ubus(\.sock|/ubus\.sock)|SW_UBUS_SOCK'; then
		report_fail "$f hardcodes a ubus socket path (use ubus_connect(NULL) — libubus resolves the socket)"
	else
		report_pass "$(basename "$f") has no hardcoded ubus socket path"
	fi
done

# ---------------------------------------------------------------------
if [ "$failures" -ne 0 ]; then
	echo "service-startup-audit: FAIL — $failures check(s) failed" >&2
	exit 1
fi
echo "service-startup-audit: OK — service startup surface is upstream-clean"
exit 0
