# verify-2512.md — build-time verification checklist (OpenWrt 25.12)

**Feature**: 001-saadway-vpn-dashboard · **Date**: 2026-10-19 · **Task**: T044

Everything below is either (a) verified against upstream sources/releases in
October 2026 and recorded here as fact, or (b) an explicit checklist item for
the on-device run (quickstart T046). Each item carries its research verify-ID
(T-verify-1 … T-verify-7).

---

## 0. Release environment facts

* OpenWrt **25.12.0** was released (branch `25.12`); maintenance releases
  25.12.2 (kernel 6.12.74) and a 25.12.6 security release were out before
  2026-10-19 (`https://openwrt.org/releases/25.12/notes-25.12.6`).
* 25.12 replaces **opkg with apk** as the package manager (apk-mbedtls 3.x in
  the base images). Verification commands on-device must use
  `apk info`/`apk list`, not opkg. Our `uci-defaults` and shell assets are
  unaffected.
* Kernel line: 6.12.x (ASU profiles show 6.12.74 on 25.12.2).

## 1. T-verify-1 — `uhttpd-mod-ubus` availability & ACL syntax

**Verified (build-time):**

* `uhttpd-mod-ubus` ships in the 25.12 *base* feeds
  (e.g. `uhttpd-mod-ubus-2025.10.03~ebb92e6b-r1.apk`, 8.3 KB, listed in the
  25.12.0 base package dir; present in default ASU profiles).
* Source `openwrt/uhttpd` `ubus.c` (plugin) behavior, from master:
  * The endpoint is auto-registered at `conf.ubus_prefix` (default `/ubus`)
    for every listening address; LAN-only exposure is achieved by binding
    uhttpd to the LAN address only (`uci-defaults/40-saadway-uhttpd`).
  * `POST /ubus` (bare path, and `/ubus/`) is the **legacy** JSON-RPC shape:
    response is `{"jsonrpc":"2.0","id":…,"result":[<ubus-status>, <payload>]}`.
    This is the shape our SPA (`htdocs/saadway/js/api.js`) uses.
  * `POST /ubus/call/<object>` is the **modern** RESTful shape
    (`{result:{…}}` or `{error:{code,message}}`), and `GET /ubus/list[/<obj>]`
    is the RESTful object listing (upstream addition).
  * The client MUST NOT send `ubus_rpc_session` inside the call payload —
    uhttpd injects the session token from `params[0]` (a client attempt is
    rejected with `-32602 Invalid parameters`).
  * Authorization pre-check: unless `ubus_noauth` is set (we never set it),
    every `call/list` carries `params[0]` as the session id; uhttpd queries
    `session.access` for `<sid>, <object>, <function>` and rejects
    unauthorized requests with **JSON-RPC `-32002 "Access denied"`** before
    the call ever reaches ubus.
  * `ubus_cors` only matters cross-origin; the SPA is served from the SAME
    origin, so it must remain UNSET (least exposure).
  * POST body cap `UH_UBUS_MAX_POST_SIZE = 65536` bytes.

**On-device checklist (goes to T046 run):**

1. `apk list --installed | grep uhttpd-mod-ubus`.
2. `test -f /usr/share/rpcd/acl.d/unauthenticated.json` — upstream default
   ACL granting the zero-session `["access","login"]` on `session` (required
   for `session.login` with the all-zero token WITHOUT logging in first).
3. Zero-token login POST with the admin user created by
   `saadway-provision` returns `result[0]==0` and `ubus_rpc_session`.
4. With the session token, `mgmt.status` succeeds; after ACL denial tests
   (below), execution falls back to the login screen.

## 2. Auth bridge — rpcd login mechanics (source-verified)

From `openwrt/rpcd` `session.c` (master, fetched 2026-10):

* `session.login` accepts `{username: string, password: string,
  timeout?: int}`. `timeout` is stored on the session; **every authorized
  call refreshes the timer** (`rpc_touch_session` via `rpc_session_get`) —
  inactivity-only expiry, no idle-max. Our SPA always passes
  `dashboard.session_timeout` (1800 s default) so the two sides agree.
* Upstream default timeout constant is `RPC_DEFAULT_SESSION_TIMEOUT = 300`
  (OpenWrt wiki/"5 minutes"); passing our own value overrides it.
* Password matching (`rpc_login_test_password`), in order:
  1. **empty/absent hash ⇒ ACCEPT** — a `login` section without a usable
     `password` option is an open door. `saadway-provision` must always set
     the password, and the T046 checklist asserts the option exists.
  2. **`$p$<username>`** ⇒ `getspnam()` against `/etc/shadow` (rpcd is built
     `HAVE_SHADOW`; on failure falls back to `getpwnam()`) — the Principle-IV
     legal path we use for `admin` (`50-saadway-rpcd`). The `$p$` suffix
     NAMES the shadow row, so it must equal the login user name.
  3. any other literal ⇒ `crypt()` comparison (NOT permitted by Principle IV;
     unused here).
* The UCI login section is found by scanning ALL `config login` sections of
  `/etc/config/rpcd` for an exact `username` match, then testing the
  password. **Wrong credentials return `UBUS_STATUS_PERMISSION_DENIED (6)`**
  — rpcd's `rpc_handle_login()` sets `rv = UBUS_STATUS_PERMISSION_DENIED`
  when no login section matches, and the upstream `ubusmsg.h` enum is
  0-based (`PERMISSION_DENIED=6`, `TIMEOUT=7`, `UNKNOWN_ERROR=9`,
  `CONNECTION_FAILED=10`). That is the `result[0]==6` the dashboard
  re-login logic keys on.
* ACLs: after login, rpcd runs
  `glob(RPC_SESSION_ACL_DIR "/*.json")` over `/usr/share/rpcd/acl.d/` and
  grants everything whose access group matches the section's
  `list read`/`list write` entries; a `write` entry implicates `read`
  (fallback check), and `!group` negative entries deny. Our
  `saadway/files/rpc-acl.json` defines the group `saadway` (least
  privilege — read group grants the status surface, write group the mutating
  methods).
* The all-zero session id (default session) gets ONLY the groups granted to
  `unauthenticated` in the login==NULL pass — upstream `unauthenticated.json`
  (see T-verify-1.2) grants exactly `session access/login`.

**The tokenless client (Phase 24 / T079) — the mechanism the dashboard
uses now.** The all-zero session id is not a login token but rpcd's
**default session** (`openwrt/rpcd` `include/rpcd/session.h:35`:
`RPC_DEFAULT_SESSION_ID = "00000000000000000000000000000000"`), created at
startup with `rpc_login_setup_acls(ses, NULL)` (`session.c:1396-1400`).
With a NULL login section `rpc_login_test_permission` returns true for
**exactly one** group name — `unauthenticated` (`session.c:944-946`) ⇒ the
mechanism is the ACL file's **group name**, not a login section.
`uhttpd-mod-ubus` forwards every `/ubus` call to rpcd's `session.access`
with the request's `params[0]` as the session id
(`openwrt/uhttpd` `ubus.c:280-297`, `820-840`), so a call carrying the
all-zero id is checked against the default session's grants. Two
consequences the implementation depends on: (i) the ACL change only reaches
the default session after rpcd restarts — the first-boot script MUST reload
rpcd; (ii) the ACL file's `read`/`write` blocks are the permission buckets
used for the login-section match and do **not** narrow the ubus method set
(`session.c:993-1025` grants (scope, object, function) regardless of the
bucket), so a `"*"` entry covers the whole object. The SPA therefore
carries no token at all, and the panel's own `uci` scope is checked
separately as `rpc_session_access(sid, "uci", <config>, "read"|"write")`
(`uci.c:324-349`) — the scope names the CONFIG, never a section.

**On-device checklist:**

* `ls -l /etc/shadow` → mode 0600, owner root (shadow-mode assumption —
  README of the provisioning flow documents this as required).
* `admin` in `/etc/passwd` with `nologin` shell and `/var` home;
  grep proves the `password '$p$admin'` option in
  /etc/config/rpcd section `saadway_admin` — and NO other credential
  material (`grep -r admin /etc/config/rpcd` shows only the `$p$admin`
  reference).
* Login with the right password works; with a wrong one returns
  `result[0]==6` (`UBUS_STATUS_PERMISSION_DENIED`, T-verify-6 recipe).

## 3. T-verify-2 — strongSwan / XFRM package set

**Verified (build-time):**

* 25.12 carries strongSwan 6.0.x (6.0.3 charon reported in the field on
  25.12.2/kernel 6.12.74).
* `kmod-xfrm-interface` is a **separate kmod** that is NOT a hard dependency
  of every strongSwan distribution — the known gap is documented in
  `openwrt/packages#21989`; images built via ASU must list it explicitly
  (our `DEPENDS` does) — KEEP in install list: `strongswan-mod-eap-mschapv2`,
  `strongswan-mod-gcm`, `kmod-xfrm-interface`.
* netifd's `proto xfrm` handler ships with netifd (`/lib/netifd/proto/xfrm.sh`)
  in 25.12 base.

**Watch item:** `openwrt/openwrt#23102` — on 25.12.2 with a **DSA bridge
slave uplink** (BPI-R4, `br-wan` public IP), a working CHILD_SA can still
drop outbound ESP (ESPin/ESPout 0). T046 must verify actual ESP traffic on
the test target (`ip -s xfrm state` counters after data flow) and, if the
bench hits this specific DSA-bridge topology, move WAN out of the bridge
before declaring a product bug.

**On-device checklist:**

* `apk info -i | grep -E 'strongswan|xfrm'` shows the three packages above.
* `ls /lib/netifd/proto/xfrm.sh` exists.
* After an IKEv2 profile activates: `ip -s xfrm state` shows nonzero
  byte counters; `xfrm_<ifid>_<seq>` interface exists in
  `/sys/class/net/`.

## 4. T-verify-3 — fw4 include strategy

**Decision recorded (deliberate divergence, constitution-compliant):** we do
NOT integrate the `saadway` nft table via fw4's include mechanism
(`/etc/nftables.d/*.nft`). fw4 re-renders its includes on every
`fw4 reload/update` and would couple our atomic single-transaction table to
firewall regeneration timing. Instead `policy.c` renders
`/var/run/saadway/policy.nft` and applies it atomically via
`nft -f` (delete+create in ONE transaction, catch-all chain fail-closed);
fw4 and the `saadway` table have independent lifecycles by design.

**On-device checklist:**

* `nft list tables` contains `table inet saadway` after service start.
* After `service firewall restart`, the table is either still present or
  re-applied by the daemon tick within seconds (`mgmt.status` shows no
  containment surprise); no fw-order-dependent leak is observable via the
  T042-verified order (see `tests/leak-check.sh`).
* `nft --debug parser -f /var/run/saadway/policy.nft` parses clean.

## 5. T-verify-6 — JSON-RPC denial envelope (exact codes)

Legacy `POST /ubus` (the transport our SPA uses):

