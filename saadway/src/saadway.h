/*
 * saadway.h - shared declarations for the saadwayd daemon
 *
 * Constitution compliance notes:
 *  - Principle I:  only fw4/nftables, procd, ubus, uci infrastructure.
 *  - Principle II: single poll()-based event loop (uloop == poll() under the
 *                  hood) with the explicit timeout table below; every outbound
 *                  network operation is async with a finite timeout.
 *  - Principle III: tunnels materialize as L3 interfaces (tunX/xfrmX);
 *                  counters/health/policy bind to that interface name.
 *  - Principle IV: secrets only in 0600 files; firewall/routing mutations
 *                  applied as one nft transaction; fail closed.
 *  - Principle V:  ubus JSON-RPC only, exact payloads per contracts/api.md.
 *  - Principle VI: UCI (/etc/config/saadway) is the only persistent state.
 */
#ifndef SAADWAY_H
#define SAADWAY_H

#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <syslog.h>
#include <sys/types.h>
#include <unistd.h>

#include <libubox/uloop.h>
#include <libubus.h>
#include <uci.h>
#include <json-c/json.h>

/* ------------------------------------------------------------------ */
/* Paths                                                               */
/* ------------------------------------------------------------------ */
#define SW_UCI_FILE       "/etc/config/saadway"
#define SW_BASE_DIR       "/etc/saadway"
#define SW_SECRETS_DIR    "/etc/saadway/secrets"
#define SW_RUN_DIR        "/etc/saadway/run/openvpn"
#define SW_SWANCTL_DIR    "/etc/saadway/swanctl"
#define SW_RUNSTATE_DIR   "/var/run/saadway"
#define SW_BYPASS_FILE    "/etc/saadway/bypass-cidr.txt"
#define SW_NFT_RENDER     "/var/run/saadway/policy.nft"
#define SW_DNSMASQ_CONF   "/var/run/saadway/dnsmasq-tunnel.conf"
#define SW_DNSMASQ_PID    "/var/run/saadway/dnsmasq-tunnel.pid"
#define SW_OVPN_HOOK      "/usr/libexec/saadway/ovpn-updown"
#define SW_SWANCTL_CONF   "/etc/saadway/swanctl/swanctl.conf"
#define SW_NFT_BIN        "/usr/sbin/nft"
#define SW_IP_BIN         "/usr/sbin/ip"
/* T069: no ubus socket path here - libubus resolves the system socket
 * (ubus_connect(NULL) -> the library's own UBUS_UNIX_SOCKET default).
 * A client-side copy of the path is a second source of truth and it went
 * stale before (pre-21.02 layout); the reasoning and the upstream
 * sources are recorded in docs/verify-2512.md section 8. */

/* ------------------------------------------------------------------ */
/* Explicit timeout table (research.md D2; UCI-overridable where noted) */
/* ------------------------------------------------------------------ */
#define SW_T_ACTIVATE      30    /* s, connect attempt budget          */
#define SW_T_SWITCH_REPORT 10    /* s, SC-001 one-click switch budget  */
#define SW_T_PROBE         5     /* s, watchdog connectivity probe     */
#define SW_T_PING          3     /* s, health ICMP                     */
#define SW_T_HTTP          4     /* s, health HTTP-204 total budget    */
#define SW_T_DNSQ          2     /* s, internal DNS query for HTTP     */
#define SW_T_STOP_CHILD    3     /* s, graceful tunnel child stop      */
#define SW_T_POLICY_APPLY  8     /* s, nft -f completion wait          */

/* ------------------------------------------------------------------ */
/* fwmark classes (Constitution III / research D4)                      */
/* ------------------------------------------------------------------ */
#define SW_MARK_MASK      0xFul
#define SW_MARK_DIRECT    0x1ul   /* device policy = direct (highest)  */
#define SW_MARK_DOMAIN    0x2ul   /* domain bypass (nftset-fed)        */
#define SW_MARK_RANGE     0x4ul   /* bundled IP-range auto bypass      */
#define SW_MARK_TUNNEL    0x8ul   /* implicit/explicit tunnel default  */

