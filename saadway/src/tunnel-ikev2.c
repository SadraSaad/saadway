/*
 * tunnel-ikev2.c — IKEv2/strongSwan profile renderer/supervisor (T013)
 *
 * Per research.md D3: each IKEv2 profile renders a swanctl connection
 * (EAP-MSCHAPv2 identity, proposals `aes256gcm16-prfsha256-ecp384-ecp521` /
 * `aes256gcm16` per FR-004/FR-005, MODP disabled) and materializes an XFRM
 * interface (`xfrmX`, unique if_id) so routing/monitoring bind to a real L3
 * device (FR-003, constitution Principle III).
 *
 * Secrets (Principle IV): the password stays ONLY in
 * /etc/saadway/secrets/<name>/eap.txt (0600).  At start the password is read
 * and rendered into SW_SWANCTL_DIR/<name>.conf (0600) — strongSwan consumes
 * EAP secrets from the connection file; UCI never sees the password.
 *
 * Supervision: charon is a resident system daemon (not our child).  The ONLY
 * child saadwayd spawns is the bounded `swanctl --initiate` client:
 *   exit 0                 -> IKE SA established (activation success path)
 *   exit != 0 / SIGALRM    -> activation failed (failover path, main.c reaper)
 * Ongoing tunnel health is the watchdog probe (failover.c), matching the
 * single poll-loop event model (Principle II) — no extra supervision threads.
 *
 * NOTE(T044 docs/verify-2512.md): confirm exact swanctl option spellings on
 * the 25.12 buildroot (--load-con/--file, --initiate/--ike, --terminate,
 * --delete-con, --timeout) — see verify checklist in docs.
 */
#define _GNU_SOURCE
#include "saadway.h"

#include <signal.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define SW_IKE_SEARCH_MAX 3       /* vici socket retry attempts after start */
#define SW_IKE_RETRY_USEC 500000  /* 0.5 s between vici socket retries      */
#define SW_IKE_INIT_TIMEOUT "25"  /* swanctl --timeout budget (s)           */
#define SW_IKE_CHILD_BUDGET 30    /* alarm() ceiling on the child           */

/* ------------------------------------------------------------------ */
/* pure renderer (host-testable; no I/O)                               */
/* ------------------------------------------------------------------ */

/* swanctl file escapes: wrap in "", escape " and \ ; drop control chars */
static void escappend(struct sw_strbuf *s, const char *v)
{
	sw_sb_append(s, "\"");
	for (const char *p = v; p && *p; p++) {
		unsigned char ch = (unsigned char)*p;
		if (ch < 0x20 || ch == 0x7f)
			continue;              /* never render control bytes */
		if (ch == '"' || ch == '\\')
			sw_sb_append(s, "\\");
		char one[2] = { (char)ch, 0 };
		sw_sb_append(s, one);
	}
	sw_sb_append(s, "\"");
}

void sw_ikev2_render(struct sw_profile *p, const char *password,
		     struct sw_strbuf *out)
{
	/* effective identities, data-model.md "same-as" enums */
	const char *local_id =
		strcmp(p->local_id, "same-as-username") ? p->local_id :
							 p->username;
	const char *remote_id =
		strcmp(p->remote_id, "same-as-hostname") ? p->remote_id :
							   p->hostname;

	sw_sb_appendf(out,
		      "# rendered by saadwayd — do not edit (0600, secret)\n"
		      "connections {\n"
		      "\t%s {\n"
		      "\t\tversion = 2\n"
		      "\t\tremote_addrs = %s\n"
		      "\t\tproposals = %s\n"
		      "\t\tlocal_main {\n"
		      "\t\t\tauth = eap-mschapv2\n"
		      "\t\t\teap_id = ",
		      p->name, p->hostname, p->proposal_ike);
	escappend(out, p->username);
	sw_sb_append(out, "\n\t\t\tid = ");
	escappend(out, local_id);
	sw_sb_append(out, "\n\t\t}\n"
			  "\t\tremote_main {\n"
			  "\t\t\tauth = pubkey\n"
			  "\t\t\tid = ");
	escappend(out, remote_id);