| observed surface | code | meaning | SPA action |
|---|---|---|---|
| `result[0] == 0` | 0 | `UBUS_STATUS_OK` | ok (`result[1]` payload) | proceed |
| rpcd login failure or handler denial | **6** (`UBUS_STATUS_PERMISSION_DENIED`) | wrong credentials / denied | re-login trigger |
| handler validation error | **2** (`UBUS_STATUS_INVALID_ARGUMENT`) | 400-class, `detail` string in error payload | surface error |
| unknown named resource | **4** (`UBUS_STATUS_NOT_FOUND`) | 404-class | surface error |
| daemon internal failure | **9** (`UBUS_STATUS_UNKNOWN_ERROR`) | 500-class | surface error |
| ubus timeout | **7** (`UBUS_STATUS_TIMEOUT`) | transient | surface error (NOT a re-login trigger) |
| ubus not reachable/transport | **10** (`UBUS_STATUS_CONNECTION_FAILED`) | transient | surface error (NOT a re-login trigger) |
| default session lost / unknown sid | −32001 "-32001 Session not found*" | defensive (upstream defines it; current uhttpd flow uses −32002) | re-login trigger |
| uhttpd ACL pre-check denial | **−32002** "Access denied" | session lacks the ACL tuple | re-login trigger |
| unknown object | −32000 "Object not found" | object absent | surface error |
| uhttpd request timeout | −32003 | exceeded `script_timeout` | surface error |
| malformed JSON / params | −32700 / −32602 | parse/params | re-login NOT implied |
| unknown/other | **−1** (client-side fallback), **17** & **401** defensive | forward-compat slots | re-login trigger |

\* defined in `uhttpd` `ubus.c` `json_errors[]` but not emitted by the
current code path; retained as a defensive re-login slot in `api.js`.

**Build-time resolution:** the `isDenied` set in `htdocs/js/api.js`
(`9`, `17`, `401`, `-1` plus the modern `error.code` nonzeros below −32000)
is a documented defensive superset; `701`-style `7` codes stayed OUT (a
transport hiccup must not log the user out). On-device, T046 records the
codes actually observed (bad password → 9; tampered token → −32002) and any
slot that never appears stays listed for a future pruning pass (harmless:
any nonzero is already an error banner).

**On-device checklist (curl recipes):**

1. bad password:
   `curl -s http://<lan-ip>/ubus -d '{"jsonrpc":"2.0","id":1,"method":"call","params":["00000000000000000000000000000000","session","login",{"username":"admin","password":"wrong"}]}'`
   → `result[0] == 6` (`UBUS_STATUS_PERMISSION_DENIED`).
2. valid token, then a call with a fabricated token:
   → `error.code == -32002` (or `result[0]` nonzero with the SPA fallback
   shape — record whichever envelope appears in the T046 table).

## 6. T-verify-7 — SHA-256 shim ⇄ libmbedtls equivalence (SUPERSEDED, Phase 10)

* Phase 10 (T049) removed the daemon-side dashboard auth module
  (`sha256_hex` / `random_salt_hex` / `sw_auth_*` in `uci-state.c`) — it was
  orphaned code from the superseded salted-hash design (zero call sites);
  dashboard authentication is the rpcd `$p$admin` shadow bridge
  (§2 here), and `libmbedtls` dropped out of the package `DEPENDS` as dead
  weight (no other consumer in the daemon).
* The dev-only host shim (`/tmp/opencode/swstubs/shim_impl/mbedtls_impl.c`,
  never shipped) therefore has no daemon consumer anymore; it is retained
  as unit-suite infrastructure only.
* Reference vectors remain pinned here for any future digest consumer
  (host shim or device libmbedtls — identical math required):

| input | SHA-256 |
|---|---|
| `""` | `e3b0c44298fc1c149afbf4c8996fb924 27ae41e4649b934ca495991b7852b855` |
| `"abc"` | `ba7816bf8f01cfea414140de5dae2223 b00361a396177a9cb410ff61f20015ad` |
| `"The quick brown fox jumps over the lazy dog"` | `d7a8fbb307d7809469ca9abcb0082e4f 8d5651e46d3cdb762d02d0bf37c9e592` |

* On-device verification of the auth digest path is no longer applicable:
  rpcd/`crypt(3)` owns it. Assert instead that `/etc/config/rpcd` has the
  `saadway_admin` section with `option password '$p$admin'`
  (§2 checklist).

## 7. Recorded implementation deviation — `health.c` probes

*research.md D2* described supervised CHILD processes for the ICMP/DNS/HTTP
probe chain. The shipped `src/health.c` implements all three phases
**in-process** on non-blocking `uloop` fds:

* ICMP: `SOCK_DGRAM`+`IPPROTO_ICMP` (kernel-owned echo id — no raw socket
  privilege beyond root; explicit `SO_BINDTODEVICE` to the tunnel L3
  interface per Principle III), deadline `SW_T_PING`;
* DNS: raw UDP socket to `127.0.0.1:5353` (tunnel resolver), bounded wait;
* HTTP: plain `SOCK_STREAM` GET (`generate_204` target), `SO_BINDTODEVICE`
  to the tunnel interface, bounded wait.

Guarantee parity with D2 is preserved: every phase has an explicit UCI-
derived deadline; nothing blocks the `poll()` loop; the ≤5 s SC-003 pair
budget holds by construction (3 s + 4 s phases overlap-constrained by the
master deadline). **Flag:** the spawn-based D2 wording in research.md was
amended during Phase 10 (T050) to the in-process probe model; this file
remains the deviation record. On-device, T046 re-verifies
the timeout semantics with blocked targets (`tests/smoke-container.sh`).

* **Recorded deviation (Phase 22 / T072) — the dashboard ships with a
  well-known default credential.** The operator asked for the dashboard to
  be reachable with `admin` / `admin` instead of the previous
  `saadway-admin` user with no default password. The default is applied
  **only** as a crypt(3) hash in the 0600 `/etc/shadow`: upstream rpcd
  resolves the `$p$<name>` suffix with `getspnam(<name>)` and compares
  through `rpc_login_test_password()` (`openwrt/rpcd` `session.c`), so the
  UCI option stays `'$p$admin'` — a reference, never a value. The
  provisioning script hands the plaintext to the password setter over
  **stdin** (`chpasswd` reads `user:password` from stdin; OpenWrt's busybox
  carries `chpasswd`/`cryptpw`/`passwd` — `openwrt/openwrt`
  `package/utils/busybox/config/loginutils/Config.in`), because argv is
  readable through `/proc/<pid>/cmdline`; it probes for a setter and fails
  closed when none exists. The SPA pre-fills the USERNAME only: it is
  served to every LAN client, so a pre-filled password would publish the
  credential in the page source.
  **Compensating controls:** the uhttpd listener is LAN-bound (Phase 20 /
  T066), the ACL grant is least-privilege (the `saadway` group only, never
  a wildcard), the account is nologin (`-s /bin/false`), the credential
  never leaves `/etc/shadow`, and `saadway-provision` prints a
  change-password warning. **Operator action:** run
  `/usr/libexec/saadway/provision passwd` to leave the default behind.
  Migration: the login section is updated in place (the old script created
  it only when missing, so an upgraded device would have kept
  `username='saadway-admin'` / `'$p$saadway-admin'` forever) and the legacy
  `saadway-admin` passwd/shadow rows are removed by the provisioning step;
  `uci commit rpcd` makes both durable (a batch alone writes only the
  `/tmp/.uci` delta). Enforced by `tests/dashboard-credential-audit.sh`
  (T073).

* **Recorded deviation (Phase 24 / T079) — the standalone dashboard is
  published to the LAN without a credential.** The maintainer directed on
  2026-10-21 that opening `/saadway/` MUST show the dashboard directly,
  with no login prompt, and that its ubus calls MUST succeed without a
  session token. The ratified spec's FR-020 ("protected by its own
  username/password … unauthenticated access from any LAN client MUST be
  denied") is therefore **withdrawn**: FR-020/FR-021/FR-028/FR-032/FR-034
  were amended, FR-035 and SC-012/SC-013 were added, and the constitution's
  Principle V was amended to **v1.2.0** (the dashboard's system
  interactions go exclusively through the platform's ubus API; the dashboard
  implements no credential exchange of its own and creates no session token).
  The mechanism is upstream-native: rpcd's all-zero session id **is** its
  default session (`openwrt/rpcd` `include/rpcd/session.h:35`,
  `RPC_DEFAULT_SESSION_ID`), and with a NULL login section rpcd grants
  **only** the access group named `unauthenticated`
  (`session.c:944-946`), so `saadway/files/rpc-acl.json` gains a group of
  that exact name. `uhttpd-mod-ubus` checks every `/ubus` call through
  rpcd's `session.access` with `params[0]` as the session id
  (`openwrt/uhttpd` `ubus.c:280-297`, `820-840`), so the SPA's tokenless
  envelope lands on the default session's grants.
  **Withdrawn protection:** the dashboard-owned credential and its session
  tokens. **Residual risk:** every host that can reach the LAN-bound
  uhttpd listener can drive the whole `saadway` ubus object — the service
  toggle, the profile/steering surface and the credential action
  (`mgmt.credentials.set`) are all reachable; a LAN-side attacker can also
  rotate the optional rpcd session credential.
  **Compensating controls:** the uhttpd listener stays LAN-bound
  (`saadway/files/uci-defaults/40-saadway-uhttpd` — the wildcard listener
  is never restored) and the firewall's WAN input policy is the
  network-layer guard, so the dashboard is not reachable from the WAN side;
  the group covers the `saadway` object only, so no other ubus object is
  exposed; the installation no longer ships a well-known default password
  (the first-boot path stopped provisioning one), so the only credential
  on the device is one the administrator chose.
  **Operational guidance:** the dashboard is a LAN-admin surface — treat
  every LAN host as inside its trust boundary, and use the router's own
  firewall (a guest VLAN / a firewall zone that denies the dashboard's
  port) to narrow it. Enforced by `tests/luci-spa-audit.sh` (T080) and, on
  device, by the T046/T057 round (SC-012/SC-013).

## 8. libubus deferred-reply signature (build-time check)

`mon.health_test` and `profile.activate` complete asynchronously (SC-003 /
SC-001) via the **real upstream API** — fetched from `openwrt/ubus@master`
`libubus.h` (the header OpenWrt 25.12 ships):

```c
/* send a reply to an incoming object method call */
int ubus_send_reply(struct ubus_context *ctx, struct ubus_request_data *req,
		    struct blob_attr *msg);

/* static inline void — copies the request data and marks the original
 * as deferred; there is NO return value and NO error signal */
static inline void ubus_defer_request(struct ubus_context *ctx,
				      struct ubus_request_data *req,
				      struct ubus_request_data *new_req)
{
    (void) ctx;
    memcpy(new_req, req, sizeof(*req));
    req->deferred = true;
}

void ubus_complete_deferred_request(struct ubus_context *ctx,
				    struct ubus_request_data *req, int ret);
```

**Corrections to the earlier record in this file** (the `**out` form
previously written here does NOT exist upstream — `struct
ubus_deferred_request` is not a libubus type at all):

