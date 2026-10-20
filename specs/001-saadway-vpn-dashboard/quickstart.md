# Quickstart Validation Guide: SaadWay VPN Tunnel & Routing Dashboard

**Feature**: 001-saadway-vpn-dashboard | **Date**: 2026-10-19

Runnable end-to-end scenarios proving the feature works per spec acceptance
scenarios AND mapping each constitution quality gate (Section 3) to a test.
Implementation detail lives in `tasks.md`; schema/contract detail lives in
`contracts/` and `data-model.md`.

## Prerequisites

- OpenWrt 25.12 router (any supported MIPS/ARM target) with `fw4`, `procd`,
  `ubus`, `uci`, `uhttpd` + `uhttpd-mod-ubus` (LAN HTTPS), `dnsmasq-full`
  (steering domain bypass uses `nftset=`, which the stock minimal `dnsmasq`
  build does not support — docs/verify-2512.md §9).
- At least one valid `.ovpn` file AND one IKEv2 provider account
  (hostname/username/password per spec example format).
- A LAN client + ability to run tcpdump/drop-watch for leak verification.
- 25.12 note: the package manager is **apk** (opkg was removed in 25.12 —
  docs/verify-2512.md section 0); install with:

```sh
# steering requires the full dnsmasq variant; swap BEFORE installing saadway
# (brief DNS gap during the swap is expected):
apk del dnsmasq && apk add dnsmasq-full && /etc/init.d/dnsmasq restart

apk add saadway luci-app-saadway openvpn-openssl \
  strongswan strongswan-swanctl strongswan-mod-eap-mschapv2 \
  strongswan-mod-gcm kmod-xfrm-interface
```

## Setup

```sh
/etc/init.d/saadway enable && /etc/init.d/saadway start
# Dashboard: http://<router-ip>/saadway/   (Persian RTL default; fa/en switch)
# LuCI: Administration → SaadWay  (status + enable/disable + open-dashboard)
```

Before the first dashboard login, provision the dedicated credential over
SSH: `/usr/libexec/saadway/provision` creates the nologin user
`saadway-admin` and sets its `/etc/shadow` password hash — credential
material never lives in a world-readable UCI option (Constitution IV; rpcd
mechanics source-verified in docs/verify-2512.md §2). Log in as
`saadway-admin`; sessions expire after `dashboard.session_timeout` (1800 s)
of inactivity. Alternative mode: set
`saadway.dashboard.authentication='system'` and log in with root instead.

## Scenario 1 — Profile import/creation & activation (Story 1; Gate 4)

1. Import `provider-a.ovpn` via dashboard; verify it appears and passes
   validation (invalid file test: upload truncated `.ovpn` → expect specific
   rejection code, no partial save — FR-025).
2. Create IKEv2 profile via form; verify fields/defaults per FR-004/FR-005.
3. Activate each in turn; within 10 s confirm: `ubus call saadway mgmt.status`
   shows `connection_status=up`, `active_profile` matches, and an L3
   interface exists (`ip link show type tun` / `type xfrm`) — Gate 4.
   Expect elapsed < 10 000 ms (SC-001).
4. Verify the service itself came up on the stock image: `logread | grep
   -i saadway` contains no "cannot connect ubus" line (the daemon must
   resolve the ubus socket through libubus, not a hardcoded path),
   `ubus list | grep saadway` shows the `saadway` ubus object, and
   `/etc/init.d/saadway start` logs no "not found" from
   `/etc/rc.d/S99saadway` (the init script may only call procd helpers
   upstream exports).

## Scenario 2 — Zero-leak under switch & drop (Story 2; Gate 5)

1. Enable kill switch (`sec.kill_switch.set`), start traffic (e.g. ping from
   LAN client through tunnel), then `kill` the tunnel daemon process.
2. Run `tests/leak-check.sh` while tunnel is down: expect **0** tunneled-mark
   packets egressing WAN (SC-002).
3. Switch profiles under load: `tests/leak-check.sh` must still report 0
   leaks across the atomic nft transaction (FR-014).
4. Verify DNS leak: from LAN client, `nslookup` any name, watch
   `tcpdump port 53 -i <wan>`; queries MUST traverse the tunnel resolver
   only. Bypassed domains' DNS follows the direct path (design D5).
5. `grep -R` credential values under `/etc/config`, `/etc/saadway`, and
   dashboard responses: no plaintext secret anywhere (0600 secret files
   only, permission check included) — FR-021/SC-008.