	/* cert_mode "from-server": trust the CA the operator dropped in place
	 * (optional pinning file; standard servers present a public chain) */
	char capath[SW_PATH_MAX];
	snprintf(capath, sizeof(capath), SW_SECRETS_DIR "/%s/ca.pem", p->name);
	struct stat st;
	if (stat(capath, &st) == 0 && S_ISREG(st.st_mode))
		sw_sb_appendf(out, "\n\t\t\tcacerts = %s", capath);

	sw_sb_appendf(out, "\n"
			   "\t\t}\n"
			   "\t\tchildren {\n"
			   "\t\t\t%s-tunnel {\n"
			   "\t\t\t\tmode = tunnel\n"
			   "\t\t\t\tlocal_ts = 0.0.0.0/0\n"
			   "\t\t\t\tremote_ts = 0.0.0.0/0\n"
			   "\t\t\t\tif_id_in = %d\n"
			   "\t\t\t\tif_id_out = %d\n"
			   "\t\t\t\tesp_proposals = %s\n"
			   "\t\t\t\tstart_action = trap\n"
			   "\t\t\t\tclose_action = none\n"
			   "\t\t\t\tdpd_delay = 30\n"
			   "\t\t\t}\n"
			   "\t\t}\n"
			   "\t}\n"
			   "}\n"
			   "secrets {\n"
			   "\teap-%s {\n"
			   "\t\tid = ",
			   p->name, p->if_id, p->if_id, p->proposal_esp,
			   p->name);
	escappend(out, p->username);
	sw_sb_append(out, "\n\t\tsecret = ");
	escappend(out, password);
	sw_sb_append(out, "\n\t}\n}\n");
}

/* ------------------------------------------------------------------ */
/* on-device helpers (bounded synchronous shell for LOCAL control ops)  */
/* ------------------------------------------------------------------ */