* the deferred handle is a **plain `struct ubus_request_data` value**, not an
  opaque pointer — `ubus_request_data` has no `hdr` member, so
  `&dreq->hdr` is invalid (it must be `&dreq`);
* `ubus_defer_request()` returns **void** — it cannot be compared against 0;
  error handling must use a separate "deferral active" flag;
* the dev-only stub pool must mirror this API exactly (it previously declared
  the fictional `struct ubus_deferred_request`, which masked the mismatch
  from the host suite — the same masking class as the `libuci.h` incident
  fixed in Phase 14);
* upstream status codes (`ubusmsg.h` enum, 0-based):
  `UBUS_STATUS_OK=0`, `INVALID_COMMAND=1`, `INVALID_ARGUMENT=2`,
  `METHOD_NOT_FOUND=3`, `NOT_FOUND=4`, `NO_DATA=5`,
  **`PERMISSION_DENIED=6`**, **`TIMEOUT=7`**, `NOT_SUPPORTED=8`,
  **`UNKNOWN_ERROR=9`**, `CONNECTION_FAILED=10` — the earlier claim in this
  file (§5/§6) and in `htdocs/…/api.js` that "9 = PERMISSION_DENIED" is
  wrong against the upstream enum and must be re-verified against rpcd's
  `session.login` return path (Phase 15 / T061).

The host syntax-check (all 13 modules, `gcc -fsyntax-only`) and the unit
suite must be re-run against the **corrected** stub; the authoritative
`<libubus.h>` match is confirmed by the on-device/rebuild compile
(T046 pre-step `make` on the router SDK or package build).
* **Phase 15 / T061 applied.** The daemon now uses the upstream API
  exactly: `saadway/src/saadway.h` holds `struct ubus_request_data dreq;`
  plus `bool dreq_active;` (no `struct ubus_deferred_request` anywhere);
  `main.c` defers with the void `ubus_defer_request()` and replies via
  `ubus_send_reply(ctx, &dreq, …)` / `ubus_complete_deferred_request(ctx,
  &dreq, status)` (no `->hdr` — `ubus_request_data` has no such member,
  no `free()` — the handle is a value); `ubus.c`'s `mon.health_test` path
  received the same correction, and it now **defers before starting the
  probe** (the async reply can no longer race ahead of the deferral) and
  reports a start failure through the deferred completion instead of a
  plain reply. The dev-only stub pool mirrors upstream verbatim
  (`static inline void ubus_defer_request` with the memcpy +
  `req->deferred = true` semantics) and the fictional type is gone.
  Status mapping corrected against rpcd's `rpc_handle_login` (which sets
  `UBUS_STATUS_PERMISSION_DENIED`) and the 0-based upstream enum:
  `PERMISSION_DENIED=6`, `TIMEOUT=7`, `UNKNOWN_ERROR=9`,
  `CONNECTION_FAILED=10`, `INVALID_ARGUMENT=2`, `NOT_FOUND=4` — the
  SPA's `isDenied` set now includes **6** and keeps **7** out of the
  re-login trigger; `contracts/api.md`, §5/§6 and the §2/§5 checklists
  were updated accordingly. Host evidence: 13/13 modules
  `gcc -fsyntax-only` clean against the corrected stub; unit suite 21/21;
  `make -C saadway/src -n` pipeline and link line unchanged; no residual
  fictional-API reference outside explanatory comments. The authoritative
  SDK build stays in the T046/T057 on-device round.
* **OpenWrt libubox/libubus API surface (upstream facts for Phase 16 /
  T062).** `openwrt/libubox@master` `blobmsg.h`: `UBUS_METHOD` lives in
  `libubus.h` and takes **three** arguments (`_name, _handler, _policy`);
  `UBUS_METHOD_NOARG(name, handler)` is the policy-less form (policy =
  NULL, n_policy = 0) and `UBUS_METHOD(name, handler, NULL)` is NOT a
  valid substitute (upstream expands `ARRAY_SIZE(_policy)`; `ARRAY_SIZE
  (NULL)` is ill-formed). libubox ships **no** `blobmsg_add_bool` and
  **no** `blobmsg_find_name` — a boolean is written with
  `blobmsg_add_field(buf, BLOBMSG_TYPE_BOOL, …)` over a 1-byte payload
  (`BLOBMSG_TYPE_BOOL == BLOBMSG_TYPE_INT8`) and a named field is found
  with `blobmsg_parse()` + a `blobmsg_policy` table, or a
  `blobmsg_for_each_attr` walk comparing `blobmsg_name()`. libubox's JSON
  encoder (`blobmsg_json.c`) renders `BLOBMSG_TYPE_BOOL` as
  `"true"`/`"false"`, so a locally implemented boolean writer keeps the
  `contracts/api.md` boolean contract exact. The dev-only stub pool
  masked all of this (fictional 2-argument `UBUS_METHOD` with
  `UBUS_METHOD_NOARG` aliased to it, plus the two non-existent blobmsg
  helpers) — same masking class as the `libuci.h` and
  `struct ubus_deferred_request` incidents; the pool must mirror
  upstream symbol-for-symbol from now on.
* **Phase 16 / T062 applied.** The 16 two-argument `UBUS_METHOD(name,
  handler)` entries are now the upstream policy-less
  `UBUS_METHOD_NOARG(name, handler)` form (all 24 method names unchanged
  and still matching `contracts/api.md`); libubox ships no boolean writer
  and no named-field lookup, so `saadway/src/saadway.h` now provides
  `blobmsg_add_bool()` (a 1-byte `BLOBMSG_TYPE_BOOL` write — upstream's
  JSON encoder renders it `true`/`false`, so the api.md boolean contract
  holds) and `blobmsg_find_name()` (a `blobmsg_for_each_attr` walk), plus
  a type-checked `blobmsg_find_name_typed()`. Because the methods carry
  no upstream policy table (libubox's `blobmsg_policy` has no
  "optional field" notion, so a policy would reject the optional
  parameters the contract allows), every value read is now type-checked
  before it is interpreted — this closed a live type-confusion in
  `mon.logs`, where the SPA sends `lines` as a number but the daemon read
  it as a string; a new `arg_int()` helper accepts a JSON number
  (`BLOBMSG_TYPE_INT32`) or a numeric string for the cursor/lines/
  seconds/attempts arguments. The dev-only stub pool now mirrors
  upstream: `libubox/blob.h` and `libubox/utils.h` were added (upstream
  `ARRAY_SIZE` and `__packed` sources), `blobmsg.h` carries the upstream
  type enum, `blobmsg_for_each_attr` macro and `blobmsg_add_field`, the
  two fictional helpers are gone, and `libubus.h` now declares upstream's
  three-argument `UBUS_METHOD` plus the real `UBUS_METHOD_NOARG` with an
  upstream-shaped `struct ubus_method`. Host evidence: 13/13 modules
  compile clean under `-Wall -Wextra` against the corrected stub; unit
  suite 21/21; `make -C saadway/src -n` pipeline and link line unchanged;
  all 24 method names verified against `contracts/api.md`; the SPA
  payloads were checked against the new type guards (booleans arrive as
  `BLOBMSG_TYPE_BOOL`, `lines`/`seconds`/`attempts` as `INT32`, `cursor`
  as a numeric string). **Observed, not fixed (out of T062 scope):**
  `uci-state.c:376` (`sw_util_normalize_name`) writes
  `dst[o++] = (char)(o ? c : 'a')` — reading and modifying `o` in one
  expression is undefined behaviour and trips `-Wsequence-point`; it needs
  its own remediation task. The authoritative SDK build stays in the
  T046/T057 on-device round.
