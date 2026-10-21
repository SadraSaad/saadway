# Phase 1 Data Model: SaadWay VPN Tunnel & Routing Dashboard

**Feature**: 001-saadway-vpn-dashboard | **Date**: 2026-10-19 | Source of truth: UCI (`/etc/config/saadway`)

This document maps the spec's Key Entities to concrete UCI sections, their
fields, validation rules, state transitions, and relationships. Storage
rules (constitution Principle VI): UCI is the only persistent state; secrets
are referenced from UCI but stored as 0600 files under `/etc/saadway/`.

---

## Entity: Profile

A named reusable tunnel configuration. Exactly one is active at a time;
the list order = failover order.

**UCI section type**: `saadway_profile`

| Field | Type | Constraints / Validation |
|---|---|---|
| `name` | string | unique, 1–64 chars, non-empty; uniqueness enforced on create/update |
| `kind` | enum | `openvpn` \| `ikev2` |
| `enabled` | boolean | default `true` |
| `order` | integer | position in failover list; unique per profile; auto-renumbered on delete |
| `secret_ref` | filepath | relative path under `/etc/saadway/`; referenced file MUST be 0600; never inlined into UCI |
| — OpenVPN only — | | |
| `ovpn_ref` | filepath | path of normalized rendered config under `/etc/saadway/run/`; source `.ovpn` validated at import (see Validation below) |
| `remote_host` | string | extracted from validated config; informational |
| — IKEv2 only — | | |
| `hostname` | fqdn | required for IKEv2; e.g. `ams-466-ike.whiskergalaxy.com` |
| `username` | string | required for IKEv2 (EAP identity) |
| `remote_id` | enum/string | default `same-as-hostname`; or explicit value |
| `local_id` | enum/string | default `same-as-username`; or explicit value |
| `cert_mode` | enum | `from-server` (default) only for spec v1 ("Leave default · from server") |
| `proxy` | enum | `off` (default) only for spec v1 |
| `proposal_ike` | string | fixed default `aes256gcm16-prfsha256-ecp384-ecp521` (AES-256-GCM, SHA-256 PRF, DH20 + iOS DH21), overridable |
| `proposal_esp` | string | fixed default `aes256gcm16` (Cipher AES-256-GCM; integrity carried by GCM/PRF) |

**Validation rules**:
- `.ovpn` import: MUST contain exactly one `remote` endpoint directive; MUST
  NOT contain unsupported/dangerous directives (up/down/exec scripts,
  `dev` overrides conflicting with saadwaydev naming, inline
  `redirect-gateway`); private material extracted to 0600 files; reject
  with specific error, no partial save (FR-025, Edge Case 2).
- IKEv2 create: hostname/username/password required; defaults per FR-004/FR-005.
- Credential files: special characters/unicode accepted byte-for-byte
  (Edge Case 5); masked in UI everywhere (FR-021).

**State transitions** (per profile):

```
inactive ⇄ activating → active ⇄ deactivating
              ↓ (failure, N attempts)
          failed → (next profile in list)
```

## Entity: SteeringRule

Per-scope exit policy. Applies whenever steering is active, independent of
which profile is active. Three kinds live in one ordered evaluation chain:
device-direct > domain-bypass > IP-range auto-bypass > implicit tunnel default.

**UCI section type** `saadway_domain_rule` (kind: Domain bypass)

| Field | Type | Constraints |
|---|---|---|
| `domain` | fqdn/zone | unique (case-insensitive); subdomains implied |
| `created` | timestamp | informational |

**UCI section type** `saadway_device_rule` (kind: Device policy)

| Field | Type | Constraints |
|---|---|---|
| `match_type` | enum | `ip` \| `mac` |
| `match_value` | IPv4 CIDR or MAC | valid syntax enforced |
| `policy` | enum | `direct` \| `tunneled` (explicit override of tunnel default) |

**UCI section type** `saadway_service` (conceptual kind: IP-range auto-bypass)

| Field | Type | Constraints |
|---|---|---|
| `iprange_bypass` | boolean | default `false`; enables bundled local/country/banking CIDR set |

