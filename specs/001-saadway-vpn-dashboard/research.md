# Phase 0 Research: SaadWay VPN Tunnel & Routing Dashboard

**Feature**: 001-saadway-vpn-dashboard | **Date**: 2026-10-19

All Technical Context decisions resolved below; no NEEDS CLARIFICATION items
remain. The four clarified decisions from spec.md (Q1–Q4) are treated as
fixed inputs: tunnel-default steering, kill switch OFF by default, cool-down
retry after exhausted failover cycles, Persian-first RTL dashboard with
English switch.

---

## D1. Platform runtime & service architecture

**Decision**: One resident daemon `saadwayd` written in C11 (musl, OpenWrt
buildroot toolchain), managed by procd, exposing a single ubus object
`saadway`. Shell (`ash`) limited to the procd init wrapper and package glue.
All orchestration, watchdog, health tests, counter sampling, policy
rendering, and backup logic live in the C daemon.

**Rationale**: OpenWrt-native C against libubus/libuci is the smallest
resident footprint compatible with the constitution's runtime ban list
(no Node.js/Python/JVM; acceptable runtimes are Lua, C, ash). A single event
loop makes the non-blocking + explicit-timeout rule (Principle II)
structurally enforceable — one poll loop, one timeout table. Lua was
considered but Lua services are interpreted per-invocation glue, not
resident watchdogs; splitting logic into many scripts would complicate the
single atomic-apply guarantee.

**Alternatives considered**:
- Lua-only daemon around `uci`/`nft` shells — larger RAM, weaker async
  primitives, rejected.
- Multiple small procd services (watchdog, policy, api) — more supervision
  complexity without footprint benefit; rejected.
- Full Python/Node service — constitution-forbidden.

## D2. Non-blocking I/O & timeout policy

**Decision**: `saadwayd` runs a single `poll()`/timer-driven event loop.
Child processes (openvpn, swanctl, and the dedicated dnsmasq
tunnel-resolver instance) are supervised non-blockingly with
per-operation timeouts sourced from UCI options (defaults listed below).
Health probes run in-process, not as supervised children: ICMP via a raw
packet socket and the HTTP-204 GET via a non-blocking socket, both
`poll()`-driven, so dropped or hung targets yield graceful Timeout results
and never block the loop (ratified deviation from this section's original
"probe as children" wording — recorded with rationale in
docs/verify-2512.md §7). Bandwidth counters read from
`/sys/class/net/<if>/statistics/{rx,tx}_bytes` on a 1 s timer tick.

**Default timeout table (UCI-configurable):**

| Operation | Default timeout |
|---|---|
| Profile activation (connect attempt) | 30 s |
| Health ICMP ping | 3 s (bounded by 5 s SC-003 pair test) |
| Health HTTP GET (`generate_204`) | 4 s |
| Failover connectivity probe (watchdog) | 5 s per attempt |
| Bandwidth/log polling cycle | 1 s (no external wait) |

**Rationale**: Directly operationalizes Principle II "no unbounded waits";
each value has an owning UCI option so administrative changes don't require
recompilation. The health-test pair stays within the 5-second SC-003 budget.

**Alternatives considered**: spawning per-test shells with `timeout`
wrapper (still unbounded children, less precise — rejected); child-process
ping/HTTP probes, this section's original wording (superseded during
implementation by the in-process probe model — ratified deviation recorded
in docs/verify-2512.md §7).

## D3. Tunnel backends & L3 interface materialization

**Decision**:
- **OpenVPN**: `openvpn-openssl` client package. Imported `.ovpn` is
  validated, normalized, and rendered to a generated config under
  `/etc/saadway/run/openvpn/<profile>.conf`; credentials written to a 0600
  `auth-user-pass` file when provided. Supervisor watches the tun interface
  (`tunX`) with explicit `dev-type tun`, `up`-less operation (saadwayd owns
  interface events via netlink).
- **IKEv2**: strongSwan stack — `strongswan`, `swanctl`,
  `strongswan-mod-eap-mschapv2`, `strongswan-mod-gcm`. Each IKEv2 profile
  renders a swanctl connection (EAP-MSCHAPv2 identity, proposal
  `aes256gcm16-prfsha256-ecp384`, and `ecp521` fallback in IKE proposal;
  MODP disabled) and materializes an **XFRM interface**
  (`xfrmX`, `if_id` unique per profile, ipsec policy bound to that interface)
  so routing/monitoring bind to a real L3 device. Desktop-style
  "iOS key exchange (DH21)" is honored by placing `ecp521` after `ecp384` in
  the IKE proposal list.