* **Link-surface audit (upstream/host facts for Phase 17 / T063).**
  `saadway/src/saadway.h:481` declares `extern struct sw_ctx
  *global_ctx;` — a declaration only; `main()` heap-allocates the
  context (`calloc(1, sizeof(*c))`, main.c:547) and assigns
  `global_ctx = c;` (main.c:552) while `ubus.c`/`dns.c` dereference it,
  so the symbol was referenced but **defined nowhere** in the daemon
  (the unit suite linked only because `src/unit/test-stubs.c:17`
  supplies its own definition, whose comment already states "owned by
  main.c"). A host-side `nm` audit over the 13 compiled modules shows
  that after this fix **every** unresolved symbol is a target-library
  API (blob*/blobmsg*/ubus*/uci*/json_object*/uloop*) or libc — the
  daemon-internal set (`sw_*`, `h_*`, `global_ctx`) is fully resolved.
  That audit is the reusable check the Phase-17 task turns into
  `tests/link-audit.sh`; it detects missing definitions long before an
  SDK build reaches the link step.
* **Phase 17 / T063 applied.** `saadway/src/main.c` now owns the single
  definition of the daemon context — a file-scope, non-static
  `struct sw_ctx *global_ctx = NULL;` — while the context itself stays
  heap-allocated and `main()` keeps assigning the pointer
  (`global_ctx = c;`) right after the `calloc()`, i.e. before logging,
  UCI load, signal handlers and the uloop loop are set up. `saadway.h`
  keeps the `extern` declaration (ubus.c/dns.c dereference it) and now
  documents that the one definition lives in `main.c`; the unit harness
  keeps its own definition in `src/unit/test-stubs.c` only because its
  SRC list excludes `main.c` (adding `main.c` there would create a
  duplicate symbol — recorded as an invariant in the task).
  New `tests/link-audit.sh` (host-side, shellcheck-clean): compiles the
  13 modules against the dev stubs and fails when a daemon-internal
  symbol (`sw_*` / `h_*` / `global_ctx` / …) is referenced but undefined
  by the module set, classifying the target-library families
  (`blob*`, `blobmsg*`, `ubus*`, `uci*`, `json_*`, `uloop*`) and the C
  library as external. Its negative self-test (the definition removed
  from a scratch copy) reproduces the original failure by naming
  `global_ctx`, so the check demonstrably has teeth. Host evidence:
  link-audit OK; 13/13 modules compile under `-Wall -Wextra`; unit suite
  21/21. The authoritative SDK link stays in the T046/T057 round.
* **LuCI app packaging in an SDK-only tree (facts for Phase 18 /
  T064).** `luci-app-saadway/Makefile` currently ends with
  `include $(TOPDIR)/feeds/luci/luci.mk` — that path exists only in a
  full OpenWrt source tree with the luci feed checked out. In an SDK
  drop-in (`<sdk>/package/luci-app-saadway/`) the include resolves to
  nothing, `BuildPackage` is never evaluated, the metadata scanner does
  not register the package, and the build fails with "No rule to make
  target 'package/luci-app-saadway/compile'". The fix is a standalone
  `include $(INCLUDE_DIR)/package.mk` definition with an explicit
  `Package/luci-app-saadway` block; note that `PKG_VERSION` and
  `PKG_RELEASE` are MANDATORY there (the build system errors with
  "missing the VERSION field" — the luci.mk flow used to supply them
  implicitly). The package also ships no LuCI menu entry
  (`/usr/share/luci/menu.d/*.json`), and LuCI2's client router builds
  its route tree from exactly those files — so the documented
  "Administration → SaadWay" entry (FR-022, quickstart Scenario 4) could
  not appear; the standalone rewrite adds it in the same `root/` tree,
  gated by the same ACL group the app already installs.
* **Phase 18 / T064 applied.** `luci-app-saadway/Makefile` is now a
  standalone package: `include $(INCLUDE_DIR)/package.mk` + an explicit
  `Package/luci-app-saadway` block (SECTION/CATEGORY/TITLE/DEPENDS
  `+saadway`/`PKGARCH:=all`/the mandatory `PKG_VERSION:=1.0.0` +
  `PKG_RELEASE:=1`/MAINTAINER/PKG_LICENSE) ending in
  `$(eval $(call BuildPackage,luci-app-saadway))` — no luci-feed
  include remains (a strict `feeds/luci` grep is clean). The install
  rules place the view under `/www/luci-static/resources/view/saadway/`,
  the session ACL under `/usr/share/rpcd/acl.d/` and the new menu entry
  under `/usr/share/luci/menu.d/`. The new
  `root/usr/share/luci/menu.d/luci-app-saadway.json` registers
  `admin/saadway` → view `saadway/dashboard` with
  `depends.acl = ["luci-app-saadway"]` — the very group this package
  installs — so the documented Administration → SaadWay surface exists
  (LuCI2 builds its route tree from exactly these menu files) while the
  page stays limited to status + toggle + link (Principle V / FR-022).
  Verification: a host-side stub-SDK make parse (stub `rules.mk` +
  `package.mk` with a `BuildPackage` probe) reports
  `BUILD_PACKAGE_CALLED:luci-app-saadway` and parses cleanly — i.e. the
  metadata scanner now registers the package and
  `package/luci-app-saadway/compile` exists; all three install-referenced
  assets exist; both JSON files parse and their ACL group / menu
  `depends.acl` names match; `PKGARCH:=all` with no shipped binary; the
  asset audit in §9 now lists the LuCI app's three files. The
  authoritative `make package/luci-app-saadway/compile` in the SDK that
  produced `saadway-1.0.0-r1.apk`, plus the on-device check that the
  page appears and its two `saadway.mgmt.*` calls are authorized, stay
  in the T046/T057 round.
* **Package metadata contract (upstream facts for Phase 19 / T065).**
  `include/package-defaults.mk` → `Package/Default` derives the version
  as `VERSION:=$(PKG_VERSION)-r$(PKG_RELEASE)` (falling back to
  `$(PKG_VERSION)` or `$(PKG_RELEASE)`), and `include/package.mk` →
  `BuildPackage` evaluates `$(eval $(Package/Default))` **before**
  `$(eval $(Package/$(1)))`; the mandatory-field check then reads the
  package-scoped `VERSION` and aborts with "Package/… is missing the
  VERSION field". Consequence: `PKG_VERSION`/`PKG_RELEASE` must be
  declared at the TOP LEVEL of the package Makefile — setting them inside
  the package define is too late, because `Package/Default` has already
  run with an empty `PKG_VERSION` (this is exactly what T064 did, and
  the SDK rejected it). Our own core package gets this right
  (`saadway/Makefile:17-19` → `saadway-1.0.0-r1.apk`), so the LuCI app
  must mirror that pattern. Second, a file-only package must define
  `Build/Configure` and `Build/Compile` as EMPTY blocks: the default
  `Build/Compile` runs `$(MAKE) -C $(PKG_BUILD_DIR)` where no Makefile
  exists, and the default `Build/Configure` looks for a `./configure`
  script that does not exist.
* **Phase 19 / T065 applied.** `luci-app-saadway/Makefile` now declares
  the version metadata at the TOP LEVEL (`PKG_NAME:=luci-app-saadway`,
  `PKG_VERSION:=1.0.0`, `PKG_RELEASE:=1`, before
  `include $(INCLUDE_DIR)/package.mk`) and the package-scoped field
  explicitly (`VERSION:=$(PKG_VERSION)-r$(PKG_RELEASE)`), and defines
  `Build/Configure` and `Build/Compile` as EMPTY blocks — a file-only
  package needs them, because upstream's default `Build/Compile` runs
  `$(MAKE) -C $(PKG_BUILD_DIR)` where no Makefile exists. The core
  package needed no change: it already declares its version at the top
  level (`saadway/Makefile:17-19`), which is why it packages as
  `saadway-1.0.0-r1.apk`.
  New `tests/pkg-metadata-audit.sh` (POSIX sh, shellcheck-clean)
  emulates the upstream contract on the build host: a stub
  `rules.mk`/`package.mk` replicating `Package/Default`'s VERSION
  derivation, upstream's evaluation order (`$(Package/Default)` BEFORE
  `$(Package/<name>)`) and the mandatory-field check, applied to BOTH
  package Makefiles. Two subtleties the emulation had to get right:
  upstream escapes the check as `$$(error …)` — an unescaped `$(error …)`
  inside a `$(foreach …)` body is expanded with the body TEXT and fires
  unconditionally, which produced a false failure; and the audit
  distinguishes an UNDEFINED `Build/Compile` (upstream's default applies)
  from a deliberately EMPTY one via `$(origin …)`. Both negative cases
  are proven: the pre-fix LuCI Makefile reproduces the SDK's exact
  "Package/luci-app-saadway is missing the VERSION field" abort, and a
  file-only package without the empty build blocks is reported as
  `Build/Compile is 'undefined' (must be an empty block)`.

* **uhttpd listener address contract (upstream facts for Phase 20 /
  T066).** `openwrt/uhttpd@master` → `listen.c` → `uh_socket_bind()`
  resolves the listener host with `getaddrinfo(host, port, &hints, &addrs)`
  and, on failure, prints `getaddrinfo(): %s` (`gai_strerror()`) and
  returns 0 — no socket is bound, so the dashboard and its `/ubus`
  transport are unreachable. `openwrt/openwrt@master` →
  `package/network/services/uhttpd/files/uhttpd.config` holds FOUR
  wildcard entries (`list listen_http 0.0.0.0:80`, `list listen_http
  [::]:80` and the two https equivalents) and `.../uhttpd.init` turns
  EVERY list entry into one `-p <host:port>` argument
  (`config_get http "$cfg" listen_http` + `for listen in $http`).
  `openwrt/netifd@master` → `ubus.c` → `interface_ip_dump_address_list()`
  reports each address as an OBJECT (`"address"` written with
  `inet_ntop()`, i.e. bare, plus a separate `"mask"` field) — so
  `jsonfilter -e '@.ipv4-address[0]'` prints the object's JSON text and
  the leaf must be selected: `@["ipv4-address"][0].address`.
  `openwrt/jsonpath@master` (the `jsonfilter` implementation) prints a
  matched STRING raw and a matched OBJECT as its JSON text.
  `openwrt/uci@master` → `list.c` → `uci_add_list()` APPENDS to an
  existing list (and CONVERTS a string option into a two-element list),
  and `cli.c` → `uci_batch()` keeps going after a failing command.
* **Phase 20 / T066 applied.** `saadway/files/uci-defaults/40-saadway-uhttpd`
  now (i) selects the address leaf explicitly
  (`jsonfilter -e '@["ipv4-address"][0].address'`), (ii) strips any CIDR
  mask (`LAN_IP="${LANADDR%%/*}"`), (iii) accepts the value only when it
  is a bare IPv4 literal — four decimal octets, each 0-255 — and
  otherwise leaves the stock listener configuration untouched (fail-safe:
  never write a value that can take uhttpd down), (iv) deletes the
  wildcard entries before adding the LAN listeners
  (`delete uhttpd.main.listen_http` + `add_list …='<LAN_IP>:80'`, same
  for https) so uhttpd cannot keep listening on every interface, and
  (v) commits the change (`uci -q commit uhttpd`) — `uci batch` alone
  writes only the `/tmp/.uci` delta, so the binding would be lost on the
  next reboot. The upgrade repair path: the script is installed with
  `$(CP) ./files/uci-defaults/* $(1)/etc/uci-defaults/` (not a conffile),
  so a package upgrade re-installs it and `/etc/init.d/boot` re-runs it
  on the next boot → the stale `192.168.5.1/24:80` value is replaced.
  `tests/uci-defaults-audit.sh` (T067) reproduces the failure offline: it
  runs the script under a stub `uci`/`ubus`/`jsonfilter` PATH and asserts
  the bare address, the CIDR strip, the "write nothing when unusable",
  the delete-before-add_list order, the commit and the exit code — it
  FAILS with 7 findings against the pre-fix snapshot
  (`tests/fixtures/40-saadway-uhttpd.prefix`) and passes against the
  shipped script. The on-device proof (`uci get uhttpd.main.listen_http`
  → `<lan-ip>:80`, no `getaddrinfo()` in `logread`, the SPA reachable at
  `http://<lan-ip>/saadway/`) is folded into the T046/T057 round and
  quickstart Scenario 4 step 5.

* **ubus socket + procd trigger contract (upstream facts for Phase 21 /
  T069).** `openwrt/ubus@master` → `CMakeLists.txt`:
  `SET(UBUS_UNIX_SOCKET "/var/run/ubus/ubus.sock")` (+ `ADD_DEFINITIONS
  (-DUBUS_UNIX_SOCKET=…)`), and `libubus-io.c` → `ubus_reconnect()`:
  `if (!path) path = UBUS_UNIX_SOCKET;` → **`ubus_connect(NULL)` resolves
  the system socket**. `UBUS_UNIX_SOCKET` is a library-internal
  compile-time constant (it is in no public header) → a client cannot
  reference it, so a client-side copy of the path is a second source of
  truth. `openwrt/openwrt@master` →
  `package/system/procd/files/procd.sh` (installed as
  `/lib/functions/procd.sh`): the `_procd_wrapper` export block IS the
  definitive `procd_*` API surface — it exports `procd_add_reload_trigger`,
  `procd_add_reload_data_trigger`, `procd_add_reload_interface_trigger`,
  `procd_add_reload_mount_trigger`, `procd_add_restart_mount_trigger`,
  `procd_add_raw_trigger`, `procd_add_config_trigger` … but **no**
  `procd_add_restart_trigger`; `_procd_add_reload_trigger()` adds a
  `config.change` trigger on the named config file →
  `/etc/init.d/<service> reload`, i.e. a RELOAD (SIGHUP) trigger.
  `procd_set_param` has no `depends` key → upstream offers no "wait for
  ubus" parameter; the boot-order race is covered by the respawn policy
  already set in `start_service()` (`respawn 3600 5 5`).
* **Phase 21 / T069 applied.** `saadway/src/saadway.h` no longer defines
  `SW_UBUS_SOCK` (the pre-21.02 path `/var/run/ubus.sock`; the stock
  25.12 socket lives in `/var/run/ubus/ubus.sock`) — a comment records
  why the daemon must not carry its own copy of the path.
  `saadway/src/main.c` now connects with `ubus_connect(NULL)` so libubus
  resolves the system socket, and the fail-closed error message no longer
  interpolates a path it does not know (`cannot connect ubus: %s`).
  `saadway/files/saadway.init:45` replaced the non-existent
  `procd_add_restart_trigger "network"` with
  `procd_add_reload_trigger "network"` → a `config.change` on
  `/etc/config/network` now runs `/etc/init.d/saadway reload` → the
  daemon's SIGHUP handler re-reads UCI and re-applies the policy
  atomically (no tunnel drop) — the intent the original comment
  described. Everything else in the init script (START/STOP, USE_PROCD,
  the respawn policy, `reload_signal SIGHUP`, the two existing reload
  triggers) is unchanged. `tests/service-startup-audit.sh` (T070) proves
  both halves offline: every `procd_*` call in the init script must be in
  the upstream exported set (`tests/fixtures/procd-exported-helpers.txt`,
  28 helpers), and the daemon sources must carry no hardcoded socket
  path — comments are stripped before both checks. Against the pre-fix
  snapshot it reports `procd_add_restart_trigger is NOT exported
  upstream` plus the two files with the hardcoded path (3 findings);
  against the fixed tree it is clean. Host-side validation:
  `tests/link-audit.sh` green (13 modules), the unit suite 21/21, both
  new audits green, shellcheck clean. The on-device proof (no
  "cannot connect ubus" in `logread`, `ubus list | grep saadway` shows
  the object, `ubus call saadway mgmt.status` answers) is folded into the
  T046/T057 round and quickstart Scenario 1 step 4.

* **Dashboard default credential (upstream facts for Phase 22 / T072).**
  `openwrt/rpcd@master` → `session.c:832-858`: a login section whose
  `option password` starts with `$p$` is resolved with
  `getspnam(hash + 3)` and the candidate password is compared through
  `rpc_login_test_password(sp->sp_pwdp, password)`; a missing shadow entry
  fails the login (fail-closed). The `$p$` suffix therefore NAMES the
  shadow row and must equal the login user name — hence `username='admin'`
  with `option password '$p$admin'`.
  `openwrt/openwrt@master` →
  `package/utils/busybox/config/loginutils/Config.in`: OpenWrt's busybox
  carries the `chpasswd` and `cryptpw` applets next to `passwd`
  (`BUSYBOX_CONFIG_CHPASSWD`, `BUSYBOX_CONFIG_CRYPTPW`) → the default
  password can be applied non-interactively with the plaintext passed over
  **stdin** (`chpasswd` reads `user:password` from stdin), never through
  argv (`/proc/<pid>/cmdline` is world-readable).
* **Phase 22 / T072 applied.** `saadway/files/uci-defaults/50-saadway-rpcd`
  now sets `username='admin'` and `password='$p$admin'`, and the script
  became idempotent AND migrating: the login section is created only when
  missing (the `read`/`write` lists belong to the creation — `add_list`
  appends and would duplicate them), while the credential mapping is
  ALWAYS refreshed, so an already-provisioned device is migrated instead
  of keeping `saadway-admin` / `$p$saadway-admin` forever. The script now
  also runs `uci -q commit rpcd` — `uci batch` writes only the `/tmp/.uci`
  delta, so without the commit the mapping (and the migration) would be
  lost on the next reboot (the same defect class as the uhttpd listener in
  Phase 20 / T066).
  `saadway/files/saadway-provision` now provisions the user `admin` and
  applies the DEFAULT password non-interactively: the plaintext is piped
  to `chpasswd` (fallbacks: `passwd` from stdin, then `cryptpw` + a staged
  0600 `/etc/shadow` rewrite), the script probes for a setter and fails
  closed when none exists, a new `passwd` action changes the password
  interactively, and both the `set` and `unset` paths remove the stale
  `saadway-admin` passwd/shadow rows. The account stays nologin
  (`-s /bin/false`), UID 355, group `saadway`, and the ACL grant stays
  least-privilege. `saadway/files/htdocs/saadway/index.html` pre-fills the
  USERNAME only — the SPA is served to every LAN client, so a pre-filled
  password would publish the credential in the page source;
  `saadway/files/rpc-acl.json` names the new login in its description
  (the ACL group stays `saadway`). No new UI strings → the fa/en i18n
  catalogs stay in parity. `tests/dashboard-credential-audit.sh` (T073)
  proves the contract offline: the `$p$` reference (never a value), the
  commit, the least-privilege grant, no plaintext credential in any
  UCI-writing file, no password in the page source, the password piped
  rather than passed as an argument, and the retirement of the legacy
  login — it reports 5 findings against the pre-change tree and is clean
  against the shipped one. The weak default is recorded as a deviation in
  §7 with its compensating controls. The on-device proof (an
  `admin`/`admin` login, `$p$admin` in `/etc/config/rpcd`, a hash-only
  0600 `/etc/shadow` row, no credential material in the page source) is
  folded into the T046/T057 round.

* **Specification amendment (2026-10-21) — dashboard authentication & LuCI
  credential management.** The maintainer requested (a) a reliably
  initialized default dashboard credential (`admin` / `admin`) and (b) a
  second, strictly bounded LuCI tab for managing that credential. (a)
  changes the shipped behaviour: the credential used to exist only after a
  manual provisioning step, so a fresh installation rejected the documented
  default ("wrong username or password"). (b) conflicts with the
  dual-UI rule, so the **constitution was amended to v1.1.0** (MINOR):
  Principle V and quality gate 6 now permit the standalone dashboard's own
  login-credential form inside LuCI (single bounded exception), with the
  rationale and migration impact in the constitution's amendment log —
  mirrored in the tracked decision record `research.md` D10 (the
  `.specify/` tree is not version-controlled). The spec now carries
  FR-022 (amended), FR-028 → FR-034, SC-009 → SC-011, the
  `DashboardCredential` entity, four new edge cases and four new
  clarification decisions; the implementation phase for it is still to be
  created (`/speckit.tasks`). The upstream facts it rests on are unchanged
  and were verified again for this round: `openwrt/rpcd` `session.c:832-858`
  (the `$p$<name>` bridge — `getspnam(hash + 3)` +
  `rpc_login_test_password`) and `openwrt/openwrt`
  `package/utils/busybox/config/loginutils/Config.in` (`chpasswd` /
  `cryptpw` — the password is applied over stdin, never through argv).

* **Dashboard credential flow (upstream facts for Phase 23 / T075–T078).**
  `openwrt/rpcd` `sys.c:81-141` (the `rpcd-mod-rpcsys` plugin's
  `rpc-sys password_set`): the upstream mechanism for setting a password
  is to **fork `/bin/passwd <user>` and write the password to the child's
  STDIN** — the plaintext never appears in argv and the platform performs
  the crypt(3) hashing into the 0600 `/etc/shadow`. Our provisioning
  helper follows the identical mechanism (`chpasswd` → `passwd` → `cryptpw`
  + a staged 0600 `/etc/shadow` rewrite). `openwrt/rpcd` `session.c:832-858`
  (the `$p$<name>` bridge: `getspnam(hash + 3)` + `rpc_login_test_password`)
  and `openwrt/openwrt`
  `package/utils/busybox/config/loginutils/Config.in` (`chpasswd`,
  `cryptpw`, `passwd`) are the other two upstream facts. On the ACL side,
  `openwrt/rpcd` `session.c:935-990` (`rpc_login_test_permission` →
  `fnmatch` over the login section's `list read`/`list write`, with `*`
  matching every group and `!group` denying) proves that a login section
  listing only `saadway` never receives the `saadway-credentials` group,
  while the stock root section (`*`) does.
* **Phase 23 / T075–T078 applied.** The default dashboard credential is
  now initialized **automatically and idempotently**: `50-saadway-rpcd`
  calls the provisioning helper's new `init` mode, which creates the
  nologin account `admin` and applies the default password **only when the
  account does not exist yet** (a package upgrade re-runs the first-boot
  script and MUST NOT overwrite a credential the administrator changed —
  FR-028/SC-009). The login section was renamed to `saadway_login`
  (`username 'admin'`, `password '$p$admin'`, `list read/write 'saadway'`),
  is still created only when missing, always refreshed, committed
  (`uci commit rpcd`), and the pre-T072 `saadway_admin` section is deleted.
  The hash is produced by the **platform's** crypt(3) with the plaintext
  piped to `chpasswd`/`passwd` over stdin — never an argument, never a
  world-readable UCI option. T076 added the apply path: the new
  `saadway/src/creds.c` module validates the input and relays the secret
  **once** over a pipe to the helper's new `set-credentials` mode (create
  the account → set the password → update `saadway_login` → commit → reload
  rpcd — in that order, so a failure leaves the previous credential
  usable); the ubus method `saadway.mgmt.credentials.set` defers its reply
  and is completed by the child reaper with a bounded 15 s timeout. The
  daemon never stores a credential and never computes a hash. The new
  root-only ACL group `saadway-credentials` grants exactly that one
  method; the dashboard's own `saadway` group does not hold it (FR-034).
  T077 refactored the LuCI view into a `form.Map` with exactly two tabs
  (upstream `tab()`/`taboption()`), the credentials tab carrying the three
  masked fields and the standard *Save & Apply*, whose write path is the
  single ubus call — the browser never writes UCI and never sees the
  stored secret. T078 shipped `tests/credential-flow-audit.sh`, an offline
  audit of the whole flow (the `$p$` reference, the stdin piping, the
  initialize-only-when-absent rule, the fail-closed ordering, the ACL
  scoping, and the absence of a credential store in the daemon): it reports
  10 findings against the pre-change state and is clean against the
  shipped tree.

