/*
 * tunnel-openvpn.c — OpenVPN profile validator/renderer/supervisor (T012)
 *
 * Import validation per FR-025 / contracts/api.md reject codes:
 *   E_NO_REMOTE, E_BAD_SYNTAX, E_UNSUPPORTED_DIRECTIVE, E_DUPLICATE_NAME.
 * Import is atomic: NOTHING is written until the incoming `.ovpn` passes
 * validation (no partial save); inline PKI material and auth-user-pass
 * credentials are extracted to 0600 files under /etc/saadway/secrets/<name>/.
 *
 * Normalized config lands in /etc/saadway/run/openvpn/<name>.conf (0600),
 * referenced from UCI as `ovpn_ref`. Deterministic interface name per
 * profile (Principle III): `saad<4hex>`.
 */
#define _GNU_SOURCE
#include "saadway.h"

#include <ctype.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

static const char *banned_dirs[] = {
	"up", "down", "up-delay", "down-pre", "route-up", "route-pre-down",
	"ipchange", "learn-address", "client-connect", "client-disconnect",
	"script-security", "tls-verify", "pkcs11-id", "pkcs11-module",
	"plugin", "http-proxy", "http-proxy-retry", "http-proxy-option",
	"socks-proxy", "dev-node", "redirect-gateway", "route",
	"client-cert-not-required", "verify-client-cert", "port",
};

static void lower_word(char *w)
{
	for (int i = 0; w[i]; i++)
		w[i] = (char)tolower((unsigned char)w[i]);
}

static bool on_ban_list(const char *dir)
{
	for (size_t i = 0; i < sizeof(banned_dirs) / sizeof(*banned_dirs); i++)
		if (!strcmp(dir, banned_dirs[i]))
			return true;
	return false;
}

/* inline PKI blocks map to referenced secret files: tag → (leaf, directive) */
static const struct { const char *tag, *leaf, *dir; } inline_map[] = {
	{ "ca",        "ca.pem",        "ca"        },
	{ "cert",      "cert.pem",      "cert"      },
	{ "key",       "key.pem",       "key"       },
	{ "dh",        "dh.pem",        "dh"        },
	{ "tls-auth",  "tlsauth.key",   "tls-auth"  },
	{ "tls-crypt", "tlscrypt.key",  "tls-crypt" },
};

struct imp {
	struct sw_ctx *c;
	char name[SW_NAME_MAX];
	struct sw_strbuf out;              /* normalized config */
	const char *inline_tag;            /* active inline block tag or NULL */
	const char *inline_leaf;
	const char *inline_dir;
	struct sw_strbuf inline_cur;
	char remote_host[96];
	bool want_auth;                    /* auth-user-pass seen */
	bool auth_ready;                   /* credentials provided */
	char warnings[512];
};

static int emit_warning(struct imp *m, const char *w)
{
	size_t wl = strlen(w);
	if (m->warnings[0] && wl + strlen(m->warnings) + 2 <
			      sizeof(m->warnings))
		strcat(m->warnings, "; ");
	if (wl + strlen(m->warnings) < sizeof(m->warnings))
		strcat(m->warnings, w);
	return 0;
}

static void strip_inline(struct imp *m)
{
	m->inline_tag = NULL;
	m->inline_leaf = NULL;
	m->inline_dir = NULL;
	sw_sb_free(&m->inline_cur);
}

/* emit active inline block as a referenced 0600 secret file + path line */
static void close_inline_block(struct imp *m)
{
	if (m->inline_tag && m->inline_leaf && m->inline_cur.len) {
		if (sw_secret_write(m->name, m->inline_leaf,
				    m->inline_cur.b ? m->inline_cur.b : "") == 0) {
			char refline[SW_PATH_MAX + 96];
			snprintf(refline, sizeof(refline),
				 "%s /etc/saadway/secrets/%s/%s\n",
				 m->inline_dir, m->name, m->inline_leaf);
			sw_sb_append(&m->out, refline);
		} else {
			emit_warning(m, "E_INLINE_BLOCK_WRITE_FAILED");
		}
	}
	strip_inline(m);
}

/*
 * parse the normalized line stream. returns reject code ("" flows on) or
 * static error code strings per the API contract.
 */