- Both providers MUST expose the interface within constitution Principle +
  III; saadwayd refuses activation if no L3 interface appears.

**Rationale**: Verified (OpenWrt forum/wiki + strongSwan docs, Oct 2026
research): swanctl is the modern client path on OpenWrt; xfrm interfaces are
the supported way to get a routable IPsec L3 device. AES-256-GCM/ECP384 maps
to `aes256gcm16-prfsha256-ecp384` (GCM uses PRF for key derivation;
integrity SHA-256 is carried by the PRF in GCM mode).

**Alternatives considered**: vti interfaces (extra IPsec nesting, no
advantage over xfrm on 25.12); legacy `ipsec`/pluto conf (deprecated per
OpenWrt wiki); running multiple daemons (swanctl + pluto) side by side — unnecessary.

## D4. Firewall, policy routing, atomicity (Principle I & IV)

**Decision**: All firewall state is nftables via fw4 includes:
`/etc/saadway/core.nft` rendered into fw4's nftables pipeline. Policy-based
routing uses **fwmark + `ip rule` + dedicated routing tables** per direction:
- Table `201` (tunnel): default via tunnel interface gateway.
- Table `202` (direct/WAN): untouched default route.
- Marks: device-policy mark bits (per MAC/IP), domain-bypass mark,
  auto-bypass (local/country/banking ranges from bundled list,
  maintained as a data file CIDR set in the daemon) mark.
- Rules evaluated in priority order: device-direct > domain-bypass > IP-
  range bypass > device-tunnel (implicit/default) > tunnel catch-all.
- Atomic apply: the daemon renders the FULL ruleset for the desired state and
  applies it in one `nft -f` transaction against the saadway fw4 table; fw4
  reloads the remainder. There is no two-step "delete then add" window.
  Profile switches flip marks/routes and nft sets in the same transaction.

**Rationale**: nftables applies complete ruleset files atomically by design —
this is the platform-native path to Principle IV's zero-transient-leak
requirement. fwmark approaches are the standard PBR mechanism supported by
fw4; per-interface routing tables guarantee traffic egress binds to the
chosen L3 tunnel interface (Principle III).

**Alternatives considered**:
- `iptables`+`iproute2` legacy PBR — forbidden by constitution.
- dnsmasq `ipset=`-style domain routing — ipset deprecated under nft;
  excluded by Principle I (use nft named sets flushed/rebuilt in the same
  atomic transaction instead).
- Building full PBR manually per packet via XDP/eBPF — out of platform
  scope, unnecessary complexity.

## D5. Kill switch, DNS leak prevention, fail-closed behavior

**Decision**:
- Kill switch default state = disabled (clarification Q2-B). Opt-in toggle
  persisted in UCI, restored on boot BEFORE service start re-opens egress:
  the boot sequence enforces containment first (if enabled), then activates
  the profile.
- Kill switch implementation: nft rules drop all egress of tunneled-marked
  traffic (default-mark or explicitly policy'd) to non-tunnel destinations
  until the tunnel interface is up AND health-verified; re-open happens in
  the same atomic nft transaction that activates the tunnel.
- DNS: dnsmasq guest instance bound to the tunnel interface's DNS (or
  provider DNS over tunnel) with `no-resolv`; nft redirect rule forces
  every port-53 UDP/TCP query from tunneled-marked clients to the tunnel
  resolver. Bypass-marked domains follow the DIRECT path (their DNS goes via
  WAN resolver, preventing broken sites behind banking/local IPs).
- Failure of ANY state change ⇒ fail closed (last-good policy retained or
  containment mask applied); never fail open.

**Rationale**: Matches Principle IV verbatim and the clarification answers
(opt-in kill switch, tunnel-default steering). The bypass-DNS nuance is
required for usability: users want banking sites to work over WAN while
tunneled traffic stays private.

**Alternatives considered**: pre-up/down hooks inside OpenVPN/strongSwan
(provider-specific, harder to keep atomic across both backends); always-on
kill switch (rejected by clarification Q2-B).