## 9. Footprint pointers → T045

* Zero references to iptables/ipset/fw3 in the whole tree (Gate 1/2) —
  enforced by the T045 audit grep, re-run at release (only negative
  assertions/comments appeared).
* Package assets actually installed by `saadway/Makefile`: `saadway`
  binary (build tree `$(PKG_BUILD_DIR)/saadway` → `/usr/bin/saadway`),
  `files/saadway.conf` → `/etc/config/saadway`, `saadway.init` →
  `/etc/init.d/`, `core.nft` + `bypass-cidr.txt` → `/etc/saadway/`,
  `rpc-acl.json` → `/usr/share/rpcd/acl.d/saadway.json`,
  `saadway-ovpn-updown` + `saadway-provision` → `/usr/libexec/saadway/`,
  SPA docroot → `/www/saadway/`, uci-defaults `40-saadway-uhttpd` +
  `50-saadway-rpcd` → `/etc/uci-defaults/`. All referenced sources exist;
  the formerly empty `/etc/saadway/htdocs` install dir is removed.
* credential-flow assets (Phase 23 / T075–T077): `saadway/src/creds.c`
  (the daemon module that validates and relays the credential change),
  `luci-app-saadway/root/usr/share/rpcd/acl.d/saadway-credentials.json`
  (the root-only group granting exactly one method —
  `saadway.mgmt.credentials.set`) and the tabbed LuCI view
  `luci-app-saadway/htdocs/luci-static/resources/view/saadway/dashboard.js`
  (two tabs). The dashboard's own `saadway` group
  (`saadway/files/rpc-acl.json`) MUST NOT contain that method — the
  standalone dashboard may never re-key its own login (FR-034). The
  credential flow is audited offline by `tests/credential-flow-audit.sh`
  (T078).