## Scenario 3 — Traffic steering (Story 3)

1. Add bypass domain `bank.example.ir`; from LAN client visit it — traffic
   observed on WAN, not tunnel interface (SC-005 - 100 % of test requests).
2. Enable auto-bypass checkbox: local + bundled banking CIDR traffic shows
   direct path without manual rules (FR-010).
3. Set LAN client MAC → `direct`; another device default (implicit tunnel):
   verify both co-exist with per-device marks (FR-011, tunnel-default per
   clarification Q1-A).
4. Re-add duplicate domain → expect `E_DUPLICATE`, no silent double rule.
   Toggle `iprange_bypass` off/on without tunnel drop (steer mutations are
   atomic; SC/FR + Story 3 scenario 4).

## Scenario 4 — Dual UI + session security (Story 4; Gate 6)

1. LuCI page shows ONLY status + enable/disable + open-dashboard button
   (FR-022) — no settings controls.
2. Request dashboard resource without session: every `saadway.*` RPC via
   `/ubus` is denied (401-level; Gate 6).
3. Login creates ubus session; second unauthenticated device cannot use
   that token (ACL + session binding); session expires after
   `dashboard.session_timeout` of inactivity.
4. Verify SPA is Persian RTL by default with fa/en switch; preference
   persists (FR-027).
5. Verify the uhttpd listener the first-boot script wrote:
   `uci get uhttpd.main.listen_http` must print `<lan-ip>:80` — a bare
   `host:port`, never a CIDR (`192.168.5.1/24:80` makes uhttpd abort the
   bind with `getaddrinfo(): Name does not resolve` and leaves the SPA
   unreachable); `uci get uhttpd.main.listen_https` must print
   `<lan-ip>:443`; `logread` must contain no `getaddrinfo()` error; and
   the SPA must be served from `http://<lan-ip>/saadway/` on the same
   origin as `/ubus`.

## Scenario 5 — Health, bandwidth, logs (Story 5; Gate 3 blocking)

1. Click health test with tunnel up: latency ms + HTTP-204 result returned
   fully within ≤ 5 s (SC-003), dashboard interactive throughout.
2. Create dead-target condition (block the test targets in nft): the probe
   returns `timeout` gracefully and `tests/smoke-container.sh` proves UI +
   daemon stay responsive — Gate 3 explicit-timeout smoke test (FR-018).
3. Real-time: `mon.stats` polls 1 → graph without freezing; live logs
   stream an auth-failure example (inject invalid credential) and show the
   scrubbed line (no password text, FR-017).

## Scenario 6 — Failover + cool-down + WAN outage (FR-026, Edge 1)

1. With kill switch ON/off both configurations: disconnect only WAN →
   system cycles all profiles once, then enters `cooldown`
   (`failover_cooldown` = 300 s default, clarification Q3-A) and retries on
   timer; watch status field `contained`/`cooldown` — no fast infinite
   switching, kill-switch containment maintained.
2. Kill only the tunnel (WAN healthy): failover activates next profile
   within 10 s (SC-001).

## Scenario 7 — Backup / restore & boot resilience (Story 6; Gate 7)

1. Export backup (include credentials) → import onto fresh install:
   expect per-item report — `restored` 100 % items, zero errors (SC-004).
   Run `tests/restore-roundtrip.sh` for automated fidelity check.
2. Power-cycle router mid-tunnel: measure next-boot state — containment
   (if enabled) applied before any LAN egress, same active profile
   re-established, steering/kill rules reapplied; zero manual steps, 0
   leaks in window (SC-006; boot sequence ordering per contracts/uci.md).
3. Indicator: `connection_status` transitions `connecting→up` automatically
   after reboot with no user input.

## Gate ↔ scenario traceability (constitution Section 3)

| Gate | Covered by |
|---|---|
| 1 Platform | Design-time grep (CI task) + full absence of banned tooling in plan |
| 2 Footprint | Package `Makefile` DEPENDS/Maintainer review task |
| 3 Blocking | Scenario 5.2 smoke-container test |
| 4 Tunnel interface | Scenario 1.3 interface verification |
| 5 Zero-leak | Scenario 2 (all) |
| 6 UI decoupling | Scenario 4.1/4.2 |
| 7 Recoverability | Scenario 7 (all) |