## D6. Failover & cool-down retry (clarification Q3)

**Decision**: Watchdog state machine per active profile: detect failure via
interface-loss event, DPD/daemon exit, or failing health probe (2 consecutive
probe misses). After the configured attempt count (default 3, UCI
`failover.attempts`) fails on the current profile, deactivate it, advance to
the next profile in list order; after one FULL cycle with zero successes,
enter cool-down (default 300 s, UCI `failover.cooldown`) and then re-attempt
another complete cycle; repeat indefinitely. Eliminates the infinite fast
switch loop from Edge Case 1 while satisfying FR-026 auto-resume. Kill-switch
containment applies the entire time whenever enabled.

**Rationale**: Direct operationalization of FR-008 + FR-026 + clarification
Q3-A; the bounded cycle + cool-down structure is precisely what prevents the
WAN-outage infinite loop while remaining zero-touch.

**Alternatives considered**: infinite rapid retries (rejected — infinite
loop); single cycle then manual-only (rejected — clarification chose A).

## D7. Dual UI, API transport, sessions (Principle V, clarification Q4)

**Decision**:
- Standalone dashboard = static SPA served by uhttpd from
  `/www/saadway/` (Persian-first RTL, embedded fa/en i18n JSON files, FR-027).
  It talks ONLY to `/ubus` (uhttpd-mod-ubus) using JSON-RPC: dashboard
  session = ubus session token (created via `session.login`, ACL-limited by
  `rpc-acl.json`, expiry on inactivity per UCI, no reuse across devices).
- Authentication mode: dedicated dashboard credentials stored as a salted
  hash (never plaintext) in a 0600 file; system-root authentication is the
  documented alternative accepted at login (config-selectable). Default =
  dedicated credentials, prompted at first dashboard visit.
- Live data: polling model — `saadway stats get` (counters), `saadway logs
  tail` (cursor-based syslog tail), both at ≥1 Hz from the SPA; no
  server-push required (keeps uhttpd thread pool small → non-blocking UI,
  FR-018). Canvas-based chart (vanilla JS, self-hosted, no CDN).
- LuCI page = `luci-app-saadway` client-side JS view: reads service status,
  toggles enabled/disabled via ubus `saadway.mgmt.set` (through LuCI's own
  authenticated ubus path), renders an Open Dashboard button. No settings
  exposed there (FR-022). **Superseded in part by D10** (2026-10-21): the
  LuCI surface now also carries the dashboard's own login-credential form
  (constitution v1.1.0, Principle V).

**Rationale**: uhttpd-mod-ubus is stock OpenWrt infra (build-time task:
verify package presence in the official 25.12 feed — `ubus` over HTTP with
session tokens is its purpose). Reusing ubus sessions satisfies
"secure session tokens" + "root authentication alternative" in FR-020
without custom auth code; ACL gating enforces Principle V's
"exclusively authenticated APIs". Polling (instead of SSE/WebSocket)
minimizes footprint and works with uhttpd's model.

**Alternatives considered**: custom CGI/auth backend in C (duplicates ubus
session machinery); SSE via custom uhttpd plugin (heavier, blocks threads);
bundling a JS framework/build toolchain (violates footprint spirit of
Principle II and adds build complexity).

## D8. State, backup, recoverability (Principle VI, SC-004/006)

**Decision**:
- UCI `/etc/config/saadway` is the ONLY persistent state (profiles,
  steering lists incl. domain/address/device rules, service enable state,
  active-profile pointer, watchdog tunables, dashboard prefs).
- Secrets live under `/etc/saadway/` with 0600 permissions (auth-user-pass
  files, swanctl EAP secrets, imported `.ovpn` private key material).
  They are referenced by name (never inlined) from UCI.
- Backup = single JSON, schema v1 (contracts/backup-schema.json), holding
  UCI-equivalent state plus REQUIRED credential blobs (marked and
  password-protected export offered as optional mode). Export default:
  full restore capable, since SC-004 requires 100 % fidelity; exporting
  without credentials yields per-item "needs-attention" results on import
  (FR/SC alignment with spec User Story 6 scenario 4).
- Import validates schema version, dedupes, reports per-item results, and
  applies via the same atomic policy pipeline.
