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
- **Remediation round 4 (Phase 15)**: depends on the Phase 14 staging/toolchain fix (T060) — T061 aligns the daemon and the dev stub pool with the real upstream libubus deferred-request API and re-verifies the ubus status-code mapping; strictly sequential (stub API → daemon call sites → status mapping), and the SDK build proof again folds into the T046/T057 round
- **Remediation round 5 (Phase 16)**: depends on the Phase 15 libubus alignment (T061) — T062 aligns the remaining libubus/libubox API surface (`UBUS_METHOD` arity, the two non-existent blobmsg helpers) in `ubus.c`, adds the local helpers to `saadway.h`, and purges the fictional declarations from the dev stub pool; sequential (helpers → method table → call sites → stub), SDK build proof folds into the T046/T057 round
- **Remediation round 6 (Phase 17)**: depends on the Phase 16 compile surface (T062) — T063 closes the last link gap (the `global_ctx` definition) and adds the reusable host-side link-surface audit that would have caught it; sequential (definition → audit script), SDK link proof folds into the T046/T057 round
- **Remediation round 7 (Phase 18)**: independent of the C-code phases (the daemon already packages) — T064 converts the LuCI app into a standalone SDK-buildable package and adds the missing LuCI menu entry; sequential (Makefile → menu asset → asset audit), SDK app-build proof folds into the T046/T057 round
- **Remediation round 8 (Phase 19)**: depends on Phase 18 (T064) — T065 repairs the package METADATA that T064 placed in the wrong evaluation scope (version fields must be top-level so `Package/Default` can derive `VERSION`) and adds the empty build blocks a file-only package needs; the asset set from T064 is unchanged, SDK app-build proof folds into the T046/T057 round
- **Remediation round 9 (Phase 20)**: depends on the Phase 6 uhttpd wiring (T032) that shipped `saadway/files/uci-defaults/40-saadway-uhttpd` — T066 sanitises the listener address the script writes (CIDR strip + validated bare IPv4 + wildcard-free LAN binding + `commit uhttpd`), T067 adds the host-side audit that reproduces the pre-fix failure, T068 traces the fix into quickstart Scenario 4 and the on-device checklist; T066 → T067 is a fix-then-prove sequence, T067 is file-disjoint from T066 and may be written in parallel (it must fail first), the on-device proof folds into the T046/T057 round
- **Remediation round 10 (Phase 21)**: depends on the Phase 1/2 daemon + init-script state (T008 `saadway.init`, T007 the ubus object) — T069 makes the daemon resolve the ubus socket through libubus (`ubus_connect(NULL)`, no hardcoded path) and replaces the non-existent `procd_add_restart_trigger` with the upstream `procd_add_reload_trigger "network"`, T070 adds the host-side audit (procd API surface + no hardcoded socket path) that reproduces both pre-fix failures, T071 traces the fix into quickstart Scenario 1 and the on-device checklist; T069 → T070 is fix-then-prove, T070 is file-disjoint from T069 and may be written in parallel (it must fail first), the on-device startup proof folds into the T046/T057 round
- **Remediation round 11 (Phase 22)**: depends on the Phase 6 auth bridge (T028/T032 — `50-saadway-rpcd`, `saadway-provision`, the SPA login form) — T072 ships the dashboard with the `admin` / `admin` default credential (username rename, hash-only password via the stock rpcd `$p$` bridge and the stock busybox applets, in-place migration of an already-provisioned device, `uci commit rpcd`), T073 adds the host-side credential audit that reproduces the pre-change state, T074 reconciles the contracts, the data model, the quickstart and the verify record (incl. the recorded deviation for the weak default); T072 → T073 is fix-then-prove, T073 is file-disjoint from T072 and may be written in parallel (it must fail first), the on-device login proof folds into the T046/T057 round
- **Remediation round 12 (Phase 23)**: depends on the Phase 22 auth-bridge work (T072/T073) and on the 2026-10-21 spec/plan amendment (FR-022 amended, FR-028 → FR-034; plan.md "Amended Design"; research.md D10; constitution v1.1.0) — T075 makes the default credential initialize automatically and idempotently at first boot (hash-only in the 0600 `/etc/shadow`, the UCI option stays the `'$p$admin'` reference, the legacy section is migrated away), T076 adds the root-only credential-apply path (`saadway.mgmt.credentials.set` → the provisioning helper, the secret piped over stdin, a dedicated ACL group the dashboard session does not hold), T077 refactors the LuCI view into the two-tab form, T078 ships the end-to-end credential-flow audit + the on-device validation steps; T075 → T076 → T077 is strictly sequential (the helper mode must exist before the method, and the method before the view), T078 runs last (it audits all three), the on-device proof folds into the T046/T057 round

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
- Phase 20: T067 (the host-side uci-defaults audit + its pre-fix fixture) is file-disjoint from T066 and can be written in parallel — it must FAIL against the pre-fix script before T066 lands
- Phase 21: T070 (the host-side service-startup audit + the upstream procd helper fixture) is file-disjoint from T069 and can be written in parallel — it must FAIL against the pre-fix init script / sources before T069 lands
- Phase 22: T073 (the host-side credential audit + its pre-change fixture) is file-disjoint from T072 and can be written in parallel — it must FAIL against the pre-change login script before T072 lands
- Phase 23: no parallel task — T075/T076/T077 form a strict chain (helper → method → view); T078 audits the result and runs last

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

## Phase 15: Remediation Round 4 — libubus Deferred-Request API Alignment

**Purpose**: Resolve the SDK compile failures caused by a wrong libubus
deferred-request API in the daemon:
`invalid use of undefined type 'struct ubus_deferred_request'`,
`void value not ignored as it ought to be`, and
`passing argument from incompatible pointer type`.

**Upstream facts (openwrt/ubus@master `libubus.h` + `ubusmsg.h`, fetched
for this task — the headers 25.12 ships):**
- `ubus_defer_request()` is a **`static inline void`** taking
  `(struct ubus_context *, struct ubus_request_data *req, struct
  ubus_request_data *new_req)`; it copies the request data and sets
  `req->deferred = true` — **no return value, no error signal**;
- `ubus_complete_deferred_request(struct ubus_context *, struct
  ubus_request_data *req, int ret)` — the handle is a **plain
  `struct ubus_request_data` value**;
- **`struct ubus_deferred_request` does not exist upstream**, and
  `struct ubus_request_data` has **no `hdr` member** (`&dreq->hdr` is
  invalid);
- `ubus_send_reply(struct ubus_context *, struct ubus_request_data *,
  struct blob_attr *)` returns `int`;
- status enum (`ubusmsg.h`, 0-based): `PERMISSION_DENIED=6`, `TIMEOUT=7`,
  `UNKNOWN_ERROR=9` — the "9 = PERMISSION_DENIED" claim in
  `htdocs/…/api.js` and in `docs/verify-2512.md` §5/§6 is wrong against
  the upstream enum.
- The host suite missed all of this because the DEV-ONLY stub pool
  declared the fictional API (same masking class as the `libuci.h`
  incident closed in Phase 14) — the stub must mirror upstream exactly.

- [X] T061: Correct libubus deferred request types and call signatures (missing) per Constitution I (Native Platform Compliance — the daemon MUST use the standard OpenWrt 25.12 libubus API), Gate 1 (platform) / Gate 2 (footprint — no new dependency), and docs/verify-2512.md §8 (corrected upstream record) / §5–§6 (status-code mapping) / §10 (build-time record) — target files: `saadway/src/saadway.h`, `saadway/src/main.c`, plus `saadway/src/ubus.c` (same defect in the `mon.health_test` path — outside the original brief but in scope):
  - `saadway/src/saadway.h` — replace `struct ubus_deferred_request *dreq;` with a **value** handle `struct ubus_request_data dreq;` plus a `bool dreq_active;` flag (the upstream deferral is void, so the "deferral in flight" state needs an explicit flag); remove every reference to the non-existent `struct ubus_deferred_request` type.
  - `saadway/src/main.c` — fix the four deferred-reply sites (defer at ~L234; complete at ~L151, ~L195, ~L260): call `ubus_defer_request(c->ubus, req, &c->act.dreq);` **without** a return-value comparison and set the active flag; reply with `ubus_send_reply(c->ubus, &c->act.dreq, bb.head)` (**no `->hdr`** — `ubus_request_data` has no such member); complete with `ubus_complete_deferred_request(c->ubus, &c->act.dreq, UBUS_STATUS_*)`; replace the `free(dreq); dreq = NULL;` pairs with clearing the active flag (the handle is a value — nothing to free).
  - `saadway/src/ubus.c` — same correction for the health-test path: the `g_htest` handle becomes `struct ubus_request_data dreq;` + `bool dreq_active;` (currently `struct ubus_deferred_request *dreq;` at ~L31) and the sites at ~L919, ~L921, ~L923–924 and ~L944–945 are rewritten to the upstream call shape (void defer + flag, `&g_htest.dreq`, no `->hdr`, no `free`).
  - DEV-ONLY stub pool (`/tmp/opencode/swstubs/libubus.h`, never shipped) — mirror the upstream header exactly: `static inline void ubus_defer_request(ctx, req, new_req)` (memcpy + `req->deferred = true`), `void ubus_complete_deferred_request(ctx, struct ubus_request_data *req, int ret)`, `int ubus_send_reply(ctx, struct ubus_request_data *req, struct blob_attr *msg)`, and **delete the fictional `struct ubus_deferred_request`**; the pool must never again declare a type upstream does not have (same rule that fixed `libuci.h`).
  - Status-code mapping — re-verify the denied-login code against rpcd's `session.login` return path and the upstream enum, then correct `htdocs/…/api.js` (the "9 = PERMISSION_DENIED" comment and the `isDenied` set) and `docs/verify-2512.md` §5/§6; the SPA's denial set MUST be a defensive superset that includes the true permission-denied code (6) and keep the transport code 7 OUT of the re-login trigger set.
  - Verification per docs/verify-2512.md §8: (i) `gcc -fsyntax-only` over all 13 modules against the **corrected** stub; (ii) unit suite `make check` still 21/21 (the stub change must not alter behaviour expectations); (iii) `make -C saadway/src -n` pipeline unchanged; (iv) the authoritative `make package/saadway/{clean,compile}` on the 25.12 SDK — the real proof — folded into the T046/T057 on-device round.

**Checkpoint**: the daemon compiles against the real OpenWrt 25.12 libubus API; no fictional libubus type remains in code or stubs; the SPA's denial-code mapping is upstream-accurate.

## Phase 16: Remediation Round 5 — libubox / libubus API Surface Alignment in ubus.c

**Purpose**: Resolve the SDK compile failures in `saadway/src/ubus.c`:
`macro "UBUS_METHOD" requires 3 arguments, but only 2 given`,
`implicit declaration of function 'blobmsg_find_name'`, and
`implicit declaration of function 'blobmsg_add_bool'`.

**Upstream facts (openwrt/libubox@master `blobmsg.h` + `blobmsg_json.c`,
openwrt/ubus@master `libubus.h` — fetched for this task):**
- `UBUS_METHOD(_name, _handler, _policy)` takes **three** arguments;
  `UBUS_METHOD_NOARG(_name, _handler)` is the policy-less form (it sets
  `policy = NULL`, `n_policy = 0`). `UBUS_METHOD(name, handler, NULL)` is
  **not** a valid substitute — upstream expands `ARRAY_SIZE(_policy)` and
  `ARRAY_SIZE(NULL)` is ill-formed, so every 2-argument use must become
  `UBUS_METHOD_NOARG` (or a real policy table).
- libubox ships **no** `blobmsg_add_bool` and **no** `blobmsg_find_name`:
  the boolean writer is `blobmsg_add_field(buf, BLOBMSG_TYPE_BOOL, …)`
  with a 1-byte payload (`BLOBMSG_TYPE_BOOL == BLOBMSG_TYPE_INT8`), and
  the named lookup is `blobmsg_parse()` with a `blobmsg_policy` table or a
  `blobmsg_for_each_attr` walk comparing `blobmsg_name()`.
- libubox's JSON encoder (`blobmsg_json.c`) renders `BLOBMSG_TYPE_BOOL`
  as `"true"`/`"false"` — a locally implemented boolean writer therefore
  keeps `contracts/api.md`'s boolean contract exact.
- The DEV-ONLY stub pool masked all three defects: its `libubus.h`
  declared a fictional 2-argument `UBUS_METHOD` (with
  `UBUS_METHOD_NOARG` aliased to it) and its `libubox/blobmsg.h` declared
  the two non-existent blobmsg helpers — the same masking class as the
  `libuci.h` (Phase 14) and `struct ubus_deferred_request` (Phase 15)
  incidents.