* auth-bypass assets (Phase 24 / T079): the standalone SPA is tokenless
  (`saadway/files/htdocs/saadway/js/api.js` sends rpcd's all-zero default
  session id; `js/app.js` has no login gate; `index.html` ships no login
  markup; the five retired i18n keys are gone from both locales, parity
  108=108), and the authorization is the ACL group named **`unauthenticated`**
  in `saadway/files/rpc-acl.json` (→ `/usr/share/rpcd/acl.d/saadway.json`),
  which reaches the `saadway` ubus object and **no** other object and
  carries **no** `uci` scope. The LuCI view
  (`luci-app-saadway/htdocs/luci-static/resources/view/saadway/dashboard.js`)
  declares the built-in `view` module and binds `form.Map('saadway')` +
  `TypedSection 'service'` — the service's own coordinates — and the app's
  ACL group (`luci-app-saadway/root/usr/share/rpcd/acl.d/luci-app-saadway.json`)
  grants the `uci` scope in the upstream shape (`"uci": [ "saadway" ]`).
  `saadway/files/uci-defaults/50-saadway-rpcd` provisions **no** default
  password, refreshes the credential mapping only when the account exists,
  and still commits the package and reloads rpcd. Asserted by
  `tests/luci-spa-audit.sh` (T080 — 17 findings against the pre-T079 tree).
* uci-defaults assets (Phase 20 / T066): `40-saadway-uhttpd` →
  `/etc/uci-defaults/` writes the uhttpd listener options. The listener
  value is assembled ONLY from a validated bare IPv4 literal (four decimal
  octets, 0-255): the address is stripped of any CIDR mask
  (`LAN_IP="${LANADDR%%/*}"`) and rejected outright when it is not a bare
  IPv4 — so no `/` can reach `uhttpd.main.listen_http` /
  `uhttpd.main.listen_https` (uhttpd resolves the host with
  `getaddrinfo()`; `192.168.5.1/24:80` made it abort the bind with
  "getaddrinfo(): Name does not resolve"). The wildcards are deleted
  before the LAN address is added (a bare `add_list` would APPEND to the
  stock `0.0.0.0:80` / `[::]:80` entries and keep uhttpd listening on
  every interface), and the change is committed (`uci batch` alone only
  writes a /tmp delta). Enforced by `tests/uci-defaults-audit.sh` (T067).
* LuCI app assets (`luci-app-saadway`, T064): the view
  `htdocs/luci-static/resources/view/saadway/dashboard.js` →
  `/www/luci-static/resources/view/saadway/`, the session ACL
  `root/usr/share/rpcd/acl.d/luci-app-saadway.json` →
  `/usr/share/rpcd/acl.d/`, and the menu entry
  `root/usr/share/luci/menu.d/luci-app-saadway.json` →
  `/usr/share/luci/menu.d/`. The ACL group name (`luci-app-saadway`) is
  the same one the menu entry's `depends.acl` references, so the menu
  gate and the RPC grant stay consistent. The package is `PKGARCH:=all`
  and ships no binary.
* `DEPENDS` (core package): `libubus libubox libuci libblobmsg-json
  libjson-c firewall4 kmod-nft-core kmod-nft-nat procd uci ubus
  dnsmasq-full ip-full openvpn-openssl strongswan strongswan-swanctl
  strongswan-mod-eap-mschapv2 strongswan-mod-gcm kmod-xfrm-interface
  uhttpd uhttpd-mod-ubus rpcd` — no banned runtime/tooling dependency.
  Phase 10 changed exactly two entries: `dnsmasq` → `dnsmasq-full`
  (T047, §10) and `libmbedtls` removed (T049, §6/§10).
  `PKG_CONFIG_DEPENDS` only over `openvpn-openssl`, `strongswan`,
  `strongswan-swanctl`.
* LuCI app is the separate package `luci-app-saadway`
  (`LUCI_DEPENDS:=+saadway +luci-base`) — Per Principle V/FR-022 the
  dashboard UI config stays OUT of the LuCI dependency chain. The app ships
  `root/usr/share/rpcd/acl.d/luci-app-saadway.json` (T048): group
  `luci-app-saadway` granting ubus `saadway` ≈ `mgmt.status`/`mgmt.set`
  only — upstream `luci-base.json` grants no ubus wildcard for third-party
  objects, and stock rpcd.config grants the root login `list read/write
  '*'`, so this group attaches to LuCI root sessions with zero rpcd config
  changes (§10 record).
* Link/dependency parity audit (Phase 12 structure superseded by the
  Phase 13 delegation restructure): the link line now lives in
  `src/Makefile` — `-lubus -lubox -luci -ljson-c -lpthread` — each with
  a staging `DEPENDS` counterpart (`+libubus +libubox +libuci
  +libjson-c`; pthread rides musl libc — no `+libpthread` package for
  musl targets, `-lpthread` is a stub there). `-lmbedtls -lmbedcrypto
  -lcrypt` stay OUT (zero daemon consumers after the T049 auth-module
  deletion — `grep -rn 'crypt(' src/*.c` empty). The host unit suite
  keeps its dev-only `-lcrypt` link flag (never shipped).

---

## 10. Phase 10 convergence record (T047–T052)

Upstream-verified resolutions appended by `/speckit.converge`, implemented
this phase; each traces to the appended tasks.md lines.

* **T047 — dnsmasq-full (CRITICAL).** openwrt.git
  `package/network/services/dnsmasq/Makefile`: plain `dnsmasq` =
  `VARIANT:=nodhcpv6` minimal build; `dnsmasq-full` = `VARIANT:=full`,
  `PROVIDES:=dnsmasq`, with `PACKAGE_dnsmasq_full_nftset` default **y**
  (its extra sub-dependency `nftables-json` is pulled transitively).
  Stock `dnsmasq` cannot parse `nftset=` — our dedicated
  `/var/run/saadway/dnsmasq-tunnel.conf` (spawned `/usr/sbin/dnsmasq
  --conf-file=…`, dns.c) emits those lines from `steering.c:120`, so the
  minimal variant would break FR-009 domain bypass. Fix: `DEPENDS:=
  +dnsmasq-full`. Install ordering (both opkg-era and apk-era precedent,
  e.g. passwall installers): remove `dnsmasq` and install `dnsmasq-full`
  in one command, DNS gap expected for the swap seconds; quickstart
  Prerequisites now carries the exact two commands. Image-builder users
  must drop stock `dnsmasq` from the package list (`-dnsmasq
  dnsmasq-full`) to avoid the file collision.
* **T048 — LuCI session ACL (HIGH).** Fetched upstream
  `luci-base.json` (master): groups `unauthenticated`/`luci-base`/
  `luci-base-network-status` — NO ubus wildcard for third-party objects,
  so the view's `mgmt.status`/`mgmt.set` calls would get the uhttpd
  pre-check permission denial envelope (§5). Fetched upstream rpcd
  `files/rpcd.config`: the default `root` login section lists
  `read '*'` / `write '*'` — any top-level group in any
  `/usr/share/rpcd/acl.d/*.json` file attaches to root sessions
  automatically. Shipped `luci-app-saadway/root/usr/share/rpcd/acl.d/
  luci-app-saadway.json` (group `luci-app-saadway`, the canonical
  luci-app-example naming shape) granting exactly `mgmt.status`
  (read) + `mgmt.status`/`mgmt.set` (write). Installed via the luci.mk
  `root/` convention; verify on-device in quickstart Scenario 4.
* **T049 — auth model reconciliation (MEDIUM).** Deleted the orphaned
  daemon-side auth module (`sw_auth_init`/`sw_auth_create_dedicated`/
  `sw_auth_verify`/`ded_verify`/`root_verify`/`sha256_hex`/
  `random_salt_hex`, `SW_AUTH_FILE`, `enum sw_auth_mode`) — zero call
  sites, superseded by the rpcd `$p$admin` shadow bridge
  (Constitution IV). Cascade: `libmbedtls` out of `DEPENDS` (the deleted
  module was its only daemon consumer); unit-suite shim infra retained
  dev-only (§6 superseded). Docs synced to the implemented model:
  `data-model.md` §DashboardSession + storage layout,
  `quickstart.md` Setup (`saadway-provision` first, then login;
  `authentication='system'` = root login), `contracts/uci.md` gained the
  shipped `dashboard` section (language/authentication/session_timeout).
  On-device T046 checks are unchanged — §2 checklist stands.
* **T050 — health-probe model ratification (MEDIUM).** research.md D2
  decision paragraph now matches `health.c`: supervised children are
  openvpn/swanctl/the dedicated dnsmasq resolver; ICMP + HTTP-204 probes
  are in-process non-blocking sockets (rationale in §7). health.c's
  header citation of "research.md D2 in-process decision" is now
  truthful; §7 keeps the full deviation rationale for reviewers.
* **T051 — i18n parity (LOW).** `"bps": "bit/s"` added to
  `htdocs/saadway/i18n/en.json` (fa.json had `"bps": "b"`); fa/en key
  sets are equal again, all `I18n.t()`/`data-i18n` keys verified
  resolvable beforehand.
* **T052 — luci-proto-xfrm (LOW, recorded-not-added).** plan.md lists
  `luci-proto-xfrm` among Primary Dependencies; the daemon materializes
  xfrm devices itself (`ip link add … type xfrm`, `kmod-xfrm-interface`
  in DEPENDS) and creates no netifd xfrm interface sections, so the LuCI
  netifd proto page is not consumed by any shipped flow. Decision: do NOT
  extend DEPENDS; keep footprint (Gate 2). If a future change adopts
  netifd xfrm interfaces, add `luci-proto-xfrm` then.
* **T053 — session_timeout runtime-effectiveness (Phase 11).** The second
  converge pass found the option loaded/clamped daemon-side
  (`uci-state.c:855–857`, min 300 s) but never consumed at runtime: the SPA
  posted a hardcoded 1800 at `session.login`, so admin-set values never
  governed expiry. Fixed by adding `session_timeout` to the
  `mgmt.status` reply (`ubus.c:h_mgmt_status`), the api.md field list, and
  `api.js` (TTL learned from any successful `mgmt.status`; the pre-status
  first login keeps the 1800 s conf-default fallback). Same pass closed a
  missed T049 residue: api.md's login paragraph still described the
  superseded "dedicated 0600 hash file" — now states the rpcd `$p$`
  shadow bridge. On-device T046 adds one assertion: with
  `dashboard.session_timeout='600'`, a dashboard idle-out beyond ~10 min
  requires re-login (Scenario 4 step 3 wording).
