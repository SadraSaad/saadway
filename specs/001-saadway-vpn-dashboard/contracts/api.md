# API Contract: ubus-over-HTTP JSON-RPC (Standalone Dashboard)

**Feature**: 001-saadway-vpn-dashboard | **Date**: 2026-10-19

The standalone dashboard's ONLY transport is `/ubus` (uhttpd-mod-ubus,
JSON-RPC 2.0 over HTTPS/LAN). Since the auth bypass of 2026-10-21 (spec
FR-020/FR-035, constitution v1.2.0, research.md D11) the dashboard performs
**no** credential exchange: every `saadway.*` call carries rpcd's **default
(unauthenticated) session id** and is authorized by the ACL group named
`unauthenticated` in `files/rpc-acl.json`; calls outside that group are
rejected by rpcd before reaching the daemon. The dashboard's access control
is the platform's network boundary — the LAN-bound uhttpd listener and the
firewall's WAN input policy (docs/verify-2512.md §7).

Transport conventions (standard uhttpd-mod-ubus):

```json
POST /ubus
{ "jsonrpc": "2.0", "id": 1, "method": "call",
  "params": [ "<session-id>", "<ubus-object>", "<method>", "<msg>" ] }
```

The dashboard's client — tokenless (T079):

```json
{ "jsonrpc":"2.0", "id":1, "method":"call",
  "params": [ "00000000000000000000000000000000", "saadway", "mgmt.status",
              {} ] }
```

`"00000000000000000000000000000000"` is rpcd's
`RPC_DEFAULT_SESSION_ID` (`openwrt/rpcd` `include/rpcd/session.h:35`) — the
**default session**, which rpcd creates at startup with a NULL login
section (`session.c:1396-1400`). With a NULL login section rpcd grants
**only** the access group literally named `unauthenticated`
(`session.c:944-946`) ⇒ that group is the dashboard's authorization
mechanism. `uhttpd-mod-ubus` forwards every `/ubus` call to rpcd's
`session.access` with the request's `params[0]` as the session id
(`openwrt/uhttpd` `ubus.c:280-297`, `820-840`).

There is therefore **no** `session.login`, **no** `ubus_rpc_session`, **no**
session TTL and **no** token in the DOM, in a URL or in a log line. The
optional operator credential (`saadway_login` in `/etc/config/rpcd`) still
exists for scripts and for the LuCI credentials tab: its secret is the
`'$p$<user>'` **reference** — the hash stays in the 0600 `/etc/shadow`
(provisioned by `saadway-provision`; docs/verify-2512.md §2 — no
daemon-side credential store, Constitution IV), it is never published in
the SPA page source, and **no default password is shipped** (the account is
created on demand by the LuCI credentials tab).

---

## ubus object `saadway` — methods

**The profile-name rule (T104)** — every `name` parameter of the profile
methods (`profile.import_openvpn`, `profile.create_ikev2`, `profile.update`)
is validated by `sw_valid_name()` (`saadway/src/uci-state.c`):

* non-empty, at most **64 bytes**;
* no leading `.`;
* no byte below 0x21 — so **no space** and no control character;
* no `"`, no `'` and no 0x7f.

**Persian / UTF-8 names are valid** — the rule is a POSIX-safety rule, not
a Latin-only one (the dashboard is Persian-first, FR-027). A rejected name
is answered with the 400-class code `E_BAD_SYNTAX` and the detail
`invalid name (1-64 characters, no spaces or quotes, must not start with
'.')`; the dashboard validates the same rule in the browser and shows the
rule in Persian before sending a request.


All mutating calls are gated by ACL; all responses redact secrets (FR-021).

### Lifecycle (`mgmt.*`)