static int sh(const char *fmt, ...)
{
	char cmd[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	return system(cmd);   /* local op (ip/init.d): finite, no network */
}

/* charon's vici socket is not instantly present after init.d start */
static int wait_vici(void)
{
	for (int i = 0; i < SW_IKE_SEARCH_MAX; i++) {
		struct stat st;
		if (stat("/var/run/charon.vici", &st) == 0)
			return 0;
		usleep(SW_IKE_RETRY_USEC);
	}
	return -1;
}

static int spawn_initiate(struct sw_profile *p)
{
	pid_t pid = fork();

	if (pid < 0)
		return -1;
	if (pid == 0) {
		alarm(SW_IKE_CHILD_BUDGET);   /* hard ceiling even if -     */
		execl("/usr/sbin/swanctl", "swanctl",     /* swanctl hangs   */
		      "--timeout", SW_IKE_INIT_TIMEOUT,   /* on a dead vici  */
		      "--initiate", "--ike", p->name, (char *)NULL);
		_exit(127);
	}
	sw_child_track(pid, SW_CHILD_TUNNEL, p);
	sw_info("ikev2 initiate '%s' pid=%d budget=%ss",
		p->name, (int)pid, SW_IKE_INIT_TIMEOUT);
	return 0;
}

/* ------------------------------------------------------------------ */
/* public start/stop                                                    */
/* ------------------------------------------------------------------ */

int sw_ikev2_start(struct sw_ctx *c, struct sw_profile *p)
{
	char passpath[SW_PATH_MAX], confpath[SW_PATH_MAX];
	struct sw_strbuf out;

	(void)c;
	if (p->kind != SW_KIND_IKEV2 || !p->hostname[0] || !p->username[0] ||
	    !sw_valid_domain(p->hostname) ||
	    p->if_id <= 0 || !p->ifname[0]) {
		sw_err("ikev2 '%s': incomplete profile, refusing activation",
		       p->name[0] ? p->name : "?");
		return -1;
	}

	/* password: 0600 secret file is mandatory (Principle IV) */
	snprintf(passpath, sizeof(passpath), SW_BASE_DIR "/%s", p->secret_ref);
	size_t plen = 0;
	char *pw = sw_util_slurp(passpath, &plen);
	if (!pw || !plen) {
		free(pw);
		sw_err("ikev2 '%s': EAP secret %s missing/empty",
		       p->name, p->secret_ref);
		return -1;
	}
	while (plen && (pw[plen - 1] == '\n' || pw[plen - 1] == '\r'))
		pw[--plen] = 0;   /* strip EOL; render loses what's inside */

	/* fresh render every start: config derives from UCI + secret file,
	 * never left stale from a previous profile edit */
	sw_sb_init(&out);
	sw_ikev2_render(p, pw, &out);
	free(pw);

	sw_util_mkdirs(SW_SWANCTL_DIR, 0700);
	snprintf(confpath, sizeof(confpath), SW_SWANCTL_DIR "/%s.conf",
		 p->name);
	if (sw_util_write_file_mode(confpath, out.b ? out.b : "", out.len,
				    0600) != 0) {
		sw_sb_free(&out);
		sw_err("ikev2 '%s': cannot write %s", p->name, confpath);
		return -1;
	}
	sw_sb_free(&out);

	/* XFRM interface materialization (FR-003, Principle III) */
	if (sh("%s link show %s 2>/dev/null || "
	       "%s link add name %s type xfrm if_id %d",
	       SW_IP_BIN, p->ifname, SW_IP_BIN, p->ifname, p->if_id) != 0) {
		sw_err("ikev2 '%s': xfrm interface create failed (%s)",
		       p->name, p->ifname);
		return -1;
	}
	if (sh("%s link set %s up", SW_IP_BIN, p->ifname) != 0) {
		sw_err("ikev2 '%s': xfrm interface up failed", p->name);
		return -1;
	}

	/* charon must be alive before we hand it the connection */
	if (sh("/etc/init.d/ipsec start") != 0 || wait_vici() != 0) {
		sw_err("ikev2 '%s': charon did not come up", p->name);
		return -1;
	}

	/* bounded load + initiate (load is fast; initiate runs as the only
	 * tracked child so its 0/nonzero exit is the activation verdict) */
	if (sh("/usr/sbin/swanctl --timeout %s --load-con %s --file %s",
	       SW_IKE_INIT_TIMEOUT, p->name, confpath) != 0) {
		sw_err("ikev2 '%s': swanctl --load-con failed", p->name);
		return -1;
	}
	if (spawn_initiate(p) != 0) {
		sh("/usr/sbin/swanctl --timeout 2 --delete-con %s", p->name);
		return -1;
	}
	sw_info("ikev2 '%s' activating, interface=%s if_id=%d", p->name,
		p->ifname, p->if_id);
	return 0;
}

void sw_ikev2_stop(struct sw_ctx *c, struct sw_profile *p, bool graceful)
{
	char confpath[SW_PATH_MAX];

	(void)c;
	(void)graceful;   /* fast paths only in v1; terminate is quick */

	/* kill any in-flight bounded initiate child first */
	pid_t pid;
	if ((pid = sw_child_find(SW_CHILD_TUNNEL, p)) > 0) {
		kill(pid, SIGTERM);
		int st;
		for (int n = 0; n < SW_T_STOP_CHILD * 20 && waitpid(pid, &st,
								    WNOHANG)
			     != pid; n++)
			usleep(50000);
		if (kill(pid, 0) == 0)
			kill(pid, SIGKILL);
	}

	/* drop SA then the connection from charon; ignore absent state */
	sh("/usr/sbin/swanctl --timeout 2 --terminate --ike %s 2>/dev/null",
	   p->name);
	sh("/usr/sbin/swanctl --timeout 2 --delete-con %s 2>/dev/null",
	   p->name);

	/* remove the xfrm interface (rendered config too) */
	sh("%s link del %s 2>/dev/null", SW_IP_BIN, p->ifname);
	snprintf(confpath, sizeof(confpath), SW_SWANCTL_DIR "/%s.conf",
		 p->name);
	unlink(confpath);
	sw_info("ikev2 '%s' stopped, interface removed", p->name);
}
