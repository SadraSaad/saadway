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
`/etc/sadway/` (credentials, imported .ovpn private material); portable
backup artifact = single JSON file (schema v1, see contracts). The
standalone dashboard's own login secret lives ONLY in the platform's
0600 `/etc/shadow` and is referenced — never copied — from
`/etc/config/rpcd` as `option password '$p$<user>'` (see the amended design
below: no credential material in any world-readable file).

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
| V. Strict Dual-UI Decoupling | ✅ PASS (amended v1.1.0) | research.md D7 (uhttpd-mod-ubus authenticated JSON-RPC only; the standalone dashboard keeps every system interaction behind an authenticated API) + D10 (the LuCI surface gains ONE bounded exception — the dashboard's own login-credential form; constitution amendment log 2026-10-21); contracts/api.md |
| VI. Single Source of Truth & Recoverability | ✅ PASS | research.md D8 (UCI is sole persistent state; backup JSON v1 round-trip); data-model state transitions |
| Section 2 platform bans (runtimes/tooling blacklist) | ✅ PASS | Technical Context dependency list contains no banned runtime |
| Section 3 gates 1–7 (traceability) | ✅ PASS (design level) | quickstart.md maps each gate to a runnable on-device scenario |
| IV-a. Credential material confinement (amendment 2026-10-21) | ✅ PASS | The login secret never enters a world-readable file: the UCI option stays a `$p$<user>` reference and the hash stays in the 0600 `/etc/shadow`; the plaintext is piped to the platform's `/bin/passwd` (stdin, never argv) exactly as upstream's `rpcd-mod-rpcsys` does; the new ubus method is granted to a dedicated root-only ACL group, so the dashboard's own session can never re-key its login |

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
│   ├── creds.c              # dashboard credential apply: validate + relay
│   │                         # to the root-only provisioning helper (stdin)
│   ├── backup.c             # JSON export/import (schema v1)
│   ├── log.c                # syslog writer with secret scrubbing
│   └── unit/                # host-side unit tests (cmocka harness)
└── files/
    ├── saadway.init         # procd service definition
    ├── saadway.conf         # UCI defaults
    ├── core.nft             # fw4/nftables include templates
    ├── rpc-acl.json         # ubus ACL for dashboard sessions
    ├── saadway-provision     # root-only credential helper (set/passwd/unset)
    └── htdocs/saadway/      # standalone dashboard SPA (vanilla JS)
        ├── index.html
        ├── css/             # RTL-first styles, Persian default
        ├── js/              # API client, views, canvas charts, i18n (fa/en)
        └── i18n/            # fa.json, en.json dictionaries

luci-app-saadway/            # LuCI app package (status page + credential tab)
├── Makefile
├── htdocs/luci-static/resources/view/saadway/
│   └── dashboard.js         # LuCI form.Map with two tabs: Status & Control
│                             # (status, enable/disable, open link) and
│                             # Dashboard Credentials (username/password/
│                             # confirmation + Save & Apply)
└── root/usr/share/rpcd/acl.d/
    ├── luci-app-saadway.json  # the view's own session grants (mgmt.status/
    │                         # mgmt.set) — NOT the credential method
    └── saadway-credentials.json  # root-only grant: exactly one method
                              # (saadway.mgmt.credentials.set)

tests/                       # on-device smoke & integration scripts
├── smoke-switching.sh       # gate 4/5/7 helpers
├── smoke-container.sh       # block-gate: UI liveness with dead test target
├── leak-check.sh            # zero-leak packet counter under switch/drop
├── restore-roundtrip.sh     # backup export→import fidelity
├── credential-flow-audit.sh # static audit of the credential contract
│                             # ($p$ reference, stdin piping, ACL scoping)
├── dashboard-credential-audit.sh  # the default-credential contract audit
├── link-audit.sh            # daemon-internal symbol resolution
├── pkg-metadata-audit.sh    # OpenWrt package metadata contract
├── service-startup-audit.sh # procd API surface + no hardcoded socket path
└── uci-defaults-audit.sh    # uhttpd listener contract
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

---

## Amended Design — Dashboard Authentication & LuCI Credential Management (2026-10-21)

**Trigger**: the maintainer's request (a) a reliably initialized default
dashboard credential (`admin` / `admin`) and (b) a second, strictly bounded
LuCI tab for changing it. Spec: FR-022 (amended), FR-028 → FR-034,
SC-009 → SC-011; decision record: research.md D10; constitution: v1.1.0
(Principle V + quality gate 6 — the dashboard's own login form is the single
bounded exception).

### Upstream facts (verified this round — upstream is the oracle)

| Fact | Source |
|---|---|
| rpcd's `rpcsys` plugin (package **`rpcd-mod-rpcsys`**, "ubus calls for sysupgrade and **password changing**") exposes the object `rpc-sys` with the method **`password_set`**, policy `{ "user": STRING, "password": STRING }` | `openwrt/rpcd` `sys.c` → `rpc_cgi_password_set()` |
| `password_set` **forks `/bin/passwd <user>` and writes the password to the child's STDIN** (never argv) with a 100 ms read timeout; the platform's `passwd` performs the crypt(3) hashing into the 0600 `/etc/shadow` | `openwrt/rpcd` `sys.c:81-141` |
| rpcd's plugins ship as **separate packages** (`rpcd-mod-file`, `rpcd-mod-rpcsys`, `rpcd-mod-iwinfo`, `rpcd-mod-ucode`) — the stock `rpcd` binary contains none of them | `openwrt/openwrt` `package/system/rpcd/Makefile` (`BuildPlugin`) |
| rpcd's `file` plugin exposes `exec`, gated by `rpc_file_access(sid, path, "exec")` → the ACL scope `"file"` maps a path to permissions (fnmatch-matched) | `openwrt/rpcd` `file.c` (`rpc_file_exec`, `rpc_file_access`), `session.c` (`uh_foreach_matching_acl`) |
| A login section's `option password '$p$<name>'` is resolved with `getspnam(<name>)` and compared via `rpc_login_test_password()`; a missing shadow row fails the login (fail-closed) | `openwrt/rpcd` `session.c:832-858` |
| LuCI provides the tab strip as `ui.tabs` (`initTabGroup`, `data-tab` / `data-tab-title` panes) | `openwrt/luci` `modules/luci-base/.../ui.js:4614-4700` |
| LuCI's form engine supports tabs natively: `tab(name, title, description)` registers a tab and `taboption(tabName, class, ...)` places an option in it; a `form.Map` renders the standard **"Save & Apply"** action | `openwrt/luci` `modules/luci-base/.../form.js:988-1110` |
| OpenWrt's BusyBox carries `passwd`, `chpasswd` (reads `user:password` from stdin) and `cryptpw` | `openwrt/openwrt` `package/utils/busybox/config/loginutils/Config.in` |

### Architectural directives — evaluation

| Directive | Verdict | Rationale |
|---|---|---|
| A dedicated `config login` section with `username 'admin'`, `list read/write 'saadway'`, maintained idempotently | **Accepted** | The section is named **`saadway_login`** per the directive; rpcd matches a login section by its `username` option, so the section name is cosmetic. The ACL lists stay `saadway` (least privilege) |
| `option password '<crypt/md5/sha512 hash>'` inside the UCI section | **Rejected** | `/etc/config/rpcd` is world-readable (0644) → a hash stored there is a world-readable credential (FR-021, Constitution IV; the same reasoning already recorded in docs/verify-2512.md §2). The upstream-sanctioned path is the **`$p$<user>` shadow bridge**: the hash stays in the 0600 `/etc/shadow`, the UCI option stays a *reference* (`option password '$p$<user>'`) |
| `openssl passwd -1 admin` (MD5-crypt) | **Rejected** | `openssl-util` is not a stock package (a new runtime dependency — Principle II) and MD5-crypt is weak. The platform's own `/bin/passwd` (BusyBox, crypt(3)) is the upstream-native mechanism — byte-for-byte the same approach `rpcd-mod-rpcsys` uses |
| The LuCI view renders standard tabs | **Accepted** | `ui.tabs` + the form engine's `tab()` / `taboption()` are upstream LuCI APIs; a `form.Map` yields the standard **"Save & Apply"** action for free |
| The tab submits "via ubus RPC or UCI binding" | **Accepted (ubus)**, UCI binding rejected | A browser-side UCI write cannot hash the password (no crypt(3) in the browser) and would leave a half-applied state (the login section updated before the shadow row exists → fail-closed lockout). One atomic ubus call keeps the shadow row and the login section in lockstep |
| Hash the password with the OpenWrt crypt mechanism | **Accepted** | Via `/bin/passwd` — the platform's own, driven exactly like `rpcd-mod-rpcsys`: the plaintext is written to the child's **stdin**, never to argv |
| Call `/etc/init.d/rpcd reload` to activate the new credentials | **Accepted** | The browser cannot execute commands (the `file` plugin's `exec` is deliberately **not** used — it re-opens the historical LuCI remote-command surface), so the reload is issued by the service that performs the change; rpcd re-reads `/etc/config/rpcd` on reload, so the new credential is effective immediately (FR-032) |

### Data flow

```text
LuCI view (root panel session)
  │  form.Map → tab 'credentials' → Save & Apply
  ▼
ubus  saadway.mgmt.credentials.set { username, password }
  │  (ACL group 'saadway-credentials' — the dashboard's own 'saadway'
  │   session does NOT have it; the stock root session does, via '*')
  ▼
saadwayd  (saadway/src/creds.c)
  1. validate: username charset/length, password non-empty + length bounds
  2. spawn /usr/libexec/saadway/provision set-credentials <username>
     with the password on the child's STDIN (never argv, never logged)
  3. await the exit status → reply { } (status only, never the secret)
  ▼
/usr/libexec/saadway/provision  (root-only helper, extended)
  a. create the shadow account if missing (busybox adduser, nologin, uid 355)
  b. set the password: chpasswd ← stdin | passwd ← stdin | cryptpw + staged
     0600 /etc/shadow rewrite (fail closed if no setter exists)
  c. update /etc/config/rpcd: section 'saadway_login' → username=<new>,
     password='$p$<new>', list read/write 'saadway'  → uci commit
  d. /etc/init.d/rpcd reload
```

**Why the daemon relays and does not own the logic**: one implementation of
the account/shadow/UCI handling stays in the audited provisioning helper
(no duplicated shell logic in C, no drift); the daemon only validates,
relays the secret once over a pipe, and reports the outcome. The daemon
never **stores** a credential and never computes a hash — the platform's
`passwd` does both. (This refines research.md D7: "the daemon keeps no
credential store of its own" → "the daemon never stores a credential; it
relays a new secret exactly once, over a pipe, to the root-only helper".)

**Default-credential initialization (FR-028)**: the first-boot helper
(`/etc/uci-defaults/50-saadway-rpcd` + the provisioning helper) creates the
account and applies the default password **only when the account does not
exist yet** → idempotent, and a credential the administrator has already
changed is never overwritten (re-running the first-boot script on an
upgrade is a no-op for the credential).

**ACL (least privilege)**: the new method lives in a **new ACL group**
(`saadway-credentials`) that grants exactly one ubus method. The
standalone dashboard's session (group `saadway`) does **not** obtain it; the
router's stock root session does (group `*`). The dashboard can therefore
never re-key its own login.

### Failure modes (fail closed)

| Condition | Behaviour |
|---|---|
| Password mismatch in the form | Nothing is sent; the form reports it inline (FR-031) |
| Empty username / password, or a username with illegal characters | Rejected before any router-side action (FR-031) |
| The account cannot be created, or no password setter exists | The helper exits non-zero; the daemon reports the failure; **the stored credential is left untouched** |
| The password cannot be applied | The helper exits non-zero **before** touching the login section → the old credential still works (no lockout) |
| The UCI update or the rpcd reload fails | The helper exits non-zero and reports it; the daemon surfaces the error — the change is never reported as successful when it did not happen |
| An unauthorized session calls the method | The ACL denies it (ubus PERMISSION_DENIED) — the dashboard session has no grant |

### Test strategy

* **Host-side** (build host, no device): a new `tests/credential-flow-audit.sh`
  proving the *static* contract — the login section carries `'$p$<user>'`
  and never a literal hash, the helper pipes the password (no password
  literal on an applet command line), the first-boot path only initializes
  when the account is absent, the new ACL group grants exactly one method
  and is not part of the dashboard's `saadway` group.
* **Unit** (host): the validation function of `creds.c` (username charset,
  password bounds) in the cmocka harness.
* **On-device** (T046/T057 round): a fresh install accepts `admin`/`admin`
  with zero manual steps; the LuCI tab changes the credential; the
  dashboard accepts the new values and rejects the old ones within 5 s; the
  `/etc/shadow` row is a hash in a 0600 file; `/etc/config/rpcd` holds no
  credential material; the dashboard session cannot call the credential
  method.

### Complexity Tracking (this amendment)

* **+1 daemon module** (`creds.c`, ~200 lines) — validation + a supervised
  child call; no new library, no new package dependency, no new runtime.
* **+1 ACL group / file** — one method, root-only by construction.
* **+1 helper mode** (`provision set-credentials`) — the account/shadow/UCI
  logic already lives there; no second implementation.
* **Rejected alternatives** (evaluated, not chosen): the `file` plugin's
  `exec` (re-opens the historical LuCI remote-command surface); a browser-side
  UCI write (no crypt(3), non-atomic, partial state); a literal hash in
  `/etc/config/rpcd` (world-readable credential — FR-021); `openssl passwd`
  (new dependency + MD5); `rpcd-mod-rpcsys`'s `password_set` for the browser
  path (an extra package dependency and a split, non-atomic flow — the
  password would change before the login section).
