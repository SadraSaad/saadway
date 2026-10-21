# Feature Specification: SaadWay VPN Tunnel & Routing Dashboard

**Feature Branch**: `001-saadway-vpn-dashboard`

**Created**: 2026-10-19

**Status**: Draft

**Input**: User description: Design the complete specification for the SaadWay package and management dashboard for tunnel and routing control on OpenWrt 25.12 routers, providing secure, stable, and intelligent steering of local network traffic through OpenVPN and IKEv2 servers.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Multi-Profile OpenVPN & IKEv2 Tunnel Management (Priority: P1)

A router administrator manages all VPN/proxy connections as named
profiles. They can import an `.ovpn` config file directly, or create an
IKEv2 profile by filling a form (no file import required). They store
several profiles and activate any of them with a single click; the
activated profile's traffic begins flowing through that provider's
tunnel. If the active connection drops and does not recover after a
configurable number of failed attempts, the system automatically
deactivates it and switches to the next profile in the list without the
administrator being at the router.

**Why this priority**: Establishing and switching encrypted tunnels is
the core product promise; nothing else works without a connectivity
layer. The auto-failover makes the system resilient, which is the main
reason an administrator installs this package.

**Independent Test**: Can be fully tested by importing one OpenVPN
profile, creating one IKEv2 profile from the form, activating each in
turn, and observing encrypted traffic established both times. Failover
is tested independently by interrupting the active tunnel and observing
the automatic switch to the second profile.

**Acceptance Scenarios**:

1. **Given** the dashboard is open and no OpenVPN profile exists,
   **When** the administrator imports a valid `.ovpn` file, **Then** the
   profile appears in the profile list ready to be activated.
2. **Given** an OpenVPN profile requiring credentials, **When** the
   administrator opens its settings form, **Then** a section titled
   *"Section to add an optional 'auth-user-pass' file with your
   credentials"* is present where they can optionally enter a username
   and password attached to the profile.
3. **Given** the dashboard profile form, **When** the administrator
   creates an IKEv2 profile entering Hostname, Username, Password and
   accepting the specified connection settings (Type: IKEv2 · username
   auth (MSCHAPv2); Remote ID set to "Same as hostname"; Local ID set to
   "Same as username"; Certificate: "Leave default · from server";
   Proxy: Off) and encryption settings (Negotiated automatically based
   on AES-256-GCM; Cipher: AES-256-GCM; Integrity: SHA-256; Key exchange:
   DH group 20 (ECP384); iOS key exchange: DH group 21 (ECP521)),
   **Then** the profile is saved with those values and can be activated
   without any file import.
4. **Given** two or more saved profiles with exactly one active,
   **When** the administrator clicks a different profile to activate it,
   **Then** the new profile becomes active within 10 seconds and the
   previously active one is fully deactivated.
5. **Given** an active connection that repeatedly fails or times out,
   **When** the configured number of failed attempts is reached, **Then**
   the current profile is deactivated automatically and the next
   available profile is activated without manual intervention.

---

### User Story 2 - Zero-Leak Security Assurance (Priority: P1)

While any tunnel profile is active, the administrator trusts that the
network's traffic cannot escape unencrypted. A kill switch capability,
when enabled, blocks all tunneled-client traffic from leaking through
the main WAN the moment the OpenVPN or IKEv2 connection drops, until the
tunnel is re-established. All DNS lookups (port 53) of tunneled clients
are forced through the active tunnel so the ISP cannot observe which
domains are being visited. Switching between profiles or losing the
connection never lets packets travel outside the defined policy, not
even briefly. Credentials (usernames, passwords, private keys, session
tokens) are stored with restricted access and never shown in live logs
or open API responses.

**Why this priority**: The product's core value is privacy and
safety. A tunnel that leaks traffic unknowingly would be worse than no
tunnel; this is a non-negotiable promise bundled with connectivity.