/* policy routing tables + ip rule priorities */
#define SW_TABLE_TUNNEL   201
#define SW_TABLE_DIRECT   202
#define SW_RULE_PRIO_DEV  7000
#define SW_RULE_PRIO_DOM  7100
#define SW_RULE_PRIO_RNG  7200
#define SW_RULE_PRIO_TUN  7300
#define SW_RULE_PRIO_LAN  7900

/* scale bounds (spec Scale/Scope) */
#define SW_MAX_PROFILES   20
#define SW_MAX_DOMAINS    500
#define SW_MAX_DEVICES    253
#define SW_NAME_MAX       65     /* 64 chars + NUL */
#define SW_PATH_MAX       256

/* ------------------------------------------------------------------ */
/* Enums                                                               */
/* ------------------------------------------------------------------ */
enum sw_kind { SW_KIND_OPENVPN = 0, SW_KIND_IKEV2 = 1 };
enum sw_mtype { SW_MTYPE_IP = 0, SW_MTYPE_MAC = 1 };
enum sw_policy { SW_POLICY_DIRECT = 0, SW_POLICY_TUNNELED = 1 };

/* connection_status values verbatim from data-model.md UCI persistence */
enum sw_conn {
	SW_CONN_DOWN = 0, SW_CONN_UP = 1, SW_CONN_CONNECTING = 2,
	SW_CONN_CONTAINED = 3, SW_CONN_COOLDOWN = 4
};

enum sw_child_type { SW_CHILD_TUNNEL, SW_CHILD_PING, SW_CHILD_DNSMASQ,
		       SW_CHILD_CREDS };

/* ------------------------------------------------------------------ */
/* Data model (data-model.md: Profile / SteeringRule / ServiceState)    */
/* ------------------------------------------------------------------ */
struct sw_profile {
	char name[SW_NAME_MAX];
	int kind;                       /* enum sw_kind */
	bool enabled;
	int order;                      /* failover position, unique */
	int attempts;                   /* runtime failed-attempt counter (this
					 * cycle; NOT in UCI — the active
					 * profile's counter is mirrored into
					 * ServiceState.run_attempts) */
	char secret_ref[SW_PATH_MAX];   /* relative to /etc/saadway, 0600 */
	/* openvpn only */
	char ovpn_ref[SW_PATH_MAX];     /* normalized config under /etc/saadway/run/ */
	char remote_host[96];           /* informational                         */
	/* ikev2 only */
	char hostname[256];
	char username[128];
	char remote_id[256];            /* or "same-as-hostname"          */
	char local_id[256];             /* or "same-as-username"          */
	char cert_mode[24];             /* spec v1: "from-server" only    */
	char proxy[24];                 /* spec v1: "off" only            */
	char proposal_ike[128];
	char proposal_esp[128];
	int if_id;                      /* unique xfrm if_id              */
	char ifname[16];                /* materialized L3 interface name */
	struct sw_profile *next;
};

struct sw_domain_rule {
	char domain[256];               /* unique case-insensitive */
	struct sw_domain_rule *next;
};

struct sw_device_rule {
	int match_type;                 /* enum sw_mtype */
	char match_value[64];           /* IPv4 CIDR or MAC */
	int ptype;                      /* enum sw_policy */
	struct sw_device_rule *next;
};

struct sw_state {
	bool enabled;
	bool kill_switch;
	bool iprange_bypass;
	bool cycle_exhausted;
	char active_profile[SW_NAME_MAX];
	int conn_status;                /* enum sw_conn */
	int run_attempts;               /* current failed-attempt counter on active profile */
	int failover_attempts;          /* tunable, default 3 */
	int failover_cooldown;          /* tunable, default 300 */
	int probe_interval;             /* tunable, default 30 */
	int stats_interval;             /* tunable, default 1 */
	char language[4];               /* dashboard, default "fa" */
	char auth_mode[12];             /* dedicated|system */
	int session_timeout;            /* default 1800 s */
	struct sw_profile *profiles;
	struct sw_domain_rule *domains;
	struct sw_device_rule *devices;
};

