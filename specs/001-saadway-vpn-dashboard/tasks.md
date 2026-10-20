---

description: "Task list for SaadWay VPN Tunnel & Routing Dashboard implementation"
---

# Tasks: SaadWay VPN Tunnel & Routing Dashboard

**Input**: Design documents from `/specs/001-saadway-vpn-dashboard/`

**Prerequisites**: plan.md (required), spec.md (required for user stories), research.md, data-model.md, contracts/ (api.md, uci.md, backup-schema.json), quickstart.md

**Tests**: The feature's on-device smoke scripts (`tests/*.sh`) are plan-structural
deliverables (constitution Section 3 gates) and are included as implementation
tasks. No test-first/TDD phases were requested and are therefore not included.

**Organization**: Tasks grouped by user story (US1–US6 from spec.md) for
independent implementation and testing.

**Constitution note**: Every task below delivers only fw4/nftables, procd,
ubus, uci infrastructure (Principle I), async-with-explicit-timeout logic
(Principle II), and 0600 secret handling (Principle IV). Any task found
tempted to use iptables/ipset/fw3 or a banned runtime is non-compliant.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (US1…US6)
- Include exact file paths in descriptions

## Path Conventions

- OpenWrt package feed layout per plan.md: `saadway/` (core service package:
  `src/` C daemon, `files/` init/nft/htdocs), `luci-app-saadway/` (LuCI app),
  `tests/` (on-device smoke scripts).

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Repository skeleton and package build scaffolding

- [X] T001 Create directory structure per plan.md: `saadway/src/`, `saadway/files/htdocs/saadway/{css,js,i18n}`, `luci-app-saadway/htdocs/luci-static/resources/view/saadway/`, `tests/`
- [X] T002 [P] Write OpenWrt package Makefile `saadway/Makefile` declaring the DEPENDS set from plan.md Technical Context (openvpn-openssl, strongswan suite, uhttpd-mod-ubus, kmod-xfrm-interface, dnsmasq) with SECTION 2 blacklist compliance (no banned tooling/runtime deps)
- [X] T003 [P] Write LuCI app package Makefile `luci-app-saadway/Makefile` (DEPENDS: saadway, luci-base only)

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Core daemon infrastructure that MUST complete before ANY user story

**⚠️ CRITICAL**: No user story work can begin until this phase is complete.

- [X] T004 Implement daemon entrypoint and single poll()-based event loop with explicit timeout table (research D2 values) in `saadway/src/main.c`
- [X] T005 Implement UCI state layer (read/write/validate all section types and enum/uniqueness rules verbatim from data-model.md and contracts/uci.md) in `saadway/src/uci-state.c`
- [X] T006 [P] Implement syslog-backed logging with secret scrubbing (FR-021: username/password/private key/token never emitted) in `saadway/src/log.c`
- [X] T007 Register ubus object `saadway` and implement `mgmt.status` + `mgmt.set` handlers returning the exact payloads in contracts/api.md in `saadway/src/ubus.c`
- [X] T008 [P] Write procd init script `saadway/files/saadway.init` with boot ordering per contracts/uci.md: containment (if kill_switch on) → policy apply → active profile start → watchdog resume
- [X] T009 [P] Write UCI defaults `saadway/files/saadway.conf` carrying the ratified clarifications: kill_switch=0 (Q2-B), tunnel-default steering implied, failover_attempts=3, failover_cooldown=300 (Q3-A), probe_interval=30, stats_interval=1, dashboard language fa (Q4-A)
- [X] T010 [P] Write fw4/nftables include skeleton `saadway/files/core.nft`: saadway table with fwmark bit definitions, base forward/output chains, tunnel table-201 and direct table-202 prerequisites (structure per research D4; no rules content yet)
- [X] T011 Implement atomic policy applier: render full desired ruleset and apply within ONE nft transaction (`nft -f` semantics), fail-closed on render error, in `saadway/src/policy.c` (Principle IV)

**Checkpoint**: Foundation ready — user story implementation can begin. Verify: service starts under procd, `ubus call saadway mgmt.status` answers, empty config parses.

---

## Phase 3: User Story 1 - Multi-Profile OpenVPN & IKEv2 Tunnel Management (Priority: P1) 🎯 MVP

**Goal**: Import/create, store, list, activate, and auto-fail over OpenVPN and IKEv2 tunnel profiles with one-click switching <10 s.

**Independent Test**: Import one `.ovpn`, create one IKEv2 profile from form defaults, activate each in turn (verify encrypted traffic + L3 interface), then kill the active tunnel and observe automatic switch to the next profile (quickstart Scenario 1 + Scenario 6.2).

### Implementation for User Story 1

