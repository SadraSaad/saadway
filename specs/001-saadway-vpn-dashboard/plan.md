# Implementation Plan: SaadWay VPN Tunnel & Routing Dashboard

**Branch**: `001-saadway-vpn-dashboard` | **Date**: 2026-10-19 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/001-saadway-vpn-dashboard/spec.md`

## Summary

SaadWay is an OpenWrt 25.12 package delivering multi-profile OpenVPN and IKEv2
client tunneling with automatic failover, policy-based traffic steering
(tunnel-default with per-device/domain/IP opt-outs), an opt-in kill switch,
forced-tunnel DNS, and a dual UI: a minimal LuCI lifecycle page plus a
standalone Persian-first (RTL) dashboard driving all features exclusively
through authenticated ubus-over-HTTP APIs. Technical approach: a single
resident C daemon (`saadwayd`) wired to procd/ubus/uci orchestrates tunnel
managers (openvpn-openssl and strongSwan/swanctl), materializes every tunnel
as an L3 interface (`tunX`/`xfrmX`), and applies all firewall/routing changes
atomically as single nftables transactions via fw4 includes. State lives only
in UCI (+ 0600 secret files); full JSON backup/restore is derived from UCI.

## Technical Context

**Language/Version**: C11 (OpenWrt 25.12 toolchain, musl libc) for the
resident daemon; POSIX `ash` (BusyBox) glue scripts; LuCI client-side
JavaScript for the LuCI page; vanilla ES2020 JavaScript + HTML/CSS for the
standalone dashboard SPA (no build-step framework).

**Primary Dependencies**: fw4 + nftables (kernel firewall, atomic rule apply),
procd (init/service supervision), ubus/libubus (RPC + events), uci (state),
uhttpd + uhttpd-mod-ubus (HTTPS JSON-RPC transport with session tokens),
dnsmasq (DNS steering helpers), openvpn-openssl (OpenVPN client),
strongSwan suite: strongswan + swanctl + strongswan-mod-eap-mschapv2 +
strongswan-mod-gcm + kmod-xfrm-interface + luci-proto-xfrm (IKEv2 client),
libmbedtls/libopenssl (digest helpers for session token material).

**Storage**: UCI config `/etc/config/saadway` (profiles, steering rules,
service state, preferences); 0600 permission secret files under
`/etc/saadway/` (credentials, imported .ovpn private material); portable
backup artifact = single JSON file (schema v1, see contracts).

**Testing**: host-side C unit tests (cmocka harness compiled in CI against
musl toolchain); `shellcheck` lint on ash scripts; on-device smoke/integration
scripts under `tests/` (executed per quickstart.md); Spec Kit quality gates
are enforced per constitution Section 3.

**Target Platform**: OpenWrt 25.12 routers (mips/mipsel/arm/arm64, musl,
BusyBox ash), LAN-only exposure of both UIs; no WAN exposure.

**Project Type**: OpenWrt package feed (multi-package: core service package +
LuCI app package + shared dashboard assets).

**Performance Goals**: profile activation/switch completes < 10 s;
health test (ICMP + HTTP-204) resolves result ≤ 5 s; live bandwidth/log
polling at ≥ 1 Hz without blocking the daemon event loop; kill-switch
containment enforced before any tunneled-client egress during transitions.

**Constraints**: no Node.js/full Python/JVM on-router; every outbound network
operation async with explicit finite timeout; all firewall/routing mutations
applied as single nftables transactions (no window of partial state);
secrets only in 0600 files, never in UCI world-readable options, logs, or
unauthenticated responses; fail closed on every failure path.

**Scale/Scope**: ≤ 20 stored profiles (any mix of OpenVPN/IKEv2); ≤ 500
domain bypass entries; ≤ 253 device policies (typical LAN); exactly one
active tunnel at a time; single-administrator dashboard usage with multiple
concurrent sessions supported.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Gate / Principle | Status | Evidence (artifact section) |
|---|---|---|
| I. Native Platform Compliance (fw4/nftables/procd/ubus/uci only) | ✅ PASS | research.md D1/D4; data-model.md storage section; no iptables/ipset/fw3 anywhere in design |
| II. Embedded Footprint & Non-Blocking I/O | ✅ PASS | research.md D2 (C daemon, event loop + explicit timeouts); SPA without heavy runtime; FR-018 mapped in quickstart |
| III. Unified L3 Tunnel Interface Abstraction | ✅ PASS | research.md D3 (openvpn → tunX; strongSwan → xfrmX with if_id); all PBR/counters/health tests bound to interface |
| IV. Zero-Leak & Atomic Security | ✅ PASS | research.md D5/D6 (single nft transaction apply, kill switch default-off per clarification Q2, DNS redirect via dnsmasq+nft); data-model secret storage rules |
| V. Strict Dual-UI Decoupling | ✅ PASS | research.md D7 (uhttpd-mod-ubus authenticated JSON-RPC only; LuCI JS page limited to status/enable/open); contracts/api.md |
| VI. Single Source of Truth & Recoverability | ✅ PASS | research.md D8 (UCI is sole persistent state; backup JSON v1 round-trip); data-model state transitions |
| Section 2 platform bans (runtimes/tooling blacklist) | ✅ PASS | Technical Context dependency list contains no banned runtime |
| Section 3 gates 1–7 (traceability) | ✅ PASS (design level) | quickstart.md maps each gate to a runnable on-device scenario |

**Post-Phase-1 re-check**: design artifacts introduce no gate violations
(see Complexity Tracking — empty).

## Project Structure

### Documentation (this feature)

```text
specs/001-saadway-vpn-dashboard/
├── plan.md              # This file (/speckit.plan command output)
├── research.md          # Phase 0 output (/speckit.plan command)
├── data-model.md        # Phase 1 output (/speckit.plan command)
├── quickstart.md        # Phase 1 output (/speckit.plan command)
├── contracts/           # Phase 1 output (/speckit.plan command)
│   ├── api.md           # ubus-over-HTTP JSON-RPC surface
│   ├── uci.md           # UCI configuration schema
│   └── backup-schema.json  # Backup/restore JSON schema v1
└── tasks.md             # Phase 2 output (/speckit.tasks command - NOT created by /speckit.plan)
```

### Source Code (repository root)

```text
saadway/                     # core service package
├── Makefile                 # OpenWrt buildroot package makefile
├── src/
│   ├── main.c               # saadwayd entrypoint: event loop, procd, signals
│   ├── ubus.c               # ubus object + method handlers
│   ├── uci-state.c          # UCI read/write (profiles, rules, state)
│   ├── tunnel-openvpn.c     # openvpn profile renderer + supervision
│   ├── tunnel-ikev2.c       # swanctl renderer + supervision (xfrm)
│   ├── policy.c             # nftables/fw4 renderer; atomic apply
│   ├── steering.c           # PBR tables, fwmarks, device/domain rules
│   ├── dns.c                # dnsmasq tunnel-listener + port-53 redirect
│   ├── health.c             # async ICMP + HTTP-204 tests with timeouts
│   ├── counters.c           # per-interface RX/TX sampling
│   ├── failover.c           # watchdog state machine + cool-down retry
│   ├── backup.c             # JSON export/import (schema v1)
│   ├── log.c                # syslog writer with secret scrubbing
│   └── unit/                # host-side unit tests (cmocka harness)
└── files/
    ├── saadway.init         # procd service definition
    ├── saadway.conf         # UCI defaults
    ├── core.nft             # fw4/nftables include templates
    ├── rpc-acl.json         # ubus ACL for dashboard sessions
    └── htdocs/saadway/      # standalone dashboard SPA (vanilla JS)
        ├── index.html
        ├── css/             # RTL-first styles, Persian default
        ├── js/              # API client, views, canvas charts, i18n (fa/en)
        └── i18n/            # fa.json, en.json dictionaries

luci-app-saadway/            # LuCI app package (lifecycle/status page only)
├── Makefile
└── htdocs/luci-static/resources/view/saadway/
    └── dashboard.js         # LuCI JS view: status, enable/disable, open link

tests/                       # on-device smoke & integration scripts
├── smoke-switching.sh       # gate 4/5/7 helpers
├── smoke-container.sh       # block-gate: UI liveness with dead test target
├── leak-check.sh            # zero-leak packet counter under switch/drop
└── restore-roundtrip.sh     # backup export→import fidelity
```

**Structure Decision**: OpenWrt package feed layout with two packages
(`saadway` core service, `luci-app-saadway` UI shim) sharing the dashboard
assets shipped inside the core package; tests directory holds on-device
gate scripts. This mirrors upstream conventions for procd+uhttpd packages
and keeps each package's Makefile/dependency set isolated.

## Complexity Tracking

> **Fill ONLY if Constitution Check has violations that must be justified**

(empty — no violations; design remains within all six principles and both
platform constraint sections)