struct sw_log_entry {
	long ts;
	int prio;
	long seq;
	char msg[512];
};

struct sw_health_http {
	int code;                       /* HTTP status code or 0 = timeout */
	int latency_ms;
};

struct sw_health {
	struct sw_health_http http;
	int ping_ok;                    /* 0=fail 1=ok */
	int ping_latency_ms;
	char ping_target[64];
	bool done;
	bool pending;
};

struct sw_counters {
	uint64_t last_rx;
	uint64_t last_tx;
	time_t last_ts;
	long rx_bps;
	long tx_bps;
};

/* per-activation pending state (single active invariant) */
struct sw_activate {
	bool pending;
	char name[SW_NAME_MAX];
	long deadline;                  /* monotonic target time */
	struct sw_profile *prof;
	long started;                   /* ms clock: elapsed_ms in reply */
	/* T061: upstream libubus API — the deferred handle is a plain
	 * struct ubus_request_data VALUE (there is no
	 * struct ubus_deferred_request type upstream) and
	 * ubus_defer_request() returns void, so the "reply in flight"
	 * state needs an explicit flag. */
	struct ubus_request_data dreq;  /* profile.activate reply       */
	bool dreq_active;               /* deferred reply in flight      */
};

/* ------------------------------------------------------------------ */
/* libubox compatibility helpers (T062)                                */
/*                                                                     */
/* libubox ships NEITHER blobmsg_add_bool() NOR blobmsg_find_name():   */
/*  - a boolean is written as BLOBMSG_TYPE_BOOL over a 1-byte payload  */
/*    (BLOBMSG_TYPE_BOOL == BLOBMSG_TYPE_INT8); libubox's JSON encoder */
/*    (blobmsg_json.c) renders that type as true/false, so the boolean */
/*    contract in contracts/api.md holds exactly;                       */
/*  - a named field is found by walking blobmsg_for_each_attr and      */
/*    comparing blobmsg_name().                                        */
/*                                                                     */
/* The optional type argument of blobmsg_find_name_typed() guards the  */
/* handlers against type confusion: the ubus methods are registered     */
/* without an upstream policy table (libubox's blobmsg_policy has no  */
/* "optional field" notion, so a policy would reject the optional      */
/* parameters the contract allows), so every value is type-checked     */
/* here before it is read.                                              */
/* ------------------------------------------------------------------ */

static inline int blobmsg_add_bool(struct blob_buf *buf, const char *name,
				   bool val)
{
	uint8_t v = val ? 1 : 0;

	return blobmsg_add_field(buf, BLOBMSG_TYPE_BOOL, name, &v, 1);
}

static inline struct blob_attr *blobmsg_find_name(struct blob_attr *msg,
						   const char *name)
{
	struct blob_attr *cur;
	int rem;

	if (!msg)
		return NULL;

	blobmsg_for_each_attr(cur, msg, rem) {
		if (!strcmp(blobmsg_name(cur), name))
			return cur;
	}
	return NULL;
}

/* type-checked lookup: NULL when absent OR of a different type */
static inline struct blob_attr *blobmsg_find_name_typed(struct blob_attr *msg,
							const char *name,
							int type)
{
	struct blob_attr *a = blobmsg_find_name(msg, name);

	if (!a)
		return NULL;
	if (type >= 0 && blobmsg_type(a) != type)
		return NULL;
	return a;
}

struct sw_failover {
	bool cooldown_active;
	long cooldown_deadline;         /* wall seconds */
	int miss_count;                 /* consecutive probe misses */
	bool child_died;
};

struct sw_ctx {
	struct ubus_context *ubus;
	struct uloop_timeout t_probe;   /* watchdog probe timer          */
	struct uloop_timeout t_stats;   /* counters timer                */
	struct uloop_timeout t_tick;    /* failover/activation housekeeping */
	struct sw_state st;
	struct sw_profile *active;      /* currently spawned profile */
	char tun_if[16];                /* active tunnel interface       */
	struct sw_health health;
	struct sw_counters ctr;
	struct sw_activate act;
	struct sw_failover fo;
	bool terminating;
	char *lan_devices;              /* comma-joined LAN iface names  */
};