- [X] T012 [P] [US1] Implement OpenVPN profile validator/renderer/supervisor in `saadway/src/tunnel-openvpn.c`: import validation per FR-025 with reject codes E_NO_REMOTE/E_BAD_SYNTAX/E_UNSUPPORTED_DIRECTIVE/E_DUPLICATE_NAME (contracts/api.md), normalized config to `saadway/files`-side run dir, optional auth-user-pass secret extracted to 0600 file referenced by `secret_ref` (never inlined in UCI), supervisor detects loss/times-out (FR-001, FR-002)
- [X] T013 [P] [US1] Implement IKEv2 renderer/supervisor in `saadway/src/tunnel-ikev2.c`: swanctl connection with EAP-MSCHAPv2 identity, proposals `aes256gcm16-prfsha256-ecp384-ecp521` / `aes256gcm16` (FR-004/FR-005 defaults), XFRM interface with unique if_id materializing xfrmX (FR-003), form fields Remote/Local ID "same-as" semantics, credentials in 0600 secret file
- [X] T014 [US1] Implement activation/switch engine in `saadway/src/main.c`: single-active invariant (FR-007), atomic transition via policy.c (FR-014), <10 s completion incl. interface-up detection and failure of activation when no L3 interface (tunX/xfrmX) appears (constitution Principle III)
- [X] T015 [P] [US1] Implement failover watchdog state machine in `saadway/src/failover.c`: per-profile attempt counter (default 3, configurable FR-008), cycle-exhaustion → cool-down retry (Q3-A: full cycle then 300 s cool-down, repeat; FR-026), connection_status transitions up/down/connecting/contained/cooldown per data-model.md state machine
- [X] T016 [US1] Implement `profile.*` RPC methods (list, import_openvpn, create_ikev2, update, delete, activate, deactivate) with exact payloads and error envelope of contracts/api.md in `saadway/src/ubus.c`
- [X] T017 [US1] Persist activation/runtime state (active_profile, connection_status, failover counters) back into UCI `saadway_service` section via `saadway/src/uci-state.c` so state survives reboot (FR-024 basis)

**Checkpoint**: User Story 1 fully functional: import → activate → switch (<10 s) → auto-failover with cool-down, verifiable independently via quickstart Scenarios 1 and 6.2.

---

## Phase 4: User Story 2 - Zero-Leak Security Assurance (Priority: P1)

**Goal**: Opt-in kill switch with zero packet egress during tunnel loss/switch, forced-tunnel DNS for tunneled clients, secret hygiene.

**Independent Test**: Enable kill switch, drop tunnel mid-traffic, run leak script expecting exactly 0 leaked packets; verify DNS queries traverse tunnel and no secret appears anywhere observable (quickstart Scenario 2).

### Implementation for User Story 2

- [X] T018 [P] [US2] Implement nftables rule content in `saadway/src/policy.c` against `saadway/files/core.nft`: kill-switch containment chain (drop tunneled-mark egress to non-tunnel destinations when containment active), tunnel egress permit only while interface up AND health-verified, re-open combined in the SAME atomic transaction as tunnel activation (FR-012, FR-014)
- [X] T019 [US2] Implement fail-closed state-change handling in `saadway/src/policy.c`: on any render/apply failure keep last-good policy or apply containment mask; never fail open (Principle IV); containment held during profile switches
- [X] T020 [P] [US2] Implement DNS leak prevention in `saadway/src/dns.c`: dnsmasq tunnel-resolver instance (no-resolv, bound to tunnel) + nft redirect of all port-53 UDP/TCP queries from tunneled-marked clients into tunnel resolver (FR-013)
- [X] T021 [US2] Implement `sec.*` RPC methods (status, kill_switch.set persisting to UCI FR-012 default-off, dns.status) per contracts/api.md in `saadway/src/ubus.c`
- [X] T022 [US2] Write leak/atomicity smoke script `tests/leak-check.sh`: packet-counter assertion of 0 tunneled-mark egress during controlled tunnel drop and during profile switch under load (Gate 5; SC-002)

**Checkpoint**: User Story 2 verifiable independently: 0 leaks during drop (SC-002), DNS leak test passes, secret-file 0600 audit clean (FR-021/SC-008).

---

## Phase 5: User Story 3 - Smart Traffic Steering & Bypass (Priority: P2)

**Goal**: Domain/subdomain bypass list, one-checkbox local/country/banking IP-range bypass, per-device (IP/MAC) policies with tunnel-default semantics.

**Independent Test**: Add bypass domain (direct path observed), enable IP-range checkbox (bundled ranges direct), set one device direct + another implicit tunnel — both co-exist; duplicate entries rejected; mutations never drop the tunnel (quickstart Scenario 3).