**Independent Test**: Can be fully tested by enabling the kill switch,
actively dropping an established tunnel, and confirming zero client
packet egress through WAN; DNS behavior is tested by observing client
DNS resolution path while the tunnel is active.

**Acceptance Scenarios**:

1. **Given** the kill switch is enabled and a tunnel is established,
   **When** the tunnel drops unexpectedly, **Then** traffic from all
   tunneled clients is contained (0 leaking packets) until the tunnel is
   restored.
2. **Given** a tunnel is active and the kill switch enabled, **When** the
   administrator or system switches to a different profile, **Then** at
   no point during the transition does tunneled-client traffic bypass
   the encrypted path.
3. **Given** tunneled clients are resolving domain names, **When** any
   DNS request is made while a tunnel is active, **Then** the request is
   handled from inside the tunnel, not toward the ISP directly.
4. **Given** a profile with stored credentials, **When** the
   administrator views live logs or calls the dashboard without
   authentication, **Then** no username, password, private key, or
   session token is ever displayed.

---

### User Story 3 - Smart Traffic Steering & Bypass (Priority: P2)

The administrator decides exactly what goes through the tunnel and what
stays on the direct WAN. They can register a list of domains and
subdomains that must always open directly from the main internet
(never through the proxy tunnel), toggle a checkbox for automatically
bypassing local/country and banking IP ranges, and assign per-device
policies for every client on the LAN — by local IP or MAC address — so
that, for example, a smart TV or a card payment terminal keeps using the
ordinary internet while a work laptop uses the tunnel.

**Why this priority**: After basic connectivity, steering is the highest
everyday value: it keeps banking, local services, and chosen devices on
the direct path while the rest of the network benefits from the tunnel.

**Independent Test**: Can be fully tested by adding a bypass domain,
enabling the IP-range bypass checkbox, and assigning one device to the
tunnel while another stays direct; each steering decision is verified by
observing which path the respective traffic takes.

**Acceptance Scenarios**:

1. **Given** the dashboard bypass section, **When** the administrator
   adds a domain (e.g. `example.com`), **Then** that domain and its
   subdomains resolve and load directly via the main WAN, not through
   the tunnel.
2. **Given** the bypass section, **When** the administrator enables the
   local/country & banking IP-range auto-bypass checkbox, **Then**
   traffic to those IP ranges goes out directly without the
   administrator entering each site manually.
3. **Given** two LAN devices, **When** the administrator sets one
   device's policy (by local IP or MAC address) to "tunneled" and the
   other to "direct", **Then** the first device's traffic travels
   through the active tunnel and the second device's traffic uses the
   ordinary internet concurrently.
4. **Given** any steering configuration change, **When** the change is
   applied, **Then** it takes effect without disconnecting the tunnel or
   briefly exposing tunneled traffic to WAN.

---

### User Story 4 - Dual-UI Experience: LuCI Controller + Standalone Dashboard (Priority: P2)

The administrator gets two complementary user interfaces. A simple page
inside the router's LuCI panel is tabbed: the *Status & Control* tab shows
the service running status, lets them turn the service on/off
(enable/disable), and offers a button that opens the dedicated dashboard;
the *Dashboard Credentials* tab (تنظیمات ورود به داشبورد) lets them set the
dashboard's own login username and password without leaving the panel. The
standalone web dashboard — completely independent of the LuCI look/feel and
templates — hosts all proxy settings, routing rules, live monitoring, and
maintenance tools, and is reachable on the LAN **without any credential
prompt** — its access control is the platform's network boundary (the
LAN-bound uhttpd listener plus the firewall's WAN input policy), so the
administrator opens it and works immediately: no login, no session token, no
manual provisioning step.

**Why this priority**: The dual-UI structure shapes the whole delivery:
a thin, stable control surface for routine use, and a full modern UI for
advanced work. It is needed to reach every other story's workflows.

