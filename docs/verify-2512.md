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
     legal path we use for `saadway-admin` (`50-saadway-rpcd`).
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

**On-device checklist:**

* `ls -l /etc/shadow` → mode 0600, owner root (shadow-mode assumption —
  README of the provisioning flow documents this as required).
* `saadway-admin` in `/etc/passwd` with `nologin` shell and `/var` home;
  grep proves the `password '$p$saadway-admin'` option in
  /etc/config/rpcd section `saadway_admin`.
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
   `curl -s http://<lan-ip>/ubus -d '{"jsonrpc":"2.0","id":1,"method":"call","params":["00000000000000000000000000000000","session","login",{"username":"saadway-admin","password":"wrong"}]}'`
   → `result[0] == 6` (`UBUS_STATUS_PERMISSION_DENIED`).
2. valid token, then a call with a fabricated token:
   → `error.code == -32002` (or `result[0]` nonzero with the SPA fallback
   shape — record whichever envelope appears in the T046 table).

## 6. T-verify-7 — SHA-256 shim ⇄ libmbedtls equivalence (SUPERSEDED, Phase 10)

* Phase 10 (T049) removed the daemon-side dashboard auth module
  (`sha256_hex` / `random_salt_hex` / `sw_auth_*` in `uci-state.c`) — it was
  orphaned code from the superseded salted-hash design (zero call sites);
  dashboard authentication is the rpcd `$p$saadway-admin` shadow bridge
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
  `saadway_admin` section with `option password '$p$saadway-admin'`
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
  sites, superseded by the rpcd `$p$saadway-admin` shadow bridge
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

**Traceability:** Gates 1–7 are executed by `quickstart.md` scenarios 1–7
(T046, recorded on-device). This file carries the build-time half: upstream
fact-checks (sections 0–2, 5, 6), the recorded deviation (section 7), and
the on-device pre-flight recipes (section 2's checklist, section 3–5
checklists), plus the Phase-10 record above.
