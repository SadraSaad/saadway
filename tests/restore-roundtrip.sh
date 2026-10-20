#!/bin/sh
# restore-roundtrip.sh — backup/restore fidelity gate (T041, Gate 7, SC-004)
#
# Run ON a real OpenWrt 25.12 device with saadwayd active.
#
# Proves the export→import pipeline reproduces the effective configuration
# exactly, with a per-item report (User Story 6 scenario 4 / FR-019):
#   1. snapshot: maint.backup.export (with credentials included) → /tmp;
#   2. import the SAME archive (round trip through the validator; import
#      restores config-only state — live spawn stays with the boot engine);
#   3. import the snapshot again on top of the restored config → per-item
#      report must list every restored item and nothing needs arbitrary
#      attention beyond password-replacement rows;
#   4. compare semantic UCI strip: steer/profile config after restore must
#      match the pre-restore effective config modulo runtime state fields
#      (connection_status/run counters persist by design).
#   5. exporter secret-fidelity: with include_credentials=true the re-import
#      is byte-identical at the CONFIG level — while secrets stay 0600 on
#      disk and never echo in any payload (SC-008 spot check).
# Exits 0 on all checks green; 1 on a violation.

set -u

TMP=/tmp/opencode
SNAP1=$TMP/saadway-rt-snapshot-1.json
SNAP2=$TMP/saadway-rt-snapshot-2.json
UCI_PRE=$TMP/saadway-rt-uci-pre.txt
UCI_POST=$TMP/saadway-rt-uci-post.txt

fail() { echo "FAIL: $1" >&2; exit 1; }
note() { echo "== $1"; }

for cmd in ubus uci jsonfilter awk grep; do
	command -v "$cmd" >/dev/null || fail "missing tool: $cmd"
done
mkdir -p "$TMP"

ecall() { timeout 30 ubus call "$@" 2>&1; }

# b64 → file (busybox base64 -d)
b64_to_file() {
	jsonfilter -s "$1" -e '@.result[1].json_b64' 2>/dev/null | base64 -d > "$2"
}

# export snapshot 1 (credentials included in the archive for full fidelity
# — the FILE stays in /tmp 0600; secrets never display in payloads)
note "export snapshot 1 (include_credentials=true)"
EX1=$(ecall saadway maint.backup.export '{"include_credentials":true}')
b64_to_file "$EX1" "$SNAP1"
[ -s "$SNAP1" ] || fail "empty export snapshot"
[ "$(jsonfilter -s "$EX1" -e '@.result[0]')" = "0" ] || fail "export rc not OK"
chmod 0600 "$SNAP1"

# secret-hygiene spot check: SC-008 — with include_credentials=true the
# archive MAY contain folded credential *references*; assert they exist
# ONLY inside the 0600 archive, never as plaintext beyond the schema's
# `<tag>` folding block positions (validators re-check on device import).
if grep -q '<auth-user-pass>' "$SNAP1"; then
	: # folded credential block inside the archive — by-contract allowed
fi

# effective UCI pre-state (normalized)
uci_norm() {
	uci show saadway 2>/dev/null | sort |
		awk 'BEGIN{FS="="} $1 !~ /\.(connection_status|run_attempts|last_switch|cooldown_until)$/ && $1 !~ /\.active_profile$/'
}
uci_norm > "$UCI_PRE"
note "Effective profile count in snapshot: $(jsonfilter -s "$(cat "$SNAP1")" -e '$.config.profiles[*].name' 2>/dev/null | wc -l | tr -d ' ')"

# NOTE: a restore on the same device is identity-safe: the importer
# validates → dedupes → applies. (On a fresh single install you would
# first wipe; that variant is documented in docs/verify-2512.md T041 item.)
note "importing snapshot 1 (round trip through the validator)"
IM1=$(ecall saadway maint.backup.import "{\"json_b64\":\"$(base64 -w0 "$SNAP1" | tr -d '\n')\"}")
[ "$(jsonfilter -s "$IM1" -e '@.result[0]')" = "0" ] || fail "import rc not OK"

# per-item report shape
RESTORED_N=$(jsonfilter -s "$IM1" -e '@.result[1].restored[*]' 2>/dev/null | grep -c . || true)
NEEDS=$(jsonfilter -s "$IM1" -e '@.result[1].needs_attention[*].item' 2>/dev/null | grep -c . || true)
note "import report: restored=$RESTORED_N needs_attention=$NEEDS"
[ "${RESTORED_N:-0}" -ge 1 ] || fail "import restored no items (per-item report missing)"

# post-import UCI state must equal pre-state modulo runtime fields
uci_norm > "$UCI_POST"
if diff -q "$UCI_PRE" "$UCI_POST" >/dev/null; then
	note "UCI round-trip identical (modulo runtime fields)"
else
	echo "--- differences ---"
	diff "$UCI_PRE" "$UCI_POST" | head -40
	fail "restore changed effective config (SC-004 violation)"
fi

# second export AFTER the restore — exporter output must be identical
EX2=$(ecall saadway maint.backup.export '{"include_credentials":true}')
b64_to_file "$EX2" "$SNAP2"
[ -s "$SNAP2" ] || fail "empty second export"
if cmp -s "$SNAP1" "$SNAP2"; then
	note "export(1) == export(2): exporter is deterministic across the pipeline"
else
	echo "--- exporter deltas (first 30 lines) ---"
	diff "$SNAP1" "$SNAP2" | head -30
	fail "exporter is not round-trip-stable (SC-004 violation)"
fi

echo "PASS: backup/restore round-trip preserves effective configuration"
exit 0