- Boot: init sequence = enforce containment (if kill switch on) → apply
  steering/kill/marks → start active profile → watchdog resumes. State
  restoration precedes client egress (Edge Case 4).

**Rationale**: Single source of truth + UCI-first guarantees state survives
reboot, avoids drift, and directly supports the 24-h uptime recovery
requirement SC-006.

**Alternatives considered**: custom state DB in /etc (drift risk, Principle
VI violation); excluding secrets from backup unconditionally (breaks 100 %
restore fidelity demanded by SC-004).

## D9. Testing strategy

**Decision**: three tiers —
1. **Host unit tests**: cmocka harness over daemon modules (policy renderer,
   backup codec, failover state machine) compiled in CI with the musl
   toolchain; `shellcheck` on all ash scripts.
2. **On-device smoke tests** (`tests/*.sh`): scripted scenarios driving the
   seven constitution quality gates (switching timing, leak counters, block
   gate for FR-018, restore round-trip). Build-time task: verify on 25.12
   release images for real hardware targets.
3. **Interactive quickstart** (quickstart.md): manual E2E validation script
   mirroring the spec's acceptance scenarios.

**Rationale**: Feasible without heavy CI infra; maps directly onto
constitution Section 3 gates 1–7.

**Alternatives considered**: pytest-driven device farm — banned runtime not
required and adds infra; skipping on-device layer — unacceptable for gate 4/5
(nftables/leak behavior cannot be host-tested meaningfully).

## D10. Dashboard credential lifecycle & the LuCI credentials tab (Principle V, amendment v1.1.0)

**Decision**:
- The standalone dashboard's login identity is a **dedicated system user**
  whose secret lives ONLY in the restricted store (0600), referenced from
  the platform's own login configuration through the `$p$<name>` bridge.
  Upstream resolves that suffix with `getspnam(<name>)` and compares the
  candidate through `rpc_login_test_password()`
  (`openwrt/rpcd` `session.c:832-858`) — so the reference NAMES the account
  and the secret never travels through a world-readable configuration
  option (FR-021, Constitution IV).
- The **default credential is initialized automatically** during
  installation/first start, idempotently: a fresh installation must accept
  the documented default with **zero** manual steps (FR-028, SC-009), and
  re-initialization MUST NOT overwrite a credential the administrator has
  already changed. The secret is applied **over stdin** (the platform's
  `chpasswd` reads `user:password` from stdin; `passwd` and `cryptpw` are
  the fallbacks) because a command argument is readable through
  `/proc/<pid>/cmdline`.
- The **LuCI page gains a second, strictly bounded tab** — "Dashboard
  Credentials" / تنظیمات ورود به داشبورد — carrying the new username, the
  new password, the confirmation and a *Save & Apply* action (FR-029 →
  FR-032). This is the single bounded exception to the dual-UI decoupling
  rule and is recorded as **constitution v1.1.0** (Principle V + quality
  gate 6, amendment log entry of 2026-10-21). The standalone dashboard's
  API surface, its session model and its "no configuration UI" split are
  unchanged: the credential form manages the dashboard's OWN login, not the
  service's configuration.
- The form is authorized by the **platform panel's own administrative
  session** (FR-034); it introduces no credential of its own, never returns
  the stored secret (FR-033) and rejects invalid input without changing
  anything (FR-031).

**Rationale**: the administrator could otherwise neither replace the
default credential nor initialize it from the panel — the only path was a
shell step, and a fresh installation consequently rejected the documented
default with "wrong username or password". The credential is a lifecycle
concern of the dashboard's login, so keeping its single form in the panel
does not erode the decoupling the standalone dashboard exists for.

**Implementation-level facts (verified 2026-10-21, upstream is the
oracle):**
- OpenWrt ships rpcd's plugins as **separate packages**
  (`package/system/rpcd/Makefile` → `BuildPlugin`): `rpcd-mod-file`,
  `rpcd-mod-rpcsys` ("ubus calls for sysupgrade and **password changing**"),
  `rpcd-mod-iwinfo`, `rpcd-mod-ucode`. The stock `rpcd` binary contains
  none of them → the design must not depend on a plugin being installed.