**Independent Test**: Can be fully tested by visiting the LuCI page and
verifying that exactly two tabs are offered — status/toggle/dashboard-link
on the first, the credentials form on the second — then opening the
standalone dashboard: it MUST show the full interface immediately (no
credential prompt, no session token) and every panel action MUST succeed; a
fresh installation MUST be operable with 0 manual steps; the dashboard MUST
be unreachable from the WAN side; and a credential change made in the LuCI
tab MUST be accepted by rpcd's `session.login` immediately.

**Acceptance Scenarios**:

1. **Given** the router's LuCI panel, **When** the administrator opens
   the SaadWay page, **Then** they see exactly two tabs — *Status &
   Control* (the service status, an on/off control and a button that opens
   the standalone dashboard) and *Dashboard Credentials* (the login-credential
   form) — and nothing else.
2. **Given** the LuCI page, **When** the administrator toggles service
   enable/disable, **Then** the service state changes accordingly and
   the LuCI page reflects it.
3. **Given** the standalone dashboard URL on the LAN, **When** a client
   opens it, **Then** the dashboard renders immediately — no credential
   prompt, no session token — and its ubus calls succeed through rpcd's
   default (unauthenticated) session.
4. **Given** the LAN-scoped dashboard, **When** the administrator opens
   it, **Then** the access control is the network boundary: the dashboard
   is served only on the LAN-bound listener, is unreachable from the WAN
   side, and no session token is created or required.
5. **Given** a fresh installation, **When** the administrator opens the
   standalone dashboard, **Then** the dashboard is fully operable — no
   credential and no manual provisioning step is required beforehand.
6. **Given** the LuCI page, **When** the administrator opens the
   *Dashboard Credentials* tab, **Then** they see exactly three input
   fields — new username, new password, password confirmation — plus a
   *Save & Apply* action, and the stored password is not shown.
7. **Given** the credentials tab filled with a new username and a matching
   new password and confirmation, **When** the administrator saves, **Then**
   rpcd accepts the new credential immediately and rejects the previous one
   (the optional rpcd session credential — the dashboard itself performs no
   login any more, FR-020).
8. **Given** the credentials tab filled with a new password whose
   confirmation does not match, **When** the administrator saves, **Then**
   nothing is changed and an inline message explains the mismatch.

---

### User Story 5 - Live Health & Bandwidth Monitoring (Priority: P2)

The administrator verifies and watches the tunnel in real time. A
health-check button runs two layered tests from inside the active
tunnel interface — a simple ICMP ping to `8.8.8.8` or `1.1.1.1`, and a
light HTTP GET to `http://cp.cloudflare.com/generate_204` — returning
exact latency in milliseconds or a Timeout status. The dashboard main
page shows a real-time graph of download and upload throughput of the
active tunnel. A small live logs panel streams connection errors (e.g.
authentication failure, timeout) for fast troubleshooting. All
monitoring keeps the dashboard and the service responsive at all times
— a slow or unreachable health-check target never freezes the UI or
blocks the service.

**Why this priority**: Monitoring converts an opaque black box into a
trustworthy product — administrators can prove the tunnel works, see
its real throughput, and self-diagnose failures without support calls.

**Independent Test**: Can be fully tested by clicking the health-check
with an active tunnel and reading the latency/timeout result, watching
the bandwidth graph update in real time, and observing the live log
panel while simulating an authentication error.

**Acceptance Scenarios**:

1. **Given** an active tunnel, **When** the administrator clicks the
   health-check button, **Then** both the ping result (latency in ms or
   Timeout) and the HTTP GET result (success/204 response or Timeout)
   from inside the tunnel are displayed.
2. **Given** an active tunnel, **When** clients are transferring data,
   **Then** the dashboard shows a continuously updating graph of the
   tunnel's live download and upload rates.
3. **Given** a connection failure occurs (e.g. wrong credentials, or ISP
   timeout), **When** the failure is logged, **Then** the related log
   line appears in the live logs panel without page refresh.
4. **Given** a health-check target that is unreachable, **When** the
   test runs, **Then** it finishes with a Timeout result within the
   allowed window while the dashboard remains fully interactive.

