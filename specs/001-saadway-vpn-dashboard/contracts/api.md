# API Contract: ubus-over-HTTP JSON-RPC (Standalone Dashboard)

**Feature**: 001-saadway-vpn-dashboard | **Date**: 2026-10-19

The standalone dashboard's ONLY transport is `/ubus` (uhttpd-mod-ubus,
JSON-RPC 2.0 over HTTPS/LAN). Every `saadway.*` call requires a valid ubus
session token obtained via `session.login`; calls outside the ACL in
`files/rpc-acl.json` are rejected by uhttpd before reaching the daemon
(Principle V: exclusively authenticated APIs; FR-020/FR-021).

Transport conventions (standard uhttpd-mod-ubus):

```json
POST /ubus
{ "jsonrpc": "2.0", "id": 1, "method": "call",
  "params": [ "<session-token>", "<ubus-object>", "<method>", "<msg>" ] }
```

Login:

```json
{ "jsonrpc":"2.0", "id":1, "method":"call",
  "params": [ "00000000000000000000000000000000", "session", "login",
              { "username":"admin", "password":"<secret>" } ] }
```

Response carries `ubus_rpc_session` (the token). Session tokens expire on
inactivity (`dashboard.session_timeout`, default 1800 s; the dashboard
requests the configured TTL at `session.login` using the
`session_timeout` field returned by `mgmt.status`, falling back to
1800 s before the first successful status call) and are never
reused across devices. `username=admin` is validated by rpcd via
the `$p$admin` shadow bridge (provisioned by
`saadway-provision`; docs/verify-2512.md §2 — no daemon-side credential
store, Constitution IV); `username=root` invokes system-root
authentication mode.

The dashboard ships with the **default credential `admin` / `admin`**:
`/usr/libexec/saadway/provision` creates the nologin user `admin` and
applies the default password non-interactively — the plaintext is handed to
the password setter over stdin and only the crypt(3) hash reaches the 0600
`/etc/shadow` (never a UCI option, never a command argument, never the SPA
page source). The default is changed with
`/usr/libexec/saadway/provision passwd`; the compensating controls for the
weak default are listed in docs/verify-2512.md §7.

---

## ubus object `saadway` — methods

All mutating calls are gated by ACL; all responses redact secrets (FR-021).

### Lifecycle (`mgmt.*`)

| Method | Params → Returns | Notes |
|---|---|---|
| `mgmt.status` | `{}` → `{ enabled, running, connection_status, active_profile, current_if, uptime_s, session_timeout }` | used by dashboard header + LuCI page; `session_timeout` feeds the login TTL (T053) |
| `mgmt.set` | `{ enabled: bool }` → `{ applied }` | enable/disable service (LuCI + dashboard) |

### Profiles (`profile.*`)

| Method | Params → Returns | Notes |
|---|---|---|
| `profile.list` | `{}` → `{ profiles: [ {name, kind, enabled, order, secret_ref, active, status} ] }` | secrets never returned |
| `profile.import_openvpn` | `{ name, ovpn_b64, credentials?: {username, password} }` → `{ profile, warnings? }` or error 400-class code with import detail | validates (FR-025); reject codes: `E_NO_REMOTE`, `E_BAD_SYNTAX`, `E_UNSUPPORTED_DIRECTIVE`, `E_DUPLICATE_NAME` |
| `profile.create_ikev2` | `{ name, hostname, username, password, remote_id?, local_id?, cert_mode?, proposal_*? }` → `{ profile }` | defaults per FR-004/FR-005 |
| `profile.update` | `{ name, patch }` → `{ profile }` | patch fields only; secrets replaced not echoed |
| `profile.delete` | `{ name }` → `{}` | refuses when active unless `force=true` |
| `profile.activate` | `{ name }` → `{ switched_to, elapsed_ms }` | single-click switch (FR-007); atomic (FR-014) |
| `profile.deactivate` | `{}` → `{}` | tunnel down; kill-switch holds if enabled |

### Steering (`steer.*`)

| Method | Params → Returns | Notes |
|---|---|---|
| `steer.list` | `{}` → `{ domains: [...], devices: [...], iprange_bypass: bool }` | |
| `steer.domain.add` / `steer.domain.remove` | `{ domain }` → `{}` | duplicate → error `E_DUPLICATE` (FR-025) |
| `steer.device.set` / `steer.device.remove` | `{ match_type, match_value, policy? }` → `{}` | policy `direct\|tunneled` |
| `steer.iprange.set` | `{ enabled }` → `{}` | bundled CIDR set toggle (FR-010) |

All steer mutations re-apply policy atomically; tunnel stays up
(User Story 3 scenario 4).

### Security (`sec.*`)

| Method | Params → Returns | Notes |
|---|---|---|
| `sec.status` | `{}` → `{ kill_switch, dns_forced, contained }` | |
| `sec.kill_switch.set` | `{ enabled }` → `{ applied }` | opt-in toggle (FR-012), persists |
| `sec.dns.status` | `{}` → `{ resolver_if, leak_risk }` | informational |

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

## Non-goals

- No SSE/WebSocket push (polling chosen for footprint, research D7).
- No WAN exposure; uhttpd ACL binds to LAN interface only.
- No secret material in ANY response body or log entry (FR-021/SC-008).