* **Phase 12 — cross-compile & packaging remediation (T054–T057).** The
  25.12 SDK build attempt exposed two real defects: (a) `Build/Prepare`
  copied `./src/*` — dragging `src/unit/` (host test Makefile, shims,
  test sources, and the previously committed ELF test binaries) into
  `PKG_BUILD_DIR`; (b) the link line still carried
  `-lmbedtls -lmbedcrypto -lcrypt` although T049 removed `libmbedtls`
  from `DEPENDS` (nothing stages it → clean-SDK link error), with zero
  remaining `crypt()` consumers after the auth-module deletion. Fixes:
  prepare copies `./src/*.c ./src/*.h` only; flags/libs hoisted into
  `SAADWAY_CFLAGS` / `SAADWAY_LIBS` (link order: objects,
  `$(TARGET_LDFLAGS)`, `-lubus -lubox -luci -ljson-c`); stray
  `saadway/src/*.o` deleted; the four committed unit-test ELF binaries
  `git rm --cached`; `.gitignore` refined (`*.o`, `*.a`,
  `saadway/src/unit/test_[a-z]*` with `!saadway/src/unit/test_*.c`
  negation so test sources stay tracked). Host-side integrity evidence:
  all 13 modules object-compile through the separated flag line with the
  dev stub headers; prepare-exclusion proof (copy contains zero `unit`
  entries); `git check-ignore` semantics verified (binaries ignored,
  `.c` sources untouched). **T057 remains device-bound**:
  `tests/smoke-container.sh` is the Gate-3 on-device gate (its header:
  real 25.12 device, nft+ubus, active tunnel; the build host has no
  container runtime) — its execution folds into the T046 round and must
  PASS there after these packaging fixes.
* **Phase 13 — idiomatic delegation rebuild (T058–T059).** The Phase-12
  in-package compile still failed on the real SDK (`ld.bfd: cannot find
  …`), so packaging now follows the idiomatic OpenWrt pattern: new
  `saadway/src/Makefile` compiles the 13 modules and links binary
  `saadway` with `-lubus -lubox -luci -ljson-c -lpthread` (json-c kept
  vs the requested lib list — `ubus.c`/`backup.c` consume
  `json_object_*`; omitting it would trade the ld.bfd error for
  undefined references); the package Makefile has NO custom
  `Build/Compile` (OpenWrt's default `$(MAKE) -C $(PKG_BUILD_DIR)`
  carries `TARGET_CC`/`TARGET_CFLAGS`/`TARGET_LDFLAGS`, all honored via
  `?=` in src/Makefile); `Build/Prepare` copies `./src/*` then removes
  `unit/` and stray `*.o`; the binary installs as `/usr/bin/saadway`
  and `saadway.init`'s `PROG` follows. The `saadwayd` identity strings
  in source (syslog tag, ICMP payload, User-Agent, rendered-file
  headers) are unchanged. Host validation: `make -n` shows the exact
  13-object pipeline + link line; a real object compile with an
  overridden `CFLAGS` proves the cross-flag plumbing; `make clean`
  works; shellcheck stays clean.
* **OpenWrt `Build/Compile` mechanics (upstream facts for Phase 14 /
  T060).** `include/package-defaults.mk` (openwrt@main): `MAKE_VARS` =
  `CFLAGS="$(TARGET_CFLAGS) $(EXTRA_CFLAGS) $(TARGET_CPPFLAGS)
  $(EXTRA_CPPFLAGS)"` and `MAKE_FLAGS` = `$(TARGET_CONFIGURE_OPTS)
  CROSS="$(TARGET_CROSS)" ARCH="$(ARCH)"`; the default `Build/Compile`
  runs `$(MAKE) -C $(PKG_BUILD_DIR)/$(MAKE_PATH)` with those vars and
  passes **no `CC`**. `include/toplevel.mk` force-clears the environment
  (`export CFLAGS=`, `export LDFLAGS=`). Therefore a delegated
  `src/Makefile` must default `CC ?= $(CROSS)gcc`, consume `$(CFLAGS)`
  and `$(CPPFLAGS)`, and receive the target staging include path through
  `TARGET_CPPFLAGS` (folded into `CFLAGS` upstream) — i.e.
  `TARGET_CPPFLAGS += -I$(STAGING_DIR)/usr/include` in the package
  Makefile, with no custom `Build/Compile` block. Header-name check for
  the same round: libuci installs **`<uci.h>`** (not `<libuci.h>`);
  `<libubox/uloop.h>`, `<libubox/blobmsg.h>`, `<libubus.h>` and
  `<json-c/json.h>` are correct as-is. The host unit suite was masked
  from this class of error because the DEV-ONLY stub pool named its
  declare-only header `libuci.h` — the pool must mirror real OpenWrt
  header names from now on.
* **Phase 14 / T060 applied.** `saadway/src/saadway.h` now includes
  `<uci.h>`; `saadway/Makefile` exports
  `TARGET_CPPFLAGS += -I$(STAGING_DIR)/usr/include` (upstream folds it into
  `CFLAGS` — no custom `Build/Compile`); `saadway/src/Makefile` selects the
  compiler with an **origin guard** (`ifeq ($(origin CC),default)` →
  `CC := $(CROSS)gcc`): GNU make predefines `CC=cc` and `?=` does NOT
  override a built-in default, so the earlier `CC ?= $(CROSS)gcc` draft
  would have kept the HOST compiler — the same "no such file or
  directory" symptom the SDK reported. The dev-only stub pool now
  provides `uci.h` (real name) and the misleading `libuci.h` was removed
  so it cannot mask the name again. Host evidence: 13/13 modules
  `gcc -fsyntax-only` clean against the renamed stub; `make -C
  saadway/src -n` shows `aarch64-linux-musl-gcc … -I<staging>/usr/include
  …` under upstream's variable set, `gcc` on a host run, and an explicit
  `CC=` still winning; a real object build through the new rule succeeds
  and `make clean` removes it; unit suite 21/21. The authoritative
  `make package/saadway/{clean,compile}` on `ipq40xx/chromium` stays in
  the T046/T057 on-device round.

---

### Phase 24 record — LuCI rendering fix & standalone SPA auth bypass (T079–T081)