| Method | Params → Returns | Notes |
|---|---|---|
| `mgmt.status` | `{}` → `{ enabled, running, connection_status, active_profile, current_if, uptime_s, session_timeout, language }` | used by dashboard header + LuCI page; `session_timeout` feeds the login TTL (T053); `language` (T087/FR-027) is the daemon-side default for a browser without a stored preference — the browser's own stored choice still wins |
| `mgmt.set` | `{ enabled: bool }` → `{ applied }` | enable/disable service (LuCI + dashboard) |
| `mgmt.credentials.set` | `{ username: string, password: string }` → `{ applied }` | applies the **optional operator credential** (the `saadway_login` mapping) — the account is created if missing, the password is hashed by the platform's `passwd` (the plaintext is piped, never an argument), `/etc/config/rpcd` section `saadway_login` is updated (`username` + `password '$p$<user>'`) and rpcd is reloaded (FR-032). Never returns the secret (FR-033); invalid input → 400-class code, no change (FR-031). **Reachability changed with the auth bypass**: the panel's group grants it, but the LAN-scoped `unauthenticated` group covers the whole `saadway` object — so the method is LAN-reachable too (recorded as a deviation in docs/verify-2512.md §7) |

### Profiles (`profile.*`)

| Method | Params → Returns | Notes |
|---|---|---|
| `profile.list` | `{}` → `{ profiles: [ {name, kind, enabled, order, secret_ref, active, status} ] }` | secrets never returned |
| `profile.import_openvpn` | `{ name, ovpn_b64, credentials?: {username, password} }` → `{ profile, warnings? }` or error 400-class code with import detail | validates (FR-025); reject codes: `E_NO_REMOTE`, `E_BAD_SYNTAX`, `E_UNSUPPORTED_DIRECTIVE`, `E_DUPLICATE_NAME` |
| `profile.create_ikev2` | `{ name, hostname, username, password, remote_id?, local_id?, cert_mode?, proposal_*? }` → `{ profile }` | defaults per FR-004/FR-005 |
| `profile.update` | `{ name, patch }` → `{ profile }` | patch fields only; secrets replaced not echoed. The patchable fields are `enabled`, `hostname` (**IKEv2 only** — an OpenVPN profile's server comes from the imported file's `remote` directive, so `hostname` is not honoured there), `username`, `password`, `remote_id`, `local_id`, `cert_mode`, `proposal_ike`, `proposal_esp`. The OpenVPN credential pair (`username` + `password`) travels **together** — a lone password is rejected with `E_BAD_SYNTAX` (T094) |
| `profile.delete` | `{ name }` → `{}` | refuses when active unless `force=true` |
| `profile.activate` | `{ name }` → `{ switched_to, elapsed_ms }` | single-click switch (FR-007); atomic (FR-014) |
| `profile.deactivate` | `{}` → `{}` | tunnel down; kill-switch holds if enabled |

### Steering (`steer.*`)

| Method | Params → Returns | Notes |
|---|---|---|
| `steer.list` | `{}` → `{ domains: [...], devices: [...], iprange_bypass: bool }` | |
| `steer.domain.add` / `steer.domain.remove` | `{ domain }` → `{}` | duplicate → error `E_DUPLICATE` (FR-025) |
| `steer.device.set` | `{ match_type, match_value, policy? }` → `{}` | policy `direct\|tunneled` (omitted ⇒ `tunneled`); `match_type` is `ip` or `mac` |
| `steer.device.remove` | `{ match_type, match_value }` → `{}` | removes the rule — **no `policy`** (the daemon reads exactly these two attributes, T096) |
| `steer.iprange.set` | `{ enabled }` → `{}` | bundled CIDR set toggle (FR-010) |

All steer mutations re-apply policy atomically; tunnel stays up
(User Story 3 scenario 4).

### Security (`sec.*`)