/* ------------------------------------------------------------------ */
/* log.c (T006) - syslog with secret scrubbing + ring for mon.logs      */
/* ------------------------------------------------------------------ */
void sw_log_init(void);
void sw_log_shutdown(void);
void sw_log(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sw_log_secret_register(const char *value);
void sw_log_secret_unregister_all(void);
/* cursor-based tail (FR-017): cursor=0 → oldest first; returns count */
int sw_log_snapshot(struct sw_log_entry *out, int max, long cursor, long *next_cursor);

#define sw_debug(fmt, ...)   sw_log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define sw_info(fmt, ...)    sw_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define sw_warn(fmt, ...)    sw_log(LOG_WARNING, fmt, ##__VA_ARGS__)
#define sw_err(fmt, ...)     sw_log(LOG_ERR, fmt, ##__VA_ARGS__)

/* ------------------------------------------------------------------ */
/* uci-state.c                                                          */
/* ------------------------------------------------------------------ */
struct sw_strbuf {
	char *b;
	size_t len;
	size_t cap;
	bool oom;
};
void sw_sb_init(struct sw_strbuf *s);
void sw_sb_appendf(struct sw_strbuf *s, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
void sw_sb_append(struct sw_strbuf *s, const char *text);
void sw_sb_free(struct sw_strbuf *s);
char *sw_sb_release(struct sw_strbuf *s, size_t *len);

bool sw_valid_name(const char *name);        /* 1..64 unique-safe chars  */
bool sw_valid_hostname(const char *s);
bool sw_valid_domain(const char *s);
bool sw_valid_mac(const char *s);
bool sw_valid_cidr4(const char *s, bool hostpart_ok); /* a.b.c.d[/plen] */
int sw_util_b64_decode(const char *in, unsigned char **out, size_t *outlen);
int sw_util_b64_encode(const unsigned char *in, size_t inlen, char **out);
char *sw_util_slurp(const char *path, size_t *len);   /* malloc'd buffer */
int sw_util_write_file_mode(const char *path, const char *data, size_t len, mode_t mode);
int sw_util_mkdirs(const char *path, mode_t mode);
void sw_util_string_list_add(char ***list, int *n, const char *s);
void sw_util_normalize_name(char *dst, size_t dstlen, const char *src);

/* ------------------------------------------------------------------ */
/* uci-state.c (T005/T017/T028)                                         */
/* ------------------------------------------------------------------ */
enum sw_uci_err {
	SW_UCI_OK = 0,
	SW_UCI_E_BOUNDS,      /* data-model contraint violation   */
	SW_UCI_E_DUPLICATE_NAME, /* profile.name */
	SW_UCI_E_DUPLICATE_DOMAIN,
	SW_UCI_E_DUPLICATE_DEVICE,
	SW_UCI_E_SYNTAX,      /* unused placeholder */
	SW_UCI_E_LIMIT,       /* scale bound (profiles/domains/devices) */
	SW_UCI_E_INTERNAL
};

/* parse/validate full UCI config into ctx->st  (empty config parses) */
int sw_uci_load(struct sw_ctx *c);
/* persist dynamic service state (active_profile/connection_status/counters) */
int sw_uci_persist_state(struct sw_ctx *c);
/* persist tunables (failover_attempts/cooldown/probe/stats/kill_switch…) */
int sw_uci_persist_tunables(struct sw_ctx *c);
/* persist dashboard prefs (language) */
int sw_uci_persist_dashboard(struct sw_ctx *c);
/* profile mutation helpers; call sw_uci_profile_renumber() after delete */
int sw_uci_profile_add(struct sw_ctx *c, struct sw_profile *p);
int sw_uci_profile_update(struct sw_ctx *c, struct sw_profile *p);
int sw_uci_profile_delete(struct sw_ctx *c, const char *name);
int sw_uci_profile_renumber(struct sw_ctx *c);
struct sw_profile *sw_uci_profile_find(struct sw_ctx *c, const char *name);
bool sw_uci_name_taken(struct sw_ctx *c, const char *name);
/* steering mutations */
int sw_uci_domain_add(struct sw_ctx *c, const char *domain);   /* E_DUPLICATE */
int sw_uci_domain_remove(struct sw_ctx *c, const char *domain);
int sw_uci_device_set(struct sw_ctx *c, int mtype, const char *value, int pol);
int sw_uci_device_remove(struct sw_ctx *c, int mtype, const char *value);
/* unique if_id / profile interface name derivation */
int sw_next_if_id(struct sw_ctx *c);
void sw_profile_compute_ifname(struct sw_profile *p);
/* secrets (Principle IV): create/verify 0600 files */
int sw_secret_write(const char *profile, const char *leaf, const char *data);
int sw_secret_check(const char *rel_ref);      /* verify 0600, repair or fail */
/* (dashboard auth lives in rpcd: see 50-saadway-rpcd + saadway-provision,   */
/*  FR-020 — no daemon-side credential store, Constitution IV)              */

/* ------------------------------------------------------------------ */
/* policy.c (T011/T018/T019) - atomic fw4/nftables + PBR applier         */
/* ------------------------------------------------------------------ */
void sw_policy_refresh_lan_devices(struct sw_ctx *c);
/* Full desired-ruleset render for the current state.
 * `contain`  → tunnel-class egress dropped (kill switch / not verified).
 * `tun_if`   → active tunnel interface name ("" when unknown).
 * `route_default` → NULL renders/installs no tunnel route (blackhole stays).
 * Render is pure (host-testable); apply() drives nft/ip in one transaction. */
int sw_policy_render(struct sw_ctx *c, bool contain, const char *tun_if,
		     bool route_default, char **out, size_t *outlen);
int sw_policy_apply(struct sw_ctx *c, bool contain, const char *tun_if,
		    bool route_default);
/* LAN default egress follows the WAN main table (tunnel absent; Q2-B) */
int sw_policy_apply_direct(struct sw_ctx *c);
/* fail-closed hard mask when even the normal path fails (Principle IV) */
int sw_policy_emergency_contain(struct sw_ctx *c);

/* ------------------------------------------------------------------ */
/* steering.c (T023/T024/T026)                                          */
/* ------------------------------------------------------------------ */
int  sw_steering_init(struct sw_ctx *c);       /* load bundled CIDR file */
int  sw_steering_count_ranges(void);           /* parsed CIDRs           */
void sw_steering_mark_rules(struct sw_strbuf *sb, struct sw_ctx *c);
void sw_steering_mark_ranges(struct sw_strbuf *sb);
void sw_steering_dnsconf_append(struct sw_strbuf *sb, struct sw_ctx *c);

/* ------------------------------------------------------------------ */
/* tunnel-openvpn.c (T012)  / tunnel-ikev2.c (T013)                     */
/* openvpn import validate/render; both: start/stop supervision          */
/* ------------------------------------------------------------------ */
/* import: returns 0 or UBUS error code with detail populated (E_* codes) */
int  sw_ovpn_import(struct sw_ctx *c, const char *name, const char *ovpn_b64,
		    const char *cred_user, const char *cred_pass,
		    char *detail, size_t detail_len, char **warnings);
int  sw_ovpn_start(struct sw_ctx *c, struct sw_profile *p);
void sw_ovpn_stop(struct sw_ctx *c, struct sw_profile *p, bool graceful);
int  sw_ikev2_start(struct sw_ctx *c, struct sw_profile *p);
void sw_ikev2_stop(struct sw_ctx *c, struct sw_profile *p, bool graceful);
/* pure swanctl.conf renderer stored as <name>-tunnel (host-testable) */
void sw_ikev2_render(struct sw_profile *p, const char *password,
		     struct sw_strbuf *out);
void sw_tunnel_cancel_children(struct sw_ctx *c); /* fail-safe reap */

/* ------------------------------------------------------------------ */
/* failover.c (T015) watchdog state machine                             */
/* ------------------------------------------------------------------ */
void sw_failover_init(struct sw_ctx *c);
void sw_failover_probe_result(struct sw_ctx *c, bool ok);
void sw_failover_child_died(struct sw_ctx *c);
void sw_failover_cooldown_check(struct sw_ctx *c);
void sw_failover_reset(struct sw_ctx *c);

/* ------------------------------------------------------------------ */
/* health.c (T033) async ICMP + HTTP-204, interface-bound                */
/* ------------------------------------------------------------------ */
struct sw_health_ctx;

/* kicks an async probe pair; cb fires once done (poll loop, not blocking) */
int  sw_health_run(struct sw_ctx *c, void (*cb)(struct sw_ctx *ctx, bool done));
bool sw_health_is_running(void);
void sw_health_abort(void);

/* counters.c (T034): 1 Hz RX/TX sampling on the tunnel interface */
void sw_counters_tick(struct sw_ctx *c);

/* ------------------------------------------------------------------ */
/* dns.c (T020/T026) dnsmasq tunnel resolver + bypass-domain exception   */
/* ------------------------------------------------------------------ */
int  sw_dns_reload_conf(struct sw_ctx *c);        /* (re)render + instance */
int  sw_dns_upstreams_current(char *buf, size_t bufsz); /* src policy.c   */
void sw_dns_notify_tunnel_dns_changed(void);      /* hook file event       */
void sw_dns_stop(void);
bool sw_dns_running(void);                        /* tunnel resolver live? */

/* ------------------------------------------------------------------ */
/* backup.c (T038) schema v1 export/import (contracts/backup-schema.json)*/
/* ------------------------------------------------------------------ */
int sw_backup_export(struct sw_ctx *c, bool include_credentials, char **json_out);
int sw_backup_import(struct sw_ctx *c, const char *json, char **report_out);

/* ------------------------------------------------------------------ */
/* main.c (T004/T014/T040)                                              */
/* ------------------------------------------------------------------ */
/* async activation engine; ubus deferred replies established here       */
int  sw_activate_profile(struct sw_ctx *c, struct sw_profile *p,
			 struct ubus_request_data *req);
void sw_deactivate_all(struct sw_ctx *c, bool apply_policy);
/* housekeeping tick: activation deadlines, cooldown, child reaping */
void sw_tick(struct uloop_timeout *t);
/* procd-style child reaper: uloop_process callback (registry dispatch) */
void sw_child_notify(struct uloop_process *up, int ret);
/* dns hook file watcher (tunnel DNS resolved) */
void sw_dnsfile_check(void);
/* shared: reload config + re-apply policy (SIGHUP / mgmt.set) */
void sw_reload(void);
/* main.c child registry (tunnel children, health pings, dnsmasq) */
/* creds.c — the standalone dashboard's own login credential (T076) */
bool sw_creds_busy(void);
bool sw_creds_validate(const char *username, const char *password);
int sw_creds_start(struct ubus_context *ctx, struct ubus_request_data *req,
		   const char *username, const char *password);
void sw_creds_child_exited(pid_t pid, int status);

void sw_child_track(pid_t pid, int type, void *arg);
bool sw_child_take(pid_t pid, int *type, void **arg);
pid_t sw_child_find(int type, void *arg); /* first live child of type owning arg */
void sw_child_quit_tunnel(void *arg);   /* requested stop of tunnel child */
/* The daemon's single context. T063: this is a DECLARATION only — the one
 * definition lives in main.c (the owner of the context lifetime; the
 * struct stays heap-allocated and main() assigns this pointer). Do not
 * add a second definition in another daemon translation unit. */
extern struct sw_ctx *global_ctx;
extern volatile sig_atomic_t g_child_exited;

/* ubus.c: object registration with all RPC methods */
int sw_ubus_init(struct sw_ctx *c);
void sw_policy_apply_now(struct sw_ctx *c);
#endif /* SAADWAY_H */
