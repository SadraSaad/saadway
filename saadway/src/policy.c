/*
 * policy.c - atomic fw4/nftables + policy-routing applier (T011,T018,T019)
 *
 * Constitution Principle IV: the FULL desired ruleset is rendered and
 * applied as ONE nft transaction (`nft -f`, files begin with
 * `delete table inet saadway` + full re-creation — delete+create inside a
 * single transaction is atomic, same technique fw4 itself uses). No packet
 * may egress outside policy at any intermediate step:
 *   - stricter direction first (contain before re-open),
 *   - route blackhole precedes any re-open;
 *   - on ANY render/apply failure: last-good kept, else containment mask,
 *     NEVER fail open (T019).
 *
 * Table ownership: `table inet saadway` is independent of fw4's table
 * (verify task T-verify-3); fw4 does not delete foreign tables.
 */
#define _GNU_SOURCE
#include "saadway.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* ------------------------------------------------------------------ */
/* bounded child execution helpers (Principle II: no unbounded waits)   */
/* ------------------------------------------------------------------ */

static int run_argv(char *const argv[], int timeout_ms)
{
	pid_t pid = fork();
	if (pid == 0) {
		for (int i = 0; i < 3; i++) {
			int fd = open("/dev/null", O_WRONLY);
			if (fd >= 0) {
				dup2(fd, i);
				if (fd != i)
					close(fd);
			}
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	if (pid < 0)
		return -1;
	long waited = 0;
	for (;;) {
		int st;
		pid_t w = waitpid(pid, &st, WNOHANG);
		if (w == pid)
			return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
		if (w < 0)
			return -1;
		if (waited >= (long)timeout_ms) {
			kill(pid, SIGKILL);
			waitpid(pid, &st, 0);
			sw_err("policy: '%s' timed out after %d ms", argv[0],
			       timeout_ms);
			return -1;
		}
		usleep(20000);
		waited += 20;
	}
}

/* capture stdout of a bounded child (used for LAN device discovery) */
static int run_capture(char *const argv[], char *out, size_t outlen,
		       int timeout_ms)
{
	int pf[2];
	if (pipe(pf) != 0)
		return -1;
	pid_t pid = fork();
	if (pid == 0) {
		close(pf[0]);
		dup2(pf[1], 1);
		int fd = open("/dev/null", O_WRONLY);
		if (fd >= 0) {
			dup2(fd, 0);
			if (fd != 1)
				close(fd);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	close(pf[1]);
	memset(out, 0, outlen);
	size_t got = 0;
	long waited = 0;
	for (;;) {
		fd_set r;
		struct timeval tv;
		FD_ZERO(&r);
		FD_SET(pf[0], &r);
		tv.tv_sec = 0;
		tv.tv_usec = 50000;
		int n = select(pf[0] + 1, &r, NULL, NULL, &tv);
		if (n > 0) {
			ssize_t k = read(pf[0], out + got, outlen - got - 1);
			if (k == 0)
				break;
			if (k > 0)
				got = (size_t)k;
			else
				break;
		}
		waited += 50;
		if (waited >= timeout_ms)
			break;
	}
	int st;
	int rc = waitpid(pid, &st, WNOHANG);
	if (rc == 0) {
		kill(pid, SIGKILL);
		waitpid(pid, &st, 0);
	}
	close(pf[0]);
	out[got] = 0;
	return 0;
}

/* ------------------------------------------------------------------ */
/* LAN interface discovery (mark scope: only LAN-side traffic gets the  */
/* tunnel-default mark; WAN ingress replies stay unmarked)              */
/* ------------------------------------------------------------------ */

static bool lanish_name(const char *n, char **wans, int nwans)
{
	if (!n || !strcmp(n, "lo"))
		return false;
	if (!strncmp(n, "saad", 4) || !strncmp(n, "xfrm", 4))
		return false;
	for (int i = 0; i < nwans; i++)
		if (!strcmp(wans[i], n))
			return false;
	return true;
}

void sw_policy_refresh_lan_devices(struct sw_ctx *c)
{
	char dump[4096];
	char *argv[] = { SW_IP_BIN, "-4", "route", "show", "default", NULL };
	char *wans[8];
	int nwans = 0;
	static char wan_names[8][16];
	char *save = NULL;
	struct sw_strbuf sb;
	sw_sb_init(&sb);

	dump[0] = 0;
	run_capture(argv, dump, sizeof(dump), 2000);
	for (char *line = strtok_r(dump, "\n", &save); line && nwans < 8;
	     line = strtok_r(NULL, "\n", &save)) {
		char *devpos = strstr(line, "dev ");
		char name[16];
		if (!devpos)
			continue;
		if (sscanf(devpos + 4, "%15s", name) == 1 && nwans < 8) {
			snprintf(wan_names[nwans], sizeof(wan_names[0]), "%s",
				 name);
			/* store the pointer FIRST, then advance the counter:
			 * `wans[nwans++] = wan_names[nwans]` modified and read
			 * nwans without a sequence point (undefined behaviour —
			 * the right-hand read may observe the incremented index
			 * and store a wild pointer). */
			wans[nwans] = wan_names[nwans];
			nwans++;
		}
	}

	/* enumerate interfaces via /proc/net/dev (kernel interface list) */
	FILE *f = fopen("/proc/net/dev", "r");
	if (f) {
		char line[256];
		while (fgets(line, sizeof(line), f)) {
			char names[32];
			if (sscanf(line, "%31[^:]", names) != 1)
				continue;
			/* trim leading spaces */
			char *n = names;
			while (*n == ' ')
				n++;
			for (char *e = n + strlen(n) - 1; e >= n && *e == ' '; e--)
				*e = 0;
			if (!lanish_name(n, wans, nwans))
				continue;
			/* bridge ports of WAN bridges are excluded indirectly
			 * (/WAN bridge device itself excluded; its ports would
			 * still pass — acceptable: no link-local WAN leak since
			 * tunnel-default is only a routing hint) */
			if (sb.len)
				sw_sb_append(&sb, ",");
			sw_sb_append(&sb, n);
		}
		fclose(f);
	}
	free(c->lan_devices);
	c->lan_devices = sw_sb_release(&sb, NULL);
	sw_info("policy: LAN devices = %s",
		c->lan_devices && c->lan_devices[0] ? c->lan_devices : "none");
}

/* ------------------------------------------------------------------ */
/* full ruleset render (pure composition; unit-test target T042)         */
/* ------------------------------------------------------------------ */

static void render_iifname_set(struct sw_ctx *c, struct sw_strbuf *sb)
{
	/* quoted iifname set: loopback + discovered WAN devices excluded;
	 * everything else (LAN + unknown bridges) is tunnel-default marked */
	sw_sb_append(sb, "\"lo\"");
	if (c->lan_devices)
		for (char *save = NULL, *tok = strtok_r(c->lan_devices, ",", &save);
		     tok; tok = strtok_r(NULL, ",", &save))
			sw_sb_appendf(sb, ", \"%s\"", tok);
}

static void render_dev_sets(struct sw_ctx *c, struct sw_strbuf *sb)
{
	struct sw_strbuf ipel, macel;
	int n = 0;

	sw_sb_init(&ipel);
	sw_sb_init(&macel);
	for (struct sw_device_rule *d = c->st.devices; d; d = d->next, n++) {
		if (d->match_type == SW_MTYPE_IP)
			sw_sb_appendf(&ipel, "%s%s", n ? ", " : "", d->match_value);
		else {
			char lc[24];
			int i;
			for (i = 0; i < 17; i++)
				lc[i] = (char)tolower((unsigned char)d->match_value[i]);
			lc[17] = 0;
			sw_sb_appendf(&macel, "%s%s", n ? ", " : "", lc);
		}
	}
	if (ipel.len || macel.len) {
		sw_sb_append(sb,
			"\tset dev_ip { type ipv4_addr; flags interval; elements = { ");
		sw_sb_append(sb, ipel.b ? ipel.b : "");
		sw_sb_append(sb, "; } }\n");
		sw_sb_append(sb,
			"\tset dev_mac { type ether_addr; elements = { ");
		sw_sb_append(sb, macel.b ? macel.b : "");
		sw_sb_append(sb, "; } }\n");
	}
	sw_sb_free(&ipel);
	sw_sb_free(&macel);
}

int sw_policy_render(struct sw_ctx *c, bool contain, const char *tun_if,
		     bool route_default, char **out, size_t *outlen)
{
	struct sw_strbuf sb;

	(void)route_default; /* routes are applied by sw_policy_apply below */
	sw_sb_init(&sb);

	sw_sb_append(&sb,
"# generated by saadwayd (atomic single-transaction render); do not edit\n"
"delete table inet saadway\n\n"
"table inet saadway {\n");

	/* dnsmasq-fed bypass-domain set (dynamic; static definition here) */
	sw_sb_append(&sb,
"\tset bypass_domains { type ipv4_addr; flags dynamic,timeout; }\n");

	/* bundled IP-range auto bypass (FR-010) — rebuilt inside the
	 * same transaction on every apply */
	{
		struct sw_strbuf rb;
		sw_sb_init(&rb);
		sw_steering_mark_ranges(&rb);
		sw_sb_appendf(&sb,
"\tset bypass_ranges { type ipv4_addr; flags interval; elements = { %s } }\n",
			rb.b ? rb.b : "");
		sw_sb_free(&rb);
	}

	/* per-device policy sets (direct | tunneled) */
	render_dev_sets(c, &sb);

	/* --- marking (prerouting / mangle) -------------------------------- */
	sw_sb_append(&sb,
"\tchain mark_prerouting {\n"
"\t\ttype filter hook prerouting priority mangle; policy accept;\n"
/* replies of established flows keep unmarked → main table */
"\t\tct state established,related return\n");
	sw_sb_append(&sb, "\t\tiifname != { ");
	render_iifname_set(c, &sb);
	sw_sb_append(&sb,
" } meta mark set 0x08\n"
"\t\tjump mark_rules\n"
"\t}\n\n"
"\tchain mark_rules {\n");
	/* steering chain: exact Q1-A evaluation order */
	sw_steering_mark_rules(&sb, c);
	sw_sb_append(&sb, "\t}\n\n");

	/* --- forced-tunnel DNS (T020) -------------------------------------- */
	sw_sb_append(&sb,
"\tchain dns_redirect {\n"
"\t\ttype filter hook prerouting priority dstnat; policy accept;\n");
	/* device-direct exemption: their DNS stays on the WAN resolver */
	for (struct sw_device_rule *d = c->st.devices; d; d = d->next) {
		if (d->ptype != SW_POLICY_DIRECT)
			continue;
		if (d->match_type == SW_MTYPE_IP)
			sw_sb_appendf(&sb,
"\t\tct state new ip saddr %s udp dport 53 return\n"
"\t\tct state new ip saddr %s tcp dport 53 return\n",
				d->match_value, d->match_value);
		else
			sw_sb_appendf(&sb,
"\t\tct state new ether saddr %s udp dport 53 return\n"
"\t\tct state new ether saddr %s tcp dport 53 return\n",
				d->match_value, d->match_value);
	}
	sw_sb_append(&sb,
"\t\tct state new udp dport 53 redirect to :5353\n"
"\t\tct state new tcp dport 53 redirect to :5353\n"
"\t}\n\n");

	/* --- containment / tunnel-permit enforcement (T018) ----------------- */
	sw_sb_append(&sb,
"\tchain contain {\n"
"\t\ttype filter hook forward priority filter - 1; policy accept;\n"
"\t\tmeta mark & 0x0f == 0x01 accept\n"
"\t\tmeta mark & 0x0f == 0x02 accept\n"
"\t\tmeta mark & 0x0f == 0x04 accept\n");
	if (contain) {
		if (tun_if && tun_if[0])
			sw_sb_appendf(&sb,
"\t\tmeta mark & 0x0f == 0x08 oifname != \"%s\" drop\n"
"\t\tmeta mark & 0x0f == 0x08 oifname \"%s\" ct state new drop\n",
				tun_if, tun_if);
		else
			sw_sb_append(&sb,
"\t\tmeta mark & 0x0f == 0x08 drop\n");
	}
	sw_sb_append(&sb,
"\t\taccept\n"
"\t}\n"
"}\n");

	*out = sw_sb_release(&sb, outlen);
	return *out ? 0 : -1;
}

static int apply_nft_transaction(const char *ruleset)
{
	struct sw_strbuf fb;
	char *body;
	int rc;

	/* attempt one: delete+create inside ONE transaction (atomic,
	 * the same replace technique fw4 itself uses) */
	sw_sb_init(&fb);
	sw_sb_append(&fb, "delete table inet saadway\n\n");
	sw_sb_append(&fb, ruleset);
	body = sw_sb_release(&fb, NULL);
	if (!body)
		return -1;
	rc = sw_util_write_file_mode(SW_NFT_RENDER, body, strlen(body), 0644);
	free(body);
	if (rc != 0) {
		sw_err("policy: cannot write render file %s", SW_NFT_RENDER);
		return -1;
	}
	{
		char *argv[] = { SW_NFT_BIN, "-f", SW_NFT_RENDER, NULL };
		rc = run_argv(argv, SW_T_POLICY_APPLY * 1000);
	}
	if (rc != 0) {
		/* fallback (first boot / table absent): create-only, still
		 * one atomic transaction */
		sw_warn("policy: delete+create txn failed (rc=%d); "
			"retrying as create-only", rc);
		rc = sw_util_write_file_mode(SW_NFT_RENDER, ruleset,
					     strlen(ruleset), 0644);
		if (rc != 0)
			return -1;
		{
			char *argv[] = { SW_NFT_BIN, "-f", SW_NFT_RENDER, NULL };
			rc = run_argv(argv, SW_T_POLICY_APPLY * 1000);
		}
	}
	return rc;
}

static int iprule_add(const char *prio, const char *match, const char *table)
{
	char pr_buf[16], mg_buf[64], tb_buf[16];
	const char *args[8];
	int n = 0;

	args[n++] = SW_IP_BIN;
	args[n++] = "rule";
	args[n++] = "add";
	args[n++] = "prio";
	snprintf(pr_buf, sizeof(pr_buf), "%s", prio);
	args[n++] = pr_buf;
	snprintf(mg_buf, sizeof(mg_buf), "%s", match);
	args[n++] = mg_buf;
	args[n++] = "table";
	snprintf(tb_buf, sizeof(tb_buf), "%s", table);
	args[n++] = tb_buf;
	return run_argv((char *const *)args, 1500);
}

static int iprule_del_prio(int prio)
{
	char pr_buf[16];
	const char *args[8];
	int n = 0;
	int rc;

	args[n++] = SW_IP_BIN;
	args[n++] = "rule";
	args[n++] = "del";
	args[n++] = "prio";
	snprintf(pr_buf, sizeof(pr_buf), "%d", prio);
	args[n++] = pr_buf;
	rc = run_argv((char *const *)args, 1500);
	return rc;
}

static int apply_ip_rules(struct sw_ctx *c, bool direct_mode)
{
	int prio;

	/* wipe only OWN priorities — never other consumers' rules */
	for (prio = SW_RULE_PRIO_DEV; prio <= SW_RULE_PRIO_LAN; prio += 100) {
		for (int k = 0; k < 24; k++)
			if (iprule_del_prio(prio) != 0)
				break;
	}

	/* T026: resolver upstream DNS always traverses the tunnel (rule
	 * priority 6000 < steering priorities).  In DIRECT mode there is no
	 * tunnel: plain routing applies and this rule class is skipped. */
	if (!direct_mode) {
	char upsbuf[192];
	sw_dns_upstreams_current(upsbuf, sizeof(upsbuf));
	for (char *save = NULL, *tok = strtok_r(upsbuf, " \t", &save); tok;
	     tok = strtok_r(NULL, " \t", &save)) {
		char match[48];
		snprintf(match, sizeof(match), "to %s", tok);
		int rc = iprule_add("6000", match, "201");
		if (rc != 0)
			sw_warn("policy: dns route rule for %s failed rc=%d",
				tok, rc);
	}
	}

	char matchstr[64];
	char priostr[16];

	snprintf(priostr, sizeof(priostr), "%d", SW_RULE_PRIO_DEV);
	iprule_add(priostr, "fwmark 0x1/0xf", "202");
	snprintf(priostr, sizeof(priostr), "%d", SW_RULE_PRIO_DOM);
	iprule_add(priostr, "fwmark 0x2/0xf", "202");
	snprintf(priostr, sizeof(priostr), "%d", SW_RULE_PRIO_RNG);
	iprule_add(priostr, "fwmark 0x4/0xf", "202");
	snprintf(priostr, sizeof(priostr), "%d", SW_RULE_PRIO_TUN);
	if (direct_mode) {
		/* DIRECT mode (Q2-B: tunnel down w/ kill switch off — LAN
		 * default egress follows the WAN main table): the two
		 * tunnel-class rules point at table 202 instead of 201, so
		 * no class can ever be routed into the (blackholed) tunnel
		 * table.  Device/domain/range direct rules above still
		 * apply; nothing changes for them. */
		iprule_add(priostr, "fwmark 0x8/0xf", "202");
	} else {
		iprule_add(priostr, "fwmark 0x8/0xf", "201");
	}
	snprintf(priostr, sizeof(priostr), "%d", SW_RULE_PRIO_LAN);
	snprintf(matchstr, sizeof(matchstr), "fwmark 0x0/0xf");
	iprule_add(priostr, matchstr, direct_mode ? "202" : "201");

	return 0;
}

static int apply_route(struct sw_ctx *c, const char *tun_if, bool route_default)
{
	char devbuf[48], tb_buf[16];
	const char *args[16];
	int n = 0;

	(void)c;
	args[n++] = SW_IP_BIN;
	args[n++] = "-4";
	args[n++] = "route";
	args[n++] = "replace";
	if (route_default && tun_if && tun_if[0]) {
		args[n++] = "default";
		args[n++] = "dev";
		snprintf(devbuf, sizeof(devbuf), "%s", tun_if);
		args[n++] = devbuf;
	} else {
		args[n++] = "blackhole";
		args[n++] = "default";
	}
	args[n++] = "table";
	snprintf(tb_buf, sizeof(tb_buf), "%d", SW_TABLE_TUNNEL);
	args[n++] = tb_buf;
	return run_argv((char *const *)args, 3000);
}

int sw_policy_apply(struct sw_ctx *c, bool contain, const char *tun_if,
		    bool route_default)
{
	char *ruleset = NULL;
	size_t len = 0;
	bool direct_mode = !contain && (!tun_if || !tun_if[0]);

	if (sw_policy_render(c, contain, tun_if, route_default,
			     &ruleset, &len) != 0) {
		sw_err("policy: RENDER FAILED — fail closed (T019, Principle IV)");
		return sw_policy_emergency_contain(c);
	}
	int rc = apply_nft_transaction(ruleset);
	free(ruleset);
	if (rc != 0) {
		sw_err("policy: nft transaction FAILED rc=%d — fail closed (T019)",
		       rc);
		return sw_policy_emergency_contain(c);
	}
	apply_ip_rules(c, direct_mode);

	/*
	 * Fail-closed ordering: containment changes go FIRST, re-open of
	 * tunnel egress LAST (the route only lands after its interface was
	 * verified-up; the strictly-more-restrictive state dominates).
	 */
	if (apply_route(c, tun_if, route_default) != 0) {
		sw_err("policy: route apply failed — fail closed (T019)");
		return sw_policy_emergency_contain(c);
	}
	sw_info("policy: applied atomically (contain=%d, tun=%s, route=%d)",
		contain, tun_if ? tun_if : "-", route_default);
	return 0;
}

/*
 * DIRECT mode (Q2-B): no tunnel is expected.  LAN default egress follows
 * the WAN main table; the tunnel routing table keeps its blackhole default
 * so nothing marked for the tunnel class can leak in this state either.
 * Used by the failover cooldown when the kill switch is off, and whenever
 * the service is disabled.
 */
int sw_policy_apply_direct(struct sw_ctx *c)
{
	char *ruleset = NULL;
	size_t len = 0;

	if (sw_policy_render(c, false, NULL, false, &ruleset, &len) != 0) {
		sw_err("policy: DIRECT RENDER FAILED — fail closed (T019)");
		return sw_policy_emergency_contain(c);
	}
	int rc = apply_nft_transaction(ruleset);
	free(ruleset);
	if (rc != 0) {
		sw_err("policy: DIRECT nft FAILED rc=%d — fail closed", rc);
		return sw_policy_emergency_contain(c);
	}
	apply_ip_rules(c, true);

	/* strictly-more-restrictive state dominates; the blackhole tunnel
	 * table default also covers any stale marked packet */
	apply_route(c, NULL, false);
	sw_info("policy: DIRECT mode (WAN default egress)");
	return 0;
}

int sw_policy_emergency_contain(struct sw_ctx *c)
{
	sw_err("SENTINEL: EMERGENCY CONTAINMENT (tunnel-class egress blackholed)");
	(void)apply_route(c, NULL, false);
	return -1;
}