### Implementation for User Story 3

- [X] T023 [P] [US3] Implement steering engine in `saadway/src/steering.c`: fwmark + `ip rule` + dedicated routing tables 201/202 evaluation chain device-direct > domain-bypass > IP-range-bypass > implicit tunnel default (Q1-A; FR-009/FR-010/FR-011), rendered into the single atomic nft/ip-rule pass via policy.c
- [X] T024 [P] [US3] Ship bundled local/country/banking CIDR set `saadway/files/bypass-cidr.txt` and loader (into nft named set, rebuilt inside the same transaction) in `saadway/src/steering.c` (FR-010 assumption: bundled, offline)
- [X] T025 [US3] Implement `steer.*` RPC methods with duplicate detection (case-insensitive domain, duplicate match_value → E_DUPLICATE; FR-025) exact payloads per contracts/api.md in `saadway/src/ubus.c`
- [X] T026 [US3] Implement bypass-domain DNS exception (bypass-marked domains resolve via WAN resolver for correctness; tunneled traffic stays tunnel-resolver) in `saadway/src/steering.c` + `saadway/src/dns.c` (research D5 nuance)

**Checkpoint**: User Story 3 independently testable per quickstart Scenario 3, including steering-change-without-tunnel-drop.

---

## Phase 6: User Story 4 - Dual-UI Experience: LuCI Controller + Standalone Dashboard (Priority: P2)

**Goal**: LuCI page limited to status/enable/open-link; standalone Persian-first RTL SPA with authenticated JSON-RPC-only backend.

**Independent Test**: LuCI page exposes exactly 3 controls; unauthenticated RPC denied; authenticated session works with inactivity expiry; RTL Persian default with fa/en switch (quickstart Scenario 4).

### Implementation for User Story 4

- [X] T027 [P] [US4] Write ubus ACL `saadway/files/rpc-acl.json`: LAN-only `/ubus` access to `saadway.*` restricted to logged-in sessions (Gate 6; Principle V); LuCI itself reaches `mgmt.*` via LuCI's own trust path
- [X] T028 [P] [US4] Implement dashboard credential material in `saadway/src/uci-state.c`: dedicated auth mode = salted hash in 0600 `saadway/files-side/etc/saadway/dashboard.auth` (created on first login provisioning), alternative `system` mode = root-auth check; `dashboard.session_timeout` default 1800 s (FR-020, research D7)
- [X] T029 [P] [US4] Build SPA shell: `saadway/files/htdocs/saadway/index.html`, RTL-first CSS in `saadway/files/htdocs/saadway/css/` (Persian default, Right-to-left), fa/en dictionaries `saadway/files/htdocs/saadway/i18n/{fa,en}.json`, language switch + persistence (FR-023, FR-027/Q4-A)
- [X] T030 [US4] Implement JSON-RPC `/ubus` API client with session-token lifecycle (login via session.login, 401 re-login handling, no token in logs) in `saadway/files/htdocs/saadway/js/api.js`
- [X] T031 [P] [US4] Implement LuCI JS view `luci-app-saadway/htdocs/luci-static/resources/view/saadway/dashboard.js`: exactly status display + enable/disable toggle (mgmt.set) + open-dashboard button — nothing more (FR-022, Gate 6)
- [X] T032 [US4] Wire uhttpd exposure: uci-defaults registering dashboard docroot at `/saadway/`, LAN-interface binding only, ACL attach for uhttpd-mod-ubus; session inactivity expiry working (FR-020; Gate 6)

**Checkpoint**: Both UIs independently verifiable per quickstart Scenario 4.

---

## Phase 7: User Story 5 - Live Health & Bandwidth Monitoring (Priority: P2)

**Goal**: ≤5 s layered health test (ICMP + HTTP-204 inside tunnel), 1 Hz bandwidth graph, cursor-based scrubbed live logs — all non-blocking.

**Independent Test**: Health-call resolves within 5 s even with dead targets (timeout result, UI stays live); graph updates from mon.stats polling; log stream shows scrubbed auth-failure (quickstart Scenario 5).

### Implementation for User Story 5