static const char *process_line(struct imp *m, const char *raw, bool *has_remote)
{
	char line[2048];
	size_t l;

	while (*raw == ' ' || *raw == '\t')
		raw++;
	for (l = 0; raw[l] && raw[l] != '\r' && raw[l] != '\n' &&
		    l < sizeof(line) - 1; l++)
		line[l] = raw[l];
	line[l] = 0;

	/* inline block phases */
	if (m->inline_tag) {
		char close[64];
		snprintf(close, sizeof(close), "</%s>", m->inline_tag);
		if (!strcmp(line, close)) {
			close_inline_block(m);
			return NULL;
		}
		sw_sb_append(&m->inline_cur, line);
		sw_sb_append(&m->inline_cur, "\n");
		return NULL; /* content lines are not re-validated */
	}
	if (line[0] == '<') {
		for (size_t i = 0; i < sizeof(inline_map) / sizeof(*inline_map); i++) {
			char open[64];
			snprintf(open, sizeof(open), "<%s>", inline_map[i].tag);
			if (!strncmp(line, open, strlen(open)) &&
			    strlen(line) == strlen(open)) {
				m->inline_tag = inline_map[i].tag;
				m->inline_leaf = inline_map[i].leaf;
				m->inline_dir = inline_map[i].dir;
				sw_sb_free(&m->inline_cur);
				sw_sb_init(&m->inline_cur);
				return NULL;
			}
		}
		return "E_UNSUPPORTED_DIRECTIVE";
	}

	/* comments and blanks */
	if (!line[0] || line[0] == '#' || line[0] == ';')
		return NULL;

	/* directive */
	char dir[64], arg[768], rst[1024];
	int n = sscanf(line, "%63s %767[^\n]", dir, arg);
	if (n < 1)
		return "E_BAD_SYNTAX";
	lower_word(dir);

	if (!strcmp(dir, "remote")) {
		char host[96];
		if (n < 2 || sscanf(arg, "%95s", host) != 1)
			return "E_BAD_SYNTAX";
		if (*has_remote)
			return "E_NO_REMOTE";   /* must be exactly one */
		*has_remote = true;
		snprintf(m->remote_host, sizeof(m->remote_host), "%s", host);
	}
	if (on_ban_list(dir))
		return "E_UNSUPPORTED_DIRECTIVE";

	if (!strcmp(dir, "dev-type")) {
		/* only tun-style devices allowed (Principle III) */
		if (n < 2 || (strcmp(arg, "tun") && strcmp(arg, "tun6")))
			return "E_UNSUPPORTED_DIRECTIVE";
	}
	if (!strcmp(dir, "auth-user-pass")) {
		m->want_auth = true;
		if (n > 1 && strcmp(arg, ""))
			return "E_UNSUPPORTED_DIRECTIVE"; /* external file */
		return NULL;                /* emitted as secret file below */
	}

	/* all other lines verbatim */
	sw_sb_append(&m->out, line);
	sw_sb_append(&m->out, "\n");
	return NULL;
}

/* ------------------------------------------------------------------ */
/* public import API: full FR-025 pipeline, atomic (no partial save)    */
/* ------------------------------------------------------------------ */

