# UCI Configuration Schema: `/etc/config/saadway`

**Feature**: 001-saadway-vpn-dashboard | **Date**: 2026-10-19

UCI is the single source of truth (constitution Principle VI). Secrets are
referenced here, never inlined (constitution Section 2 / credential rule).

## Section `service` (unique, `option ...` one named section)

```conf
config service 'service'
    option enabled '1'
    option active_profile ''      # profile name or empty
    option connection_status 'up' # up|down|connecting|contained|cooldown
    option failover_attempts '3'
    option failover_cooldown '300'
    option probe_interval '30'
    option stats_interval '1'
    option kill_switch '0'
    option iprange_bypass '0'
```

## Section `dashboard` (unique, `option ...` one named section)

```conf
config dashboard 'dashboard'
    option language 'fa'          # fa|en (Q4-A Persian-first; switch persists per-browser)
    option authentication 'dedicated'  # dedicated = rpcd 'admin' shadow bridge;
                                  # 'system' = log in as root (FR-020 alternative)
    option session_timeout '1800'
```

Credential material is never a UCI option: the dedicated user's hash lives
in `/etc/shadow`, referenced from `/etc/config/rpcd` as
`option password '$p$admin'` (uci-defaults/50-saadway-rpcd +
`saadway-provision`). The dashboard ships with the default credential
`admin` / `admin`; the password is applied by `saadway-provision` over
stdin and is changed with `saadway-provision passwd` or from the LuCI
credentials tab. The option values here are presentation/lifecycle
preferences only.

### The optional login mapping in `/etc/config/rpcd` (not part of `/etc/config/saadway`)

```conf
config login 'saadway_login'
    option username 'admin'
    option password '$p$admin'
    list read 'saadway'
    list write 'saadway'
```

- Since the auth bypass (T079 — spec FR-020/FR-035, constitution v1.2.0)
  this mapping is the **optional operator credential**, NOT the dashboard's
  entry point: the dashboard is authorized as rpcd's default
  (unauthenticated) session through the ACL group named
  **`unauthenticated`** in `saadway/files/rpc-acl.json` (installed to
  `/usr/share/rpcd/acl.d/saadway.json`), which covers the `saadway` ubus
  object only. The dashboard performs no `session.login` and creates no
  session token.
- The first-boot script provisions **no** default password (there is no
  consumer for a well-known one); the account is created on demand by the
  LuCI credentials tab.
- The section name is cosmetic — rpcd matches a login section by its
  `username` option; it is named `saadway_login` and is maintained
  idempotently by `saadway/files/uci-defaults/50-saadway-rpcd` (create only
  when missing, refresh `username`/`password` **only when the account
  exists**, `uci commit rpcd`, and reload rpcd — rpcd binds the default
  session's ACLs at startup, `openwrt/rpcd` `session.c:1396-1400`).
- `option password '$p$<user>'` is a **reference**, not a secret: the hash
  stays in the 0600 `/etc/shadow`. The `<user>` suffix MUST equal the
  account name — upstream resolves it with `getspnam(<name>)` and compares
  with `crypt(3)`; a missing shadow row fails the login (fail closed).
- The ACL lists stay `saadway` (least privilege). The credential-change
  method is granted through a **separate** ACL group
  (`saadway-credentials`, file
  `luci-app-saadway/root/usr/share/rpcd/acl.d/saadway-credentials.json`)
  that the dashboard's session does not obtain — only the router's stock
  root session does (group `*`).
- A rename (the LuCI tab's "New Username") updates `username` **and** the
  `$p$` suffix together, and creates the new shadow account first — the
  mapping is never left pointing at a non-existent account.

## Section types

### `saadway_profile` (kind openvpn)

```conf
config saadway_profile
    option name 'provider-a-ovpn'
    option kind 'openvpn'
    option enabled '1'
    option order '1'
    option ovpn_ref 'run/openvpn/provider-a.conf'
    option secret_ref 'secrets/provider-a/auth.txt'   # optional, 0600
    option remote_host 'gw.provider-a.com'           # informational
```

### `saadway_profile` (kind ikev2)

```conf
config saadway_profile 'p2'
    option name 'provider-b-ike'
    option kind 'ikev2'
    option enabled '1'
    option order '2'
    option hostname 'ams-466-ike.whiskergalaxy.com'
    option username 'eap-user'
    option remote_id 'same-as-hostname'   # or explicit value
    option local_id 'same-as-username'    # or explicit value
    option cert_mode 'from-server'        # spec v1 fixed
    option proxy 'off'                    # spec v1 fixed
    option proposal_ike 'aes256gcm16-prfsha256-ecp384-ecp521'
    option proposal_esp 'aes256gcm16'
    option secret_ref 'secrets/provider-b/eap.secret'  # 0600
```

### `saadway_domain_rule`

```conf
config saadway_domain_rule
    option domain 'bank.example.ir'
    # uniqueness enforced case-insensitively (FR-025)
```

### `saadway_device_rule`

```conf
config saadway_device_rule
    option match_type 'mac'          # mac|ip
    option match_value 'AA:BB:CC:DD:EE:FF'
    option policy 'direct'           # direct|tunneled
```

## Validation rules (enforced by daemon before any write)

| Rule | Enforcement |
|---|---|
| Profile `name` unique | on create/update; error `E_DUPLICATE_NAME` |
| `order` unique per profile | auto-renumber on delete/reorder |
| `.ovpn` import validity | FR-025; no partial save |
| Domain rule uniqueness (case-insensitive) | error `E_DUPLICATE` |
| Device `match_value` syntax | IPv4 CIDR or MAC validator |
| Secret files 0600 | daemon checks at reference time; repairs to 0600 or refuses activation |
| Enum fields | reject any value outside enumerated set |

## Boot application order (Edge Case 4 / SC-006)

1. `/etc/init.d/saadway` procd start →
2. enforce containment (if `kill_switch='1'`) — BEFORE any tunnel start
3. apply steering/kill/marks (single nft transaction)
4. start `active_profile` (if any)
5. watchdog resumes; remaining recovery = failover machinery