**Uniqueness** (FR-025, Edge Case 3): duplicate domain (case-insensitive) or
duplicate match_value is detected at create; response = reject with
non-destructive feedback (choosing "reject" — merge semantics of overlapping
entries stay simple and rule removal never silently drops an overlapping
distinct entry).

## Entity: ServiceState

Global operational state; UCI-persisted and runtime-cached.

**UCI section type** `saadway_service`

| Field | Type | Constraints |
|---|---|---|
| `enabled` | boolean | service enable/disable (LuCI toggle, FR-022) |
| `active_profile` | string | name of active profile or empty |
| `connection_status` | enum | `up` \| `down` \| `connecting` \| `contained` (kill-switch hold) \| `cooldown` |
| `failover_attempts` | integer | current failed-attempt counter |
| `cycle_exhausted` | boolean | one full profile cycle failed |

**Watchdog/kill-switch config** (same `saadway_service` section, defaults
per research D2/D5/D6):

| Field | Type | Default |
|---|---|---|
| `kill_switch` | boolean | `0` (clarification Q2-B) |
| `failover_attempts` | integer | `3` |
| `failover_cooldown` | seconds | `300` (clarification Q3-A) |
| `probe_interval` | seconds | `30` |
| `stats_interval` | seconds | `1` |

**State machine** (connection_status):

```
(up) ──probe-fail×N──▶ (contained) ──next-profile──▶ (connecting) ──if-up+health-ok──▶ (up)
                        ▲    ↓ (cycle exhausted)
                        │    ▼
                   (cooldown) ──timer──▶ (connecting, new cycle)
(down) — service disabled
```

## Entity: BackupExport

A single portable JSON snapshot, schema v1 — see
`contracts/backup-schema.json`. Contains: schema version, profile list
(incl. referenced or inlined credential material per export mode), all
steering rules, service preferences, and a mac/uci checksum. Import =
validate → dedupe → per-item report (restored / needs-attention) → atomic
policy apply (User Story 6 scenario 4).

## Entity: DashboardCredential

The **optional operator credential** of the `saadway_login` rpcd mapping
(research D10/D11; spec FR-020/FR-028 → FR-035). Since the auth bypass
(T079) the standalone dashboard has **no** credential of its own: it is
authorized as rpcd's default (unauthenticated) session and its access
control is the platform's network boundary.

- **Username**: the login name of the dedicated dashboard account. Created
  on demand (no default is shipped any more); changeable by the
  administrator from the LuCI "Dashboard Credentials" tab.
- **Secret**: kept ONLY in the restricted store (0600) and referenced — not
  copied — by the platform's login configuration. Never part of the
  credential as observed by any client: reads return at most the username.
- **State**: *absent* (the default on a fresh installation — the first-boot
  path provisions no password) or *customized* (set through the LuCI
  credentials form, effective for the next `session.login` immediately).

### The dashboard's ubus method inventory (the authorized surface)

The tokenless client calls exactly the methods of the `saadway` object that
the SPA uses (`saadway/files/htdocs/saadway/js/app.js`): `mgmt.status`,
`profile.list`, `profile.import_openvpn`, `profile.create_ikev2`,
`profile.activate`, `profile.deactivate`, `profile.delete`, `steer.list`,
`steer.domain.add`, `steer.domain.remove`, `steer.device.set`,
`steer.device.remove`, `steer.iprange.set`, `sec.status`,
`sec.kill_switch.set`, `mon.stats`, `mon.logs`, `mon.health_test`,
`maint.backup.export`, `maint.backup.import`, `maint.cooldown.set`. The
ACL group `unauthenticated` covers the whole `saadway` object — the
published surface and the panel's surface therefore carry the same power
set (recorded as a deviation in `docs/verify-2512.md` §7).

**State transitions** (all applied in ONE atomic operation — the shadow
account, the login mapping and the rpcd reload; a failure at any step
leaves the previous credential intact and usable):