---

### User Story 6 - Backup, Restore & Boot Resilience (Priority: P3)

The administrator can export everything — all profiles, routing rules,
and bypass lists — into a single standard JSON file, and import that
file onto the same or a different router with one action, restoring the
complete configuration. Survivability is automatic: after a sudden power
loss or reboot, the service returns to its prior state — reactivating
the profile that was active and re-applying kill-switch and steering
rules — before clients can leak traffic, with no manual reconfiguration.

**Why this priority**: Protects the administrator's setup investment and
guarantees recovery after crashes/power cuts — important, but not part
of the first-run value chain.

**Independent Test**: Can be fully tested by exporting the JSON backup
from a configured system, importing it into a freshly installed (empty)
system, and verifying every profile, rule, and bypass list is
functionally identical; boot resilience by simulating power loss during
an active tunnel and verifying state recovery after reboot.

**Acceptance Scenarios**:

1. **Given** a configured system, **When** the administrator exports a
   backup, **Then** a single JSON file containing all profiles,
   settings, and bypass lists is produced.
2. **Given** a fresh installation, **When** the administrator imports
   that JSON file, **Then** every profile (type and settings),
   routing rule, and bypass entry is restored exactly and the service
   operates with identical behavior, without errors.
3. **Given** an active tunnel and any prior configuration, **When** the
   router loses power and reboots, **Then** the same profile is
   reactivated and kill-switch/steering rules are enforced again
   automatically before client traffic leaves the router.
4. **Given** an imported backup containing a profile that does not suit
   the new router's environment, **When** restore finishes, **Then** the
   administrator is shown a clear per-item restoration result (restored
   vs. needs-attention) rather than a silent partial import.

### Edge Cases

- **WAN outage while Failover AND Kill Switch are both enabled**: when
  the main internet itself is down, the failover loop must not cycle
  endlessly through profiles (infinite switch loop). After attempting
  all available profiles or detecting persistent WAN loss, the system
  must stop switching, keep tunneled clients contained by the kill
  switch, and automatically resume when connectivity or a tunnel
  becomes possible.
- **Incomplete or syntactically invalid `.ovpn` upload**: import is
  rejected with a clear, specific error identifying the problem (e.g.
  missing remote endpoint, unsupported directive), and nothing is saved
  or activated from the broken file.
- **Duplicate domain or IP entry in the bypass list**: re-adding an
  existing domain/subdomain or IP/range is detected; the system either
  rejects it with feedback or merges it, but never stores P1 exact
  duplicates — and removing an entry cannot accidentally remove a
  distinct overlapping entry.
- **Sudden power loss while a tunnel is being established**: on the next
  boot, kill-switch containment and steering rules are restored before
  client traffic flows, and the previously active profile's tunnel is
  re-established automatically.
- **Credentials with special characters/unicode in forms** must be
  accepted and stored without corruption (masking rules must not break
  entry).
- **Dashboard access from concurrently connected devices**: multiple
  simultaneous administrator sessions must be supported without one
  session invalidating another's configuration changes mid-edit.
- **The default credential must not fight the administrator**: automatic
  initialization of the default login must never overwrite a credential
  the administrator has already changed — a reboot, an upgrade or a
  repeated initialization MUST leave the customized credential untouched.
- **A credential change while a dashboard session is open**: the session
  that is already established keeps working until it expires on its own;
  only subsequent logins use the new credential.
- **Saving the credentials form with empty or mismatching input**: an
  empty username, an empty password or a confirmation that does not match
  the new password MUST be rejected with an inline explanation, and the
  stored credential MUST stay exactly as it was.
- **A client without an administrative session reaching the credentials
  form**: the form and its action MUST be unreachable, and the stored
  credential MUST NOT be readable.

## Clarifications

### Session 2026-10-19