- `rpcd-mod-rpcsys`'s `rpc-sys password_set` (`openwrt/rpcd` `sys.c:81-141`)
  takes `{ user, password }`, **forks `/bin/passwd <user>` and writes the
  password to the child's stdin** (never argv; 100 ms read timeout) — the
  platform's `passwd` does the crypt(3) hashing into the 0600
  `/etc/shadow`. Our helper follows the same mechanism, so no hashing
  logic exists in our code at all.
- rpcd's `file` plugin exposes `exec`, gated per path by the ACL scope
  `"file"` (`file.c` → `rpc_file_access(sid, path, "exec")`; fnmatch
  matching in `session.c`). **Not used**: granting `exec` re-opens the
  historical LuCI remote-command surface for no gain.
- LuCI's tab strip is `ui.tabs` (`ui.js:4614-4700`, `data-tab` /
  `data-tab-title` panes) and its form engine supports tabs natively
  (`form.js:988-1110`: `tab(name, title, description)` +
  `taboption(tabName, class, …)`); a `form.Map` renders the standard
  **"Save & Apply"** action.

**Alternatives considered**: managing the credential inside the standalone
dashboard (would put a credential-change surface behind a credential, and
it contradicts the dual-UI split); keeping a shell-only path (reproduces
the reported failure mode); storing the secret in the service's own
configuration (violates FR-021 / Constitution IV — the platform already
provides a restricted, non-readable store); a literal hash in
`/etc/config/rpcd` (world-readable credential — rejected); `openssl passwd
-1` (new dependency + MD5 — rejected); a browser-side UCI write (no
crypt(3) in the browser, non-atomic — rejected); rpcd's `file.exec`
(re-opens the LuCI remote-command surface — rejected); `rpcd-mod-rpcsys`'s
`password_set` for the browser path (an extra package dependency and a
split, non-atomic flow — the password would change before the login
mapping).

## D11. Standalone dashboard without a login (Principle V, amendment v1.2.0)

**Decision**: the standalone dashboard at `/saadway/` performs **no**
credential exchange. Its SPA calls `POST /ubus` with rpcd's default
(unauthenticated) session id (`"00000000000000000000000000000000"`) and is
authorized through a dedicated rpcd access group named `unauthenticated`
in `saadway/files/rpc-acl.json`, which covers the `saadway` ubus object
only. The dashboard's access control is the platform's network boundary:
the LAN-bound uhttpd listener written by
`saadway/files/uci-defaults/40-saadway-uhttpd` plus the firewall's WAN
input policy.

**Why** (upstream facts, 2026-10-21 — upstream is the oracle):
- `openwrt/rpcd` `include/rpcd/session.h:35` — the all-zero id **is**
  rpcd's default session (`RPC_DEFAULT_SESSION_ID`), so it needs no login.
- `openwrt/rpcd` `session.c:1396-1400` — the default session is created
  once at startup with `rpc_login_setup_acls(ses, NULL)`; a NULL login
  section means `session.c:944-946` grants **only** the group named
  `unauthenticated` ⇒ the group's *name* is the authorization mechanism.
- `openwrt/rpcd` `session.c:606-660`/`:1414` — the check is (scope, object,
  function) with fnmatch matching, so `"saadway": ["*"]` covers the whole
  object.
- `openwrt/uhttpd` `ubus.c:820-840`/`:280-297` — every `/ubus` call is
  checked through rpcd's `session.access` with `params[0]` as the session
  id, so a tokenless call lands on the default session's grants.

**Alternatives considered**: uhttpd's `ubus_noauth` (skips the ACL check
for **every** ubus object — far too broad; rejected); keeping the SPA's
login (withdrawn by the maintainer directive of 2026-10-21; rejected);
granting the `unauthenticated` group an object other than `saadway` (the
dashboard's blast radius must stay inside its own object; rejected);
shipping a well-known default password with no consumer (a liability —
rejected: the first-boot path no longer provisions one).

## Open questions for build-time verification (tracked, all low-risk)

- Confirm `uhttpd-mod-ubus` package availability and ACL syntax in the
  official OpenWrt 25.12 feeds (task T-verify-1).
- Confirm `strongswan-mod-gcm`/`kmod-xfrm-interface` naming/versions in 25.12
  (task T-verify-2).
- Confirm fw4 include-file extension strategy in 25.12 (task T-verify-3).