```text
             fresh install
   (absent) ───────────────► (no account, no mapping — the dashboard
        │                    needs no credential; nothing is provisioned)
        │                            │
        │                            │  LuCI tab: Save & Apply
        │                            │  (the account is created on demand)
        │                            ▼
        │                      customized (new username + password)
        │                            │
        │                            │  username change ⇒ new shadow
        │                            │  account created FIRST, then the
        │                            │  mapping (username + '$p$<user>')
        │                            ▼
        └────────────────────► renamed (old mapping superseded; the
                                 previous account keeps its restricted
                                 row, but no login section references it)
```

- The `$p$` suffix always equals the account name — a mapping pointing at a
  non-existent account fails closed (rpcd: `getspnam` → no row → the
  login is denied).
- **No default credential is shipped** (T079/T094): the installation
  provisions no account and no password — the optional credential is
  created only when the administrator saves it in the LuCI credentials tab.
  Re-running the first-boot script on an existing account is a **no-op**:
  the mapping is refreshed only when the account exists, so an
  administrator-set credential is never clobbered.

## Entity: DashboardSession

**Retired by the auth bypass (2026-10-21 — FR-020/FR-035, constitution
v1.2.0, research.md D11).** The dashboard creates **no** session; the
entity is kept here to document the transport's session model, not a
dashboard credential.

- Session: **none**. The SPA sends rpcd's **default (unauthenticated)
  session** — the all-zero session id `00000000000000000000000000000000`
  (`openwrt/rpcd` `include/rpcd/session.h:35`,
  `RPC_DEFAULT_SESSION_ID`) — on every call. rpcd creates that session at
  startup with a NULL login section (`session.c:1396-1400`) and grants it
  **only** the access group named `unauthenticated`
  (`session.c:944-946`), which `saadway/files/rpc-acl.json` defines for the
  `saadway` ubus object. uhttpd-mod-ubus forwards every `/ubus` call to
  rpcd's `session.access` with the request's `params[0]` as the session id
  (`openwrt/uhttpd` `ubus.c:280-297`, `820-840`).
- Expiry: **none** — the default session never expires and carries no TTL
  (the `dashboard.session_timeout` option and the `session_timeout` field
  of `mgmt.status` are vestigial; the dashboard no longer logs in).
- Auth material: **none for the dashboard**. The optional
  `saadway_login` mapping plus the nologin `admin` account remain for
  operators and scripts (the secret is the `'$p$<user>'` **reference**;
  the hash lives only in the 0600 `/etc/shadow`; no credential material
  ever sits in a world-readable UCI option, Constitution IV) and are
  created on demand by the LuCI credentials tab. The daemon keeps no
  credential store of its own.
- Token handling: no token is created, stored, sent in a URL or logged;
  secrets are never embedded in payloads (FR-021).
- Absent token: the **normal** path — the `saadway` calls succeed through
  the `unauthenticated` group, while every other ubus object is refused.
  The dashboard's access control is the platform's network boundary (the
  LAN-bound uhttpd listener + the firewall's WAN input policy); the
  residual risk is recorded in `docs/verify-2512.md` §7. The LuCI root
  session rides rpcd's wildcard-group default (`list read/write '*'`) for
  the lifecycle calls only (FR-022).

---

## Storage layout summary

```
/etc/config/saadway                 ← sole persistent state (UCI)
/etc/config/rpcd                    ← saadway_login login section ('$p$' → /etc/shadow)
/etc/shadow (admin)                 ← 0600 hash, provisioned by /usr/libexec/saadway/provision
/etc/saadway/secrets/<profile>/     ← 0600 auth-user-pass / swanctl secrets
/etc/saadway/run/openvpn/*.conf     ← rendered OpenVPN configs (0600)
/etc/saadway/swanctl/*.conf         ← rendered swanctl configs (0600 secrets refs)
/etc/saadway/bypass-cidr.txt        ← bundled local/country/banking CIDR set
/var/run/saadway/                   ← runtime state (counters cache, pid, sockets)
```

## Scale bounds (from spec Scale/Scope)

- ≤ 20 `saadway_profile` sections
- ≤ 500 `saadway_domain_rule` sections
- ≤ 253 `saadway_device_rule` sections
- 1 active profile; failover order = UCI section `order` field