- Q: When a LAN device has no explicit routing rule set by the administrator, should its traffic go through the active tunnel by default, or stay on the direct internet by default? → A: Tunnel by default (Option A) — unlisted devices route through the active tunnel; per-device rules exist only to send chosen devices direct.
- Q: Should the kill switch be enabled automatically from the very first install, or stay off until the administrator turns it on? → A: Disabled by default (Option B) — leak protection stays off until the administrator explicitly enables the kill switch.
- Q: After failover has already tried every saved profile once and none connected, what should the system do next? → A: Cool-down retry (Option A) — after one full failed cycle, wait a configurable cool-down interval and re-attempt the cycle, repeating until a profile connects.
- Q: Which language should the standalone dashboard's interface ship in first? → A: Persian-first (Option A) — right-to-left layout by default with an optional English language switch in the dashboard.

### Session 2026-10-21 — dashboard authentication & LuCI credential management

- Q: May the LuCI page grow beyond status/toggle/dashboard-link? → A: Yes — a second, strictly bounded tab for the standalone dashboard's own login credential is added ("Dashboard Credentials" / تنظیمات ورود به داشبورد). Everything else stays prohibited; the dual-UI decoupling principle is amended accordingly (constitution v1.1.0, Principle V).
- Q: How is the default credential made available on a fresh installation, given that a manual provisioning step left the dashboard unreachable ("wrong username or password")? → A: The default credential is initialized automatically during installation/first start, idempotently, and never overwrites a credential the administrator has already changed.
- Q: May the administrator change the username as well as the password? → A: Yes — both are changeable from the credentials tab; the tab is the single place where the credential is managed.
- Q: Do dashboard sessions established before a credential change keep working? → A: Yes — they remain valid until they expire on their own inactivity window; only subsequent logins use the new credential.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: System MUST allow import of `.ovpn` configuration files
  directly from the dashboard, producing a ready-to-activate OpenVPN
  profile.
- **FR-002**: The OpenVPN settings form MUST include a section titled
  *"Section to add an optional 'auth-user-pass' file with your
  credentials"* where username and password may optionally be entered
  and attached to the profile.
- **FR-003**: System MUST allow creating an IKEv2 profile purely via a
  form (no file import), accepting Hostname (e.g.
  `ams-466-ike.whiskergalaxy.com`), Username, and Password.
- **FR-004**: The IKEv2 form MUST provide the specified connection
  settings with the stated defaults: Type = IKEv2 · username auth
  (MSCHAPv2); Remote ID = "Same as hostname"; Local ID = "Same as
  username"; Certificate = "Leave default · from server"; Proxy = Off —
  each field overridable by the user with "Same as …" options preserved.
- **FR-005**: The IKEv2 form MUST display encryption as automatically
  negotiated based on AES-256-GCM with the stated parameters: Cipher:
  AES-256-GCM; Integrity: SHA-256; Key exchange: DH group 20 (ECP384);
  iOS key exchange: DH group 21 (ECP521).
- **FR-006**: System MUST store multiple OpenVPN and IKEv2 profiles in a
  persistent profile list and show them all in the dashboard.
- **FR-007**: System MUST allow activating any stored profile with a
  single click, with at most one profile active at a time; switching
  MUST complete within 10 seconds.
- **FR-008**: When the active connection fails or times out, system MUST
  automatically disable it and switch to the next available profile
  after a configured number of failed attempts; the attempts value MUST
  be configurable.
- **FR-009**: System MUST provide a bypass-list section where
  administrators add domains and subdomains that MUST always open
  directly via the main WAN (never through the tunnel).
- **FR-010**: System MUST provide a single checkbox enabling automatic
  direct routing for local/country and banking IP ranges, without
  manual per-site entry.
- **FR-011**: System MUST allow per-device exit policy assignment for
  LAN clients by local IP address or MAC address, controlling whether
  each device's traffic uses the tunnel or the direct internet. The
  default policy for any device without an explicit rule MUST be the
  tunnel — unlisted traffic goes through the active tunnel — so
  per-device rules serve only as opt-outs to the direct path.
