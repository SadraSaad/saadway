#!/bin/sh
# link-audit.sh — host-side link-surface check (T063, Gate 1/2 evidence)
#
# Compiles the 13 daemon modules against the dev stub headers and fails
# when a DAEMON-INTERNAL symbol is referenced but not defined by the
# module set. This is the check that catches missing definitions (e.g.
# the historical `undefined reference to 'global_ctx'`) long before an
# OpenWrt SDK build reaches its link step.
#
# Usage: tests/link-audit.sh [stub-dir]
#   stub-dir  dev-only stub header pool (default: $SWSTUBS or
#             /tmp/opencode/swstubs) — never shipped
# Env:
#   SW_SRC    override the daemon source directory (used by the
#             negative self-test to prove the audit actually fails)
#
# Exit: 0 = clean, 1 = unresolved daemon-internal symbol(s), 2 = setup error

set -u

REPO=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
SRC=${SW_SRC:-$REPO/saadway/src}
SWSTUBS=${1:-${SWSTUBS:-/tmp/opencode/swstubs}}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

MODULES="main uci-state log ubus policy tunnel-openvpn tunnel-ikev2 failover steering dns health counters backup creds"

die() {
	echo "link-audit: $1" >&2
	exit 2
}

command -v nm >/dev/null 2>&1 || die "nm not present"
command -v cc >/dev/null 2>&1 || die "cc not present"
[ -d "$SWSTUBS" ] || die "stub dir '$SWSTUBS' not found"
[ -d "$SRC" ] || die "source dir '$SRC' not found"

for m in $MODULES; do
	[ -f "$SRC/$m.c" ] || die "missing module source $SRC/$m.c"
	cc -O1 -std=c11 -I"$SWSTUBS" -I"$SRC" -c -o "$TMP/$m.o" "$SRC/$m.c" \
		2>"$TMP/$m.err" || {
		echo "link-audit: COMPILE FAILED: $m" >&2
		cat "$TMP/$m.err" >&2
		exit 2
	}
done

# symbols DEFINED by the daemon module set
nm --defined-only "$TMP"/*.o 2>/dev/null | awk '{print $NF}' | sort -u \
	> "$TMP/defined.txt"
# symbols REFERENCED by the module set
nm -u "$TMP"/*.o 2>/dev/null | awk '{print $NF}' | sort -u > "$TMP/undef.txt"
# referenced but not defined anywhere in the module set
comm -23 "$TMP/undef.txt" "$TMP/defined.txt" > "$TMP/missing.txt"

# The target libraries (libubox/libubus/libuci/json-c) provide the
# blob*/blobmsg*/ubus*/uci*/json_object*/uloop* families; the C library
# provides the rest. Anything else is daemon-internal and MUST be
# defined by the module set.
grep -E '^(blob|blobmsg|ubus|uci|json_|uloop)' "$TMP/missing.txt" \
	| sort -u > "$TMP/external.txt"

LIBC=$(cc -print-file-name=libc.so.6 2>/dev/null || true)
if [ -n "$LIBC" ] && [ -f "$LIBC" ]; then
	nm -D "$LIBC" 2>/dev/null | awk '{print $NF}' | sed 's/@.*//' | sort -u \
		> "$TMP/libc.txt"
	comm -23 "$TMP/missing.txt" "$TMP/external.txt" > "$TMP/internal.txt"
	comm -23 "$TMP/internal.txt" "$TMP/libc.txt" > "$TMP/unresolved.txt"
else
	# no libc symbol list available: fall back to the daemon's own
	# naming conventions (sw_*, h_*, global_ctx, …)
	grep -E '^(sw_|h_|global_ctx$|act_|arg_int)' "$TMP/missing.txt" \
		| sort -u > "$TMP/unresolved.txt"
fi

if [ -s "$TMP/unresolved.txt" ]; then
	echo "link-audit: FAIL — unresolved daemon-internal symbol(s):" >&2
	sed 's/^/  /' "$TMP/unresolved.txt" >&2
	exit 1
fi

echo "link-audit: OK — no unresolved daemon-internal symbol (${MODULES} modules checked)"
exit 0