- [X] T033 [P] [US5] Implement async layered health checks in `saadway/src/health.c`: ICMP ping to 8.8.8.8/1.1.1.1 bound to tunnel interface (3 s timeout) + HTTP GET to `http://cp.cloudflare.com/generate_204` (4 s timeout), child-process supervision, graceful Timeout result, total ≤5 s (FR-015, FR-018, SC-003)
- [X] T034 [P] [US5] Implement 1 s RX/TX counter sampler reading `/sys/class/net/<tunnel-if>/statistics/{rx,tx}_bytes` with delta math to bps in `saadway/src/counters.c` (FR-016; Principle III — bound to tunnel interface)
- [X] T035 [US5] Implement `mon.*` RPC methods (health_test, stats, logs cursor-based syslog tail with secret scrubbing) per contracts/api.md in `saadway/src/ubus.c` (FR-017, FR-021)
- [X] T036 [US5] Implement dashboard monitoring views in `saadway/files/htdocs/saadway/js/` + `css/`: canvas bandwidth chart (self-hosted, no CDN), health-result panel with timeout states, live log panel with cursor follow (FR-016/FR-017/FR-018)
- [X] T037 [US5] Write blocking-gate smoke script `tests/smoke-container.sh`: block health targets via nft, prove probe returns timeout AND daemon/API stay responsive (Gate 3, FR-018)

**Checkpoint**: User Story 5 independently verifiable per quickstart Scenario 5.

---

## Phase 8: User Story 6 - Backup, Restore & Boot Resilience (Priority: P3)

**Goal**: Single JSON backup (100 % restore fidelity incl. per-item reporting) and automatic pre-leak boot recovery.

**Independent Test**: Export → import into fresh install = identical behavior with per-item report; power-cut mid-tunnel reboots into same active profile with containment applied before LAN egress (quickstart Scenario 7).

### Implementation for User Story 6

- [X] T038 [P] [US6] Implement backup/restore codec in `saadway/src/backup.c`: export/import exactly per `specs/001-saadway-vpn-dashboard/contracts/backup-schema.json` (schema v1 const, ≤20 profiles, ≤500 domains, ≤253 devices, credentials optional-inclusive mode for 100 % fidelity SC-004), import = validate → dedupe → per-item report (restored/needs-attention) → apply via policy.c (FR-019; User Story 6 scenario 4)
- [X] T039 [US6] Implement `maint.*` RPC methods (backup.export, backup.import, cooldown.set) per contracts/api.md in `saadway/src/ubus.c`
- [X] T040 [US6] Implement boot-recovery path in `saadway/files/saadway.init` + `saadway/src/main.c`: enforce containment before tunnel start (if kill switch on), re-apply steering, reactivate last active_profile with zero manual steps (FR-024; Edge Case 4; SC-006)
- [X] T041 [US6] Write restore-fidelity smoke script `tests/restore-roundtrip.sh`: export/import round-trip asserting identical effective UCI state and service behavior (Gate 7; SC-004)

**Checkpoint**: User Story 6 independently verifiable per quickstart Scenario 7.

---

## Phase 9: Polish & Cross-Cutting Concerns

**Purpose**: Corner hardening, verification tasks, docs

- [X] T042 Implement host-side unit tests (cmocka harness) for policy renderer, backup codec, failover state machine, and steering evaluation order in `saadway/src/unit/`
- [X] T043 [P] Run shellcheck lint and fix findings across `saadway/files/saadway.init` and `tests/*.sh`
- [X] T044 [P] Write build-time verification checklist into `docs/verify-2512.md`: confirm uhttpd-mod-ubus presence/ACL syntax, strongSwan package naming (strongswan-mod-eap-mschapv2, strongswan-mod-gcm, kmod-xfrm-interface), fw4 include strategy in OpenWrt 25.12 feeds (research.md T-verify-1/2/3)
- [X] T045 Enforce footprint budget task: audit package size and DEPENDS declarations against plan.md Technical Context constraints; verify zero references to iptables/ipset/fw3 in whole tree (Gate 1, Gate 2)
- [ ] T046 Run full quickstart.md Scenarios 1–7 in order on-device and record gate outcomes (Gates 1–7 traceability table at quickstart bottom; final compliance pass per constitution Section 3)
      > NOTE: PENDING — no OpenWrt router available during the T042–T045
      > implementation run (host-only development). All deliverables it
      > consumes are ready: quickstart.md, tests/leak-check.sh,
      > tests/smoke-container.sh, tests/restore-roundtrip.sh,
      > docs/verify-2512.md (T-verify recipes). Execute on first available
      > device before release sign-off.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: none — can start immediately