- **FR-012**: System MUST provide an optional kill switch; when enabled,
  the moment an OpenVPN or IKEv2 connection drops it MUST prevent all
  tunneled-client traffic from leaking unencrypted through the main WAN
  until the tunnel is re-established. The kill switch MUST be disabled
  by default on fresh installation and activated only by an explicit
  administrator opt-in; the chosen state MUST persist across reboots.
- **FR-013**: While a tunnel is active, system MUST direct all
  tunneled-client DNS queries (port 53) through that tunnel (DNS leak
  prevention) so the ISP cannot observe visited domains.
- **FR-014**: Profile switches and connection-state changes MUST be
  applied atomically from the network's point of view — no packet may
  leave outside the current policy during the transition.
- **FR-015**: System MUST provide a multi-layer health-test control
  that, from inside the active tunnel, performs (a) an ICMP ping to
  `8.8.8.8` or `1.1.1.1` and (b) an HTTP GET to a minimal URL
  (`http://cp.cloudflare.com/generate_204`), reporting ping latency in
  milliseconds or Timeout, and HTTP success or Timeout.
- **FR-016**: System MUST display a continuously updating real-time
  graph of download and upload throughput of the active tunnel interface
  on the dashboard main page.
- **FR-017**: System MUST provide a live log view streaming connection
  events and errors (e.g. authentication failure, timeout) without
  requiring a page refresh.
- **FR-018**: All monitoring, health tests, and log streaming MUST run
  with defined time budgets and MUST never block or freeze the dashboard
  UI or the service's main operation; a slow/unreachable test target
  MUST yield a graceful Timeout result.
- **FR-019**: System MUST export all profiles, settings, and bypass
  lists into a single standard JSON backup file, and MUST import such a
  file onto the same or another system, restoring the complete
  configuration.
- **FR-020**: The standalone web dashboard MUST be reachable and fully
  operable **without any credential prompt**: it MUST NOT implement a login
  of its own, and its ubus calls MUST be authorized as rpcd's default
  (unauthenticated) session rather than through a session token. Its access
  control is therefore delegated to the platform's network boundary — the
  LAN-bound uhttpd listener and the firewall's WAN input policy — and the
  dashboard MUST NOT be reachable from the WAN side (amended 2026-10-21; the
  previous dashboard-owned username/password requirement is withdrawn — see
  FR-035 and the deviation record in `docs/verify-2512.md` §7).
- **FR-021**: Stored authentication secrets (usernames, passwords,
  private keys, session tokens) MUST be kept with restricted system
  access and MUST NEVER be displayed in live logs, monitoring panels, or
  any API response — the API surface is now reachable without prior
  authentication (FR-020), so the "unauthenticated response" carve-out no
  longer applies (amended 2026-10-21).
- **FR-022**: A lightweight page inside the router's LuCI panel MUST
  offer exactly two tabs and nothing beyond them: **(1) Status & Control** —
  service status display, on/off enable/disable control, and a button
  opening the standalone dashboard — and **(2) Dashboard Credentials** — the
  login-credential form specified in FR-030.
- **FR-023**: The standalone dashboard MUST be a fully independent web
  panel (not built from the LuCI templates/rendering), delivering all
  proxy configuration, routing, monitoring, and maintenance functions.
- **FR-027**: The standalone dashboard interface MUST ship
  Persian-first with a right-to-left layout as the default, and MUST
  provide an optional English language switch; the selected language
  preference MUST persist for the administrator.
- **FR-024**: System MUST persist all profiles, service enabled-state,
  and bypass lists; after reboot or crash it MUST automatically return
  to the last desired state (reactivate the previously active profile,
  re-apply kill-switch and steering rules) with no user action.
- **FR-025**: System MUST validate the `.ovpn` import (rejecting
  incomplete/syntactically invalid files with a specific error and no
  partial save) and MUST detect duplicate bypass entries (rejecting or
  merging with clear feedback).