- [X] T062: Implement missing blobmsg helpers and fix UBUS_METHOD signatures in ubus.c (missing) per Constitution I (Native Platform Compliance — the daemon MUST use the standard OpenWrt 25.12 libubus/libubox APIs), Gate 1 (platform) / Gate 2 (footprint — no new dependency) and docs/verify-2512.md §8 + §10 (build-time record) — target files: `saadway/src/ubus.c`, `saadway/src/saadway.h`:
  - `saadway/src/ubus.c` — replace the **16** two-argument `UBUS_METHOD(name, handler)` entries with the upstream policy-less form `UBUS_METHOD_NOARG(name, handler)` (the 8 existing `UBUS_METHOD_NOARG` entries stay unchanged). Where a method DOES take arguments, prefer the upstream-idiomatic `UBUS_METHOD(name, handler, policy)` with a `static const struct blobmsg_policy` table whose entries mirror the parameters documented in `contracts/api.md` (e.g. `mgmt.set` → `enabled` `BLOBMSG_TYPE_BOOL`; `profile.import_openvpn` → `name`/`ovpn_b64` STRING + optional `credentials` TABLE; `steer.domain.add` → `domain` STRING; `maint.backup.import` → `json_b64`/`schema_version`; `mon.logs` → optional `cursor`/`lines`) so ubus performs upstream-standard type validation before the handler runs; the handlers keep their manual parsing and must treat a policy-rejected message as `UBUS_STATUS_INVALID_ARGUMENT`.
  - `saadway/src/saadway.h` — provide the two helpers libubox does not ship, as local `static inline` helpers (not upstream symbols): `blobmsg_add_bool(buf, name, val)` via `blobmsg_add_field(buf, BLOBMSG_TYPE_BOOL, name, &b, 1)` with a 1-byte payload (upstream's JSON encoder emits `true`/`false` for this type, so the api.md boolean contract holds), and a named-field lookup `blobmsg_find_name(msg, name)` implemented as a `blobmsg_for_each_attr` walk comparing `blobmsg_name()`, with an **optional expected-type argument** so a policy-less method cannot be fed a wrong-typed field (type-confusion guard — e.g. `mgmt.set` must not read a BOOL out of a STRING attribute).
  - `saadway/src/ubus.c` — route every call site of the two helpers through the local implementations and type-check the values read afterwards (`blobmsg_get_bool` / `blobmsg_get_u32` only after the type check); record in the file that `arg_missing()` (ubus.c:101) is a no-op today and that with real policy tables ubus performs the argument validation.
  - DEV-ONLY stub pool (`/tmp/opencode/swstubs/libubus.h` + `libubox/blobmsg.h`, never shipped) — drop the fictional declarations so the pool mirrors upstream exactly: replace the 2-argument `UBUS_METHOD` and its `UBUS_METHOD_NOARG` alias with upstream's 3-argument `UBUS_METHOD` plus the real `UBUS_METHOD_NOARG` (policy = NULL), and remove the non-existent `blobmsg_add_bool` / `blobmsg_find_name` declarations (they are now local helpers in `saadway.h`); the pool must never again declare a symbol upstream does not provide.
  - Verification per docs/verify-2512.md: (i) `gcc -fsyntax-only` over all 13 modules against the **corrected** stub; (ii) unit suite `make check` still 21/21; (iii) `make -C saadway/src -n` pipeline and link line unchanged; (iv) the authoritative `make package/saadway/{clean,compile}` on the 25.12 SDK — the real proof — folded into the T046/T057 on-device round, where `mgmt.status` must still report `enabled`/`running` as JSON booleans and all 24 `saadway.*` method names must still match `contracts/api.md`.

**Checkpoint**: the daemon compiles against the real OpenWrt 25.12 libubus/libubox API surface; no fictional libubus/libubox symbol remains in code or stubs; the RPC surface and its JSON types are unchanged from `contracts/api.md`.

## Phase 17: Remediation Round 6 — Daemon Context Symbol & Link Surface

**Purpose**: Resolve the final link failure of the 25.12 SDK build:
`main.c:(.text+0x88): undefined reference to 'global_ctx'` — every object
compiled, but the link could not resolve the daemon's context symbol.

**Verified facts (this repo + a host-side `nm` symbol audit):**
- `saadway/src/saadway.h:481` declares `extern struct sw_ctx
  *global_ctx;` — a **declaration only**: no daemon translation unit
  defines it. `main()` heap-allocates the context
  (`calloc(1, sizeof(*c))`, main.c:547) and assigns `global_ctx = c;`
  (main.c:552), while `ubus.c` and `dns.c` dereference it — referenced
  everywhere, defined nowhere.
- The unit suite linked only because `saadway/src/unit/test-stubs.c:17`
  supplies its own definition ("owned by main.c; unit harness uses
  NULL") — the intent was always that `main.c` owns it; the definition
  was simply never written.
- Host-side audit: after compiling the 13 modules, **every** unresolved
  symbol is either a target-library API (blob*/blobmsg*/ubus*/uci*/
  json_object*/uloop*) or libc — `global_ctx` is the **only**
  daemon-internal symbol missing, so this closes the complete link gap.

- [X] T063: Instantiate global_ctx in main.c and link daemon context (missing) per Constitution I (Native Platform Compliance — the daemon must link and run as a procd service) and Gate 2 (footprint — no new dependency), with docs/verify-2512.md §8 + §10 as the build-time record — target files: `saadway/src/saadway.h`, `saadway/src/main.c`:
  - `saadway/src/main.c` — define the symbol in exactly ONE daemon translation unit, the owner of the context lifetime: a file-scope, non-static `struct sw_ctx *global_ctx;` alongside the daemon's other globals; keep `main()`'s existing `global_ctx = c;` assignment (the context stays heap-allocated, so the POINTER is the global, not the struct) and keep the fail-closed `return 1` paths untouched.
  - `saadway/src/saadway.h` — keep the `extern` declaration unchanged (it must stay `extern`: `ubus.c` and `dns.c` dereference it) and document next to it that the single definition lives in `main.c`, so a second definition is never added.
  - Invariant to preserve: `saadway/src/unit/test-stubs.c` keeps its own `struct sw_ctx *global_ctx;` ONLY because the unit suite does not compile `main.c` (its SRC list excludes it) — if `main.c` ever joins the unit SRC list, the stub definition must be removed rather than duplicated (a duplicate symbol would break the unit link).
  - Reusable host-side link-surface check: add `tests/link-audit.sh`, which compiles the 13 daemon modules against the dev stub headers and fails when any daemon-internal symbol (the `sw_*` / `h_*` / `global_ctx` set) is referenced but not defined by the module set — the check that would have caught this defect before the SDK build; it must be green from the start.
  - Verification per docs/verify-2512.md: (i) `tests/link-audit.sh` green — no unresolved daemon-internal symbol; (ii) `gcc -fsyntax-only` over all 13 modules; (iii) unit suite `make check` still 21/21 (the stub's own definition stays — no duplicate); (iv) the authoritative full link on the 25.12 SDK — `make package/saadway/{clean,compile}` must now produce the `saadway` binary — folded into the T046/T057 on-device round.

**Checkpoint**: the daemon links with no unresolved daemon-internal symbol; the context has exactly one owner; the link-surface check guards future rounds.

## Phase 18: Remediation Round 7 — Standalone luci-app-saadway Package

**Purpose**: Make the LuCI app buildable outside the upstream LuCI feed
tree. The core daemon built and packaged (`saadway-1.0.0-r1.apk`), but
the app failed with `make[1]: *** No rule to make target
'package/luci-app-saadway/compile'. Stop.`

**Root cause (verified):** `luci-app-saadway/Makefile` ends with
`include $(TOPDIR)/feeds/luci/luci.mk` — that path exists only inside a
full OpenWrt source tree with the luci feed checked out. In an SDK
drop-in (`<sdk>/package/luci-app-saadway/`) the include resolves to
nothing, so the package's `BuildPackage` is never evaluated: the
metadata scanner does not register the package, and the build system
has no `package/luci-app-saadway/compile` target to run. The app
therefore needs a **standalone `package.mk` definition** that depends on
nothing but the core SDK machinery.

**Related gap found while assessing this (same files, same install
pass):** the package ships **no LuCI menu entry**
(`/usr/share/luci/menu.d/*.json`). In LuCI2 the client router builds its
route tree from those menu JSONs, so a view that no menu entry
references is not reachable through the UI — the documented
"Administration → SaadWay" surface (quickstart Scenario 4, FR-022) could
never appear. The standalone rewrite is the right moment to add it,
because the file belongs to the same `root/` tree the new install rules
handle.

- [X] T064: Convert luci-app-saadway/Makefile to standalone package.mk definition (missing) per Constitution I (Native Platform Compliance — a package must build with the standard OpenWrt 25.12 SDK machinery alone), Gate 1 (platform) / Gate 2 (footprint — `PKGARCH:=all`, no new runtime), and docs/verify-2512.md §9 (install-asset audit) + §10 (build-time record) — target file: `luci-app-saadway/Makefile`:
  - `luci-app-saadway/Makefile` — replace the luci.mk-based definition with a self-contained package: `include $(TOPDIR)/rules.mk` + `include $(INCLUDE_DIR)/package.mk`, an explicit `define Package/luci-app-saadway` with `SECTION:=luci`, `CATEGORY:=LuCI`, `TITLE:=LuadWay…` (the current `LUCI_TITLE` text), `PKGARCH:=all`, `MAINTAINER`/`PKG_LICENSE` as today, and **`PKG_VERSION:=1.0.0` + `PKG_RELEASE:=1`** — the version fields are mandatory (the build system errors with "missing the VERSION field" without them; the luci.mk flow used to supply them implicitly). End the file with `$(eval $(call BuildPackage,luci-app-saadway))`.
  - `luci-app-saadway/Makefile` — `Package/luci-app-saadway/install` must install exactly the three assets, with `$(INSTALL_DIR)` for each target directory and `$(INSTALL_DATA)` for the sources: `htdocs/luci-static/resources/view/saadway/dashboard.js` → `$(1)/www/luci-static/resources/view/saadway/`, `root/usr/share/rpcd/acl.d/luci-app-saadway.json` → `$(1)/usr/share/rpcd/acl.d/` (the session ACL that makes the view's `mgmt.*` calls work — Phase 14/T048), and the new menu entry → `$(1)/usr/share/luci/menu.d/`.
  - `luci-app-saadway/root/usr/share/luci/menu.d/luci-app-saadway.json` (new) — register the view so the documented surface exists: a single "Administration" entry (`"admin/saadway"`) with `"action": {"type": "view", "path": "saadway/dashboard"}` (the view file is `resources/view/saadway/dashboard.js`) and `"depends": {"acl": ["luci-app-saadway"]}` — the ACL group already defined by this package's own `acl.d` file, so the menu entry and the RPC grant stay consistent. Keep the entry minimal: no configuration sub-menus (Principle V — the LuCI surface is status + toggle + dashboard link only, FR-022).
  - `DEPENDS` — use `+saadway` (the daemon) and drop `+luci-base` from the hard dependency: an SDK staging tree may not carry the luci feed, and the app is inert without LuCI anyway (the page simply does not appear on a device without LuCI). When building an IMAGE that must pull LuCI in, add `+luci-base` explicitly in the image's package list instead of the app's DEPENDS.
  - Verification per docs/verify-2512.md: (i) the Makefile includes `$(INCLUDE_DIR)/package.mk` and evaluates `BuildPackage` — no `feeds/luci` reference remains (grep); (ii) every file the install rules reference exists in the package tree (extend the §9 install-asset audit with the LuCI app's three assets); (iii) the app must still be architecture-independent (`PKGARCH:=all`) and must not ship a compiled binary; (iv) the authoritative proof — `make package/luci-app-saadway/compile` in the SDK layout that produced `saadway-1.0.0-r1.apk` — is folded into the T046/T057 round, together with the check that the page appears under Administration → SaadWay and that its two `saadway.mgmt.*` calls are authorized by the shipped ACL group.

**Checkpoint**: `package/luci-app-saadway/compile` exists in an SDK-only tree; the app installs the view, its ACL and its menu entry; the LuCI surface is reachable and remains limited to status + toggle + link.

## Phase 19: Remediation Round 8 — luci-app-saadway Package Metadata

**Purpose**: The SDK still rejects the LuCI package:
`Makefile:63: *** Package/luci-app-saadway is missing the VERSION field.
Stop.` → `ERROR: package/luci-app-saadway failed to build.`

**Root cause (verified against openwrt@main):**
- `include/package-defaults.mk` → `Package/Default` derives the package
  version: `VERSION:=$(PKG_VERSION)-r$(PKG_RELEASE)` (or `$(PKG_VERSION)`,
  or `$(PKG_RELEASE)` when `PKG_VERSION` is empty).
- `include/package.mk` → `BuildPackage` evaluates `$(eval
  $(Package/Default))` **before** `$(eval $(Package/$(1)))`, and the
  mandatory-field check then reads the package-scoped `VERSION`
  (`$(foreach FIELD, TITLE CATEGORY SECTION VERSION, …)` → *"Package/…
  is missing the VERSION field"*).
- Therefore `PKG_VERSION`/`PKG_RELEASE` must be set at the **top level**
  of the package Makefile, before the `BuildPackage` evaluation. T064 set
  them **inside** `define Package/luci-app-saadway` — too late:
  `Package/Default` had already run with an empty `PKG_VERSION`, so
  `VERSION` stayed empty and the check fired.
- The in-repo precedent is our own core package: `saadway/Makefile:17-19`
  declares `PKG_NAME`/`PKG_VERSION:=1.0.0`/`PKG_RELEASE:=1` at the top
  level — which is exactly why the daemon packaged as
  `saadway-1.0.0-r1.apk`. The LuCI app must mirror that pattern.
- A second latent failure sits behind the first: the default
  `Build/Compile` runs `$(MAKE) -C $(PKG_BUILD_DIR)`, where this package
  has **no Makefile** (it ships files only) → *"no targets specified and
  no makefile found"*; the default `Build/Configure` would look for a
  `./configure` script that does not exist. Both must be empty blocks.

- [X] T065: Explicitly define VERSION in Package/luci-app-saadway and ensure valid package structure (missing) per Constitution I (Native Platform Compliance — the package must satisfy the standard OpenWrt 25.12 build system's metadata contract) and Gate 2 (footprint — still `PKGARCH:=all`, no new runtime), with docs/verify-2512.md §9 (install-asset audit) + §10 (build-time record) — target file: `luci-app-saadway/Makefile`:
  - Move the version metadata to the TOP LEVEL of the Makefile, before `include $(INCLUDE_DIR)/package.mk`, exactly as the core package does: `PKG_NAME:=luci-app-saadway`, `PKG_VERSION:=1.0.0`, `PKG_RELEASE:=1` (remove the in-define copies — they are evaluated too late to feed `Package/Default`).
  - Inside `define Package/luci-app-saadway`, declare the field explicitly: `VERSION:=$(PKG_VERSION)-r$(PKG_RELEASE)` — upstream's own format (the `r` matches the daemon's `saadway-1.0.0-r1.apk` naming; the check itself only requires a non-empty `VERSION`). Keep `SECTION`, `CATEGORY`, `TITLE`, `DEPENDS:=+saadway`, `PKGARCH:=all`, `MAINTAINER` and `PKG_LICENSE` as they are.
  - Add the two empty build blocks the file-only package needs, so the defaults never run: `define Build/Configure` / `endef` and `define Build/Compile` / `endef`.
  - Leave the install rules, the view, the session ACL and the menu entry untouched — the T064 asset set is correct and only the package metadata was wrong.
  - Verification per docs/verify-2512.md: (i) a host-side metadata audit that emulates the upstream rules — `Package/Default`'s `VERSION` inheritance, the mandatory-field check and the "no Makefile in the build dir" compile behaviour — applied to BOTH package Makefiles (the check must fail on the pre-fix LuCI Makefile — reproducing the SDK's exact "missing the VERSION field" abort — and on a file-only package whose empty build blocks are missing, distinguishing an UNDEFINED `Build/Compile` (upstream's default would run make in a directory without a Makefile) from a deliberately EMPTY one, and pass after the fix); ship it as `tests/pkg-metadata-audit.sh` next to the existing `tests/link-audit.sh`; (ii) strict greps: `PKG_VERSION`/`PKG_RELEASE` present at the top level, `VERSION` present inside the package define, `Build/Configure` and `Build/Compile` defined as empty blocks; (iii) the authoritative proof — `make package/luci-app-saadway/compile` in the SDK that produced `saadway-1.0.0-r1.apk` — must yield `luci-app-saadway-1.0.0-r1.apk`, folded into the T046/T057 on-device round.

**Checkpoint**: the LuCI package satisfies the build system's metadata contract (top-level version, explicit `VERSION`, empty build blocks); the metadata audit is green for both packages; the SDK produces the app package.

---

## Phase 20: Remediation Round 9 — uhttpd Listener Address Sanitising in the First-Boot Script

**Purpose**: The first-boot script wrote `192.168.5.1/24:80` into
`uhttpd.main.listen_http`. uhttpd resolves the listener host with
`getaddrinfo()`, which treats the whole string as a host name — the slash makes
it unresolvable, uhttpd prints `getaddrinfo(): Name does not resolve` and binds
**no** socket at all. The standalone dashboard (`/saadway/`) and the `/ubus`
JSON-RPC transport it depends on are therefore both unreachable, and the LuCI
"Open Dashboard" link (FR-022) dies with them.

**Root cause (verified against upstream — see the facts below):**
- `saadway/files/uci-defaults/40-saadway-uhttpd` used the address string
  exactly as it found it: the UCI fallback (`uci get network.lan.ipaddr`)
  returns the prefix form `192.168.5.1/24` when the LAN address is configured
  in CIDR notation, and the script never stripped the mask.
- The ubus branch is broken in the other direction: netifd reports each
  address as an OBJECT (`{"address":…,"mask":…}`), so the leaf
  `@.ipv4-address[0]` makes jsonfilter print the object's JSON text — not an
  address.
- The value was never validated, so whatever the two branches returned was
  written straight into the listener option (through an unquoted heredoc).
- Two further latent defects sit behind it: `add_list` on top of the stock
  wildcard list would keep uhttpd listening on EVERY interface (WAN exposure),
  and the `uci batch` was never committed — so even a correct LAN binding would
  not survive the next reboot.

**Upstream facts (fetched for this task — upstream is the oracle):**
- `openwrt/netifd@master` → `ubus.c` → `interface_ip_dump_address_list()`:
  every address is a table with `"address"` (written with `inet_ntop()`, i.e. a
  BARE address — never a prefix) plus a separate `"mask"` field → the status
  JSON is `"ipv4-address": [ { "address": "192.168.5.1", "mask": 24 } ]`.
- `openwrt/jsonpath@master` (the upstream `jsonfilter` implementation) prints a
  matched STRING raw and a matched OBJECT as its JSON text → the leaf
  `@.ipv4-address[0]` yields `{"address":"192.168.5.1","mask":24}`; the address
  must be selected explicitly: `@["ipv4-address"][0].address`.
- `openwrt/uhttpd@master` → `listen.c` → `uh_socket_bind()` →
  `getaddrinfo(host, port, &hints, &addrs)`; on failure it prints
  `getaddrinfo(): %s` (`gai_strerror()`) and returns 0 → no listener is
  created — exactly the reported symptom.
- `openwrt/openwrt@master` → `package/network/services/uhttpd/files/uhttpd.config`:
  the stock config holds FOUR wildcard entries — `list listen_http 0.0.0.0:80`,
  `list listen_http [::]:80`, `list listen_https 0.0.0.0:443`,
  `list listen_https [::]:443`.
- `openwrt/openwrt@master` → `.../files/uhttpd.init`: `config_get http "$cfg"
  listen_http` followed by `for listen in $http; do procd_append_param command
  -p "$listen"; done` → EVERY list entry becomes one `-p <host:port>` argument;
  `uhttpd_interfaces()` parses the same entries to derive the hotplug IPs
  (`${listen%%:*}`) — a CIDR string never matches an interface there either.
- `openwrt/uci@master` → `list.c` → `uci_add_list()`: creates the option when it
  is missing, APPENDS to an existing list, and CONVERTS a string option into a
  two-element list; `cli.c` → `uci_batch()` keeps going after a failing command
  (only 254/255 are special) and `-q` silences the error output.
- `uci batch` only writes the package delta under `/tmp/.uci` (tmpfs) — without
  `uci commit uhttpd` the change is lost on the next reboot.

- [X] T066: Sanitise the uhttpd listener address in the first-boot uci-defaults script (broken) per Constitution IV (Zero-Leak & Atomic Security — fail closed: never write a listener value that can take uhttpd down, never leave a WAN-facing wildcard behind) and FR-020/FR-022 (the dashboard and its `/ubus` transport must be reachable over the LAN), with docs/verify-2512.md §10 (build-time record) — target file: `saadway/files/uci-defaults/40-saadway-uhttpd`:
  - Strip any CIDR mask from the resolved address before it is used anywhere: `LAN_IP="${LANADDR%%/*}"` → `192.168.5.1/24` becomes `192.168.5.1`. The listener host is resolved with `getaddrinfo()`, which treats the whole string as a host name — a slash makes it unresolvable ("Name does not resolve"), and the same string is fed to the init script's `uhttpd_interfaces()` hotplug matcher.
  - Fix the ubus leaf: netifd reports `"ipv4-address": [ { "address": "192.168.5.1", "mask": 24 } ]` (objects, not strings), so `jsonfilter -e '@.ipv4-address[0]'` yields the object's JSON text. Use `jsonfilter -e '@["ipv4-address"][0].address'` so a bare address is extracted; KEEP the `uci get network.lan.ipaddr` fallback for the first-boot case where netifd is not up yet — that fallback is the one that produced the observed `192.168.5.1/24`.
  - Validate BEFORE writing: accept the value only if it is a bare IPv4 literal (exactly four decimal octets, each 0–255, no `/`, no `:`, no whitespace); otherwise leave the stock listener configuration untouched and `exit 0` (fail-safe: the stock `0.0.0.0:80` keeps uhttpd serving, and the firewall's WAN input policy remains the network-layer guard). Never interpolate an unvalidated value into the uci batch — the heredoc is unquoted, so a stray `'` or `$()` in the value would break out of the command.
  - Write the listeners LAN-bound WITHOUT leaving the wildcards behind: `delete uhttpd.main.listen_http` + `add_list uhttpd.main.listen_http='<LAN_IP>:80'`, and the same pair for `listen_https` → `'<LAN_IP>:443'`. A bare `add_list` is NOT sufficient: the stock config already holds `list listen_http 0.0.0.0:80` and `list listen_http [::]:80` (plus the two https wildcards), and `uci_add_list()` APPENDS to an existing list → uhttpd would keep listening on every interface (WAN exposure — api.md non-goal "No WAN exposure; uhttpd ACL binds to LAN interface only", Constitution IV). `uci_batch()` continues after a failing command, so a `delete` of an absent option is harmless.
  - Persist the change: add `uci commit uhttpd` after the batch. `uci batch` only writes the delta under `/tmp/.uci` (tmpfs) → without the commit the LAN binding is silently lost on the next reboot and the stock wildcards return.
  - Keep the rest of the contract unchanged: the "no certificates / no redirect_https / no LuCI paths" rule, the "restart uhttpd only when it is already running and enabled" behaviour, the final `exit 0`, and POSIX-sh/busybox portability (no bashisms, no non-busybox tools).
  - Repair path for devices that already ran the broken script: the file is installed with `$(CP) ./files/uci-defaults/* $(1)/etc/uci-defaults/` (not a conffile — only `/etc/config/saadway` is), so a package upgrade re-installs the script and `/etc/init.d/boot` re-runs it on the next boot → the stale `192.168.5.1/24:80` value is replaced and uhttpd picks up the committed config on its next start (or is restarted by the script when it is already running and enabled). Record this repair path in docs/verify-2512.md §10.
  - Verification per docs/verify-2512.md: (i) the host-side audit of T067 is green against the shipped script; (ii) `shellcheck -s sh -S warning` clean; (iii) strict greps: no code path can put a `/` into `listen_http`/`listen_https` — the listener value is assembled only from a validated bare IPv4, and the CIDR strip is present; (iv) the authoritative on-device proof — folded into the T046/T057 round: `uci get uhttpd.main.listen_http` must print `<lan-ip>:80` (no `/`), `uci get uhttpd.main.listen_https` must print `<lan-ip>:443`, `logread | grep -i getaddrinfo` must be empty, the SPA must answer on `http://<lan-ip>/saadway/` and `/ubus` on `https://<lan-ip>/ubus`.

- [X] T067 [P]: Ship a host-side regression audit for the first-boot uci-defaults script (missing) per Constitution I (Native Platform Compliance — the script must behave like the stock OpenWrt tooling) and Gate 3 (runtime behaviour), with docs/verify-2512.md §10 (build-time record) — target files: `tests/uci-defaults-audit.sh` (new), `tests/fixtures/40-saadway-uhttpd.prefix` (new):
  - `tests/uci-defaults-audit.sh` — POSIX sh, shellcheck-clean, next to the existing `tests/link-audit.sh` and `tests/pkg-metadata-audit.sh`. It runs `saadway/files/uci-defaults/40-saadway-uhttpd` under a stub `PATH` whose `uci`, `ubus` and `jsonfilter` are tiny recording stubs, then asserts on the recorded uci commands. The script path is an argument, defaulting to the shipped script, so the same audit can be pointed at the pre-fix fixture. Cases: (A) ubus path → a netifd-shaped status JSON → the written listener is `<ipv4>:80` with NO JSON-object text and no `/`; (B) the OBSERVED failure → `uci get network.lan.ipaddr` returns `192.168.5.1/24` → the written listener is `192.168.5.1:80` and NOT `192.168.5.1/24:80`; (C) an unusable value (`not-an-ip`, an empty value, the jsonfilter object text) → the script writes NOTHING (the stock configuration must be left untouched); (D) the recorded commands contain `delete uhttpd.main.listen_http` BEFORE the `add_list` → no `0.0.0.0:80` / `[::]:80` wildcard can survive; (E) the recorded commands contain `commit uhttpd` → the binding survives a reboot; (F) the script exits 0 in EVERY case (it is sourced by `/etc/init.d/boot` in a subshell — a non-zero exit would abort the boot sequence).
  - The `jsonfilter` stub must mirror the upstream jsonpath semantics it stands in for: a matched STRING is printed raw, a matched OBJECT is printed as its JSON text. That is what makes case (A) meaningful — the pre-fix leaf `@.ipv4-address[0]` therefore yields `{"address":"192.168.5.1","mask":24}` instead of an address.
  - `tests/fixtures/40-saadway-uhttpd.prefix` — a verbatim snapshot of the PRE-FIX script, kept ONLY as the negative fixture (never installed on a device). Capture it with `git show HEAD:saadway/files/uci-defaults/40-saadway-uhttpd` BEFORE the T066 change is committed (after the commit, HEAD no longer holds the broken version).
  - Verification per docs/verify-2512.md: (i) `sh tests/uci-defaults-audit.sh` exits 0 against the shipped script; (ii) `sh tests/uci-defaults-audit.sh tests/fixtures/40-saadway-uhttpd.prefix` exits 1 and NAMES the failing case(s) — the proof that the audit has teeth, in the same spirit as the negative cases of `tests/pkg-metadata-audit.sh`; (iii) `shellcheck -s sh -S warning` clean.

- [X] T068: Trace the uhttpd listener fix into the quickstart and the on-device checklist (missing) per Gate 6 (dual UI + session security) and FR-020/FR-022, with docs/verify-2512.md §9 (asset audit) + §10 (build-time record) — target files: `specs/001-saadway-vpn-dashboard/quickstart.md`, `docs/verify-2512.md`:
  - `quickstart.md` Scenario 4 (Dual UI + session security) — APPEND one numbered step (the four existing steps and their numbering stay untouched): after the first boot, `uci get uhttpd.main.listen_http` must print `<lan-ip>:80` — a bare `host:port`, never a CIDR — `uci get uhttpd.main.listen_https` must print `<lan-ip>:443`, `logread` must contain no `getaddrinfo(): Name does not resolve`, and the SPA must be served from `http://<lan-ip>/saadway/` on the same origin as `/ubus`.
  - `docs/verify-2512.md` — §10: record the applied change (the CIDR strip, the `.address` leaf, the delete+add_list rule with the wildcard rationale, the `commit uhttpd` persistence fix and the upgrade repair path) together with the upstream sources it was derived from (netifd `ubus.c` → `interface_ip_dump_address_list`, jsonpath `main.c`, uhttpd `listen.c` → `uh_socket_bind`, openwrt `uhttpd.config` + `uhttpd.init`, uci `list.c` → `uci_add_list` and `cli.c` → `uci_batch`); §9: extend the asset audit with the uci-defaults script — the listener value is assembled only from a validated bare IPv4, so no `/` can reach `listen_http`/`listen_https`.
  - The authoritative on-device proof (the listener values, the uhttpd log line and the SPA reachability listed in the first bullet) is folded into the T046/T057 on-device round.

**Checkpoint**: the first-boot script can no longer write an unresolvable listener host; the LAN binding is validated, wildcard-free and survives a reboot; the host-side audit proves both the fixed behaviour and the pre-fix failure; the on-device proof is queued with T046/T057.

---

## Phase 21: Remediation Round 10 — ubus Socket Auto-Resolution & procd Trigger Correctness

**Purpose**: On a stock OpenWrt 25.12 device the daemon dies during startup
with `saadwayd: cannot connect ubus at /var/run/ubus.sock: No such file or
directory`, and the service's rc link logs `procd: /etc/rc.d/S99saadway: line
45: procd_add_restart_trigger: not found`. Neither defect is a configuration
problem — both are the code using an API that no longer exists the way it
does.

**Root cause (verified against upstream — see the facts below):**
- `saadway/src/saadway.h:51` hardcodes the legacy socket path
  (`#define SW_UBUS_SOCK "/var/run/ubus.sock"`) and `saadway/src/main.c:591`
  connects with `ubus_connect(SW_UBUS_SOCK)`. Since OpenWrt 21.02 the system
  socket lives at **`/var/run/ubus/ubus.sock`**, so the connect fails with
  ENOENT, `main()` returns 1 and the daemon exits **before** registering its
  ubus object → no `saadway` object, no management surface, no dashboard, no
  LuCI page. libubus already knows the correct path — the caller must not
  second-guess it.
- `saadway/files/saadway.init:45` calls `procd_add_restart_trigger "network"`,
  which upstream does not export at all: the complete `procd_*` helper set
  (the `_procd_wrapper` export list) contains `procd_add_reload_trigger`,
  `procd_add_reload_data_trigger`, `procd_add_reload_interface_trigger`,
  `procd_add_reload_mount_trigger`, `procd_add_restart_mount_trigger`,
  `procd_add_raw_trigger`, `procd_add_config_trigger` — but **no**
  `procd_add_restart_trigger` → rc.common's shell reports "not found" on every
  start. The *reload* trigger is also the semantically correct one here: it
  fires `config.change` on /etc/config/network → `/etc/init.d/saadway reload`
  → the daemon's SIGHUP handler re-reads UCI and re-applies the policy
  atomically (no tunnel drop), which is exactly what the comment on lines
  41–42 asks for.

**Upstream facts (fetched for this task — upstream is the oracle):**
- `openwrt/ubus@master` → `CMakeLists.txt`: `SET(UBUS_UNIX_SOCKET "/var/run/ubus/ubus.sock")` + `ADD_DEFINITIONS(-DUBUS_UNIX_SOCKET=…)`; `libubus-io.c` → `ubus_reconnect()`: `if (!path) path = UBUS_UNIX_SOCKET;` → **`ubus_connect(NULL)` resolves the system socket**. `UBUS_UNIX_SOCKET` is a library-internal compile-time constant (it is NOT in any public header) → a client cannot reference it, so the daemon must not carry its own copy of the path either.
- `openwrt/openwrt@master` → `package/system/procd/files/procd.sh` (installed as `/lib/functions/procd.sh`): the `_procd_wrapper` export block IS the definitive `procd_*` API surface; `_procd_add_reload_trigger()` adds a `config.change` trigger on the named config file → `/etc/init.d/<service> reload` — a RELOAD (SIGHUP) trigger, not a restart.
- `procd_set_param` has no `depends` key → upstream offers no "wait for ubus" parameter; the boot-order race is covered by the respawn policy already set in `start_service()` (`respawn 3600 5 5` → up to 5 retries with a 5 s backoff).
- The dev stub pool already mirrors the upstream signature (`struct ubus_context *ubus_connect(const char *path);`) → `ubus_connect(NULL)` compiles unchanged against it; no stub change is needed.

- [X] T069: Resolve the ubus socket through libubus and use an upstream-exported procd trigger (broken) per Constitution I (Native Platform Compliance — the daemon must run against the STOCK OpenWrt 25.12 userland: the stock ubus socket location and the stock procd helper API) and Constitution IV (fail closed: the daemon must not die before its management surface exists), with docs/verify-2512.md §10 (build-time record) — target files: `saadway/src/saadway.h`, `saadway/src/main.c`, `saadway/files/saadway.init`:
  - `saadway/src/saadway.h:51` — DELETE the `SW_UBUS_SOCK` define (and the `SW_UBUS_SOCK` line itself). It encodes the pre-21.02 path `/var/run/ubus.sock`; the stock 25.12 path is `/var/run/ubus/ubus.sock` and libubus owns that knowledge. Add NO replacement constant: `UBUS_UNIX_SOCKET` is a library-internal compile-time define, not part of the public headers, so a client-side copy would be a second source of truth.
  - `saadway/src/main.c:591` — connect with `ubus_connect(NULL)` so libubus resolves the system socket (upstream chain: `ubus_connect(NULL)` → `ubus_connect_ctx(ctx, NULL)` → `ubus_reconnect(ctx, NULL)` → `if (!path) path = UBUS_UNIX_SOCKET;`). Keep the single client context and the fail-closed `if (!c->ubus) { … return 1; }` path — but the error message must no longer interpolate the deleted constant: log `cannot connect ubus: %s` with `strerror(errno)` (the socket path is no longer known to the daemon, and errno is the connect() error — ENOENT on a stock device). The daemon must never carry its own copy of the socket path.
  - `saadway/files/saadway.init:45` — replace `procd_add_restart_trigger "network"` with `procd_add_reload_trigger "network"`. The helper does not exist upstream; the reload trigger is both the valid call and the correct semantics (config.change on /etc/config/network → `/etc/init.d/saadway reload` → the daemon's SIGHUP path re-reads UCI and re-applies the policy atomically, no tunnel drop — matching the existing comment on lines 41–42).
  - Keep the surrounding contract untouched: `START=99` / `STOP=10`, `USE_PROCD=1`, `PROG=/usr/bin/saadway`, the respawn policy (`respawn 3600 5 5` — it covers the case where the daemon is started before ubusd is up: procd retries with a 5 s backoff), `procd_set_param reload_signal SIGHUP`, `procd_set_param file /etc/config/saadway`, the two existing `procd_add_reload_trigger "firewall"` / `"saadway"` lines, and the file's `/etc/rc.common` + POSIX-sh contract (no bashisms; the existing shellcheck disable comment stays as it is).
  - Do NOT invent a "wait for ubus" mechanism: upstream `procd_set_param` has no `depends` key, and the respawn policy is the upstream-sanctioned recovery path.
  - Verification per docs/verify-2512.md: (i) strict greps — no `SW_UBUS_SOCK` reference and no absolute ubus socket path (`/var/run/ubus.sock`, `/var/run/ubus/ubus.sock`) may remain anywhere in `saadway/`; (ii) the host-side service-startup audit of T070 is green; (iii) `gcc -fsyntax-only` over all 13 modules against the dev stub pool (the stub already declares `ubus_connect(const char *)` exactly like upstream → `ubus_connect(NULL)` compiles unchanged), the unit suite still 21/21 and `tests/link-audit.sh` still green (the link surface must not change — `ubus_connect` was already linked); (iv) `shellcheck -s sh` clean on `saadway/files/saadway.init`; (v) the authoritative on-device proof — folded into the T046/T057 round: `/etc/init.d/saadway start` must log NO "cannot connect ubus" line and NO "procd_add_restart_trigger: not found" line, `ubus list | grep saadway` must show the `saadway` object, and `ubus call saadway mgmt.status` must answer (quickstart Scenario 1 step 3).

- [X] T070 [P]: Ship a host-side service-startup audit that would have caught both defects (missing) per Constitution I (Native Platform Compliance — the init script may only call helpers the platform actually exports) and Gate 3 (runtime behaviour), with docs/verify-2512.md §10 (build-time record) — target files: `tests/service-startup-audit.sh` (new), `tests/fixtures/procd-exported-helpers.txt` (new):
  - `tests/service-startup-audit.sh` — POSIX sh, shellcheck-clean, next to the existing `tests/link-audit.sh`, `tests/pkg-metadata-audit.sh` and the Phase-20 `tests/uci-defaults-audit.sh`. Two offline checks:
    (a) **procd API surface** — every `procd_*` function called by `saadway/files/saadway.init` must appear in the upstream exported set (`tests/fixtures/procd-exported-helpers.txt`). A call to a helper upstream does not export is exactly the defect that produced "procd_add_restart_trigger: not found" → the audit must FAIL on it and name the offending call and its line.
    (b) **no hardcoded ubus socket path** — no absolute ubus socket path (`/var/run/ubus.sock`, `/var/run/ubus/ubus.sock`) and no `SW_UBUS_SOCK` reference may remain in `saadway/src/*.c` / `saadway/src/*.h`: the daemon must let libubus resolve the socket (`ubus_connect(NULL)`), the only variant that survives a change of the system socket location.
  - The audit takes the init-script path and the daemon source dir as arguments (defaults: the shipped ones), so the same audit can be pointed at a pre-fix snapshot — the negative case must FAIL: against the pre-fix init script it must report `procd_add_restart_trigger` as not exported, and against the pre-fix sources it must report the hardcoded socket path. Capture the snapshots with `git show HEAD:saadway/files/saadway.init` and `git show HEAD:saadway/src/saadway.h` BEFORE the T069 change is committed (after the commit, HEAD no longer holds the broken versions).
  - `tests/fixtures/procd-exported-helpers.txt` — the upstream helper list, one name per line, with a header comment naming its source (openwrt/openwrt `package/system/procd/files/procd.sh`, the `_procd_wrapper` export block; the file is installed as /lib/functions/procd.sh). It is a dev-time fixture: it is NEVER installed on a device and never influences runtime behaviour.
  - Verification per docs/verify-2512.md: (i) `sh tests/service-startup-audit.sh` exits 0 against the shipped tree; (ii) the same audit pointed at the pre-fix snapshots exits 1 and NAMES the offending helper / socket literal; (iii) `shellcheck -s sh -S warning` clean.

- [X] T071: Trace the ubus-socket / procd-trigger fix into the quickstart and the on-device checklist (missing) per Gate 1 (platform) + Gate 4 (the ubus surface must answer for the tunnel activation checks) and FR-020/FR-022, with docs/verify-2512.md §10 (build-time record) — target files: `specs/001-saadway-vpn-dashboard/quickstart.md`, `docs/verify-2512.md`:
  - `quickstart.md` Scenario 1 — APPEND one numbered step (the existing steps keep their numbering and text): on a stock 25.12 device the daemon must come up — `logread | grep -i saadway` contains no "cannot connect ubus" line, `ubus list | grep saadway` shows the `saadway` ubus object, and starting the service through `/etc/init.d/saadway start` logs no "not found" from `/etc/rc.d/S99saadway` (the init script calls only upstream-exported procd helpers).
  - `docs/verify-2512.md` — §10: record the applied change (the socket auto-resolution, the deleted `SW_UBUS_SOCK`, the error-message change and the trigger correction) together with the upstream sources it was derived from (openwrt/ubus `CMakeLists.txt` → `UBUS_UNIX_SOCKET` and `libubus-io.c` → `ubus_reconnect`; openwrt/openwrt `package/system/procd/files/procd.sh` → the `_procd_wrapper` export list and `_procd_add_reload_trigger`), and add the on-device checks (no "cannot connect ubus", no "not found", `ubus list` shows the object) to the device checklist.
  - The authoritative on-device proof (the startup log, the `ubus list` object and `ubus call saadway mgmt.status`) is folded into the T046/T057 on-device round.

**Checkpoint**: the daemon resolves the ubus socket through libubus (no hardcoded socket path anywhere in the tree), the init script calls only upstream-exported procd helpers, the host-side audit proves both the fixed state and the two pre-fix failures, and the on-device startup proof is queued with T046/T057.

---

## Phase 22: Remediation Round 11 — Dashboard Default Credential (`admin` / `admin`)

**Purpose**: The dashboard's dedicated credential is today the system user
`saadway-admin` with **no** default password — the operator sets one over SSH
through `/usr/libexec/saadway/provision`. The operator asked for the dashboard
to ship with the **default credential `admin` / `admin`**.

**What changes — and the invariants that must survive the change:**
- The rpcd login section's username becomes `admin` and its shadow bridge
  becomes `'$p$admin'` (upstream resolves `$p$<name>` with
  `getspnam(<name>)` — the suffix MUST name the shadow row of the user
  `admin`).
- The default password `admin` is applied **only** as a crypt(3) hash in
  `/etc/shadow` (0600): never as plaintext in a UCI option, never in any
  world-readable file, never as a command argument (argv is readable through
  `/proc/<pid>/cmdline`), and never in the SPA's page source.
- The SPA pre-fills the **username only**. The password field MUST stay
  empty: the SPA is served to every LAN client, so a pre-filled password
  would publish the credential to anyone who can fetch `index.html`.
- The account stays nologin (`-s /bin/false`), UID 355, group `saadway`, and
  the ACL grant stays least-privilege (`list read saadway` / `list write
  saadway`) — the rename must NOT widen the grant (no `list read '*'`).
- The account is still created by the provisioning step — the change makes
  that step **non-interactive**, not optional: until it has run,
  `session.login` fails closed (rpcd: `getspnam("admin")` fails →
  PERMISSION_DENIED, never a fail-open).
- Devices that already ran the old script must be **migrated**: the login
  section is updated in place (not skipped because it already exists) and the
  stale `saadway-admin` shadow/passwd rows are removed.

**Upstream facts (fetched for this task — upstream is the oracle):**
- `openwrt/rpcd@master` → `session.c:832-858`: a login section whose
  `option password` starts with `$p$` is resolved with
  `getspnam(hash + 3)` and the candidate password is compared through
  `rpc_login_test_password(sp->sp_pwdp, password)`; a missing shadow entry
  fails the login (fail-closed). Hence the option MUST be `'$p$admin'` and
  the hash MUST live in `/etc/shadow` — a plaintext or a copied hash in UCI
  would violate FR-021 / Constitution IV.
- `openwrt/openwrt@master` → `package/utils/busybox/config/loginutils/Config.in`:
  OpenWrt's busybox carries the `chpasswd` and `cryptpw` applets next to
  `passwd` (`BUSYBOX_CONFIG_CHPASSWD`, `BUSYBOX_CONFIG_CRYPTPW`) → the
  password can be applied non-interactively with the plaintext passed over
  **stdin** (`chpasswd` reads `user:password` from stdin), never through
  argv. The provisioning script must still probe at runtime and fail closed
  with a clear message when none of `chpasswd` / `passwd` (stdin) /
  `cryptpw` is present.
- `spec.md` FR-020 ("MUST be protected by its own username/password") stays
  valid — `admin` is such a credential; FR-021 + Constitution IV are what
  constrain WHERE it may live (0600, never in a response or a log).
- `contracts/api.md` carries the username in its login example → it changes
  with the implementation.

**Recorded deviation (must be recorded, not silently shipped):** a well-known
default credential is weak. The compensating controls are: the uhttpd
listener is LAN-bound (Phase 20 / T066), the ACL grant is least-privilege,
the account is nologin, the credential never leaves `/etc/shadow`, and the
provisioning step prints a change-password warning. The deviation is
recorded in `docs/verify-2512.md` in the §7 "recorded implementation
deviation" style.

- [X] T072: Ship the dashboard with the default credential `admin` / `admin` (broken) per FR-020 (the dashboard MUST be protected by its own username/password) and FR-021 + Constitution IV (the secret lives ONLY in a 0600 file and never in a response, a log or a world-readable UCI option), with docs/verify-2512.md §2 (auth-bridge record) + the §7-style deviation entry — target files: `saadway/files/uci-defaults/50-saadway-rpcd`, `saadway/files/saadway-provision`, `saadway/files/htdocs/saadway/index.html`, `saadway/files/rpc-acl.json`:
  - `saadway/files/uci-defaults/50-saadway-rpcd` — the login mapping: `set rpcd.saadway_admin.username='admin'` and `set rpcd.saadway_admin.password='$p$admin'`. The section NAME stays `saadway_admin` (rpcd matches a login section by its `username` option, the section name is cosmetic — keeping it avoids colliding with the stock `login` sections in `/etc/config/rpcd`). The ACL lists stay `saadway` (read+write) — the rename must not widen the grant.
  - `saadway/files/uci-defaults/50-saadway-rpcd` — the script must become **idempotent AND migrating**: it currently creates the section only when `uci -q get rpcd.saadway_admin` fails, so an upgraded device would keep the OLD `username='saadway-admin'` / `'$p$saadway-admin'` mapping forever. Split it: create the section (with its `read`/`write` lists) only when it is missing; ALWAYS set `username` and `password` afterwards. `add_list` must stay inside the create branch — re-adding the lists on an existing section would duplicate the entries.
  - `saadway/files/uci-defaults/50-saadway-rpcd` — add `uci -q commit rpcd` after the batch: `uci batch` writes only the package delta under `/tmp/.uci` (tmpfs), so without the commit the login mapping (and the migration!) is silently lost on the next reboot. Same defect class as the uhttpd listener in Phase 20 / T066.
  - `saadway/files/saadway-provision` — `USER=admin` (the name MUST match the `$p$` suffix in the rpcd section — upstream `getspnam(hash + 3)`); the nologin shell, the UID 355 / GID 355 and the `saadway` group stay as they are.
  - `saadway/files/saadway-provision` — the `set` action becomes NON-INTERACTIVE: it creates the user (existing `do_add` unchanged) and then applies the default password over **stdin** — `printf '%s:%s\n' "$USER" "$DEFAULT_PASSWORD" | chpasswd` — with the fallbacks `passwd` fed from stdin and `cryptpw` + a staged `/etc/shadow` rewrite (the script already does staged `/etc/shadow` rewrites in `do_add`/`do_del`). The plaintext MUST never be an argument: `chpasswd`/`passwd`/`cryptpw` invoked with the password in argv would expose it through `/proc/<pid>/cmdline`. Probe the available applet at runtime and, if none of `chpasswd` / `passwd` / `cryptpw` exists, FAIL CLOSED with a clear message and a non-zero exit — never leave the account without a usable hash.
  - `saadway/files/saadway-provision` — after the password is set, print a prominent warning that the default credential is in use and how to change it (`/usr/libexec/saadway/provision passwd`), and keep the existing `restart_rpcd` call so rpcd picks the mapping up.
  - `saadway/files/saadway-provision` — add a `passwd` action that runs the interactive `passwd admin` (the operator's way to leave the default behind) and keep the existing `unset` action; BOTH must also remove the stale legacy `saadway-admin` rows from `/etc/passwd` and `/etc/shadow` (same staged-rewrite pattern) so the old credential cannot survive an upgrade.
  - `saadway/files/htdocs/saadway/index.html` — the login form's username input becomes `value="admin"`; the password input MUST NOT get a `value` attribute. The SPA is served to every LAN client, so a pre-filled password would publish the credential to anyone who can read the page source — the default password is typed by the operator, it is not shipped in the document.
  - `saadway/files/rpc-acl.json` — update the group description text to name the `admin` login (the ACL group name stays `saadway`; the grant stays least-privilege).
  - No new UI strings are required → the fa/en i18n catalogs stay in parity (113 = 113); if a hint string IS added, both catalogs must be updated together.
  - Verification per docs/verify-2512.md: (i) the host-side credential audit of T073 is green; (ii) `shellcheck -s sh` clean on both shipped scripts; (iii) strict greps — no `saadway-admin` reference may remain in the shipped tree, the rpcd option must be exactly `'$p$admin'`, and no plaintext password may appear in any UCI-writing file; (iv) the authoritative on-device proof — folded into the T046/T057 round: `session.login` with `admin`/`admin` succeeds on a provisioned device, `uci get rpcd.saadway_admin.password` prints `$p$admin` (never a hash or a plaintext), `/etc/shadow` carries a hash for `admin` and the file stays 0600, `grep -r admin /etc/config/rpcd` shows no credential material, and the SPA's page source contains no password.

- [X] T073 [P]: Ship a host-side audit for the dashboard credential (missing) per Constitution I (Native Platform Compliance — the credential must flow through the stock rpcd `$p$` bridge and the stock busybox applets) and FR-021 (the secret must never appear in a world-readable file or in the page source), with docs/verify-2512.md §10 (build-time record) — target files: `tests/dashboard-credential-audit.sh` (new), `tests/fixtures/50-saadway-rpcd.prefix` (new):
  - `tests/dashboard-credential-audit.sh` — POSIX sh, shellcheck-clean, next to the existing `tests/link-audit.sh`, `tests/pkg-metadata-audit.sh`, `tests/uci-defaults-audit.sh` and `tests/service-startup-audit.sh`. It takes the paths under audit as arguments (defaults: the shipped ones) so the same audit can be pointed at a pre-change snapshot. Checks:
    (a) **rpcd mapping** — `saadway/files/uci-defaults/50-saadway-rpcd` sets `username='admin'` and `password='$p$admin'` (the `$p$` reference exactly — never a hash, never a plaintext) and commits the package (`uci ... commit rpcd`, or the migration would be lost on the next reboot); the ACL lists stay `saadway` — a wildcard (`'*'`) must not appear in the login section.
    (b) **no plaintext credential** — no shipped file may contain a UCI password assignment whose value is not a `$p$` reference (`option password '...'`), and the default password literal must not appear in any UCI-writing file.
    (c) **SPA** — `saadway/files/htdocs/saadway/index.html`: the username input carries `value="admin"`, and the password input line carries NO `value=` attribute (the credential must not be published in the page source).
    (d) **provisioning** — `saadway/files/saadway-provision`: the password is piped to the setter (stdin) and never passed as an argument — no line may contain a password-setting applet (`chpasswd` / `passwd` / `cryptpw`) AND the password literal in the same command; the script must probe for the applets and fail closed when none is available.
    (e) **no legacy reference** — the string `saadway-admin` must not remain in the shipped tree (`saadway/`, `luci-app-saadway/`).
  - `tests/fixtures/50-saadway-rpcd.prefix` — a verbatim snapshot of the PRE-CHANGE login script, kept ONLY as the negative fixture (never installed on a device). Capture it with `git show HEAD:saadway/files/uci-defaults/50-saadway-rpcd` BEFORE the T072 change is committed (after the commit, HEAD no longer holds the old version). The audit MUST fail against it (checks (a), (b) and (e)) and pass against the shipped script — the proof that the audit has teeth, in the same spirit as the negative cases of `tests/pkg-metadata-audit.sh` and `tests/uci-defaults-audit.sh`.
  - The audit must NEVER execute `/usr/libexec/saadway/provision` on the build host: it touches `/etc/passwd` and `/etc/shadow` — the checks are static by design.
  - Verification per docs/verify-2512.md: (i) `sh tests/dashboard-credential-audit.sh` exits 0 against the shipped tree; (ii) the same audit pointed at the pre-change snapshot exits 1 and NAMES the failing check; (iii) `shellcheck -s sh -S warning` clean.

- [X] T074: Reconcile the contracts, the data model, the quickstart and the verify record with the new default credential (missing) per FR-020/FR-021 and Gate 6 (dual UI + session security), with docs/verify-2512.md §2 (auth-bridge record) + the §7-style recorded-deviation entry — target files: `specs/001-saadway-vpn-dashboard/contracts/api.md`, `specs/001-saadway-vpn-dashboard/contracts/uci.md`, `specs/001-saadway-vpn-dashboard/data-model.md`, `specs/001-saadway-vpn-dashboard/quickstart.md`, `docs/verify-2512.md`:
  - `contracts/api.md` — the `session.login` example carries `"username":"saadway-admin"`: it becomes `"admin"`, and the prose that names the `$p$saadway-admin` bridge becomes `$p$admin`. Add the default-credential statement and the change procedure, and keep the transport conventions (JSON-RPC over HTTPS/LAN, the 32-zero session id for login) untouched.
  - `contracts/uci.md` — the `dashboard` section's `option authentication 'dedicated'` comment names the `saadway-admin` bridge: it becomes the `admin` bridge; the paragraph that states where credential material lives must keep its invariant (the hash in `/etc/shadow`, never a UCI option) and name the new user.
  - `data-model.md` — the `DashboardSession` entity (the auth-material bullet, L139ff) and the storage-layout summary (`/etc/shadow (saadway-admin)` → `/etc/shadow (admin)`, `/etc/config/rpcd` → `saadway_admin` login section) must match the implementation.
  - `quickstart.md` — the first-login prose (before Scenario 1) must state the new enablement flow: `/usr/libexec/saadway/provision` creates the nologin user `admin` and applies the DEFAULT password non-interactively (the operator is prompted for nothing), the dashboard is then reachable with `admin` / `admin`, and `/usr/libexec/saadway/provision passwd` changes it (the interactive path) — with the explicit note that the credential material never lives in a world-readable UCI option.
  - `docs/verify-2512.md` — §2 (the auth-bridge record): the new username, the `$p$admin` bridge, the non-interactive default-password mechanism (stdin → `chpasswd` / `passwd` / `cryptpw` with the busybox applet evidence), the migration path (the login section is updated in place, the legacy `saadway-admin` rows are removed) and the upstream sources they were derived from (`openwrt/rpcd` `session.c` → `getspnam(hash + 3)` + `rpc_login_test_password`, `openwrt/openwrt` `package/utils/busybox/config/loginutils/Config.in` → `chpasswd`/`cryptpw`). Add a §7-style **recorded deviation**: a well-known default credential is weak — list the compensating controls (LAN-bound uhttpd listener per T066, least-privilege ACL, nologin account, hash-only storage, the change-password warning) and the operator action (`provision passwd`).
  - `spec.md` needs NO change: FR-020 names no username ("its own username/password") and stays valid for the `admin` credential.
  - The authoritative on-device proof (a successful `admin`/`admin` login, `$p$admin` in `/etc/config/rpcd`, a hash-only 0600 `/etc/shadow` row, no credential material in the page source) is folded into the T046/T057 on-device round.

**Checkpoint**: the dashboard ships with the `admin` / `admin` default credential, the credential exists only as a hash in the 0600 `/etc/shadow`, the SPA publishes no password, the account stays nologin and least-privilege, the host-side audit proves both the new state and the pre-change failure, and the weak default is recorded as a deviation with its compensating controls.

---

## Phase 23: Remediation Round 12 — Default Credentials & LuCI Credentials Tab

**Purpose**: Two defects/gaps from the 2026-10-21 amendment (spec FR-022
amended, FR-028 → FR-034, SC-009 → SC-011; plan.md "Amended Design";
research.md D10; constitution v1.1.0):
1. **The default credential is not reliably initialized** — the account is
   created only by a manual `/usr/libexec/saadway/provision` run over SSH,
   so a fresh installation rejects the documented default `admin`/`admin`
   with "نام کاربری یا رمز عبور اشتباه است" (wrong username or password).
   FR-028/SC-009 require the default to work out of the box with **0**
   manual steps, initialized idempotently and never overwriting a
   credential the administrator has already changed.
2. **The credential cannot be changed from the panel** — the standalone
   dashboard's login is only changeable through a shell step (FR-029 →
   FR-032). The LuCI page must gain a second, strictly bounded tab
   ("Dashboard Credentials" / تنظیمات ورود به داشبورد) whose Save & Apply
   makes the new credential effective immediately.

**Upstream facts (verified 2026-10-21 — upstream is the oracle):**
- `openwrt/openwrt` `package/system/rpcd/Makefile` → `BuildPlugin`: rpcd's
  plugins ship as **separate packages** (`rpcd-mod-file`, `rpcd-mod-rpcsys`
  — "ubus calls for sysupgrade and **password changing**", `rpcd-mod-iwinfo`,
  `rpcd-mod-ucode`); the stock `rpcd` binary contains none of them → the
  design adds **no new package dependency**.
- `openwrt/rpcd` `sys.c:81-141`: `rpc-sys password_set {user,password}`
  **forks `/bin/passwd <user>` and writes the password to the child's
  STDIN** (never argv; 100 ms read timeout) — the platform's `passwd` does
  the crypt(3) hashing into the 0600 `/etc/shadow`. Our helper follows the
  identical mechanism, so **no hashing code exists in our tree**.
- `openwrt/rpcd` `session.c:832-858`: `option password '$p$<name>'` is
  resolved with `getspnam(<name>)` and compared via
  `rpc_login_test_password()`; a missing shadow row fails the login
  (fail-closed). The suffix therefore **must equal the account name**.
- `openwrt/openwrt` `package/utils/busybox/config/loginutils/Config.in`:
  BusyBox carries `passwd`, `chpasswd` (reads `user:password` from stdin)
  and `cryptpw`.
- `openwrt/luci` `modules/luci-base/.../ui.js:4614-4700` (`ui.tabs`) and
  `.../form.js:988-1110` (`tab(name,title,description)` + `taboption(...)`;
  a `form.Map` renders the standard **"Save & Apply"** action).
- **Rejected by design** (plan.md "Architectural directives — evaluation"):
  a literal hash in `/etc/config/rpcd` (that file is world-readable → a
  world-readable credential, FR-021/Constitution IV — the option stays the
  `'$p$<user>'` reference); `openssl passwd -1` (not a stock package, MD5);
  a browser-side UCI write (no crypt(3) in the browser, non-atomic);
  rpcd's `file.exec` (re-opens the historical LuCI remote-command surface).

- [X] T075: Initialize the default dashboard credential at first boot so `admin`/`admin` works with zero manual steps (missing) per FR-028/SC-009 and Constitution IV (the secret lives ONLY in the 0600 `/etc/shadow` — never in a world-readable UCI option), with docs/verify-2512.md §2 (auth-bridge record) + §9/§10 — target files: `saadway/files/uci-defaults/50-saadway-rpcd`, `saadway/files/saadway-provision`:
  - `saadway/files/uci-defaults/50-saadway-rpcd` — maintain the login section under the name **`saadway_login`** (per the plan; rpcd matches a login section by its `username` option, so the section name is cosmetic): `option username 'admin'`, `option password '$p$admin'`, `list read 'saadway'`, `list write 'saadway'` — the ACL lists stay least-privilege. Keep the create-only-when-missing branch (the `read`/`write` lists belong to the creation — `add_list` appends and would duplicate them), ALWAYS refresh `username`/`password` afterwards (so an already-provisioned device is migrated), and keep `uci -q commit rpcd` (a batch alone writes only the `/tmp/.uci` delta — the mapping would be lost on the next reboot).
  - `saadway/files/uci-defaults/50-saadway-rpcd` — **migrate the legacy section**: if a login section named `saadway_admin` (the pre-T072 name) still exists, remove it (`uci -q delete rpcd.saadway_admin`) so only `saadway_login` remains — a stale section is a stale credential reference.
  - `saadway/files/saadway-provision` — the account itself must be created **automatically at first boot** (this is the FR-028 fix): the first-boot path creates the nologin account `admin` (uid 355, gid 355, group `saadway`, home `/var`, shell `/bin/false`) and applies the default password — **only when the account does not exist yet**, so re-running the first-boot script on an upgrade is a NO-OP for a credential the administrator has already changed (idempotence, FR-028).
  - `saadway/files/saadway-provision` — the password hash must come from the **platform's crypt(3) mechanism**, never from a literal we compute: drive `/bin/passwd` (or `chpasswd`, which reads `user:password` from stdin) with the plaintext on **stdin** — the plaintext MUST NOT appear in argv (`/proc/<pid>/cmdline` is world-readable) and MUST NOT be written to any file. Keep the fallback chain `chpasswd` → `passwd` (stdin) → `cryptpw` + a staged 0600 `/etc/shadow` rewrite, and **fail closed** (non-zero exit, clear message) when no setter exists — never leave the account without a usable hash.
  - The UCI option stays the **`'$p$admin'` reference**: the hash itself lives only in the 0600 `/etc/shadow` (upstream resolves the suffix with `getspnam()`); a literal hash in `/etc/config/rpcd` would be a world-readable credential and is forbidden (FR-021).
  - Verification per docs/verify-2512.md: (i) `tests/dashboard-credential-audit.sh` stays green; (ii) `tests/credential-flow-audit.sh` (T078) is green; (iii) `shellcheck -s sh -S warning` clean on both scripts; (iv) strict greps — no literal hash and no password literal in any UCI-writing file, the section is `saadway_login`, the legacy `saadway_admin` section is gone; (v) the authoritative on-device proof — folded into the T046/T057 round: on a fresh installation `session.login` with `admin`/`admin` succeeds with **0** manual steps, `/etc/shadow` holds the hash in a 0600 file, `/etc/config/rpcd` holds only the `'$p$admin'` reference, and re-running the first-boot script does NOT change an administrator-set password.

- [X] T076: Add the credential-apply path (ubus method + root-only helper) so a new credential takes effect immediately (missing) per FR-031/FR-032/FR-033/FR-034 and Constitution IV + V (the dashboard's own session must never be able to re-key its login), with docs/verify-2512.md §8 + §10 — target files: `saadway/src/creds.c` (new), `saadway/src/saadway.h`, `saadway/src/ubus.c`, `saadway/src/main.c`, `saadway/src/Makefile`, `saadway/files/saadway-provision`, `luci-app-saadway/root/usr/share/rpcd/acl.d/saadway-credentials.json` (new):
  - `saadway/src/creds.c` (new module) — the handler for the new ubus method: validate the input, then relay it to the root-only helper. Validation rules (FR-031, from the spec/data-model): the username MUST be non-empty, ≤ 32 characters and restricted to `[A-Za-z0-9._@-]`; the password MUST be non-empty and ≤ 128 characters; the confirmation is checked **in the UI** (the form), the method receives the final password only. Invalid input MUST be rejected with a 400-class ubus status and **no** router-side action.
  - `saadway/src/creds.c` — the relay: spawn `/usr/libexec/saadway/provision set-credentials <username>` with the password written to the child's **stdin** (never argv, never logged, never stored). The daemon holds the secret only for the duration of the call and **never stores it, never computes a hash** (the platform's `passwd` does both) — this refines research.md D7's "the daemon keeps no credential store of its own" to "the daemon never stores a credential; it relays a new secret exactly once, over a pipe, to the root-only helper". The call MUST be supervised with an explicit timeout (Constitution II: no unbounded wait) and its exit status mapped to the ubus reply (`{}` on success, an error status on failure) — the reply MUST NOT contain the secret (FR-033).
  - `saadway/src/ubus.c` + `saadway/src/saadway.h` — register the method **`mgmt.credentials.set`** on the `saadway` object with the contract from `contracts/api.md`: params `{ username: string, password: string }` → `{ applied }`; use the upstream-idiomatic `UBUS_METHOD_NOARG`/policy form consistent with the other 24 methods, and register a typed lookup for both parameters (no `blobmsg_add_*`/`blobmsg_find_name` — those do not exist upstream; use the local helpers in `saadway.h`).
  - `saadway/src/main.c` — wire the new module (include + init) exactly like the other subsystems: it must be safe to call before the tunnel subsystem is up (it touches no tunnel state).
  - `saadway/src/Makefile` — add `creds.c` to the module list (13 → 14 modules) and update `tests/link-audit.sh`'s module list accordingly (the audit must stay green and must know the new module).
  - `saadway/files/saadway-provision` — add the **`set-credentials`** mode: create the account if it is missing (nologin, uid 355, group `saadway`), set the new password from **stdin** (the platform's `passwd`/`chpasswd`/`cryptpw` chain of T075), then update `/etc/config/rpcd` section `saadway_login` → `option username '<new>'`, `option password '$p$<new>'` (the suffix MUST equal the account name — the new shadow account is created FIRST, so the mapping never points at a missing account), `list read/write 'saadway'` untouched, `uci commit rpcd`, and finally `/etc/init.d/rpcd reload`. **Ordering is fail-closed**: the password step happens BEFORE the mapping update, so a failure leaves the previous credential intact and usable; the helper exits non-zero on any failure and must not report success.
  - `luci-app-saadway/root/usr/share/rpcd/acl.d/saadway-credentials.json` (new) — a dedicated ACL group granting **exactly one** method: `ubus.saadway = ["mgmt.credentials.set"]`. This group MUST NOT be part of the dashboard's `saadway` group (in `saadway/files/rpc-acl.json`) — the standalone dashboard's session must be denied the call (FR-034); only the router's stock root session obtains it (group `*`). Keep the existing `luci-app-saadway.json` group unchanged (it grants only `mgmt.status`/`mgmt.set`).
  - Verification per docs/verify-2512.md: (i) `tests/link-audit.sh` green with 14 modules (no unresolved daemon-internal symbol); (ii) `gcc -fsyntax-only -Wall -Wextra` over all 14 modules against the dev stub pool, the unit suite still green; (iii) `tests/dashboard-credential-audit.sh` + `tests/credential-flow-audit.sh` green; (iv) `shellcheck -s sh -S warning` clean on the helper; (v) the on-device proof — folded into the T046/T057 round: a root session can call `saadway.mgmt.credentials.set` and the new credential is accepted by the standalone dashboard within 5 s (FR-032/SC-010), while the dashboard's own session is refused (ubus PERMISSION_DENIED).

- [X] T077: Refactor the LuCI view into a tabbed page with a dashboard-credentials form (missing) per FR-022 (amended), FR-029/FR-030/FR-031/FR-033 and constitution v1.1.0 (the single bounded exception), with docs/verify-2512.md §9 (install-asset audit) + §10 — target file: `luci-app-saadway/htdocs/luci-static/resources/view/saadway/dashboard.js`:
  - Convert the view into a `form.Map` with **exactly two tabs** (upstream LuCI: `tab(name, title, description)` registers a tab, `taboption(tabName, class, ...)` places an option in it — `form.js:988-1110`; the tab strip is `ui.tabs`, `ui.js:4614-4700`):
    - **Tab 1 — "Status & Control"**: the existing content unchanged in substance — the service status rows, the enable/disable switch (`saadway.mgmt.set`) and the "Open Dashboard" link (`http://<router>/saadway/`). No other controls.
    - **Tab 2 — "Dashboard Credentials"** (تنظیمات ورود به داشبورد): exactly three input fields — **New Username** (نام کاربری جدید), **New Password** (رمز عبور جدید), **Password Confirmation** (تکرار رمز عبور) — plus the standard **"Save & Apply"** action that `form.Map` renders.
  - The password fields MUST be masked (`password = true` on the `form.Value`) and MUST NOT be pre-filled; the username field MAY be pre-filled with the current login name (read-only, from the login mapping — at most the username is ever revealed, FR-033); the stored password MUST NEVER be requested, returned or displayed (FR-033).
  - The write path MUST be the single ubus call `saadway.mgmt.credentials.set { username, password }` (contract in `contracts/api.md`) — implemented as a `form.Value` subclass whose `write()` performs the call; the view MUST NOT write UCI directly (the browser cannot hash the password, and a half-applied change would lock the administrator out).
  - Client-side validation before anything is sent (FR-031): a non-empty username and password, and the confirmation MUST match the new password exactly — on a mismatch, show an inline error and send NOTHING; the router-side validation remains the authority.
  - The labels MUST use the LuCI translation mechanism (`_()`), with the Persian wording of FR-029/FR-030 as the intended phrasing; the view stays free of any configuration surface beyond the two tabs (constitution v1.1.0).
  - The status polling of Tab 1 keeps working (the form's render phase refreshes the status rows); the form MUST NOT introduce a `handleSaveApply` that writes UCI — only the credentials call.
  - Verification per docs/verify-2512.md: (i) the view parses as valid JavaScript and keeps the `mgmt.status`/`mgmt.set` calls plus the new `mgmt.credentials.set` call; (ii) the two tabs and the three fields are present with the exact labels; (iii) the password inputs carry no value attribute and are masked; (iv) the page ships no credential material (grep the shipped asset); (v) the on-device proof — folded into the T046/T057 round: the page shows the two tabs, a mismatch is rejected inline without a router-side call, a valid save makes the new credential work in the standalone dashboard immediately (FR-032/SC-010).

- [X] T078: Validate the end-to-end authentication flow on the LuCI page and the standalone dashboard (missing) per Gate 6 (dual UI + session security), FR-020/FR-021/FR-032/FR-033/FR-034 and SC-009/SC-010/SC-011, with docs/verify-2512.md §9 + §10 (build-time record) — target files: `tests/credential-flow-audit.sh` (new), `specs/001-saadway-vpn-dashboard/quickstart.md`, `docs/verify-2512.md`:
  - `tests/credential-flow-audit.sh` — a host-side, offline audit of the credential contract (POSIX sh, shellcheck-clean, next to the existing `tests/dashboard-credential-audit.sh`): (a) the login mapping is the `'$p$<user>'` **reference** — a literal hash in any UCI-writing file is a violation; (b) the password is piped to the setter — no line may contain a password-setting applet (`passwd`/`chpasswd`/`cryptpw`) AND the password literal; (c) the first-boot path initializes the default **only when the account is absent** (the "never overwrite an administrator's change" rule is checkable: the default must be guarded by an existence check); (d) the credential-change path creates the account **before** it updates the mapping (the fail-closed ordering); (e) the ACL group `saadway-credentials` grants **exactly one** method and the dashboard's `saadway` group does **not** contain `mgmt.credentials.set`; (f) the daemon source contains no credential store — no hash table, no secret file, no password in a log call. The audit must fail against the pre-T075/T076 state (a negative fixture: the pre-change `50-saadway-rpcd` and `saadway-provision`, captured with `git show HEAD:<path>` before the change is committed) and pass against the shipped tree.
  - `specs/001-saadway-vpn-dashboard/quickstart.md` — extend the on-device validation: Scenario 1 gains the credential checks (fresh install → `admin`/`admin` accepted with 0 manual steps; the LuCI tab changes the credential; the standalone dashboard accepts the new values and rejects the previous ones within 5 s; a mismatching confirmation is refused without a change) and Scenario 4 gains the authorization check (the dashboard's own session is refused by `saadway.mgmt.credentials.set`).
  - `docs/verify-2512.md` — §9: extend the install-asset audit with the new assets (`saadway/src/creds.c`, the new ACL file `saadway-credentials.json`, the new test script) and assert the credential-flow contract; §10: record the applied change with the upstream sources it was derived from (`openwrt/rpcd` `sys.c` (the `/bin/passwd`-over-stdin mechanism), `session.c` (the `$p$` bridge), `openwrt/openwrt` `package/utils/busybox/config/loginutils/Config.in` (`chpasswd`/`cryptpw`), `openwrt/luci` `ui.js`/`form.js` (the tab APIs)) and the on-device evidence.
  - The authoritative on-device evidence is folded into the T046/T057 round: (i) a fresh installation accepts `admin`/`admin` (SC-009); (ii) after a LuCI save, the new values are accepted and the old ones rejected within 5 s (SC-010); (iii) the stored secret appears **0** times in any page source, response body, log line or monitor panel (SC-011); (iv) `/etc/shadow` is 0600 and holds a hash; (v) `/etc/config/rpcd` holds no credential material; (vi) the dashboard session is refused by the credential method.

**Checkpoint**: the dashboard's default credential is initialized automatically and idempotently (a fresh install works with `admin`/`admin` and 0 manual steps), the credential is changeable from the LuCI page through one atomic, root-only, secret-safe path, the dashboard's own session can never re-key its login, and the host-side audit proves both the new contract and the pre-change failures.

## Phase 24: Remediation Round 13 — LuCI Rendering Fix & Standalone SPA Auth Bypass

**Purpose** (device report + explicit user directive, 2026-10-21): two defect
clusters.

1. **The LuCI page crashes and renders empty.**
   - `TypeError: view.extend is not a function` — the view file never declared
     `'require view';`, so the global `view` is undefined (`view` is a
     **built-in LuCI module**, not a global).
   - `RPCError: uci/get failed with code 6 (Permission denied)` — the view
     reads `rpcd.saadway_login.username` through LuCI's `uci` module, but the
     app's ACL group grants **no `uci` scope** at all.
   - "This section contains no values yet" — the form binds to the wrong UCI
     coordinates: `new form.Map('luci', …)` + `TypedSection 'system'`
     addresses the **luci** config's `system` sections, not the service's
     `saadway` config / `service` section (which holds the state the page
     shows).
2. **The standalone dashboard's login wall must be removed** (explicit user
   directive): opening `/saadway/` MUST show the dashboard with **no**
   credential prompt, and every ubus call it makes MUST succeed **without a
   session token** — the SPA becomes an unauthenticated (rpcd *default
   session*) client. The compensating control is the platform's network
   boundary (the LAN-bound uhttpd listener written by
   `saadway/files/uci-defaults/40-saadway-uhttpd` + the firewall's WAN input
   policy) — it MUST NOT be weakened by this change.

**Upstream facts (verified 2026-10-21 — upstream is the oracle):**
- `openwrt/luci` `modules/luci-base/htdocs/luci-static/resources/luci.js:1943-1951`
  (the `View` class) and `:2243` (the built-in module map `view: View`):
  `view` is a **built-in module** — a view file MUST start with
  `'require view';`; without it `view.extend` is `undefined` (the reported
  `TypeError`).
- `openwrt/luci` `modules/luci-base/.../form.js` (the `CBIMap` /
  `CBINamedSection` classes): a `form.Map` is bound to **one** UCI config
  (`new form.Map('<config>', …)`) and its options live in a section of that
  config — `form.Map('luci')` + `TypedSection 'system'` therefore renders the
  *luci* config's sections (none of which carry the service state) ⇒ the
  empty-section message.
- `openwrt/luci` `modules/luci-base/.../form.js` (`CBIMap.load`): the map
  performs `callSessionAccess('uci', <config>, 'write')` — without that grant
  the map is silently marked `readonly` (no error, but no writes).
- `openwrt/rpcd` `include/rpcd/session.h:35`: `RPC_DEFAULT_SESSION_ID` =
  `"00000000000000000000000000000000"` — the **all-zero session id is rpcd's
  default session**, not a magic login token.
- `openwrt/rpcd` `session.c:1396-1400`: rpcd creates that default session
  **once at startup** with `rpc_login_setup_acls(ses, NULL)` (a NULL login
  section) ⇒ an ACL change only reaches the default session after rpcd
  (re)starts — the first-boot script MUST reload rpcd.
- `openwrt/rpcd` `session.c:944-946` (`rpc_login_test_permission`): with a
  NULL login section, **only** the access group literally named
  `unauthenticated` is granted ⇒ an ACL file with a top-level
  `unauthenticated` group is what authorizes a tokenless caller.
- `openwrt/rpcd` `session.c:606-660` (`rpc_session_acl_allowed`) and `:1414`
  (`rpc_session_access`): the authorization check is
  (scope, object, function) with fnmatch matching; the ACL file's
  `read`/`write` blocks are only the *permission* buckets used for the
  login-section match — they do **not** narrow the ubus method set (a method
  listed in either block is granted).
- `openwrt/uhttpd` `ubus.c:820-840` (the JSON-RPC "call" branch) and
  `:280-297` (`uh_ubus_allowed`): uhttpd-mod-ubus forwards **every** `/ubus`
  call to rpcd's `session.access` with the request's `params[0]` as the
  session id ⇒ a call carrying the all-zero id is checked against the
  default session's ACLs. `conf.ubus_noauth` would skip the check for
  **every** object — **rejected** (far too broad).
- `openwrt/rpcd` `uci.c:324-349` (`rpc_uci_read_access` /
  `rpc_uci_write_access`): the uci scope is checked as
  `rpc_session_access(sid, "uci", <config>, "read"|"write")` — the
  "function" is literally `read`/`write`, **not** a section name ⇒ the ACL
  entry is `"uci": { "<config>": ["read", "write"] }`.
- `saadway/files/saadway.conf`: the service state lives in
  `config service 'service'` of the `saadway` config (plus
  `config dashboard 'dashboard'`).

**The dashboard's exhaustive ubus method set** (every `SwApi.saadway(...)`
call in `saadway/files/htdocs/saadway/js/app.js`, 21 methods):
`mgmt.status`, `profile.list`, `profile.import_openvpn`,
`profile.create_ikev2`, `profile.activate`, `profile.deactivate`,
`profile.delete`, `steer.list`, `steer.domain.add`, `steer.domain.remove`,
`steer.device.set`, `steer.device.remove`, `steer.iprange.set`, `sec.status`,
`sec.kill_switch.set`, `mon.stats`, `mon.logs`, `mon.health_test`,
`maint.backup.export`, `maint.backup.import`, `maint.cooldown.set`.

- [X] T079: LuCI dashboard rendering fix and complete standalone SPA auth bypass (broken) per FR-020 (amended), FR-022, FR-028 (amended), FR-032 (amended), FR-035 (new) + SC-012/SC-013 (new) and Constitution IV + V (v1.2.0 — the access control of the standalone dashboard is delegated to the platform's network boundary), with docs/verify-2512.md §7 (recorded deviation) — target files: `specs/001-saadway-vpn-dashboard/spec.md`, `.specify/memory/constitution.md`, `specs/001-saadway-vpn-dashboard/plan.md`, `specs/001-saadway-vpn-dashboard/research.md`, `luci-app-saadway/htdocs/luci-static/resources/view/saadway/dashboard.js`, `luci-app-saadway/root/usr/share/rpcd/acl.d/luci-app-saadway.json`, `saadway/files/rpc-acl.json`, `saadway/files/uci-defaults/50-saadway-rpcd`, `saadway/files/htdocs/saadway/js/api.js`, `saadway/files/htdocs/saadway/js/app.js`, `saadway/files/htdocs/saadway/index.html`, `saadway/files/htdocs/saadway/i18n/en.json`, `saadway/files/htdocs/saadway/i18n/fa.json`:
  - **Step 1 — governance first (the traceability precondition; the implementation below MUST trace to the amended artifacts).** Amend `specs/001-saadway-vpn-dashboard/spec.md`: rewrite **FR-020** ("MUST be protected by its own username/password") to state that the standalone dashboard MUST be reachable and fully operable **without any credential prompt**, that its access control is delegated to the platform's network boundary (the LAN-bound uhttpd listener of `40-saadway-uhttpd` + the firewall's WAN input policy), and that its ubus calls are authorized as rpcd's **default (unauthenticated) session**; amend **FR-028** (the default credential is no longer the dashboard's entry point — the dashboard needs 0 credentials; the optional `saadway_login` rpcd mapping and the `admin` account stay available for operators/scripts); amend **FR-032** (the new credential MUST be accepted by rpcd's `session.login` for the `saadway_login` mapping — the dashboard itself no longer logs in); add **FR-035** ("the dashboard's ubus calls MUST be authorized ONLY through the `unauthenticated` ACL group, which MUST grant exactly the dashboard's 21 methods — never `mgmt.set`, never `mgmt.credentials.set`, never a non-`saadway` object, and never the `saadway-credentials` group"); add **SC-012** ("opening `/saadway/` shows the dashboard with no login prompt and every panel action succeeds without a session token") and **SC-013** ("the unauthenticated session is granted exactly the 21 dashboard methods and is refused for `mgmt.set`, `mgmt.credentials.set` and every non-`saadway` object"). Amend the constitution to **v1.2.0**: Principle V's second bullet ("ALL of its system interactions MUST go exclusively through secure, authenticated APIs") MUST be rewritten to the delegated-boundary model ("…MUST go exclusively through the platform's ubus API, whose access control is the `unauthenticated` ACL group plus the platform's network boundary; the dashboard MUST NOT implement its own credential exchange and MUST NOT expose any secret"), with the rationale recording the LAN-trust model and the residual risk; the amendment MUST follow the constitution's own procedure (version bump + the SYNC IMPACT REPORT + the amendment log). **`.specify/` is git-ignored** — therefore mirror the amendment in the tracked artifacts: `specs/001-saadway-vpn-dashboard/plan.md` (the "Amended Design" section gains a "Standalone SPA auth bypass" subsection with the upstream facts listed above), `specs/001-saadway-vpn-dashboard/research.md` (a new decision **D11** — the credential lifecycle is retired from the dashboard's path; the rejected alternatives MUST be recorded: uhttpd's `ubus_noauth` (disables the ACL check for every object — far too broad), a browser-side credential exchange (re-opens the credential surface this round removed), granting the `unauthenticated` group the full daemon API instead of the 21 dashboard methods (violates FR-035)) and `docs/verify-2512.md` §7 (a recorded-deviation entry, in the style of the existing §7 entry, naming the withdrawn protection, the compensating controls and the residual risk).
  - **Step 2 — the LuCI view (all three reported defects).** `luci-app-saadway/htdocs/luci-static/resources/view/saadway/dashboard.js`: (a) add `'require view';` as the **first** require line — `view` is a built-in module (`luci.js:2243`), not a global, so `view.extend` is undefined without it; (b) bind the form to the **service's own coordinates**: `new form.Map('saadway', …)` with the section `m.section(form.NamedSection, 'service', 'service', _('Service'))` (the `service` section of `/etc/config/saadway` exists — `saadway/files/saadway.conf` → `config service 'service'`) instead of `form.Map('luci')` + `TypedSection 'system'`; the map MUST NOT write any UCI option (every option keeps its `write()` override) so Save & Apply cannot touch the daemon's state, and the map's "Revert" only discards the `saadway` delta (the daemon commits every write, so that delta is normally empty); (c) keep the two tabs and the three credential fields of T077 unchanged in substance, but re-label the tab's description to state that the credential it manages is the **optional rpcd session credential** (the dashboard itself no longer logs in) — the credential machinery (T075–T078) stays functional and root-only.
  - **Step 3 — the LuCI ACL (the `uci` scope).** `luci-app-saadway/root/usr/share/rpcd/acl.d/luci-app-saadway.json`: add the `uci` scope to the existing group, in the upstream shape (`uci.c:324-349` — the function is literally `read`/`write`, **not** a section name): `"uci": { "rpcd": ["read"], "saadway": ["read"] }` in the `read` block and `"uci": { "saadway": ["write"] }` in the `write` block. The `rpcd` read grant covers the username pre-fill (`uci.get('rpcd', 'saadway_login', 'username')` — the option holds only the `'$p$<user>'` **reference**, never a hash, FR-021); the `saadway` read/write grants cover the map's own `callSessionAccess('uci', 'saadway', 'write')` load check. The `ubus` scopes MUST stay as they are (`mgmt.status` read; `mgmt.status` + `mgmt.set` write) and the group MUST NOT gain `mgmt.credentials.set` (that stays in the root-only `saadway-credentials` group, FR-034).
  - **Step 4 — the ACL for the tokenless dashboard.** `saadway/files/rpc-acl.json`: add a top-level access group **literally named `unauthenticated`** (`session.c:944-946` — with a NULL login section only that group is granted) whose `ubus.saadway` lists are **exactly** the 21 dashboard methods listed above — no `mgmt.set`, no `mgmt.credentials.set`, no `sec.dns.status`, no `profile.update`, no other ubus object (FR-035). The existing `saadway` group (the authenticated operator path) MUST stay unchanged. The file's JSON MUST stay valid.
  - **Step 5 — the SPA transport (no session token).** `saadway/files/htdocs/saadway/js/api.js`: every ubus call MUST carry the **all-zero default session id** (`RPC_DEFAULT_SESSION_ID` = `"00000000000000000000000000000000"`) as `params[0]` — the constant already exists in the file and MUST be reused (renaming it to `DEFAULT_SESSION_ID` is allowed; keep the literal value). Remove the session lifecycle: the `login()`/`logout()`/`is_logged_in()` helpers and the `session_timeout` learning from `mgmt.status` MUST go (there is no session to expire), and the denial handler MUST NOT trigger a re-login flow — a denied call MUST surface as an ordinary error (the `stale`/re-login branch is removed). The token MUST NOT appear in a URL, a log line or the DOM (unchanged rule, FR-021).
  - **Step 6 — the SPA UI (no login wall).** `saadway/files/htdocs/saadway/js/app.js`: delete the login gate — `show_login()`, `on_login()`, `on_logout()` and the `SwApp.on_session_lost` hook — and make `boot()` show the application shell directly (the status poll starts as before; no status probe is needed to decide whether to log in). `saadway/files/htdocs/saadway/index.html`: remove the `#view-login` section and the `#btn-logout` button, and update the header comment (the "authenticated ubus session" note MUST be replaced by the delegated-boundary rule). `saadway/files/htdocs/saadway/i18n/en.json` + `i18n/fa.json`: remove the retired keys (`login_title`, `login`, `logout`, `login_failed`, `session_expired`) — **both** files MUST change together so the translation parity stays 1:1.
  - **Step 7 — the first-boot rpcd wiring.** `saadway/files/uci-defaults/50-saadway-rpcd`: keep the `saadway_login` mapping (the optional credential path) and the legacy-section cleanup, but (a) **drop the `provision init` call** — with the login wall gone there is no consumer for a well-known default password, and shipping `admin`/`admin` on every device would be a liability, not a feature (the account is created on demand by the LuCI credentials tab, which calls the helper's `set-credentials`); (b) make the credential mapping refresh **conditional on the account existing** (e.g. only refresh `username`/`password` when the `admin` row is present in `/etc/passwd`), so an operator-set credential is never clobbered by an upgrade; (c) keep `uci -q commit rpcd` and keep the **`/etc/init.d/rpcd` reload** — it is now mandatory, not cosmetic: rpcd grants the default session's ACLs **once at startup** (`session.c:1396-1400`), so an already-running rpcd would keep the old ACL set (and the dashboard would stay permission-denied) until the next reboot.
  - **Verification (host-side, this task):** the LuCI view parses as valid JavaScript (`node --check`) and declares `'require view';`; the map is bound to `saadway`/`service`; the SPA ships no login markup and no `session.login` call (grep `saadway/files/htdocs/saadway/`), every `/ubus` call uses the all-zero session id, and the i18n files stay 1:1; `saadway/files/rpc-acl.json` and both ACL files are valid JSON and the `unauthenticated` group grants exactly the 21 methods; the two shell scripts stay shellcheck-clean; the existing audits stay green except for the rules T080 is told to invert. **On-device proof** (folded into T046/T057): the LuCI page renders both tabs with no JavaScript error in the browser console, the "Open Dashboard" link opens `/saadway/` straight into the dashboard, every panel action succeeds without a token, `ubus call saadway mgmt.set` from an unauthenticated shell is **refused**, and the dashboard is unreachable from the WAN side.

- [X] T080 [P]: Ship the host-side regression audit for the rendering fix and the auth bypass (missing) per Constitution I (Native Platform Compliance — the fixes must hold against the stock OpenWrt 25.12 rpcd/uhttpd/LuCI semantics) and FR-035 + SC-012/SC-013, with docs/verify-2512.md §9 (install-asset audit) + §10 (build-time record) — target files: `tests/luci-spa-audit.sh` (new), `tests/credential-flow-audit.sh`, `tests/dashboard-credential-audit.sh`:
  - `tests/luci-spa-audit.sh` (new, POSIX sh, shellcheck-clean, next to the existing host audits, with the same `[tree-root]` argument convention and the same positive/negative-run pattern): (a) the LuCI view declares `'require view';` **before** any use of `view.` and binds the map to the `saadway` config with the `service` section; (b) the app's ACL file grants the `uci` scope with the upstream shape (the functions are literally `read`/`write`, never a section name) and still grants only `mgmt.status`/`mgmt.set` on the `ubus` scope; (c) the SPA ships **no** login markup (`login-form`, `login-user`, `login-pass`), **no** `session.login` call and no session-token storage, while every `/ubus` request carries the all-zero session id; (d) `saadway/files/rpc-acl.json` contains an access group named exactly `unauthenticated` whose ubus method set equals the dashboard's 21-method inventory, and contains **neither** `mgmt.set` **nor** `mgmt.credentials.set` (FR-035); (e) the first-boot script MUST NOT provision a well-known default password (no `provision init` call) and MUST reload rpcd (the default session's ACLs are granted once at startup). The audit MUST fail against the pre-T079 state (a negative fixture captured with `git show HEAD:<path>` before the change is committed) and pass against the shipped tree.
  - `tests/credential-flow-audit.sh` — invert the rules that the bypass makes obsolete: check C ("the default credential is initialized only when the account is absent") MUST become "the first-boot path MUST NOT provision a well-known default password" (the `init` mode MAY exist in the helper but MUST NOT be called by the first-boot script), and the check that the first-boot script initializes the default credential MUST be replaced by the check that the script still refreshes the mapping conditionally and still commits/reloads rpcd. Every other check (the `$p$` reference, the stdin piping, the fail-closed ordering, the ACL scoping, the absence of a credential store in the daemon) MUST stay unchanged.
  - `tests/dashboard-credential-audit.sh` — keep every rule that still holds (the `$p$` reference, no plaintext credential material in the shipped tree, the SPA page source carries no password, the password is never an argument, the legacy login is retired) and adjust only the rules that assumed the SPA logs in.
  - Verification: all three audits green, the negative run of the new audit fails with the expected findings, `shellcheck -s sh -S warning` clean on all three, and the existing validation stack (the link audit, the package-metadata audit, the uci-defaults audit, the service-startup audit, the unit suite) unaffected.

- [X] T081: Reconcile the contracts, the data model, the quickstart and the verify record with the auth bypass (missing) per FR-020/FR-021/FR-035 + SC-012/SC-013 and Gate 6 (dual UI + session security), with docs/verify-2512.md §2 (auth-bridge record) + §9 (install-asset audit) + §10 (build-time record) — target files: `specs/001-saadway-vpn-dashboard/contracts/api.md`, `specs/001-saadway-vpn-dashboard/contracts/uci.md`, `specs/001-saadway-vpn-dashboard/data-model.md`, `specs/001-saadway-vpn-dashboard/quickstart.md`, `docs/verify-2512.md`:
  - `specs/001-saadway-vpn-dashboard/contracts/api.md` — the transport section MUST describe the tokenless client (the JSON-RPC envelope's `params[0]` is rpcd's all-zero default session id; there is no `session.login`, no `ubus_rpc_session` and no session TTL), the non-goals MUST gain "no authentication on the dashboard's own surface — the access control is the LAN-bound listener plus the `unauthenticated` ACL group", and the method table MUST mark `mgmt.credentials.set` as **not reachable** from the dashboard (root-only group).
  - `specs/001-saadway-vpn-dashboard/contracts/uci.md` — the `/etc/config/rpcd` `saadway_login` mapping block MUST be documented as the **optional** operator credential (created on demand, never provisioned with a default password at first boot) and the `unauthenticated` ACL group MUST be documented as the dashboard's authorization path.
  - `specs/001-saadway-vpn-dashboard/data-model.md` — the `DashboardCredential` entity MUST be marked optional/operator-scoped (it is no longer the dashboard's entry point) and the method inventory of the dashboard client MUST be listed.
  - `specs/001-saadway-vpn-dashboard/quickstart.md` — Scenario 4 (dual UI + session security) MUST gain the bypass checks: `/saadway/` opens straight into the dashboard (no login prompt, no token in the DOM or the network log), every panel action succeeds, the unauthenticated session is refused for `mgmt.set`/`mgmt.credentials.set` and for every non-`saadway` ubus object, the dashboard is unreachable from the WAN side, and the LuCI page renders both tabs with an empty browser console. The credential checks of Scenario 1/Scenario 4 (the `admin`/`admin` default and the LuCI save) MUST be rewritten to the optional-credential semantics.
  - `docs/verify-2512.md` — §2 (the auth-bridge record) MUST be rewritten for the tokenless path; §7 MUST carry the recorded deviation (the withdrawn protection, the compensating controls: the LAN-bound listener + the WAN input policy, and the residual risk: any host that can reach the LAN listener can drive the dashboard's 21 methods); §9 MUST extend the install-asset audit with the changed assets (the view, the two ACL files, the SPA transport, the i18n files) and assert the FR-035 method set; §10 MUST record the applied change with the upstream sources it was derived from (`openwrt/luci` `luci.js` (the built-in `view` module) and `form.js` (`CBIMap`/`CBINamedSection`/`CBIMap.load`), `openwrt/rpcd` `include/rpcd/session.h` (`RPC_DEFAULT_SESSION_ID`), `session.c:944-946` (the `unauthenticated` group), `session.c:1396-1400` (the default session's startup-time grants), `uci.c:324-349` (the uci scope's `read`/`write` functions) and `openwrt/uhttpd` `ubus.c:280-297`/`820-840` (the `/ubus` → `session.access` path)) and the on-device evidence.
  - The authoritative on-device evidence is folded into the T046/T057 round: (i) the LuCI page renders both tabs with an empty console (SC-012); (ii) the dashboard opens and operates with no credential prompt and no session token (SC-012); (iii) the unauthenticated session is granted exactly the 21 dashboard methods and is refused for `mgmt.set`, `mgmt.credentials.set` and every non-`saadway` object (SC-013); (iv) the dashboard is unreachable from the WAN side; (v) no secret material appears in any response body, page source or log line.

**As implemented (T079)** — two deviations from the task text above, both
recorded in `docs/verify-2512.md` §7 and in the amended spec:
1. **The `unauthenticated` group grants the whole `saadway` object** (`"*"`),
   not the enumerated 21-method list — the maintainer's implementation
   instruction of 2026-10-21 asks for the wildcard, and with it the group
   is internally consistent (the public and the authenticated surfaces carry
   the same power set, so there is no incremental escalation). Consequence:
   `mgmt.credentials.set` is LAN-reachable too, so FR-034 was amended and
   the residual risk is recorded in §7.
2. **The credential tab's username pre-fill was dropped** — reading the
   `/etc/config/rpcd` mapping needs a `uci` scope on the `rpcd` config,
   which the panel's ACL group does not hold (it holds `saadway` only), and
   a failed read raises exactly the `uci/get … Permission denied` error this
   round fixes. FR-033 permits the field to stay empty ("MAY be
   pre-filled").

Two host audits are RED on purpose after this task — precisely the two rules
T080 inverts: `tests/dashboard-credential-audit.sh` (the SPA's login-markup
rules) and `tests/credential-flow-audit.sh` ("the first-boot script
initializes the default credential"). `tests/leak-check.sh` needs
`WTN_WAN_DEV` (an on-device check; unchanged).

**Checkpoint**: the LuCI page renders both tabs against the service's own UCI coordinates (no JavaScript error, no empty section, no uci permission error), the standalone dashboard opens and operates with **no** credential prompt and **no** session token, its authorization is exactly the 21-method `unauthenticated` group (the credential method and the service toggle stay out of reach), the first-boot path no longer ships a well-known password, the host-side audits prove the new state and the pre-change failures, and the withdrawn protection plus its compensating controls and residual risk are recorded as a deviation.

## Phase 25: Convergence — spec/plan/code gap closure (2026-10-21)

**Purpose**: the convergence assessment of 2026-10-21 compared the shipped
code against `spec.md` (35 FRs, 13 SCs, 6 user stories), `plan.md` (the
project structure, the module list, the package touch-points, the transport
contract, the test strategy) and the constitution (v1.2.0 — principles I–VI
and the seven quality gates). It found **no** missing and **no** contradicting
work: 8 partial gaps and 1 unrequested artifact remained. The on-device
items are already tracked as **T046** and **T057** (device-bound) and are
NOT re-appended here.

- [X] T082: Extend the standalone dashboard's IKEv2 form with the FR-004 connection settings and the FR-005 encryption readout per FR-004, FR-005 (partial) — target file: `saadway/files/htdocs/saadway/js/app.js`:
  - The IKEv2 card currently renders **only** `name`, `hostname`, `username`
    and `password` (`js/app.js:151-161`). FR-004 requires the form to
    provide the connection settings with the stated defaults — **Type** =
    IKEv2 · username auth (MSCHAPv2); **Remote ID** = "Same as hostname";
    **Local ID** = "Same as username"; **Certificate** = "Leave default ·
    from server"; **Proxy** = Off — and each of them MUST be overridable
    with the "Same as …" options preserved.
  - The daemon already applies those defaults and the API already accepts
    the overrides (`contracts/api.md`: `profile.create_ikev2 { name,
    hostname, username, password, remote_id?, local_id?, cert_mode?,
    proposal_*? }`; the defaults live in `saadway/src/uci-state.c:629-632`
    as `same-as-hostname` / `same-as-username` / `from-server` / `off`) —
    the gap is **UI-only**: add the four fields (selects whose first entry
    is the "Same as …" value, plus the certificate and proxy modes) and
    send them in the `profile.create_ikev2` payload.
  - FR-005 additionally requires the form to **display** the automatically
    negotiated encryption — Cipher: AES-256-GCM; Integrity: SHA-256; Key
    exchange: DH group 20 (ECP384); iOS key exchange: DH group 21 (ECP521)
    (the daemon emits `aes256gcm16-prfsha256-ecp384-ecp521`,
    `saadway/src/tunnel-ikev2.c:5-6`) — add it as a static, translated
    readout next to the form.
  - The new labels MUST be added to **both** locales
    (`saadway/files/htdocs/saadway/i18n/en.json` +
    `i18n/fa.json`) so the parity stays 1:1, and the daemon's accepted
    values MUST match the form's option values exactly (`same-as-hostname`,
    `same-as-username`, `from-server`, `off`) — the daemon ignores unknown
    values and falls back to its defaults.
  - Verification: the SPA parses (`node --check`), the i18n locales stay
    1:1, `tests/luci-spa-audit.sh` stays green, and the created profile's
    `remote_id`/`local_id`/`cert_mode`/`proxy` are observable in
    `/etc/config/saadway` (on-device proof folded into T046/T057).

- [X] T083: Fix the undefined behaviour in the WAN-interface enumeration per FR-011, FR-014 (partial) — target file: `saadway/src/policy.c`:
  - `saadway/src/policy.c:163` reads and modifies `nwans` in one expression
    without a sequence point — `wans[nwans++] = wan_names[nwans];` — which
    the compiler flags as `-Wsequence-point` ("operation on 'nwans' may be
    undefined"). The right-hand read MAY observe the incremented index, so
    a **wild pointer** can enter the WAN-interface list that the nftables
    renderer later dereferences (a daemon crash in the policy path — not a
    fail-closed outcome, Constitution IV).
  - Sequence the two operations explicitly (assign first, increment after)
    and keep/extend the array bound check so the enumeration cannot write
    past `wan_names`/`wans`.
  - Verification: `gcc -fsyntax-only -std=c11 -Wall -Wextra` on
    `saadway/src/policy.c` is clean of the sequence-point warning, the
    unit suite stays green, and `tests/link-audit.sh` resolves the module.

- [X] T084: Remove the undefined behaviour from the profile-name normalization helper per FR-006 (partial) — target file: `saadway/src/uci-state.c`:
  - `saadway/src/uci-state.c:376` — `dst[o++] = (char)(o ? c : 'a');` —
    reads and modifies `o` without a sequence point (`-Wsequence-point`).
    The helper `sw_util_normalize_name()` currently has **no caller** (it
    is dead code), so the defect is latent; either sequence the operations
    correctly (read `o` into a local before the increment) or delete the
    dead helper and its declaration in `saadway/src/saadway.h` — the
    decision MUST be recorded in the commit message.
  - Verification: the module compiles clean under `-Wall -Wextra`, the
    unit suite stays green, and `tests/link-audit.sh` still resolves every
    daemon-internal symbol.

- [X] T085: Give the OpenVPN import form its own titled credential section per FR-002 (partial) — target files: `saadway/files/htdocs/saadway/js/app.js`, `saadway/files/htdocs/saadway/i18n/en.json`, `saadway/files/htdocs/saadway/i18n/fa.json`:
  - FR-002 requires a section titled *"Section to add an optional
    'auth-user-pass' file with your credentials"* where the username and
    password may optionally be entered. The optional fields ARE present
    and functional (`imp-user` / `imp-pass` → `credentials: { username,
    password }` per `contracts/api.md`), but they sit inline in the import
    card with no section of their own.
  - Split the credential block into its own titled sub-section inside the
    import card, with the title as a **new** i18n key in both locales
    (the parity must stay 1:1). The section MUST remain optional: an empty
    username must omit `credentials` from the payload entirely (the current
    behaviour) — never send an empty credential pair.
  - Verification: the SPA parses, the locales are 1:1, and the import of a
    profile with and without credentials still behaves as before (the
    secret lands in the 0600 `auth-user-pass` file, never in UCI).

- [X] T086: Clear the compiler warnings in the daemon modules per Constitution I (partial) — target files: `saadway/src/ubus.c`, `saadway/src/tunnel-openvpn.c`, `saadway/src/counters.c`:
  - Five unused variables remain: `saadway/src/ubus.c:1049` (`lv`, `cv` in
    `h_mon_logs`), `saadway/src/tunnel-openvpn.c:156` (`rst`) and `:204`
    (`ret`), `saadway/src/counters.c:21` (`v`). Remove them (or use them
    where the value was clearly meant to be used — e.g. the log cursor
    variables in `h_mon_logs`).
  - The whole daemon MUST compile clean under
    `gcc -fsyntax-only -std=c11 -Wall -Wextra` — all **14** modules, not
    just the touched ones. This is the check that would have caught the two
    sequence-point defects (T083/T084).
  - Verification: the sweep over the 14 modules reports **0** warnings, the
    unit suite stays green, and `tests/link-audit.sh` stays green.

- [X] T087: Make the dashboard's language preference single-sourced per FR-027 (partial) — target files: `saadway/src/uci-state.c`, `saadway/files/htdocs/saadway/js/i18n.js`:
  - The SPA keeps the language in the browser's `localStorage`
    (`js/i18n.js:14`, key `saadway.lang`) while the daemon reads and
    re-persists a `dashboard.language` UCI option
    (`saadway/src/uci-state.c:849` reads it, `:1255` writes it back) that
    **no surface can change** — it is a constant that can silently drift
    from the browser's preference (and it is part of the backup schema,
    `saadway/src/backup.c:284`).
  - Pick ONE authoritative surface and make the other follow. The
    LAN-scoped dashboard has no UCI write access (the `unauthenticated`
    ACL group carries no `uci` scope — FR-035), so the browser's
    `localStorage` MUST remain the dashboard's own preference and the UCI
    option MUST stop being presented as a setting: remove the
    `dashboard.language` read/write round-trip in
    `saadway/src/uci-state.c` (and its entry in the backup schema) or
    document it explicitly as the **daemon-side default** that the SPA
    falls back to. The decision MUST be recorded in
    `specs/001-saadway-vpn-dashboard/contracts/uci.md`.
  - Verification: the daemon compiles clean, the unit suite stays green,
    the backup round-trip (`tests/restore-roundtrip.sh`) stays green, and
    the SPA still restores the browser's language preference across a
    reload.

- [X] T088: Reconcile the root-only credential ACL group with the post-bypass reality per FR-034 (unrequested) — target files: `luci-app-saadway/root/usr/share/rpcd/acl.d/saadway-credentials.json`, `tests/credential-flow-audit.sh`:
  - The group still describes itself as the **root-only** gate for
    `saadway.mgmt.credentials.set`, but since the auth bypass the
    LAN-scoped `unauthenticated` group covers the whole `saadway` object —
    the method is reachable from the LAN regardless of this group, so the
    description overstates the isolation (the withdrawal is recorded in
    `docs/verify-2512.md` §7).
  - Keep the group (it is the narrower grant and stays the correct gate
    for a future tightening of the wildcard) but **re-document** it: it
    MUST NOT claim exclusivity it no longer has. Alternatively delete the
    file and its install rule in `luci-app-saadway/Makefile` — the
    decision MUST be recorded.
  - `tests/credential-flow-audit.sh` check E currently asserts that the
    dashboard's `saadway` group does **not** contain
    `mgmt.credentials.set` — after the wildcard that assertion is
    **vacuously true** (the literal method name never appears; the group
    holds `*`). Make check E assert something real: that the
    `saadway-credentials` group grants exactly one method and that the
    audit's verdict on the `saadway` group's coverage is stated explicitly
    (the wildcard covers the method — recorded as a deviation, not hidden).
  - Verification: the audit stays green and its negative run (the pre-T079
    tree) still fails, and the ACL file remains valid JSON.

- [X] T089: Align the plan's project-structure tree with the shipped tree per plan: project structure (partial) — target file: `specs/001-saadway-vpn-dashboard/plan.md`:
  - The structure tree names `tests/smoke-switching.sh` — a script that
    does not exist (the on-device switching/leak coverage is
    `tests/smoke-container.sh` + `tests/leak-check.sh`) — and omits the
    four audits added since the tree was written:
    `tests/luci-spa-audit.sh`, `tests/pkg-metadata-audit.sh`,
    `tests/service-startup-audit.sh`, `tests/uci-defaults-audit.sh`.
  - Update **only** the structure tree (the plan's decisions and the
    amended-design sections stay untouched); the tree MUST list the files
    that actually ship.
  - Verification: every path named in the tree exists in the repository
    (a `find`-based existence check over the tree's file names).

- [X] T090: Drop the retired session-error string from the dashboard locales per FR-020 (partial) — target files: `saadway/files/htdocs/saadway/i18n/en.json`, `saadway/files/htdocs/saadway/i18n/fa.json`:
  - `err_http_401` ("Invalid session — log in again" / "نشست نامعتبر — دوباره
    وارد شوید") is unreferenced since the dashboard became tokenless
    (T079) — there is no session to invalidate.
  - Remove the key from **both** locales (the parity must stay 1:1) and
    grep the shipped SPA for any remaining reference to it.
  - Verification: the SPA parses, the locales are 1:1, and no shipped
    JavaScript references the removed key.

**As implemented (T082–T090)** — two decisions worth recording:
1. **T084 kept the helper** (it fixed the sequencing instead of deleting the
   dead `sw_util_normalize_name()`); the daemon now compiles with **0**
   warnings under `-Wall -Wextra` across all 14 modules — the check that
   would have caught both sequence-point defects.
2. **T087 made the daemon-side `dashboard.language` the DEFAULT**, not the
   authoritative copy: the browser's stored choice still wins, the daemon
   exposes its value as `mgmt.status → language`, and the SPA applies it
   before the first render. The decision is recorded in
   `specs/001-saadway-vpn-dashboard/contracts/uci.md` ✓.

**Checkpoint**: the shipped code satisfies every requirement of the amended
specification except the nine items above; the two sequence-point defects
are gone and the daemon compiles clean under `-Wall -Wextra`, the IKEv2
form offers the FR-004/FR-005 surface the API already supports, the
access-control artifacts describe the post-bypass reality, and the plan's
structure tree matches the tree that ships.

## Phase 26: Convergence — the DNS-status panel and the profile settings form (2026-10-21)

**Purpose**: the second convergence pass (after Phase 25) re-assessed the
shipped code against the amended `spec.md` (35 FRs, 13 SCs, 6 user stories),
`plan.md` and the constitution v1.2.0. It confirmed the Phase 25 fixes and
found **two** functional gaps and **one** documentation contradiction. The
on-device items remain tracked as **T046** and **T057** (device-bound) and
are NOT re-appended.

- [X] T091: Render the real DNS-leak status in the dashboard's security panel per FR-013, US2 (contradicts) — target files: `saadway/files/htdocs/saadway/js/app.js`, `specs/001-saadway-vpn-dashboard/contracts/api.md`:
  - `js/app.js:376` — `tab_security()` calls **only** `saadway.mgmt.status`
    → `sec.status` and hands its payload to `dns_status_rows()`, which reads
    `dns_forced`, **`resolver_if`** and **`leak_risk`**. `sec.status`
    returns `{ kill_switch, dns_forced, contained }`; `resolver_if` and
    `leak_risk` are returned by the **separate** method `sec.dns.status`
    (`{ resolver_if, leak_risk }` — `contracts/api.md`). The two fields are
    therefore always `undefined` and the panel renders "—" for the resolver
    and "unknown" for the leak risk: the zero-leak indication US2/FR-013
    asks for can never be shown.
  - Fetch the DNS status from its own method — e.g. call
    `SwApi.saadway("sec.dns.status")` in the same panel and pass **that**
    payload to `dns_status_rows()` (keeping `dns_forced` from `sec.status`),
    or merge the two payloads before rendering. The panel MUST then show the
    real resolver interface and the real leak-risk verdict.
  - The panel MUST keep working when `sec.dns.status` is unavailable
    (e.g. the DNS subsystem is not up): render the "unknown" fallback
    instead of throwing.
  - `contracts/api.md` — the method table MUST state where each DNS field
    comes from (`sec.status` vs `sec.dns.status`) so the two cannot be
    confused again.
  - Verification: the panel's DNS section renders the resolver interface and
    the leak-risk verdict returned by the daemon (on-device proof folded
    into T046/T057); the SPA parses (`node --check`); the i18n locales stay
    1:1.

- [X] T092: Add the OpenVPN profile's own settings form to the dashboard per US1/AC2, FR-002, FR-023 (missing) — target files: `saadway/files/htdocs/saadway/js/app.js`, `saadway/files/htdocs/saadway/i18n/en.json`, `saadway/files/htdocs/saadway/i18n/fa.json`:
  - US1/AC2: *"Given an OpenVPN profile requiring credentials, **When** the
    administrator opens **its settings form**, Then a section titled 'Section
    to add an optional "auth-user-pass" file with your credentials' is
    present where they can optionally enter a username and password attached
    to the profile."* The dashboard has no per-profile form: the credential
    fields exist only in the **import** card (Phase 25 / T085), so an
    existing profile's settings — and its credentials — cannot be edited.
    The API is complete for this: `profile.update { name, patch }` patches
    `enabled`, `hostname`, `username`, `password`, `remote_id`, `local_id`,
    `cert_mode` and the proposals (`saadway/src/ubus.c:465`), and the
    contract documents it ("patch fields only; secrets replaced not
    echoed") — the SPA calls it **0** times.
  - Add a per-profile settings form to the profiles panel: an action that
    opens the form for the chosen profile (the profile list row gains an
    "edit" action next to activate/delete), carrying the FR-002 section
    ("Section to add an optional 'auth-user-pass' file with your
    credentials") with the optional username/password, plus the profile's
    editable fields (`enabled`, and the OpenVPN hostname/username) and — for
    an IKEv2 profile — the FR-004 connection settings.
  - The form MUST submit ONE `profile.update` call with the changed fields
    only (an unchanged secret MUST NOT be sent again), MUST NOT echo the
    stored secret back into the form (FR-033), and MUST report the daemon's
    rejection codes (`E_BAD_SYNTAX`, `E_DUPLICATE_NAME`, …) inline.
  - The new labels MUST be added to **both** locales (the parity stays 1:1)
    and the form MUST reuse the existing FR-002 title key where it applies
    (the import card and the settings form share the wording).
  - Verification: the SPA parses; the locales are 1:1; editing a profile's
    credentials/enabled state is observable in `/etc/config/saadway` and in
    the rendered profile (on-device proof folded into T046/T057);
    `tests/luci-spa-audit.sh` stays green.

- [X] T093: Supersede the obsolete authentication answers in the specification's clarification sessions per FR-020, FR-028 (partial) — target file: `specs/001-saadway-vpn-dashboard/spec.md`:
  - The clarification session of 2026-10-21 still answers: *"How is the
    default credential made available on a fresh installation …? → A: The
    default credential is initialized automatically during installation/first
    start …"* and *"Do dashboard sessions established before a credential
    change keep working? → A: Yes — they remain valid until they expire …"*.
    Both were superseded by the auth bypass of the same date: the
    installation now ships **no** default password (FR-028 amended) and the
    dashboard has **no** sessions at all (FR-020 amended — the tokenless
    client). The FRs were amended in Phase 24; the clarification record was
    not, so the specification now contradicts itself.
  - Append a **superseding clarification session** (or mark the two obsolete
    answers as superseded with a pointer to the new rule) that states: the
    dashboard needs no credential, no default password is shipped, the
    optional `saadway_login` credential is created on demand by the LuCI
    credentials tab, and the dashboard's ubus surface is authorized as
    rpcd's default (unauthenticated) session.
  - The FR/SC text, the user stories and the acceptance scenarios MUST NOT
    change in this task — only the clarification record is reconciled.
  - Verification: no statement in `spec.md` still claims a default
    credential, a dashboard login or a dashboard session (a grep over the
    document for "default credential", "session" and "login" must show only
    the amended wording and the historical Q/A marked as superseded).

**As implemented (T091–T093)** — two decisions worth recording:
1. **T091 merged the two payloads** rather than replacing the call: the
   security panel now fetches `sec.status` **and** `sec.dns.status` and
   merges them, so the kill-switch rows keep their source and the DNS rows
   finally get `resolver_if`/`leak_risk`; a missing `sec.dns.status` leaves
   the "unknown" fallback instead of breaking the panel.
2. **T092's settings form is a patch form**: the daemon's `profile.list`
   payload exposes only `{ name, kind, enabled, order, secret_ref, active,
   status }` — no hostname, no user name — and the stored secret is never
   echoed (FR-033), so every field starts **empty** and an empty field
   means "keep the current value". The OpenVPN credential pair travels
   together (the daemon rejects a lone password with `E_BAD_SYNTAX`), which
   the form checks before sending.

**Checkpoint**: the dashboard shows the daemon's real DNS/leak-risk state,
every profile can be configured from its own settings form through the
already-implemented `profile.update` API, and the specification no longer
contradicts itself about the authentication model.

## Phase 27: Convergence — the OpenVPN hostname field and the stale auth model (2026-10-21)

**Purpose**: the third convergence pass (after Phases 25 and 26) re-assessed
the shipped code against the amended `spec.md`, `plan.md`, the data model and
the constitution v1.2.0, this time probing the areas the earlier passes did
not: the contract↔daemon method surface, the data model's entities, and the
behaviour of the surface Phase 26 added. The code itself is sound — the
findings are one misleading UI field and two stale model descriptions. The
on-device items remain tracked as **T046** and **T057** (device-bound) and
are NOT re-appended.

- [X] T094: Drop the ineffective hostname field from the OpenVPN branch of the profile settings form per FR-001, FR-006, US1/AC2 (contradicts) — target files: `saadway/files/htdocs/saadway/js/app.js`, `specs/001-saadway-vpn-dashboard/contracts/api.md`:
  - The form added in Phase 26 / T092 offers a **"Server hostname/IP"**
    field for an OpenVPN profile and sends it as `patch.hostname`. The
    daemon accepts the patch (`saadway/src/ubus.c`, `h_profile_update` →
    `p->hostname`) — but **`tunnel-openvpn.c` never reads `p->hostname`**:
    an OpenVPN profile's server comes from the imported file's `remote`
    directive (`m.remote_host`, `tunnel-openvpn.c:162-169`, written into
    the 0600 rendered config at import time). The field therefore promises
    a change that never reaches the running configuration, and the value it
    shows the operator is the one they typed — not the one in effect.
  - Remove the hostname field from the **OpenVPN** branch of the form (the
    imported `.ovpn` file is the server's single source of truth; changing
    the server means re-importing the file). The IKEv2 branch keeps its
    identity fields, where `hostname` IS the server (`remote_addrs` in the
    rendered swanctl config, `tunnel-ikev2.c`).
  - The empty-field-means-unchanged semantics stay ✓ and the form MUST NOT
    offer any other field the daemon cannot honour.
  - `contracts/api.md` — the `profile.update` row MUST state that
    `hostname` applies to the **IKEv2** profile only (the OpenVPN server
    comes from the `.ovpn` file), so the contract and the daemon agree on
    what the patch does.
  - Verification: the OpenVPN settings form has no hostname field; the
    IKEv2 form still exposes it and the change is observable in
    `/etc/config/saadway` and in the rendered 0600 config (on-device proof
    folded into T046/T057); the SPA parses; the i18n locales stay 1:1
    (the now-unused `edit_hostname` key MUST be removed from **both**
    locales if the field disappears).

- [X] T095: Rewrite the stale authentication model in the data model per FR-020, FR-028 (contradicts) — target file: `specs/001-saadway-vpn-dashboard/data-model.md`:
  - The **DashboardSession** entity still describes the pre-bypass model:
    the dedicated `admin` account provisioned with the *default* password,
    the ubus session token, the 1800 s inactivity expiry, and — worst —
    *"Invalid/absent token: all `saadway.*` RPC calls rejected"*, which is
    the exact opposite of the shipped behaviour (the tokenless client is
    authorized by the `unauthenticated` ACL group, `docs/verify-2512.md`
    §7). The **DashboardCredential** entity's state diagram likewise still
    shows the "default (admin / admin, hash in /etc/shadow)" state, which
    the auth bypass retired.
  - Rewrite both entities to the shipped model: the dashboard client uses
    rpcd's **default (unauthenticated) session** (the all-zero session id)
    and is authorized by the ACL group named `unauthenticated`, which
    covers the `saadway` ubus object only; the dashboard creates **no**
    session and has **no** credential; the `saadway_login` mapping plus the
    `admin` account are the **optional operator credential**, created on
    demand by the LuCI credentials tab, with the hash only in the 0600
    `/etc/shadow`; the "absent token" case is now the **normal** path (the
    calls succeed) and the residual risk is the network boundary (the
    LAN-bound listener + the firewall's WAN input policy).
  - The other entities (Profile, SteeringRule, ServiceState, BackupExport)
    MUST stay semantically unchanged — only the two authentication-related
    entities are in scope.
  - Verification: no statement in the data model still claims a dashboard
    login, a session token, an inactivity expiry, a default password or a
    tokenless rejection (a grep over the file must show only the amended
    wording).

- [X] T096: Correct the device-rule contract row per plan: the API contract (partial) — target file: `specs/001-saadway-vpn-dashboard/contracts/api.md`:
  - The method table documents `steer.device.set` / `steer.device.remove` in
    **one** row with the params `{ match_type, match_value, policy? }` —
    but the daemon's **remove** takes only `{ match_type, match_value }`
    (`h_steer_device_remove` reads exactly those two attributes and ignores
    a policy). A client following the contract literally would send a
    `policy` that the method silently drops.
  - Split the row into two (or state the remove params explicitly) so the
    contract states the exact payload of each method, and check the other
    combined rows (`steer.domain.add` / `steer.domain.remove`,
    `maint.backup.export` / `maint.backup.import`) for the same ambiguity
    — the daemon's payloads are the authority
    (`saadway/src/ubus.c`, the registered methods).
  - Verification: every row of the method table names the exact parameters
    the daemon reads for that method (a read of `saadway/src/ubus.c` per
    row), and no row implies a parameter the method ignores.

**As implemented (T094–T096)** — two decisions worth recording:
1. **T094 dropped the field rather than teaching the renderer to honour
   it**: the OpenVPN profile's server is the imported file's `remote`
   directive, and the rendered 0600 config is written at import time — a
   `p->hostname` patch could never reach the running configuration, so
   offering the field was the actual defect. The now-unused `edit_hostname`
   locale key was removed from **both** locales.
2. **T096 verified the other combined rows** instead of assuming they were
   wrong: `steer.domain.add` / `steer.domain.remove` both take exactly
   `{ domain }` (the daemon reads one attribute in each handler) ⇒ that row
   was already correct and stays. Only the device row was split. The
   contract's `policy?` for `steer.device.set` is right — the daemon
   defaults a missing policy to `tunneled`.

**Checkpoint**: the dashboard's OpenVPN settings form no longer offers a
field the daemon cannot honour, the data model describes the tokenless
authentication model that actually ships, and the API contract states each
method's exact payload.

## Phase 28: Convergence — the `.ovpn` device override and the data model's UCI types (2026-10-21)

**Purpose**: the fourth convergence pass (after Phases 25–27) probed the
remaining surfaces: the `.ovpn` import validation against the data model's
rules, the data model's UCI section types against the shipped
`/etc/config/saadway` and `contracts/uci.md`, and the backup schema against
the codec. The daemon, the SPA and the contracts are sound; the findings are
one import-validation rule that is not enforced, and two data-model
statements that do not match the shipped UCI. The on-device items remain
tracked as **T046** and **T057** (device-bound) and are NOT re-appended.

- [X] T097: Reject a `dev` directive in an imported `.ovpn` file per FR-001, FR-025, Constitution IV (contradicts) — target file: `saadway/src/tunnel-openvpn.c`:
  - The data model's validation rules require the importer to refuse
    dangerous directives, explicitly listing *"`dev` overrides conflicting
    with saadwaydev naming"*. The shipped ban list
    (`banned_dirs[]`, `saadway/src/tunnel-openvpn.c:22-29`) contains
    `dev-node` but **not** `dev`, so a `dev tun5` line in an imported file
    falls through to the "all other lines verbatim" branch and is copied
    into the rendered 0600 config. It is only neutralised because the
    renderer appends its own `dev-type tun` + `dev <ifname>` identity last
    (`:273-274`) — i.e. the safety currently depends on the option's
    last-one-wins behaviour rather than on the import refusing the
    directive, and the operator gets no feedback that the file carried one.
  - Add `dev` to the banned directives so the import **fails closed** with
    `E_UNSUPPORTED_DIRECTIVE` (the same code the other dangerous
    directives produce). The `dev-type` check (tun/tun6 only) and the
    `dev-node` ban stay as they are.
  - The rejection MUST happen during the import (no partial save, FR-025)
    and MUST NOT touch the running profile.
  - Verification: the unit/host suite stays green, the 14 modules keep
    compiling clean under `-Wall -Wextra`, and a `.ovpn` carrying a `dev`
    line is rejected with `E_UNSUPPORTED_DIRECTIVE` while one without it
    imports unchanged (on-device proof folded into T046/T057).

- [X] T098: Correct the data model's UCI section types per FR-024, plan: the UCI contract (contradicts) — target file: `specs/001-saadway-vpn-dashboard/data-model.md`:
  - The data model declares the UCI section type **`saadway_service`** in
    two places — the IP-range auto-bypass ("conceptual kind") and the
    `ServiceState` entity — but **no such section type exists**. The
    shipped and contracted type is **`service`**:
    `contracts/uci.md` ("Section `service` (unique, `option ...` one named
    section)"), `saadway/files/saadway.conf` (`config service 'service'`),
    and the daemon's own reader (`uci-state.c:896`, `!strcmp(s->type,
    "service")`). The dashboard preferences live in the **`dashboard`**
    section (`config dashboard 'dashboard'`), not in a service section.
  - Correct both declarations to the real types (`service` for the
    service state and the auto-bypass option, `dashboard` for the
    dashboard preferences) and make the `ServiceState` entity's field
    table match the options the daemon actually reads and writes in the
    `service` section (`enabled`, `active_profile`, `connection_status`,
    `failover_attempts`, `failover_cooldown`, `probe_interval`,
    `stats_interval`, `kill_switch`, `iprange_bypass`).
  - No other entity changes: `Profile` (`saadway_profile` ✓),
    `SteeringRule`'s rule sections (`saadway_domain_rule` ✓,
    `saadway_device_rule` ✓) and `BackupExport` are already correct.
  - Verification: every section type named in the data model exists in
    `saadway/files/saadway.conf` and is handled by the daemon
    (`uci-state.c`), and the data model agrees with `contracts/uci.md`.

- [X] T099: State the two different uniqueness behaviours in the data model per FR-025 (partial) — target file: `specs/001-saadway-vpn-dashboard/data-model.md`:
  - The `SteeringRule` entity's uniqueness rule says a duplicate
    **`match_value`** is "detected at create; response = reject", but the
    daemon's `sw_uci_device_set` has **set semantics**: an existing rule
    with the same `match_type` + `match_value` is **updated** in place
    (`saadway/src/uci-state.c`, the loop that assigns `d->ptype = pol`),
    never rejected. Only the **domain** rule rejects a duplicate, and it
    does so case-insensitively (`domain_taken` → `strcasecmp` ✓) with
    `E_DUPLICATE` — the behaviour the quickstart's Scenario 3 step 4
    exercises.
  - Rewrite the uniqueness rule to state both behaviours exactly: a
    duplicate **domain** (case-insensitive) is rejected with
    `E_DUPLICATE` (no silent double rule), while a repeated device
    **match_value** updates the existing rule's policy — the "set"
    semantics the dashboard's device form relies on.
  - Verification: the data model's wording matches the daemon's behaviour
    for both rule kinds (a read of `sw_uci_domain_add` and
    `sw_uci_device_set`).

**As implemented (T097–T099)** — one decision worth recording:
**T097 refused the directive at import** instead of keeping the neutralising
trailing identity lines as the only defence: the import now fails closed
with `E_UNSUPPORTED_DIRECTIVE` (the same code the other dangerous
directives produce), the operator is told, and the safety no longer depends
on openvpn's last-one-wins option order. The renderer's own
`dev-type tun` + `dev <saadwaydev>` lines stay (they are the interface
identity, Principle III).

**Checkpoint**: the importer refuses a device override instead of relying on
the option order, the data model names the UCI sections that actually
exist and the uniqueness behaviour the daemon implements, and the shipped
code, the contracts and the model agree with each other.

## Phase 29: Convergence — the credential edge case and the plan's compliance table (2026-10-21)

**Purpose**: the fifth convergence pass (after Phases 25–28) probed the last
two unexamined surfaces: the specification's **edge-case list** and the
plan's **Constitution Check** table. The code is sound — the boot sequence
matches D8, the `core.nft` skeleton matches the renderer (mark classes
0x01/0x02/0x04/0x08, rule priorities 7000/7100/7200/7300/7900), and the
updown script's contract holds. The findings are three **document**
statements that still describe the pre-auth-bypass model. The on-device
items remain tracked as **T046** and **T057** (device-bound) and are NOT
re-appended.

- [X] T100: Reconcile the credential edge case with the amended access model per FR-020, FR-034 (contradicts) — target file: `specs/001-saadway-vpn-dashboard/spec.md`:
  - The edge-case list still contains: *"A client without an administrative
    session reaching the credentials form: the form and its action MUST be
    unreachable, and the stored credential MUST NOT be readable."* The
    **panel form** still satisfies that half — LuCI itself requires the
    router's administrative login — but the **action** no longer does: the
    `unauthenticated` ACL group grants the whole `saadway` ubus object
    (the wildcard), so `saadway.mgmt.credentials.set` is reachable by a
    session-less LAN client. That is the state recorded in
    `docs/verify-2512.md` §7 (the withdrawn protection + the residual
    risk) and in the amended FR-034, but this edge case still states the
    opposite.
  - Rewrite the edge case to the shipped model: the **panel form** is
    unreachable without an administrative panel session (the LuCI login
    gate) and the stored credential is never readable (no response ever
    carries a secret, FR-021/FR-033) — while the underlying ubus action
    is **LAN-scoped** like the rest of the `saadway` surface, which is the
    recorded deviation (§7) and the reason the network boundary is the
    access control. The edge case MUST NOT claim an isolation the shipped
    model does not have.
  - The FR/SC text, the user stories and their acceptance scenarios MUST NOT
    change in this task — only the edge case is reconciled.
  - Verification: the edge-case list contains no statement that a
    session-less client is refused the credential **action** (a grep over
    the document must show only the amended wording and the marked
    history), and the two neighbouring edge cases keep their SUPERSEDED
    markers.

- [X] T101: Update the plan's Constitution Check table to the v1.2.0 model per plan: the Constitution Check, Constitution V / IV (contradicts) — target file: `specs/001-saadway-vpn-dashboard/plan.md`:
  - Two rows of the compliance table still assert the pre-auth-bypass
    model:
    - **Principle V**: *"research.md D7 (uhttpd-mod-ubus authenticated
      JSON-RPC only; the standalone dashboard keeps every system
      interaction behind an authenticated API) + D10 (the LuCI surface gains
      ONE bounded exception …)"* — the dashboard's transport is no longer
      an authenticated API (spec FR-020/FR-035, constitution v1.2.0,
      research.md D11): it is a tokenless JSON-RPC client authorized as
      rpcd's default (unauthenticated) session.
    - **IV-a (credential material confinement)**: *"… the new ubus method is
      granted to a dedicated root-only ACL group, so the dashboard's own
      session can never re-key its login"* — the method is still granted to
      the narrow `saadway-credentials` group, but that group is no longer
      an exclusive boundary: the LAN-scoped `unauthenticated` group covers
      the whole `saadway` object (T088 re-documented it; the §7 deviation
      records the withdrawal).
  - Update both rows to the shipped model: the delegated-boundary access
    control (the LAN-bound listener + the WAN input policy), the
    `unauthenticated` group as the dashboard's authorization path, the
    narrow credential group as the grant to use if the wildcard is ever
    tightened, and a pointer to the §7 residual risk. The principle's
    status column MUST stay a PASS with the correct evidence (the
    constitution itself is v1.2.0 — `research.md` D11 is the reference,
    not D7 alone).
  - The other nine rows and the "Post-Phase-1 re-check" note MUST stay
    unchanged.
  - Verification: no row of the table claims an authenticated dashboard
    API or a root-only credential grant, and each row's evidence still
    points at an artifact that exists.

- [X] T102: Mark the concurrent-dashboard edge case as superseded per FR-020 (partial) — target file: `specs/001-saadway-vpn-dashboard/spec.md`:
  - The edge case *"Dashboard access from concurrently connected devices:
    multiple simultaneous administrator sessions must be supported without
    one session invalidating another's configuration changes mid-edit"*
    still speaks of administrator **sessions** — the dashboard has none
    since the auth bypass (its calls are authorized as rpcd's default
    session). The two other session-flavoured edge cases were marked
    SUPERSEDED in T093; this one was missed.
  - Mark it superseded and restate the surviving requirement: several
    clients may drive the dashboard **concurrently** and the
    configuration changes of one must not be invalidated by another's
    mid-edit — which the UCI-backed state model (constitution VI) and the
    per-rule set semantics (`steer.device.set` updates its own rule) do
    satisfy.
  - Verification: the edge-case list contains no unmarked reference to a
    dashboard session.

**Checkpoint**: the specification's edge cases, the plan's compliance table
and the shipped code describe the same access model — a LAN-scoped,
tokenless dashboard whose access control is the platform's network
boundary, with the credential action's reach recorded as a deviation rather
than claimed as an isolation.

## Phase 30: Remediation Round 14 — IKEv2 creation feedback & the nftables transaction failure

**Purpose** (device report, 2026-10-21): two defects observed on a live
device — the dashboard's IKEv2 form does nothing when "Create" is pressed,
and the daemon's nftables policy never loads, so the service sits in the
fail-closed emergency containment.

**Root causes — verified in the tree (not guesses):**

- **Frontend** — `saadway/files/htdocs/saadway/js/app.js`: `panel_events()`
  (line 697) opens `if (btn) { … }` and that block ends with an
  **unconditional `return;`** (line 783), so the fall-through
  `panel_click(ev);` (line 784) is **unreachable** for every click that
  carries a `data-act` attribute. The whole profiles panel is therefore
  dead — *import*, **ike-create**, *activate*, *deactivate*, *edit* and
  *delete* never run — which is exactly "clicking Create produces no
  reaction and no console errors".
- **Backend** — `saadway/src/policy.c`: `render_dev_sets()` emits the
  per-device sets with a broken element list:
  * the terminator is `"; } }"` → `elements = { 10.0.0.1; }` — a `;`
    where nftables expects a `,`;
  * the empty case yields `elements = { ; }` — an empty element list;
  * the leading separator uses the **shared** loop counter `n`, so a MAC
    rule that is not the first rule yields `elements = { , aa:bb:…; }`.

  The correct shape already exists in the tree:
  `sw_steering_mark_ranges()` (`saadway/src/steering.c`) uses a **per-list**
  counter and the `elements = { %s }` form. nft rejects the table → the
  delete+create transaction fails (`rc=1`) → the create-only retry fails
  with the same content → `policy: DIRECT nft FAILED rc=1 — fail closed`
  → `SENTINEL: EMERGENCY CONTAINMENT (tunnel-class egress blackholed)`
  (`policy.c:580`): the tunnel-class egress stays blackholed.
- **Diagnosability** — `run_argv()` (`policy.c:31`) redirects the child's
  **stdout and stderr to `/dev/null`**, so nft's own error text is
  discarded: the log shows only `rc=1`, never the reason.

- [X] T103: Fix IKEv2 creation submission feedback and resolve nftables transaction failure per FR-003, FR-014, FR-025, US1/AC3, Constitution IV (fail closed) — target files: `saadway/files/htdocs/saadway/js/app.js`, `saadway/files/htdocs/saadway/js/api.js`, `saadway/files/core.nft`, `saadway/src/policy.c`, `saadway/src/unit/test_policy_render.c`:
  - **Part A — the frontend (`js/app.js`, `js/api.js`).**
    - `panel_events()` MUST stop swallowing the profiles-panel actions: the
      unconditional `return;` at the end of its `if (btn)` block MUST be
      removed so the fall-through `panel_click(ev);` runs for every action
      that block does not handle — the profiles panel's *import*,
      *ike-create*, *activate*, *deactivate*, *edit* and *delete* MUST
      work again.
    - `panel_click()` MUST scope itself to the profiles panel: it already
      computes the panel id (`pid`) but never uses it — with the fall-through
      in place, the steering / security / monitoring / maintenance buttons
      would also reach it. It MUST ignore any button that is not inside the
      profiles panel, and it MUST keep matching only `button[data-act]`
      elements.
    - The IKEv2 "Create" action MUST always give visible feedback: every
      validation failure (an empty name or hostname, a custom Remote/Local ID
      selected without a value) MUST toast an actionable message, and the
      ubus failure MUST show the daemon's reason instead of the generic
      "save_error".
    - `js/api.js` MUST surface the failure reason: the ubus status codes
      MUST map to readable messages (e.g. `2` = invalid argument,
      `6` = permission denied, `4` = not found) and the daemon's `detail`
      field (the `E_*` codes the daemon returns) MUST be included when the
      transport delivers it — the current `err_of()` helper only looks at
      `r.data.detail`, which the legacy `result` envelope does not carry
      for an error; the envelope position MUST be verified against the
      shipped transport (uhttpd-mod-ubus: the legacy `result` array for
      errors and empty results, the modern `{error:{code,message}}`
      envelope for pre-call rejections).
    - The "Create" button MUST NOT submit a form: give it `type="button"`
      so a click can never trigger a page navigation instead of the RPC.
  - **Part B — the backend (`src/policy.c`, `files/core.nft`).**
    - `render_dev_sets()` MUST emit each per-device set **only when it has
      elements** (an empty set is declared without an `elements` clause),
      MUST use a **per-list** separator counter (not the shared loop
      counter `n`), and MUST use nftables' element syntax — a
      comma-separated list inside `elements = { a, b }` — following the
      `sw_steering_mark_ranges()` pattern already in the tree. The rendered
      table MUST contain neither an empty `elements` list nor a stray
      leading/trailing separator.
    - The per-device rules MUST keep their semantics: `dev_ip` carries the
      IP rules (with `flags interval`), `dev_mac` the MAC rules, and the
      rule set of the dashboard's device form MUST keep the set-semantics
      contract (a repeated `match_value` updates its rule).
    - `files/core.nft` — the design-reference skeleton MUST document the two
      per-device sets (it currently documents only `bypass_domains` and
      `bypass_ranges`) and MUST match the renderer's shape, as the file's own
      header requires ("Keep it in sync with policy.c").
    - The failure MUST become diagnosable: the nft invocation MUST capture
      the child's **stderr** (bounded, the existing `run_capture()` pattern
      in the same file) and log it with the failing transaction, so a
      rejected ruleset reports nft's own message instead of a bare `rc=1`.
      The fail-closed behaviour itself MUST NOT change: a failed apply still
      blackholes the tunnel-class egress (Constitution IV) — only the
      diagnosis improves.
  - **Part C — the regression test (TDD).** `saadway/src/unit/test_policy_render.c`
    MUST gain the device-set assertions it never had: render with an
    IP-only device rule, a MAC-only device rule, both kinds (with the MAC
    rule **not** first) and with no device rule at all, and assert that the
    output contains a well-formed `set dev_ip` / `set dev_mac` (comma
    separated, no empty element list, no leading separator) and that a kind
    with no rules is **not** emitted. The new assertions MUST fail against
    the current renderer and pass after the fix.
  - **Part D — the verification.**
    - Host-side: the unit suite green (including the new render
      assertions), the 14 modules compiling clean under `-Wall -Wextra`, all
      host audits green, the shipped JavaScript parsing, and a static check
      that the renderer can no longer emit `elements = { ; }`.
    - On-device (folded into T046/T057): `nft -f` accepts the rendered
      table (no `rc=1`), the `SENTINEL: EMERGENCY CONTAINMENT` line does
      **not** appear in the log, the dashboard's IKEv2 "Create" produces a
      visible result (a profile in the list, or a readable error), and the
      profiles panel's import / activate / deactivate / delete actions work
      again.

**As implemented (T103)** — two decisions worth recording:
1. **The frontend fix was the dispatch, not the form**: the IKEv2 form
   never reported anything because `panel_events()` ended its `if (btn)`
   block with an unconditional `return;`, so `panel_click()` — which
   owns *import*, **ike-create**, *activate*, *deactivate*, *edit* and
   *delete* — was unreachable for every `data-act` click. Removing that
   return restores the whole profiles panel, and `panel_click()` now
   scopes itself to `#panel-profiles` so the other panels' buttons cannot
   cross-talk into it. The transport already delivered the daemon's
   `detail` (uhttpd-mod-ubus's legacy envelope carries it in
   `result[1]`), so `api.js` only had to add the ubus-status → readable
   reason mapping as the fallback.
2. **The nftables fix was the element list, not the transaction**: the
   delete+create transaction and its create-only fallback are correct; the
   rendered table was invalid. The device sets now use a per-list counter,
   nftables' comma syntax, and are emitted only when they have elements.
   `run_argv()` now captures the child's stderr into a 0600 file next to
   the render file, so a rejected ruleset reports nft's own parse error
   instead of a bare `rc=1` — the fail-closed behaviour itself is
   unchanged.

**Checkpoint**: the profiles panel's actions reach the daemon again, the
IKEv2 creation always reports its outcome, and the rendered nftables table
is accepted by nft so the service leaves the emergency containment and
operates under the intended policy.

## Phase 31: Remediation Round 15 — profile-name validation & the feedback the user sees

**Purpose** (device report, 2026-10-22): creating a profile in the
standalone dashboard with a name the daemon rejects (`E_BAD_SYNTAX: invalid
name`) leaves the administrator with the raw error string — no hint, no
friendly Persian message, and the input offers no indication of what is
allowed.

**Root cause — verified in the tree (the report's assumption was partly
wrong; the fix must follow the code, not the assumption):**

- The backend rule is `sw_valid_name()` — `saadway/src/uci-state.c:39`,
  called from `saadway/src/ubus.c:404` (the profile create/update paths),
  `saadway/src/tunnel-openvpn.c:209` (the import) and
  `saadway/src/backup.c:489` (the restore). It is **not** a
  `^[a-zA-Z0-9_-]{1,32}$` POSIX pattern: the shipped rule is
  * non-empty, at most **64 bytes**;
  * no leading `.`;
  * no leading or trailing space, and **no byte below 0x21 anywhere**
    (so no space and no control character);
  * no `"`, no `'` and no 0x7f.
- **Persian / UTF-8 names are ACCEPTED** (verified on the host against the
  shipped function: `پروفایل` → accepted, `پروفایل-۱` → accepted). The
  dashboard is Persian-first (FR-027), so the fix MUST NOT forbid
  non-ASCII names — only the rules the daemon actually enforces.
- The profile's UCI section name is a **hash**, not the profile name
  (`u_gen_section()` → `p%06x`, `saadway/src/uci-state.c`), so the UCI
  identifier syntax is **not** the constraint either.
- The real gap is therefore entirely on the **frontend**: the name inputs
  (`#imp-name`, `saadway/files/htdocs/saadway/js/app.js:199`, and
  `#ike-name`, `:217`) carry no `pattern`, no `maxlength` and no hint, and
  no client-side validation; the rejection therefore travels
  `sw_valid_name` → `E_BAD_SYNTAX: invalid name` → `err_of()` →
  `r.data.detail` and is shown **raw**.
- The data model understates the rule: `Profile.name` says *"unique, 1–64
  chars, non-empty"* (`specs/001-saadway-vpn-dashboard/data-model.md:21`)
  and does not mention the space/quote/leading-dot restrictions.

- [ ] T104: Add frontend slug validation and user feedback for profile names per FR-001, FR-003, FR-027, US1/AC1, US1/AC3 (partial) — target files: `saadway/files/htdocs/saadway/js/app.js`, `saadway/files/htdocs/saadway/index.html`, `saadway/files/htdocs/saadway/i18n/en.json`, `saadway/files/htdocs/saadway/i18n/fa.json`, `saadway/src/ubus.c`, `saadway/src/unit/test_name_rules.c` (new), `saadway/src/unit/Makefile`, `specs/001-saadway-vpn-dashboard/data-model.md`, `specs/001-saadway-vpn-dashboard/contracts/api.md`:
  - **The shared rule.** The dashboard MUST validate a profile name with
    **exactly** the rule the daemon enforces (`sw_valid_name`): non-empty,
    at most 64 bytes, no leading `.`, no leading/trailing space, no byte
    below 0x21 (no space, no control character), no `"`, no `'` and no
    0x7f — **non-ASCII (Persian) names stay valid** (FR-027: the
    dashboard is Persian-first; the rule is a POSIX-safety rule, not a
    Latin-only rule). Implement it once in `js/app.js` as a helper
    (e.g. `valid_profile_name(name)`) and use it for **every** name input:
    the OpenVPN import card (`#imp-name`), the IKEv2 card (`#ike-name`)
    and the per-profile settings form (`#pf-*` — the profile name there is
    read-only, but the helper MUST be the single source of truth).
  - **The feedback.** A rejected name MUST produce a **friendly Persian
    message** (a new i18n key, e.g. `name_invalid`, in **both** locales —
    the parity MUST stay 1:1), naming the rule in one line ("۱ تا ۶۴
    نویسه، بدون فاصله و بدون کوتیشن") instead of the raw
    `E_BAD_SYNTAX: invalid name`. The toast path already exists
    (`toast()` / `err_of()` in `js/app.js`) — the validation MUST run
    **before** the RPC, so a doomed request is never sent.
  - **The input affordances.** The name inputs MUST get
    `maxlength="64"`, `dir="auto"` (so a Persian name renders in its own
    direction), `spellcheck="false"` and an HTML `pattern` that mirrors the
    rule, plus a one-line hint under the field (a new i18n key, e.g.
    `name_hint`, in both locales) stating the rule. The pattern attribute
    is a *hint*, not the authority: the JS helper remains the authority
    (the pattern cannot express "no leading dot" or the byte rules).
  - **The backend's detail.** `saadway/src/ubus.c` MUST make the
    rejection self-describing: the `E_BAD_SYNTAX: invalid name` detail
    MUST name the rule (e.g. *"invalid name: 1-64 characters, no spaces or
    quotes, must not start with '.'"*) so a script or a future client can
    act on it without guessing. The reject **code** stays `E_BAD_SYNTAX`
    (the documented set in `contracts/api.md` MUST NOT grow for this).
  - **The documentation.** `contracts/api.md` MUST document the profile
    name rule next to the `profile.import_openvpn` / `profile.create_ikev2`
    / `profile.update` rows (including the "no spaces, Persian allowed"
    wording), and `data-model.md`'s `Profile.name` constraint MUST quote
    the full rule the code enforces (not only "1–64 chars, non-empty").
  - **The regression test (TDD).** A new `saadway/src/unit/test_name_rules.c`
    (registered in the unit `Makefile`'s `TESTS` list) MUST pin the
    accept/reject table of `sw_valid_name()`: accepted — `prov-a`,
    `UPPER_ok`, `123`, `پروفایل`, `پروفایل-۱`; rejected — the empty
    string, a 65-byte name, a leading `.`, a leading/trailing space, a
    name with an inner space, a name with `"`, a name with `'` and a name
    with 0x7f. The Persian cases are part of the contract (FR-027) and
    MUST NOT be "fixed" into rejections.
  - **Verification.** Host-side: the new unit test passes; the unit suite
    stays green; the 14 modules keep compiling clean under `-Wall -Wextra`;
    all host audits stay green; the shipped JavaScript parses; the i18n
    locales stay 1:1. On-device (folded into T046/T057): a name with a
    space, a leading dot or a 65-character name is refused **in the
    browser** with the Persian hint and **no** ubus call is made; a
    Persian name is accepted and the profile is created; the daemon's
    rejection detail still names the rule for a direct
    `ubus call saadway profile.create_ikev2` with a bad name.

**Checkpoint**: the administrator is told the name rule before the request
is sent, in the dashboard's own language; the browser and the daemon agree
on the rule (one implementation, one documented contract), and Persian
profile names keep working.