* **Upstream sources the change was derived from** (fetched 2026-10-21,
  upstream is the oracle): `openwrt/luci`
  `modules/luci-base/htdocs/luci-static/resources/luci.js:1943-1951` and
  `:2243` (the `View` class and the built-in module map `view: View` — a
  view MUST declare `'require view';`; without it `view.extend` is
  undefined → the reported `TypeError`), the same file's `form.js`
  (`CBIMap`/`CBINamedSection`/`CBIMap.load` — a `form.Map` binds to exactly
  ONE uci config and the map's load check is
  `callSessionAccess('uci', <config>, 'write')`); `openwrt/rpcd`
  `include/rpcd/session.h:35` (`RPC_DEFAULT_SESSION_ID` — the all-zero id
  IS the default session), `session.c:944-946` (with a NULL login section
  ONLY the group named `unauthenticated` is granted — the mechanism),
  `session.c:1396-1400` (the default session's ACLs are granted once at
  rpcd's startup ⇒ the first-boot script MUST reload rpcd),
  `session.c:606-660`/`:1414` (the (scope, object, function) check with
  fnmatch matching), `uci.c:324-349` (the uci scope's functions are
  literally `read`/`write` — the scope names the CONFIG, never a section);
  `openwrt/uhttpd` `ubus.c:280-297` and `:820-840` (every `/ubus` call is
  checked through rpcd's `session.access` with `params[0]` as the session
  id).
* **Applied:** the LuCI view declares the built-in `view` module first and
  binds `form.Map('saadway')` + `TypedSection 'service'` (the service's own
  coordinates — `saadway/files/saadway.conf` → `config service 'service'`);
  the app's ACL group grants the `uci` scope (`"uci": [ "saadway" ]`) plus
  the ubus scope on the saadway object; `saadway/files/rpc-acl.json` gained
  the access group literally named `unauthenticated` (the `saadway` object
  only, no `uci` scope) and the `saadway` group's method lists became the
  wildcard; the SPA sends rpcd's all-zero default session id on every
  call, its login gate and session lifecycle are gone, the login markup and
  the five i18n keys are retired (parity 108=108), and the footer note no
  longer claims an authenticated API; the first-boot script provisions no
  default password, refreshes the credential mapping only when the account
  exists, and still commits the rpcd package and reloads rpcd.
* **Host evidence:** `tests/luci-spa-audit.sh` (new, T080) — clean against
  the shipped tree, **17 findings** against the pre-T079 tree;
  `tests/credential-flow-audit.sh` and `tests/dashboard-credential-audit.sh`
  re-based on the new contract (the pre-T075 and pre-T079 negative runs
  still fail); the link / package-metadata / uci-defaults / service-startup
  audits green; the unit suite 21/21; shellcheck clean on every audit; all
  shipped JavaScript parses; all shipped JSON valid; the i18n locales 1:1.
* **On-device evidence** stays in the T046/T057 round: the LuCI page
  renders both tabs with an empty browser console, `/saadway/` opens
  straight into the dashboard (no prompt, no token), every panel action
  succeeds, the tokenless client is refused for every non-`saadway` ubus
  object, and the dashboard is unreachable from the WAN side (SC-012,
  SC-013). The residual risk of a LAN-scoped, unauthenticated dashboard
  surface is recorded in section 7.

### Phase 25 record — convergence gap closure (T082–T090)

* **Two undefined behaviours removed** (both flagged by `-Wsequence-point`):
  `saadway/src/policy.c` — `wans[nwans++] = wan_names[nwans];` modified and
  read `nwans` without a sequence point, so the right-hand read could observe
  the incremented index and store a **wild pointer** in the WAN-interface
  list; the store and the increment are now sequenced explicitly and the
  array bound is checked. `saadway/src/uci-state.c` —
  `dst[o++] = (char)(o ? c : 'a');` had the same defect in the
  profile-name normalization helper (kept, not deleted: the sequencing is
  fixed).
* **The daemon now compiles clean**: 5 unused variables removed
  (`ubus.c` ×2, `tunnel-openvpn.c` ×2, `counters.c` ×1) — the 14 modules
  report **0** warnings under `gcc -fsyntax-only -std=c11 -Wall -Wextra`,
  which is the check that would have caught the two defects above.
* **The IKEv2 form now carries the FR-004 surface**: Type (IKEv2 · username
  auth / MSCHAPv2, fixed), Remote ID and Local ID (each with its "Same as …"
  default plus a custom override that is sent as `remote_id`/`local_id`),
  Certificate ("Leave default · from server" — the only mode the daemon
  accepts) and Proxy (Off, fixed), plus the FR-005 encryption readout
  (AES-256-GCM · SHA-256 · ECP384 / ECP521 — the daemon's own default
  proposals). The daemon and the API already supported all of it; the gap
  was UI-only.
* **The import card** has its own titled credential section (FR-002), and the
  retired session strings (`err_http_401`, the login form's `username` /
  `password` labels) are gone from both locales — the dictionaries are
  **120 = 120** ✓.
* **The access-control artifacts now describe the post-bypass reality**: the
  `saadway-credentials` group no longer claims an exclusivity the
  `unauthenticated` wildcard took away, and
  `tests/credential-flow-audit.sh` check E asserts the real state (the
  narrow grant names the method; the wildcard coverage is acknowledged and
  points at the §7 deviation) instead of a vacuous literal grep.
* **The language preference has one authority**: the daemon's
  `dashboard.language` is the *default* for a browser without a stored
  preference (exposed as `mgmt.status → language`), the browser's own
  stored choice still wins, and the decision is recorded in
  `contracts/uci.md`.
* **Host evidence**: all seven host audits green, the unit suite 21/21, the
  14-module `-Wall -Wextra` sweep clean, shellcheck clean, all shipped
  JavaScript parses, all shipped JSON valid, the i18n locales 1:1, and the
  plan's project-structure tree lists only files that exist.

### Phase 26 record — the DNS-status panel and the profile settings form (T091–T093)

* **T091 — the zero-leak rows could never show the truth.** The dashboard's
  security panel called **only** `sec.status` (`{ kill_switch, dns_forced,
  contained }`) and fed that payload to `dns_status_rows()`, which reads
  `resolver_if` and `leak_risk` — those two fields are returned by
  **`sec.dns.status`** (`{ resolver_if, leak_risk }`), so they were always
  `undefined` and the panel rendered "—" and "unknown" (FR-013/US2). The
  panel now fetches both methods and merges the payloads (a missing
  `sec.dns.status` leaves the "unknown" fallback), and `contracts/api.md`
  states which method owns which field.
* **T092 — the profile's own settings form (US1/AC2, FR-002, FR-023).** The
  dashboard had no per-profile form: the credential fields existed only in
  the import card and the SPA called `profile.update` **zero** times, even
  though the daemon implements the whole patch surface
  (`enabled`/`hostname`/`username`/`password`/`remote_id`/`local_id`/
  `cert_mode`/proposals — `saadway/src/ubus.c:465`). The profile list now
  carries an **edit** action that opens the form: the enabled switch, the
  OpenVPN hostname, the FR-002 credential section and — for IKEv2 — the EAP
  credentials plus the FR-004 identities. The form is a **patch form**:
  every field starts empty (the daemon's `profile.list` exposes no hostname
  or user name and never echoes a secret, FR-033), an empty field means
  "keep the current value", the OpenVPN credential pair travels together
  (the daemon rejects a lone password with `E_BAD_SYNTAX`) and the change
  is applied with **one** `profile.update` call whose rejections are shown
  inline.
* **T093 — the specification no longer contradicts itself.** The 2026-10-21
  clarification session still claimed that the default credential is made
  available automatically and that dashboard sessions survive a credential
  change; the two obsolete answers and the two obsolete edge cases are now
  marked **SUPERSEDED**, and a new session records the auth-bypass model
  (no credential, no session, the `unauthenticated` ACL group, the
  network-boundary access control, and the §7 residual risk). The FR/SC
  text, the user stories and their acceptance scenarios were **not**
  changed.
* **Host evidence**: all seven host audits green, the unit suite 21/21, the
  14-module `-Wall -Wextra` sweep clean, shellcheck clean, all shipped
  JavaScript parses, all shipped JSON valid, the i18n locales **129 = 129**,
  and the spec's obsolete-claim grep shows only the amended wording and the
  marked history.

### Phase 27 record — the OpenVPN hostname field and the stale auth model (T094–T096)

* **T094 — the form no longer promises what the daemon cannot do.** The
  OpenVPN branch of the profile settings form (Phase 26 / T092) offered a
  "Server hostname/IP" field and the daemon accepted the patch into
  `p->hostname` — but `saadway/src/tunnel-openvpn.c` never reads
  `p->hostname`: an OpenVPN profile's server comes from the imported
  file's `remote` directive (`m.remote_host`, `tunnel-openvpn.c:162-169`),
  written into the 0600 rendered config at import time. The field is
  removed from the OpenVPN branch (the `.ovpn` file is the server's single
  source of truth); the IKEv2 branch keeps its identity fields, where
  `hostname` **is** the server (`remote_addrs` in the swanctl config).
  `contracts/api.md` now states the patchable fields and that `hostname`
  applies to IKEv2 only, and that the OpenVPN credential pair travels
  together.
* **T095 — the data model describes the model that ships.** The
  `DashboardSession` entity claimed a dedicated account with a *default*
  password, a session token, a 1800 s inactivity expiry and — worst —
  *"Invalid/absent token: all `saadway.*` RPC calls rejected"*, the exact
  opposite of the tokenless behaviour; the `DashboardCredential` state
  diagram still showed the retired "default (admin / admin)" state. Both
  are rewritten: no session, no credential, the all-zero default session
  id, the `unauthenticated` group as the authorization path, the optional
  credential created on demand, and the network boundary as the access
  control (the §7 residual risk).
* **T096 — the contract states each method's exact payload.** The
  `steer.device.set` / `steer.device.remove` row implied a `policy?` for
  the remove, which the daemon does not read; the row is split. The other
  combined row (`steer.domain.add` / `steer.domain.remove` — both take
  exactly `{ domain }`) was verified against the daemon and left as it is.
* **Host evidence**: all seven host audits green, the unit suite 21/21, the
  14-module `-Wall -Wextra` sweep clean, shellcheck clean, all shipped
  JavaScript parses, all shipped JSON valid, the i18n locales **128 = 128**,
  and the data model's stale-claim grep is clean.

### Phase 28 record — the `.ovpn` device override and the data model's UCI types (T097–T099)

* **T097 — the import now refuses a device override.** The data model
  requires the importer to reject "`dev` overrides conflicting with
  saadwaydev naming", but the ban list carried `dev-node` and not `dev`:
  a `dev tun5` line was copied verbatim into the rendered 0600 config and
  was only neutralised because the renderer appends its own
  `dev-type tun` + `dev <saadwaydev>` identity **last** — i.e. the safety
  rested on openvpn's option order rather than on the import refusing the
  directive, and the operator got no feedback. `dev` is now banned: the
  import fails closed with `E_UNSUPPORTED_DIRECTIVE` (no partial save),
  and the renderer's own identity lines remain as the interface identity
  (Principle III).
* **T098 — the data model names the sections that exist.** It declared the
  UCI section type `saadway_service` in two places (the IP-range auto-bypass
  and the `ServiceState` entity) — a type that does not exist: the shipped
  and contracted type is `service` (`contracts/uci.md`, `saadway.conf`,
  `uci-state.c:896`), with the dashboard preferences in `dashboard`. Both
  declarations are corrected, and the `ServiceState` field table now lists
  the options the daemon actually reads and writes in the `service`
  section (`run_attempts` included).
* **T099 — the uniqueness rule states both behaviours.** The model claimed a
  duplicate device `match_value` is rejected; the daemon's
  `sw_uci_device_set` has **set semantics** (the existing rule's policy is
  replaced in place). The rule now states: a duplicate **domain**
  (case-insensitive, `strcasecmp`) is rejected with `E_DUPLICATE`, while a
  repeated device `match_value` updates the existing rule — the behaviour
  the dashboard's device form relies on.
* **Host evidence**: all seven host audits green, the unit suite 21/21, the
  14-module `-Wall -Wextra` sweep clean, shellcheck clean, all shipped
  JavaScript parses, all shipped JSON valid, the i18n locales **128 = 128**,
  and the data model's section types now match `saadway.conf`.

### Phase 30 record — the IKEv2 form submission & the nftables transaction (T103)

* **The frontend defect was the event dispatch, not the form.**
  `saadway/files/htdocs/saadway/js/app.js`: `panel_events()` (line 697) opened
  `if (btn) { … }` and that block ended with an **unconditional `return;`**
  (line 783), so the fall-through `panel_click(ev);` (line 784) was
  **unreachable** for every click carrying a `data-act` attribute. The whole
  profiles panel was therefore dead — *import*, **ike-create**, *activate*,
  *deactivate*, *edit* and *delete* — which is exactly "clicking Create
  produces no reaction and no console errors". The return is removed and
  `panel_click()` now scopes itself to `#panel-profiles` (it computed the
  panel id but never used it, so the other panels' buttons would otherwise
  cross-talk into it). The profile-form buttons are `type="button"`.
* **The transport already carried the reason.** uhttpd-mod-ubus's legacy
  envelope carries the daemon's reply attributes in `result[1]`
  (`openwrt/uhttpd` `ubus.c:474-492`), so `detail` reached the SPA; the
  missing piece was the fallback — `js/api.js` now maps the ubus status
  codes (2 = invalid argument, 6 = permission denied, …) to a readable
  reason and exposes `describe()`, which `err_of()` uses.
* **The nftables defect was the element list, not the transaction.**
  `saadway/src/policy.c`: `render_dev_sets()` emitted **both** device sets
  whenever either kind of rule existed, with a `;` terminator and a
  **shared** separator counter — so a single IP rule produced
  `elements = { ; }` for `dev_mac`, and a MAC rule that was not the first
  rule produced a leading `", "`. Both are nftables syntax errors: nft
  rejects the whole table, the delete+create transaction fails, the
  create-only retry fails with the same content, and the daemon falls back
  to the emergency containment (`SENTINEL: EMERGENCY CONTAINMENT
  (tunnel-class egress blackholed)`, `policy.c:580`). The sets are now
  emitted only when they have elements, with a per-list counter and
  nftables' comma syntax — the pattern `sw_steering_mark_ranges()` already
  uses in `steering.c`.
* **Diagnosability.** `run_argv()` (`policy.c:31`) sent the child's stdout
  *and* stderr to `/dev/null`, so nft's own parse error was discarded and
  the log only ever showed `rc=1`. The child's stderr now lands in a 0600
  file next to the render file and is logged with the failing transaction.
  The fail-closed behaviour is unchanged — only the diagnosis improves.
* **TDD.** `saadway/src/unit/test_policy_render.c` never asserted the
  device sets, which is why the defect shipped. It now covers an IP-only
  rule, a MAC-only rule, both kinds (with the MAC rule **not** first) and
  no rule at all, plus a "no malformed element list" guard. The new cases
  were verified to **fail against the pre-fix renderer** (3 of the 4) and to
  pass after the fix.
* **`saadway/files/core.nft`** now documents the two per-device sets and
  their element syntax, as its own header requires ("Keep it in sync with
  policy.c").
* **Host evidence**: all seven host audits green, the unit suite green, the
  14-module `-Wall -Wextra` sweep clean, shellcheck clean, all shipped
  JavaScript parses, all shipped JSON valid, the i18n locales **128 = 128**.

---

**Traceability:** Gates 1–7 are executed by `quickstart.md` scenarios 1–7
(T046, recorded on-device). This file carries the build-time half: upstream
fact-checks (sections 0–2, 5, 6), the recorded deviation (section 7), and
the on-device pre-flight recipes (section 2's checklist, section 3–5
checklists), plus the Phase-10 record above.