- **FR-028**: On a fresh installation the standalone dashboard MUST be
  reachable and fully operable with **0** manual provisioning steps and
  **without any credential** (FR-020). The installation MUST NOT ship a
  well-known default password: the optional rpcd session credential of the
  `saadway_login` mapping MAY be created on demand, and re-initialization
  MUST NOT overwrite a credential the administrator has already changed
  (amended 2026-10-21).
- **FR-029**: The LuCI page MUST present the two tabs of FR-022, and the
  second tab MUST be titled "Dashboard Credentials" (تنظیمات ورود به
  داشبورد).
- **FR-030**: The "Dashboard Credentials" tab MUST provide exactly three
  input fields — **New Username** (نام کاربری جدید), **New Password**
  (رمز عبور جدید) and **Password Confirmation** (تکرار رمز عبور) — plus
  one **Save & Apply** action; the password fields MUST be masked and MUST
  NOT be pre-filled with the stored secret.
- **FR-031**: The Save & Apply action MUST validate the input — a
  non-empty username, a non-empty password, and a confirmation that
  matches the new password — and MUST reject invalid input with an inline,
  actionable message while leaving the stored credential unchanged.
- **FR-032**: After a successful save, the new credential MUST be accepted
  by rpcd's `session.login` for the `saadway_login` mapping immediately
  (no restart, no re-provisioning step), and the previous credential MUST
  stop being accepted (amended 2026-10-21 — the standalone dashboard itself
  performs no login any more, see FR-020).
- **FR-033**: The credential surface MUST NOT display or return any stored
  secret: reading the current configuration MUST reveal at most the
  username, and no stored password may appear in any response, log, page
  source or monitor panel (extends FR-021).
- **FR-034**: The "Dashboard Credentials" tab MUST be reachable only from
  an authenticated administrative session of the router's panel; an
  unauthenticated or non-administrative client MUST neither read nor
  change the credential **through the panel**. The underlying ubus action
  is published to the LAN-scoped dashboard surface (FR-035), so it MUST
  NOT expose any stored secret and MUST NOT weaken the panel's own
  authentication (amended 2026-10-21).
- **FR-035**: The dashboard's ubus access MUST be granted through a
  dedicated rpcd access group named `unauthenticated`, and it MUST cover
  the `saadway` ubus object only — no other ubus object may be reachable
  by the dashboard's unauthenticated client. The dashboard's transport MUST
  carry rpcd's default (unauthenticated) session id and MUST NOT perform a
  `session.login` (amended 2026-10-21).
- **FR-026**: System MUST handle the combined-active case of failover +
  kill switch during a total WAN outage by halting repeated profile
  cycling (no infinite loop), keeping clients contained, and resuming
  automatically when connectivity returns. After a full cycle over all
  profiles completes with no connection, the system MUST wait a
  configurable cool-down interval and then re-attempt the full cycle,
  repeating until a profile connects; kill-switch containment MUST
  remain in effect throughout, whenever the kill switch is enabled.

### Key Entities *(include if feature involves data)*

- **Profile**: A named reusable tunnel configuration. Kind: OpenVPN
  (imported config content + optional attached credentials) or IKEv2
  (hostname, username, password, connection settings, encryption
  settings). Single active profile at any time; belongs to an ordered
  list defining failover order.
- **SteeringRule**: Per-scope exit policy. Kind: Domain bypass
  (domain/subdomain → direct WAN), IP-range auto-bypass flag
  (local/country & banking), or Device policy (local IP or MAC →
  tunneled/direct). Independent of profiles; applies whenever steering
  is active.
- **ServiceState**: Global operational state: service
  enabled/disabled, current active profile, last known connection
  status, failover attempt counter. Survives reboots and crashes.
- **BackupExport**: A single portable JSON snapshot of allProfiles +
  SteeringRules + ServiceState preferences, importable onto any
  compatible system.
- **DashboardSession**: An authenticated administrator session
  (token + validity window) granting access to the standalone
  dashboard API surface; secrets are never embedded in its content.