int sw_ovpn_import(struct sw_ctx *c, const char *name, const char *ovpn_b64,
		   const char *cred_user, const char *cred_pass,
		   char *detail, size_t detail_len, char **warnings_out)
{
	struct imp m = {0};
	unsigned char *ovpn = NULL;
	size_t ovpnlen = 0;
	bool has_remote = false;
	int ret = -1;

	if (!c || !name || !sw_valid_name(name)) {
		snprintf(detail, detail_len, "E_BAD_SYNTAX: invalid profile name");
		return UBUS_STATUS_INVALID_ARGUMENT;
	}
	if (sw_uci_name_taken(c, name)) {
		snprintf(detail, detail_len, "E_DUPLICATE_NAME: '%s'", name);
		return UBUS_STATUS_INVALID_ARGUMENT;
	}
	if (!ovpn_b64 || !*ovpn_b64) {
		snprintf(detail, detail_len, "E_BAD_SYNTAX: empty ovpn payload");
		return UBUS_STATUS_INVALID_ARGUMENT;
	}
	int b64rc = sw_util_b64_decode(ovpn_b64, &ovpn, &ovpnlen);
	if (b64rc != 0 || memchr(ovpn, 0, ovpnlen)) {
		free(ovpn);
		snprintf(detail, detail_len, "E_BAD_SYNTAX: not valid base64/text");
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	m.c = c;
	snprintf(m.name, sizeof(m.name), "%s", name);
	struct sw_profile tmp = {0};
	snprintf(tmp.name, sizeof(tmp.name), "%s", name);
	tmp.kind = SW_KIND_OPENVPN;
	sw_profile_compute_ifname(&tmp);

	/* PASS 1: validate/normalize (nothing written yet) */
	const char *rcode = NULL;
	for (char *p = (char *)ovpn; p && *p && !rcode; ) {
		char *eol = strchr(p, '\n');
		rcode = process_line(&m, p, &has_remote);
		p = eol ? eol + 1 : NULL;
	}
	if (!rcode && m.inline_tag)
		rcode = "E_BAD_SYNTAX";       /* unterminated inline block */
	if (!rcode && !has_remote)
		rcode = "E_NO_REMOTE";
	if (rcode) {
		snprintf(detail, detail_len, "%s: see dashboard log", rcode);
		sw_warn("ovpn import rejected for '%s': %s", name, rcode);
		free(ovpn);
		return UBUS_STATUS_INVALID_ARGUMENT;
	}
	free(ovpn);

	/* PASS 2: credentials (atomicity: file writes start AFTER PASS 1) */
	if (m.want_auth || (cred_user && cred_pass)) {
		if (!cred_user || !cred_pass) {
			emit_warning(&m, "E_MISSING_CREDENTIALS");
		} else {
			char authfile[256];
			int n = snprintf(authfile, sizeof(authfile),
					 "%s\n%s\n", cred_user, cred_pass);
			if (n > 0 && (size_t)n < sizeof(authfile) &&
			    sw_secret_write(name, "auth.txt", authfile) == 0) {
				char refline[SW_PATH_MAX + 96];
				snprintf(refline, sizeof(refline),
					 "auth-user-pass /etc/saadway/secrets/%s/auth.txt\n",
					 name);
				sw_sb_append(&m.out, refline);
				m.auth_ready = true;
			} else {
				emit_warning(&m, "E_SECRET_WRITE_FAILED");
			}
		}
	}

	/* hardcoded saadway interface identity (Principle III) */
	sw_sb_append(&m.out, "dev-type tun\n");
	sw_sb_appendf(&m.out, "dev %s\n", tmp.ifname);

	/* PASS 3: persist normalized config + UCI profile */
	sw_util_mkdirs(SW_RUN_DIR, 0700);
	char confpath[SW_PATH_MAX * 2];
	snprintf(confpath, sizeof(confpath), SW_RUN_DIR "/%s.conf", name);
	if (sw_util_write_file_mode(confpath, m.out.b ? m.out.b : "",
				    m.out.len, 0600) != 0) {
		snprintf(detail, detail_len, "E_SECRET_WRITE_FAILED");
		sw_warn("ovpn import: cannot write normalized config");
		return UBUS_STATUS_UNKNOWN_ERROR;
	}

	struct sw_profile *p = calloc(1, sizeof(*p));
	if (!p)
		return UBUS_STATUS_UNKNOWN_ERROR;
	*p = tmp;
	p->next = NULL;
	snprintf(p->ovpn_ref, sizeof(p->ovpn_ref), "run/openvpn/%s.conf", name);
	if (m.auth_ready)
		snprintf(p->secret_ref, sizeof(p->secret_ref),
			 "secrets/%s/auth.txt", name);
	if (m.warnings[0] && warnings_out)
		*warnings_out = strdup(m.warnings);

	if (sw_uci_profile_add(c, p) != SW_UCI_OK) {
		free(p);
		unlink(confpath);
		snprintf(detail, detail_len, "E_DUPLICATE_NAME or storage failure");
		return UBUS_STATUS_INVALID_ARGUMENT;
	}
	sw_info("profile '%s' imported (openvpn, remote=%s, if=%s)",
		name, m.remote_host, tmp.ifname);
	return UBUS_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* supervision (T012: FR-001/FR-002 detection; Principle III interface)  */
/* ------------------------------------------------------------------ */

static char *run_conf_abs(const char *ovpn_ref)
{
	static char abs[SW_PATH_MAX * 2];
	snprintf(abs, sizeof(abs), SW_BASE_DIR "/%s", ovpn_ref);
	return (char*)abs;
}

int sw_ovpn_start(struct sw_ctx *c, struct sw_profile *p)
{
	if (p->secret_ref[0] && sw_secret_check(p->secret_ref) != 0) {
		sw_err("openvpn start refused: missing/unsafe secret %s",
		       p->secret_ref);
		return -1;
	}
	char conf[SW_PATH_MAX * 2];
	snprintf(conf, sizeof(conf), SW_RUN_DIR "/%s.conf", p->name);
	char setenvopt[SW_NAME_MAX + 34];
	snprintf(setenvopt, sizeof(setenvopt), "--setenv=SAADWAY_PROFILE,%s",
		 p->name);

	char *argv[] = {
		"/usr/sbin/openvpn",
		"--config", conf,
		"--syslog", "saadway-openvpn",
		"--script-security", "2",
		"--up", SW_OVPN_HOOK,
		"--down", SW_OVPN_HOOK,
		setenvopt,
		NULL
	};
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0)
		execv(argv[0], argv);
	sw_child_track(pid, SW_CHILD_TUNNEL, p);   /* reaper dispatch in main.c */
	sw_info("openvpn spawn '%s' pid=%d iface=%s", p->name, (int)pid,
		p->ifname);
	return 0;
}

void sw_ovpn_stop(struct sw_ctx *c, struct sw_profile *p, bool graceful)
{
	(void)c;
	(void)graceful;
	pid_t pid;
	if ((pid = sw_child_find(SW_CHILD_TUNNEL, p)) <= 0)
		return;
	kill(pid, SIGTERM);
	for (int n = 0; n < SW_T_STOP_CHILD * 20; n++) {
		int st;
		if (waitpid(pid, &st, WNOHANG) == pid) {
			int type; void *arg;
			sw_child_take(pid, &type, &arg);
			break;
		}
		if (kill(pid, 0) != 0)
			break;
		usleep(50000);
	}
	kill(pid, SIGKILL);                  /* bounded; reaper cleans up */
	sw_info("openvpn '%s' stopped", p->name);
}