- **Foundational (Phase 2)**: depends on Phase 1 completion; BLOCKS all stories (T011 atomic-apply + T007 ubus object are prerequisites for every story's RPC/apply paths)
- **US1 (Phase 3)**: depends on Phase 2; US2/US3/US4/US5/US6 may start in parallel with US1 after Phase 2 — but note soft coupling: US2's containment chain (T018) builds on policy.c from T011 and is most productive after T014 (activation engine) exists; US4's SPA needs at least T007 mgmt methods to have functional content
- **US2 (Phase 4)**: depends on Phase 2; recommended order directly after US1 (same P1 priority, shared policy.c)
- **US3 (Phase 5)**: depends on Phase 2; uses policy.c + steering.c (T023) — independent of US1 activation flow
- **US4 (Phase 6)**: depends on Phase 2 (T007 for mgmt.*); independent of tunnel stories except demo value
- **US5 (Phase 7)**: depends on an activatable tunnel (US1) for meaningful interface-bound monitoring
- **US6 (Phase 8)**: depends on profile state (US1) and policy/nft rendering (US2/US3) for round-trip fidelity
- **Polish (Phase 9)**: depends on all desired stories complete
- **Convergence (Phases 10–11)**: appended after implementation; T047–T053 carry the code/contract fixes derived from the converge audits (T047 dnsmasq-full, T048 LuCI ACL, T049 auth reconciliation incl. `libmbedtls` removal, T050 D2 ratification, T051 i18n, T052 record, T053 session_timeout)
- **Remediation (Phase 12)**: depends on the Phase 1–11 code state — specifically on T049's `libmbedtls` DEPENDS removal whose stale link-line echo is what T054 repairs; T057 validates the packaged tree once T054–T056 land
- **Remediation round 2 (Phase 13)**: supersedes the Phase-12 in-package compile approach after the real-SDK link failure persisted; depends on the Phase 1–12 code state; T058 → T059 strictly sequential (same-file/delegation coupling); final validation folds into the T046/T057 on-device round
- **Remediation round 3 (Phase 14)**: depends on the Phase 13 delegation structure (T058) — T060 corrects the header name and the toolchain/staging-include propagation inside it; T060 is the only task, sequential by nature (header fix → staging path → cross-CC default); the authoritative SDK build proof folds into the T046/T057 on-device round

### User Story Dependencies

- **US1 (P1)**: no story-dependencies; start after Phase 2
- **US2 (P1)**: build-on policy.c (foundational), but no dependency on US1 completion
- **US3 (P2)**: independent; observable with or without active tunnel
- **US4 (P2)**: independent; splittable — backend ACL/credential tasks (T027/T028/T032) before SPA polish
- **US5 (P2)**: needs US1 for real tunnel interface; code can be written in parallel
- **US6 (P3)**: integrates with US1 state + atomic apply; last story by design

### Within Each User Story

- Validators/renderers before engine/engine before RPC wiring; RPC methods last (they glue modules); smoke scripts after their subject functionality.

### Parallel Opportunities

- Setup tasks T002/T003 in parallel
- Phase 2 [P] tasks (T005–T010) in parallel after T004
- Across stories: US2/US3/US4 task groups are file-disjoint and can proceed in parallel by different developers after Phase 2
- Story-local [P] tasks as marked (e.g. T012/T013 both tunnel backends concurrently)
- Phase 12: T056 (worktree/`.gitignore` cleanup) is parallel-safe; T055 review can overlap T056 but NOT T054 (same `saadway/Makefile` file); T057 runs last, after T054–T056

---

## Implementation Strategy

### MVP First (User Story 1 + minimal mgmt surface)

1. Phase 1 → Phase 2 (foundation)
2. Phase 3 (US1): import one `.ovpn`, create IKEv2, activate, failover works
3. **STOP and VALIDATE**: quickstart Scenario 1 + 6.2 pass; Gate 4 demonstrated
4. Ship-able demo: manual state via ubus/uci

### Incremental Delivery (recommended order)

1. US1 (tunnel core) → US2 (zero-leak; security with switches)
2. US3 (steering value) → US5 (monitoring trust) → US4 (polished UI)
3. US6 (backup/boot resilience) → Polish/T046 gate pass → release

### Parallel Team Strategy (3 developers)

- Dev A: US1 → US5 (tunnel + monitoring)
- Dev B: US2 → US6 (security + resilience)
- Dev C: US3 → US4 (steering + dual UI)
- Conflicts limited to `policy.c`/`ubus.c`/`core.nft` — coordinate on US1/US2 handoff

## Notes

- Every task must preserve the six constitution principles; T045 is the hard audit ensuring nothing crept in
- All RPC payload/error norms MUST match contracts/api.md verbatim
- All UCI field values MUST match data-model.md/contracts/uci.md constraints verbatim
- Commit after each task or logical group; stop at any checkpoint to validate the story independently

## Phase 10: Convergence

- [X] T047 CRITICAL: Replace `+dnsmasq` with `+dnsmasq-full` in `saadway/Makefile` DEPENDS — OpenWrt's stock `dnsmasq` build has no nftset support, so the dedicated tunnel resolver's `nftset=/%domain/4#inet#saadway#bypass_domains` directives (steering.c:120, dnsmasq-tunnel.conf) break domain bypass on stock installs; `dnsmasq-full` requires no config change (tooling identical, `dnsmasq-full` PROVIDES `dnsmasq`); update `docs/verify-2512.md` dependency audit and add an on-device checklist line re-verifying the dedicated resolver starts and populates `inet#saadway#bypass_domains` per FR-009/FR-011, research D5 (partial)
- [X] T048 HIGH: Ship `luci-app-saadway/root/usr/share/rpcd/acl.d/luci-app-saadway.json` granting the LuCI session access group ubus object `saadway` methods `mgmt.status`/`mgmt.set` (read+write) — upstream `luci-base.json` grants no ubus wildcard for third-party objects, so the view's calls (view/…/dashboard.js:22,28) are denied −32002 without it; confirm the luci.mk install hook picks up `root/` and re-check on-device in quickstart Scenario 4 per FR-022, US4/AC, Gate 6 dual-UI (missing)
- [X] T049: Reconcile the FR-020 auth model across code and artifacts: delete or deliberately wire the orphaned `sw_auth_*` module (`saadway/src/uci-state.c:1312–1445`, zero call sites — superseded by the ratified rpcd `$p$saadway-admin` shadow bridge), and update `data-model.md` §DashboardSession (L139/153 `dashboard.auth`), `quickstart.md` first-login prose, and `contracts/uci.md` (add the shipped `dashboard` section: language/authentication/session_timeout) to match per FR-020, T028 text, Constitution IV (contradicts)
- [X] T050: Ratify or revert the health-probe execution model: research.md D2 still specifies ping/HTTP probes as supervised child processes while `health.c` implements in-process probes (deviation recorded in `docs/verify-2512.md` §7 and cited by the health.c header) — either amend research.md D2 to the in-process decision (preferred; doc-only) or refactor health.c to spawn-based probes, so plan text and code agree per plan: D2, Constitution III (contradicts)
- [X] T051: Add the missing `"bps"` key to `saadway/files/htdocs/saadway/i18n/en.json` (present in fa.json as "b") so EN fallback never renders the raw key in chart unit labels per FR-027, Q4-A dictionary parity (partial)
- [X] T052: Align the IKEv2 dependency statement: add `luci-proto-xfrm` to `saadway/Makefile` DEPENDS as listed in plan.md Primary Dependencies, or record in `docs/verify-2512.md` why daemon-materialized xfrm devices (ip link) make the netifd proto handler unnecessary per plan.md Primary Dependencies (partial)

## Phase 11: Convergence

- [X] T053: Make `dashboard.session_timeout` runtime-effective (FR-020 expiry): add `session_timeout` to the `mgmt.status` reply (`ubus.c:h_mgmt_status` returns the loaded `c->st.session_timeout` — already loaded/clamped ≥300 s at uci-state.c:855); extend the `contracts/api.md` `mgmt.status` field list verbatim; update `htdocs/…/api.js` to fetch it from `mgmt.status` (keep `SESSION_TIMEOUT=1800` only as the fallback for the pre-first-call login path) and use the returned value at `session.login` — `files/saadway.conf`, `contracts/uci.md` and quickstart prose stay unchanged per data-model.md §DashboardSession (partial)

## Phase 12: OpenWrt 25.12 Cross-Compilation and Packaging Fixes

**Purpose**: Remediation (user-directed) — repair the real-SDK cross-compile
link failure and packaging-tree pollution before the T046 on-device round.
Found during a 25.12 SDK build attempt: `Build/Prepare` drags `src/unit/`
(host test Makefile, shims, ELF test binaries) into `PKG_BUILD_DIR`, and
`Build/Compile` still links `-lmbedtls -lmbedcrypto` although the T049
dependency cleanup removed `libmbedtls` from `DEPENDS` (nothing stages it →
undefined-library link error), with flags/sources/libs interleaved in one
command.

- [X] T054 Fix `saadway/Makefile` linking error by explicitly excluding `src/unit/` and separating C sources from flags (missing) per T002 packaging task + Constitution II footprint — in `Build/Prepare`, copy only the daemon sources and headers (`./src/*.c` + `./src/*.h`) instead of `./src/*`, so `src/unit/` (unit-test Makefile, `test-stubs.c`, `shim_uci.c`, test sources, host-only ELF binaries) can never enter `PKG_BUILD_DIR`; in `Build/Compile`, hoist flags into named variables (e.g. `SAADWAY_CFLAGS := $(TARGET_CFLAGS) -std=c11 -D_GNU_SOURCE -Os -I$(PKG_BUILD_DIR)`), keep the object list strictly the 13 `SAADWAY_MODULES`, and link only libraries actually staged by DEPENDS — drop the dead `-lmbedtls -lmbedcrypto` (T049 removed `libmbedtls` from DEPENDS; the daemon has no mbedtls consumer since the Phase-10 T049 auth-module deletion), keep `-lubus -lubox -luci -ljson-c -lcrypt`, and place `$(TARGET_LDFLAGS)` after the object list; keep the shared pattern rule `$(PKG_BUILD_DIR)/%.o: %.c + saadway.h` using the same `SAADWAY_CFLAGS` (depends on nothing)
- [X] T055 [P] Verify `saadway` dependencies in `saadway/Makefile` (`libubus`, `libubox`, `libuci`) (partial) per T002 + plan.md Primary Dependencies — confirm `DEPENDS` carries `+libubus +libubox +libuci` (plus `libblobmsg-json`) exactly once each, that every `-l<name>` on the `Build/Compile` link line has a staging `DEPENDS` counterpart, and record the audit result in `docs/verify-2512.md` §9 (review after T054's link-line rework; same file — coordinate, do not run concurrently with T054)
- [X] T056 [P] Clean up residual object files (`*.o`) from `saadway/src/` and update `.gitignore` (partial) per T045 audit / Constitution II — delete stray `saadway/src/*.o` from the worktree, `git rm --cached` the unit-test ELF binaries that were committed before `.gitignore` existed (tracked files ignore `.gitignore`), and refine `.gitignore` so build artifacts (`*.o`, `*.a`, unit test binaries) stay ignored while test sources remain tracked (add the `!saadway/src/unit/test_*.c` negation — the current broad `unit/test_*` pattern shadows `test_*.c`); end state: `git status` clean of artifacts, `git ls-files` contains no ELF or `*.o` (parallel-safe — touches no Makefile)
- [ ] T057 Run `tests/smoke-container.sh` to ensure package integrity (missing) per Gate 3 + quickstart Scenario 5.2 — execute the smoke test after T054–T056 against the rebuilt/cleaned package tree, expect PASS (no unit/ pollution in the package, link succeeds, budget behavior intact); record the outcome in `docs/verify-2512.md` §10 (depends on T054, T055, T056; on-device hardware checks remain in T046) — EXECUTION NOTE (2026-10-20): device-bound, left unexecuted on the build host (script requires nft+ubus+an active tunnel on real OpenWrt; no container runtime available); host-side package-integrity proxies executed instead — Build/Prepare exclusion proof (zero `unit` entries in the staged copy) + all 13 modules object-compiled via the separated `SAADWAY_CFLAGS` (13/13 OK); run the literal script during the T046 device round

**Checkpoint**: a clean-SDK `make package/saadway/{clean,compile}` links and stages without any `src/unit/` residue; the repo carries no build outputs; `tests/smoke-container.sh` passes.

## Phase 13: Remediation Round 2 — Idiomatic Delegation Rebuild (25.12 SDK linker fix)

**Purpose**: The Phase-12 in-package compile still failed on the real SDK
(`ld.bfd: cannot find …`). Round 2 replaces it with the idiomatic OpenWrt
pattern: a plain source-tree `src/Makefile` plus the default
`Build/Compile` delegation.

- [X] T058: Create `saadway/src/Makefile` compiling all 13 daemon sources (`backup.c counters.c dns.c failover.c health.c log.c main.c policy.c steering.c tunnel-ikev2.c tunnel-openvpn.c ubus.c uci-state.c`) into `.o` objects and linking binary `saadway` with `-lubus -lubox -luci -lpthread` (+ `-ljson-c`, required by `ubus.c`/`backup.c` `json_object_*` consumers — prescribed lib list corrected accordingly), with `?=` toolchain vars (`CC`/`CFLAGS`/`LDFLAGS`) so OpenWrt's default `Build/Compile` cross flags always win; refactor `saadway/Makefile`: remove the custom `Build/Compile` define + `SAADWAY_*` vars + pattern rule so OpenWrt defaults to `$(MAKE) -C $(PKG_BUILD_DIR)`, keep `Build/Prepare` copying `./src/*` then explicitly removing the `unit/` directory and stray `*.o` files, and install the built binary as `$(1)/usr/bin/saadway` per T002 + Constitution II (missing)
- [X] T059: Sync the binary-path change across consumers: `saadway/files/saadway.init` `PROG=/usr/bin/saadway`; `docs/verify-2512.md` §9 install list + §9 link-parity audit (musl `-lpthread` stub note); host-side validation — `make -C saadway/src -n` pipeline + link line, real object compile with overridden `CFLAGS`, `make clean`, shellcheck on the init script — per Gate 3 proxies (partial)

**Checkpoint**: package Makefile carries no `Build/Compile`; a clean-SDK build delegates to `src/Makefile` and stages `/usr/bin/saadway`; path consumers agree.

## Phase 14: Remediation Round 3 — Cross-Compile Header & Staging-Include Fix

**Purpose**: Resolve the OpenWrt 25.12 SDK (`ipq40xx/chromium`) cross-compilation
failure `saadway.h:31:10: fatal error: libuci.h: No such file or directory`.

**Upstream facts (openwrt/openwrt@main, fetched for this task):**
- `include/package-defaults.mk` → `MAKE_VARS` passes
  `CFLAGS="$(TARGET_CFLAGS) $(EXTRA_CFLAGS) $(TARGET_CPPFLAGS) $(EXTRA_CPPFLAGS)"`,
  and `MAKE_FLAGS` passes `CROSS="$(TARGET_CROSS)"` — but the default
  `Build/Compile` passes **no `CC`**.
- `include/toplevel.mk` force-clears `CFLAGS`/`LDFLAGS` in the environment
  (`export CFLAGS=`, `export LDFLAGS=`).
- Consequence for a delegated `src/Makefile` (Phase 13 structure): it must
  (a) default to the **cross** compiler via `$(CROSS)gcc`, (b) honor **both**
  `$(CFLAGS)` and `$(CPPFLAGS)`, and (c) obtain the target staging include
  path through `TARGET_CPPFLAGS`, which upstream folds into `CFLAGS` — so no
  custom `Build/Compile` block is required and the Phase 13 delegation
  stays intact.

- [X] T060: Fix UCI include directive and propagate TARGET_CPPFLAGS (missing) per Constitution I (Native Platform Compliance — standard OpenWrt 25.12 headers and toolchain), Gate 1 (platform grep) and Gate 2 (footprint — no new dependency introduced), and docs/verify-2512.md §8 (build-time checks), §9 (dependency/asset audit), §10 (build-time record) — target files: `saadway/src/saadway.h`, `saadway/Makefile`, `saadway/src/Makefile`:
  - `saadway/src/saadway.h:31` — replace `#include <libuci.h>` with `#include <uci.h>` (the header libuci actually installs; the DEV-ONLY declare-only stub pool named its header `libuci.h`, which is exactly why the host unit suite compiled while the SDK build failed). Re-audit every angle-bracket include against real OpenWrt header names — `<libubox/uloop.h>`, `<libubox/blobmsg.h>`, `<libubus.h>`, `<json-c/json.h>` are already correct and MUST stay unchanged.
  - `saadway/Makefile` — export the target staging include path to the delegated sub-make: `TARGET_CPPFLAGS += -I$(STAGING_DIR)/usr/include` (upstream's default `Build/Compile` folds `TARGET_CPPFLAGS` into `CFLAGS`, so the Phase 13 "no custom `Build/Compile`" decision is preserved); no `DEPENDS` change (staging headers arrive only from already-declared `+libuci`, `+libubox`, `+libubus`, `+libjson-c`).
  - `saadway/src/Makefile` — use the cross toolchain and both flag variables, selecting the compiler with an **origin guard, not `?=`**: GNU make PREDEFINES `CC=cc` and `?=` does not override a built-in default, so `CC ?= $(CROSS)gcc` would silently keep the HOST compiler (upstream passes `CROSS` but never `CC` — the exact reason the target headers/sysroot stayed invisible). Use `ifeq ($(origin CC),default)` → `CC := $(CROSS)gcc` (same guard for `CFLAGS` → `-O2`), so an explicitly supplied CC/CFLAGS (command line or environment) always wins; compile via `$(CC) $(CFLAGS) $(CPPFLAGS) -std=c11 -I. -c -o $@ $<` so the staging path (delivered through `CFLAGS`) and any explicit `CPPFLAGS` both apply; keep the link rule and libs unchanged.
  - DEV-ONLY host shim pool (`/tmp/opencode/swstubs`, never shipped): provide the stub under the real name `uci.h` (same declarations as today's `libuci.h`) so the unit suite keeps compiling after the include rename, and update the `saadway/src/unit/Makefile` header comment that names the old stub — the shim pool must mirror the REAL OpenWrt header names from now on.
  - Verification per docs/verify-2512.md: (i) `gcc -fsyntax-only` over all 13 modules against the dev stub headers; (ii) `make -C saadway/src -n` shows the cross compiler plus `-I$(STAGING_DIR)/usr/include` on the compile line; (iii) unit suite `make check` still 21/21; (iv) authoritative proof — a real SDK `make package/saadway/{clean,compile}` on `ipq40xx/chromium`, recorded in docs/verify-2512.md §10 together with the upstream facts cited above and folded into the T046 on-device round.

**Checkpoint**: the 25.12 SDK build compiles and links `saadway` with no header-resolution error; no new runtime dependency; host unit suite unchanged.