- **DashboardCredential**: The standalone dashboard's login identity — a
  username plus a secret kept only in the restricted store. It has a
  *default* state (username `admin`, password `admin`) that is
  initialized automatically on a fresh installation and remains until the
  administrator changes it; the secret itself is never part of the
  credential as observed by any client.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Switching between two profiles — whether manually by the
  user or automatically by failover after failure detection — completes
  in under 10 seconds.
- **SC-002**: With the kill switch enabled, when the active tunnel
  drops, the count of tunneled-client packets leaking onto the main WAN
  is exactly **0** until the tunnel is restored.
- **SC-003**: A health test (ping and HTTP 204) returns an exact result
  (latency in milliseconds or Timeout) within at most 5 seconds, with
  the dashboard remaining fully interactive (no freeze).
- **SC-004**: A JSON backup export restores **100%** of profiles and
  routing rules onto a fresh installation with zero errors.
- **SC-005**: A bypassed domain is loaded via the direct WAN in **100%**
  of tested requests while a tunnel is active, verified by observing
  its traffic path.
- **SC-006**: After an uninvited reboot (power loss) during an active
  tunnel, the service re-establishes the same active profile and
  re-applies kill-switch/steering rules with **zero** manual steps and
  **0** leaked packets in the recovery window.
- **SC-007**: An authenticated administrator completes a profile import
  and activation workflow (either OpenVPN or IKEv2) in under 2 minutes
  on first use, without documentation.
- **SC-008**: In 100% of live-log views and unauthenticated API checks,
  no credential value is ever visible.
- **SC-009**: On 100% of fresh installations, the standalone dashboard
  opens and is fully operable with **0** manual provisioning steps and
  **without any credential**.
- **SC-010**: After saving new credentials from the LuCI page, rpcd's
  `session.login` accepts the new values and rejects the previous ones —
  both within **5 seconds** of the save.
- **SC-011**: The stored password value appears **0** times in any page
  source, response body, log line or monitor panel.
- **SC-012**: Opening `/saadway/` shows the dashboard with **no**
  credential prompt; every panel action succeeds and **no** session token
  appears in the DOM, the URL or the network log.
- **SC-013**: The unauthenticated client is granted the `saadway` ubus
  object and is refused for every other ubus object; the dashboard is
  unreachable from the WAN side.

## Assumptions

- One tunnel profile is active at any time; "multiple simultaneous
  tunnels" is out of scope for this feature.
- The dashboard is intended for use from the LAN side (the router
  administrator network); exposure to the WAN/internet is out of scope.
- A reasonable default profile limit (≈10–20 profiles) is sufficient
  for personal/small-office use; bulk profile management is out of
  scope.
- Default failover behavior: 3 failed attempts before switching;
  configurable by the administrator.
- The auto-bypass checkbox relies on a curated, bundled classification
  of local/country and banking IP ranges maintained within the package;
  real-time online classification services are not a dependency.
- The LuCI controller page is a delivery target of the router's stock
  admin panel; the standalone dashboard targets modern desktop and
  mobile browsers.
- The router runs a current-generation OpenWrt 25.12 environment whose
  standard networking services (firewall, service manager, config
  store) are available; the feature does not need to support older
  platforms.
- The standalone dashboard requires no credential: it is reachable and fully
  operable on the LAN as soon as the package is installed (FR-020). The
  optional rpcd session credential is created on demand, and re-initialization
  never resets a credential the administrator has changed.
- Dashboard sessions established before a credential change stay valid until
  they expire on their own inactivity window.
- The credentials tab is authorized by the router panel's own session model
  (an authenticated administrative session); no separate credential of its
  own is introduced.
- The LuCI page labels follow the router panel's translation mechanism; the
  Persian wording quoted in FR-029/FR-030 is the intended phrasing.
- Backend components must respect the project constitution: use only
  the platform's native infrastructures, avoid heavyweight runtimes on
  the router, execute all monitoring asynchronously with explicit
  timeouts, and preserve user secrets with restricted access.