| Method | Params → Returns | Notes |
|---|---|---|
| `sec.status` | `{}` → `{ kill_switch, dns_forced, contained }` | the **kill-switch** state only — `dns_forced` says whether the tunnel DNS redirect is active; the resolver and the leak verdict come from `sec.dns.status` (T091) |
| `sec.kill_switch.set` | `{ enabled }` → `{ applied }` | opt-in toggle (FR-012), persists |
| `sec.dns.status` | `{}` → `{ resolver_if, leak_risk }` | informational — **the only source** of `resolver_if` and `leak_risk` (they are NOT part of `sec.status`; the dashboard's security panel merges both payloads, T091) |

### Monitoring (`mon.*`)

| Method | Params → Returns | Notes |
|---|---|---|
| `mon.health_test` | `{}` → `{ ping: {target, latency_ms \| "timeout"}, http: {code \| "timeout"} }` | async internally; returns within SC-003 5 s budget |
| `mon.stats` | `{}` → `{ ts, rx_bps, tx_bps, iface }` | 1 Hz SPA polling (FR-016) |
| `mon.logs` | `{ cursor?: token, lines?: int }` → `{ entries: [{ts, level, msg}], next_cursor }` | cursor-based syslog tail; secret-scrubbed (FR-017/FR-021) |

### Maintenance (`maint.*`)

| Method | Params → Returns | Notes |
|---|---|---|
| `maint.backup.export` | `{ include_credentials?: bool }` → `{ json_b64, schema_version }` | schema v1 (contracts/backup-schema.json) |
| `maint.backup.import` | `{ json_b64 }` → `{ restored: [...], needs_attention: [...], errors: [...] }` | per-item report (User Story 6) |
| `maint.cooldown.set` | `{ seconds, attempts? }` → `{ applied }` | FR-026 tunables |

### Error envelope

| ubus code | Meaning |
|---|---|
| 400 | invalid params / validation rejected (detail string provided) |
| 401 | session invalid/expired (dashboard must re-login) |
| 404 | unknown object/method or unknown named resource |
| 500 | internal failure (fail-closed state reported via `mgmt.status`) |

These labels are CLASS tags; the concrete wire codes over the legacy
`POST /ubus` endpoint are libubus statuses inside `result[0]` — the
upstream `ubusmsg.h` enum is **0-based**: `UBUS_STATUS_INVALID_ARGUMENT=2`
(400 classes), `UBUS_STATUS_NOT_FOUND=4` (404 classes),
`UBUS_STATUS_PERMISSION_DENIED=6` (401 classes — wrong credentials;
rpcd's `rpc_handle_login` returns exactly this),
`UBUS_STATUS_TIMEOUT=7` (transient — never a re-login trigger),
`UBUS_STATUS_UNKNOWN_ERROR=9` (500 classes),
`UBUS_STATUS_CONNECTION_FAILED=10` (transport), and uhttpd's own
JSON-RPC envelopes `-32002` "Access denied" / `-32000` "Object not found"
/ `-32003` request timeout for pre-call rejections. Full mapping and
on-device checklist: `docs/verify-2512.md` (T-verify-6, Phase 15/T061).

## LuCI page surface (FR-022 — exactly two tabs)

`luci-app-saadway` view is tabbed:

- **Status & Control** — reads `mgmt.status` and toggles via `mgmt.set`
  through LuCI's own ubus session; renders one "Open Dashboard" link to the
  SPA (`http://<router>/saadway/`).
- **Dashboard Credentials** (تنظیمات ورود به داشبورد) — the login-credential
  form of the standalone dashboard: new username (نام کاربری جدید), new
  password (رمز عبور جدید), password confirmation (تکرار رمز عبور) and a
  *Save & Apply* action (FR-029 → FR-032). The action requires an
  authenticated administrative session (FR-034) and never returns the
  stored secret (FR-033); the standalone dashboard's own API surface is
  unchanged.

No other configuration UI inside LuCI (constitution v1.1.0, Principle V).

The credentials tab is a `form.Map` with two tabs (`tab()` / `taboption()`,
upstream LuCI) and the standard **Save & Apply** action. Its write path is a
single ubus call — `saadway.mgmt.credentials.set` — never a browser-side
UCI write: the browser cannot hash the password, and a half-applied change
(credentials updated before the login mapping) would lock the
administrator out. The dashboard's own session has **no** grant for that
method, so it can never re-key its own login (FR-034).

## Non-goals

- No SSE/WebSocket push (polling chosen for footprint, research D7).
- No WAN exposure; uhttpd ACL binds to LAN interface only.
- No secret material in ANY response body or log entry (FR-021/SC-008).
- **No authentication on the dashboard's own surface** — the access control
  is the LAN-bound listener plus the firewall's WAN input policy, and the
  ubus surface is limited to the `saadway` object through the
  `unauthenticated` ACL group (FR-035).
