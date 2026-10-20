/*
 * dns.c - dnsmasq tunnel resolver instance + forced-tunnel DNS (T020, T026)
 *
 * Per research D5 / data-model FR-013:
 *  - a SECOND dnsmasq instance ("tunnel resolver") runs on port 5353 with
 *    no-resolv, whose upstreams are only the tunnel-provided DNS servers.
 *  - an nft REDIRECT rule sends every port-53 UDP/TCP query of
 *    non-device-direct LAN clients into that instance (policy.c).
 *  - bypass domains (steering) are an EXCEPTION forwarded by this instance
 *    to the main (WAN) resolver at 127.0.0.1#53 (T026, research D5 nuance)
 *    and their resolved addresses land in the @bypass_domains nft set via
 *    dnsmasq `nftset=` directives.
 *  - upstream DNS IPs are installed as `ip rule prio 6000 to <dns>` rules
 *    by policy.c, so the resolver's own upstream queries always traverse
 *    the tunnel (Principle IV, zero DNS leak), even though router-local
 *    traffic is otherwise direct by default.
 */
#define _GNU_SOURCE
#include "saadway.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define SW_DNS_UPSTREAM_MAX 4
#define SW_DNS_DEF_UPSTREAM "1.1.1.1"

static char upstreams[SW_DNS_UPSTREAM_MAX][40];
static int n_upstreams;

/*
 * Tunnel upstream DNS list. Source: the hook file written by our
 * supervised openvpn up/down glue (line format "dns=<ip>" per server).
 * Provider-agnostic default: anycast resolvers routed via the tunnel.
 */
int sw_dns_upstreams_current(char *buf, size_t bufsz)
{
	struct sw_ctx *c = global_ctx;
	char of[SW_PATH_MAX];
	size_t flen = 0;

	buf[0] = 0;
	if (n_upstreams) {
		snprintf(buf, bufsz, "%s", upstreams[0]);
		return n_upstreams;
	}
	n_upstreams = 0;
	if (c && c->active) {
		snprintf(of, sizeof(of), SW_RUNSTATE_DIR "/openvpn-%s.dns",
			 c->active->name);
		char *filebuf = sw_util_slurp(of, &flen);
		if (filebuf) {
			for (char *p = filebuf;
			     p && n_upstreams < SW_DNS_UPSTREAM_MAX; ) {
				char line[128];
				char *eol = strchr(p, '\n');
				size_t l = eol ? (size_t)(eol - p) : strlen(p);
				if (l >= sizeof(line))
					l = sizeof(line) - 1;
				memcpy(line, p, l);
				line[l] = 0;
				p = eol ? eol + 1 : NULL;
				if (strncmp(line, "dns=", 4) == 0 &&
				    sw_valid_cidr4(line + 4, false))
					snprintf(upstreams[n_upstreams++],
						 sizeof(upstreams[0]), "%s",
						 line + 4);
			}
			free(filebuf);
		}
	}
	if (!n_upstreams)
		snprintf(upstreams[n_upstreams++], sizeof(upstreams[0]), "%s",
			 SW_DNS_DEF_UPSTREAM);
	snprintf(buf, bufsz, "%s", upstreams[0]);
	return n_upstreams;
}

static bool dnsmasq_running(void)
{
	size_t len = 0;
	char *buf = sw_util_slurp(SW_DNSMASQ_PID, &len);
	bool ok = false;
	if (buf) {
		long pid = atol(buf);
		if (pid > 1 && kill((pid_t)pid, 0) == 0)
			ok = true;
		free(buf);
	}
	return ok;
}

/* public status hook (ubus.c sec.status/sec.dns.status) */
bool sw_dns_running(void)
{
	return dnsmasq_running();
}

/* render the tunnel-resolver dnsmasq config; returns malloc'd text */
static char *render_conf(struct sw_ctx *c)
{
	struct sw_strbuf sb;
	char ups[40];

	sw_sb_init(&sb);
	sw_dns_upstreams_current(ups, sizeof(ups));
	sw_sb_appendf(&sb,
		"# rendered by saadwayd — do not edit\n"
		"port=5353\n"
		"no-resolv\n"
		"no-hosts\n"
		"cache-size=1000\n"
		"log-queries=0\n"
		"pid-file=%s\n",
		SW_DNSMASQ_PID);
	sw_sb_appendf(&sb, "server=%s\n", ups[0] ? ups : SW_DNS_DEF_UPSTREAM);
	for (int i = 1; i < SW_DNS_UPSTREAM_MAX && upstreams[i][0]; i++)
		sw_sb_appendf(&sb, "server=%s\n", upstreams[i]);
	/* T026 bypass-domain exception: resolve via WAN (main LAN) resolver */
	sw_steering_dnsconf_append(&sb, c);
	return sw_sb_release(&sb, NULL);
}

int sw_dns_reload_conf(struct sw_ctx *c)
{
	char *conf = render_conf(c);
	if (!conf)
		return -1;
	int rc = sw_util_write_file_mode(SW_DNSMASQ_CONF, conf, strlen(conf), 0644);
	free(conf);
	if (rc != 0)
		return rc;

	if (dnsmasq_running()) {
		size_t len = 0;
		char *pidbuf = sw_util_slurp(SW_DNSMASQ_PID, &len);
		if (pidbuf) {
			long pid = atol(pidbuf);
			free(pidbuf);
			if (pid > 1)
				kill((pid_t)pid, SIGHUP);  /* drop-in reload */
			return 0;
		}
	}

	/* spawn supervised instance; main.c child manager restarts on exit */
	char *argv[] = { "/usr/sbin/dnsmasq", "--conf-file=" SW_DNSMASQ_CONF,
			 NULL };
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
		execv(argv[0], argv);
		_exit(127);
	}
	if (pid < 0)
		return -1;
	return 0;
}

void sw_dns_notify_tunnel_dns_changed(void)
{
	struct sw_ctx *c = global_ctx;
	n_upstreams = 0;                       /* re-resolve on next render */
	if (c)
		sw_dns_reload_conf(c);
}

void sw_dns_stop(void)
{
	n_upstreams = 0;
	if (dnsmasq_running()) {
		size_t len = 0;
		char *pidbuf = sw_util_slurp(SW_DNSMASQ_PID, &len);
		if (pidbuf) {
			long pid = atol(pidbuf);
			free(pidbuf);
			if (pid > 1) {
				kill((pid_t)pid, SIGTERM);
			}
		}
	}
}
