# SaadWay host-side unit tests (T042)

cmocka harnesses for four pure/bounded units (constitution verifiable
OFF-device — no router required):

| harness                 | unit under test                                  | gates it pins      |
|-------------------------|--------------------------------------------------|--------------------|
| `test_policy_render.c`  | `sw_policy_render()` atomic ruleset composition  | FR-012/FR-014, IV  |
| `test_steer_order.c`    | `sw_steering_mark_rules()` Q1-A precedence chain | FR-009/FR-010/FR-011/Q1-A |
| `test_failover.c`       | failover state machine (attempt budget, cooldown, re-arm) | FR-008/FR-026, Q3-A, Q2-B |
| `test_backup_codec.c`   | backup export/import codec round-trip           | FR-019, SC-004     |

## Build & run (host)

Prerequisites (Debian/Ubuntu naming):

    apt install build-essential pkg-config libcmocka-dev libjson-c-dev \
                libmbedtls-dev

Then:

    cd saadway/src/unit
    make
    make check     # test_policy_render, test_steer_order, test_failover,
                   # test_backup_codec — every binary must exit 0

Notes:

* Tests compile against the SAME sources shipped to OpenWrt
  (`../<module>.c`) plus `test-stubs.c`, which stubs the
  process-engine symbols only reachable on-device (`sw_activate_profile`,
  the child-registry API, and the tunnel-stop verdict wrappers). The stubs
  make every spawn attempt fail deterministically so the state-machine
  tests exercise the roll/cooldown/re-arm behavior WITHOUT any child
  process — the fail-closed-but-responsive contract (Principle II) is
  thereby asserted, not bypassed.
* libuci is represented by `shim_uci.c`, which FAILS CLOSED: any code path
  that would try to persist while running under the harness logs a warning
  and receives an error verdict (like an absent backing store on-device).
  Export/steer/render paths never enter; whole-unit assertions pin the
  renderer and codec surfaces. Set `SW_UNIT_STRICT_SHIM=1` to abort instead
  when hunting scope leakage.
* Real host libraries are used where available: json-c (backup codec),
  mbedtls (sha256), cmocka (harness). Any of them that is NOT installed
  is replaced automatically at link time by the dev-only shim pool
  (`/tmp/opencode/swstubs`, never shipped): the cmocka runner, a
  json-c-compatible codec subset, and a SHA-256 reference implementation
  (device equivalence recorded under verify item T-verify-7).

Device-side equivalence is re-verified by the on-device smoke scripts
(`tests/leak-check.sh`, `tests/smoke-container.sh`,
`tests/restore-roundtrip.sh`) and the quickstart run (T046) — these unit
harnesses are a fast host-side pre-check, not a substitute.
