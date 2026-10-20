/*
 * health.c — multi-layer tunnel health probe (T033, FR-015)
 *
 * From INSIDE the active tunnel (Principle III: packets bind to the L3
 * tunnel interface via SO_BINDTODEVICE, so they physically enter the VPN):
 *   (a) ICMP echo to the probe target (default 1.1.1.1, budget
 *       SW_T_PING = 3 s),
 *   (b) HTTP GET http://cp.cloudflare.com/generate_204 — host resolved
 *       through the tunnel resolver instance (127.0.0.1#5353, budget
 *       SW_T_DNSQ = 2 s), then connect+response capped by SW_T_HTTP.
 *
 * Everything is uloop poll-driven and NEVER blocks (Constitution
 * Principle II).  The ping chain and the DNS/HTTP chain run concurrently
 * on separate sockets, each with hard timers; a master deadline caps the
 * whole pair inside the SC-003 5 s RPC budget.  Whatever is not settled
 * by its deadline is reported as `timeout` by mon.health_test.
 *
 * Wire formats implemented locally (no extra daemon dependency, per
 * research.md D2 in-process probe decision): ICMP datagram ping, raw DNS
 * A query, minimal HTTP/1.x status-line parse.
 */
#define _GNU_SOURCE
#include "saadway.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define SW_PING_TARGET      "1.1.1.1"
#define SW_HEALTH_TARGET    "cp.cloudflare.com"
#define SW_HEALTH_PATH      "/generate_204"
#define SW_RESOLVER_ADDR    "127.0.0.1"
#define SW_RESOLVER_PORT    5353
#define SW_MASTER_BUDGET_MS 5000        /* SC-003: probe pair cap        */

struct hjob {
	struct uloop_fd ping_fd;         /* ICMP DGRAM socket             */
	struct uloop_timeout ping_to;
	struct uloop_fd xfd;             /* resolver socket, then http    */
	struct uloop_timeout x_to;       /* per-phase deadline            */
	struct uloop_timeout master;
	struct sw_ctx *c;
	void (*cb)(struct sw_ctx *c, bool done);
	bool live;
	bool done_fired;
	bool ping_settled;
	bool http_settled;
	bool http_phase;                 /* xfd carries the TCP conn      */
	bool conn_wait;                  /* non-blocking connect armed    */
	bool http_sent;
	struct timeval master_start;
	char ip[64];
};

static struct hjob job;

static void http_start(struct sw_ctx *c, const char *ip);
static void ping_event(struct uloop_fd *u, unsigned int events);
static void x_event(struct uloop_fd *u, unsigned int events);

/* ------------------------------------------------------------------ */
/* small helpers                                                        */
/* ------------------------------------------------------------------ */

static long ms_since(struct timeval *t0)
{
	struct timeval now;

	gettimeofday(&now, NULL);
	return (now.tv_sec - t0->tv_sec) * 1000 +
	       (now.tv_usec - t0->tv_usec) / 1000;
}

/* SO_BINDTODEVICE forces the L3 tunnel interface (Principle III) */
static int bind_dev(int fd, const char *ifname)
{
	struct ifreq ifr;

	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	return setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &ifr,
			  sizeof(ifr));
}

static void set_nonblocking(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);

	if (fl >= 0)
		fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void xfd_close(void)
{
	if (job.xfd.fd >= 0) {
		uloop_fd_delete(&job.xfd);
		close(job.xfd.fd);
		job.xfd.fd = -1;
	}
}

static void finish(bool fire_cb)
{
	struct sw_ctx *c = job.c;
	void (*cb)(struct sw_ctx *, bool) = job.cb;

	if (!job.live)
		return;
	uloop_timeout_cancel(&job.ping_to);
	uloop_timeout_cancel(&job.x_to);
	uloop_timeout_cancel(&job.master);
	if (job.ping_fd.fd >= 0) {
		uloop_fd_delete(&job.ping_fd);
		close(job.ping_fd.fd);
		job.ping_fd.fd = -1;
	}
	xfd_close();
	job.live = false;
	c->health.pending = false;
	c->health.done = c->health.ping_ok == 1 || c->health.http.code > 0;
	job.done_fired = true;
	if (fire_cb && cb)
		cb(c, c->health.done);
}

static void finish_if_settled(void)
{
	if (job.ping_settled && job.http_settled)
		finish(true);
}

/* ------------------------------------------------------------------ */
/* phase (a): ICMP echo                                                 */
/* ------------------------------------------------------------------ */

static uint16_t checksum16(const unsigned char *b, int len)
{
	uint32_t sum = 0;

	for (int i = 0; i + 1 < len; i += 2)
		sum += ((uint32_t)b[i] << 8) | b[i + 1];
	if (len & 1)
		sum += (uint32_t)b[len - 1] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

static void ping_send(void)
{
	unsigned char pkt[48];
	size_t plen = 12 + sizeof("saadwayd");

	memset(pkt, 0, sizeof(pkt));
	pkt[0] = 8;                /* kernel owns id for DGRAM ping socks */
	pkt[1] = 0;
	pkt[4] = 0x5a;
	pkt[5] = 0xad;
	pkt[6] = (unsigned char)((getpid() >> 8) & 0xff);
	pkt[7] = (unsigned char)(getpid() & 0xff);
	memcpy(pkt + 8, "saadwayd\0", 9);
	{
		uint16_t cs = checksum16(pkt, (int)plen);
		pkt[2] = (unsigned char)(cs >> 8);
		pkt[3] = (unsigned char)(cs & 0xff);
	}

	struct sockaddr_in dst;
	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	inet_pton(AF_INET, SW_PING_TARGET, &dst.sin_addr);
	sendto(job.ping_fd.fd, pkt, plen, 0, (struct sockaddr *)&dst,
	       sizeof(dst));
}

static void ping_settle(bool ok)
{
	struct sw_ctx *c = job.c;

	if (job.ping_settled)
		return;
	job.ping_settled = true;
	if (ok) {
		c->health.ping_ok = 1;
		c->health.ping_latency_ms = (int)ms_since(&job.master_start);
	} else {
		c->health.ping_ok = 0;
		c->health.ping_latency_ms = 0;
	}
	uloop_timeout_cancel(&job.ping_to);
	finish_if_settled();
}

static void ping_timeout(struct uloop_timeout *t)
{
	(void)t;
	ping_settle(false);
}

static void ping_start(struct sw_ctx *c)
{
	int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);

	job.ping_fd.fd = -1;
	job.ping_to.cb = ping_timeout;
	if (fd < 0) {
		ping_settle(false);
		return;
	}
	if (bind_dev(fd, c->tun_if) != 0) {
		close(fd);
		ping_settle(false);
		return;
	}
	set_nonblocking(fd);
	job.ping_fd.fd = fd;
	job.ping_fd.cb = ping_event;
	uloop_fd_add(&job.ping_fd, ULOOP_READ);
	ping_send();
	uloop_timeout_set(&job.ping_to, SW_T_PING * 1000);
}

static void ping_event(struct uloop_fd *u, unsigned int events)
{
	unsigned char buf[512];
	(void)u;
	(void)events;

	for (;;) {
		ssize_t n = recv(job.ping_fd.fd, buf, sizeof(buf),
				 MSG_DONTWAIT);
		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			return;                 /* spurious / EAGAIN     */
		}
		/* recv delivers with or without the outer IP header */
		const unsigned char *icmp = buf;
		if ((buf[0] >> 4) == 4 && (buf[0] & 0x0f) >= 5)
			icmp = buf + (size_t)(buf[0] & 0x0f) * 4;
		size_t off = (size_t)(icmp - buf);
		if ((size_t)n < off + 8)
			continue;
		if (icmp[0] == 0) {             /* ICMP ECHOREPLY        */
			ping_settle(true);
			return;
		}
	}
}

/* ------------------------------------------------------------------ */
/* phase (b1): DNS A query through the tunnel resolver (127.0.0.1:5353)  */
/* ------------------------------------------------------------------ */

static uint16_t get16(const unsigned char *b)
{
	return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}

static void dns_send(void)
{
	unsigned char q[256];
	size_t n = 0;
	uint16_t id = (uint16_t)(getpid() ^ 0x5add);

	q[n++] = (unsigned char)(id >> 8);      /* header: RD=1, qd=1   */
	q[n++] = (unsigned char)(id & 0xff);
	q[n++] = 0x01;
	q[n++] = 0x00;
	q[n++] = 0;
	q[n++] = 1;
	memset(q + n, 0, 6);
	n += 6;

	for (const char *p = SW_HEALTH_TARGET; p && *p &&
				      n + 64 + 5 < sizeof(q); ) {
		const char *dot = strchr(p, '.');
		size_t ll = dot ? (size_t)(dot - p) : strlen(p);

		if (!ll || ll > 63)
			return;
		q[n++] = (unsigned char)ll;
		memcpy(q + n, p, ll);
		n += ll;
		p = dot ? dot + 1 : NULL;
	}
	q[n++] = 0;
	q[n++] = 0;  q[n++] = 1;                /* QTYPE A               */
	q[n++] = 0;  q[n++] = 1;                /* QCLASS IN             */

	struct sockaddr_in dst;
	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons(SW_RESOLVER_PORT);
	inet_pton(AF_INET, SW_RESOLVER_ADDR, &dst.sin_addr);
	sendto(job.xfd.fd, q, n, 0, (struct sockaddr *)&dst, sizeof(dst));
}

/* parse the reply; extract the first A record (bounded) */
static bool dns_parse_a(const unsigned char *r, ssize_t len, char *ip,
			size_t ipcap)
{
	if (len < 13 || (r[3] & 0x0f) != 0)
		return false;
	size_t n = 12;
	int qd = get16(r + 4);

	for (int i = 0; i < qd; i++) {
		while (n < (size_t)len && r[n] && !(r[n] & 0xc0)) {
			if ((n += (size_t)r[n] + 1) >= (size_t)len)
				return false;
		}
		if (n >= (size_t)len)
			return false;
		n += r[n] ? 2 : 1;              /* ptr or root           */
		if (n + 4 > (size_t)len)
			return false;
		n += 4;
	}
	int an = get16(r + 6);

	for (int i = 0; i < an; i++) {
		if (n + 11 > (size_t)len)
			return false;
		bool comp = (r[n] & 0xc0) != 0;

		if (comp) {
			n += 2;
		} else {
			n++;
			while (n < (size_t)len && r[n] && !(r[n] & 0xc0)) {
				if ((n += (size_t)r[n] + 1) >= (size_t)len)
					return false;
			}
			if (n >= (size_t)len)
				return false;
			n++;                    /* root                  */
		}
		uint16_t type = get16(r + n);
		uint16_t rdlen = get16(r + n + 8);
		if (type == 1 && rdlen == 4 && n + 12 <= (size_t)len) {
			char a[64];
			if (inet_ntop(AF_INET, r + n + 10, a, sizeof(a))) {
				snprintf(ip, ipcap, "%s", a);
				return true;
			}
		}
		n += 10 + rdlen;
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* phase (b2): HTTP-204 over the resolved IP (bound to the tunnel)      */
/* ------------------------------------------------------------------ */

static void http_chain_settle(bool got_code)
{
	struct sw_ctx *c = job.c;

	if (job.http_settled)
		return;
	job.http_settled = true;
	if (!got_code)
		c->health.http.code = 0;    /* timeout verdict stands    */
	uloop_timeout_cancel(&job.x_to);
	xfd_close();
	finish_if_settled();
}

static void http_send_request(int fd)
{
	char req[256];
	int len = snprintf(req, sizeof(req),
			   "GET " SW_HEALTH_PATH " HTTP/1.0\r\n"
			   "Host: " SW_HEALTH_TARGET "\r\n"
			   "User-Agent: saadwayd/1.0\r\n"
			   "Connection: close\r\n\r\n");

	send(fd, req, (size_t)len, 0);
	job.http_sent = true;
	uloop_fd_add(&job.xfd, ULOOP_READ);  /* same fd switches to READ */
}

static void http_try_read(void)
{
	char buf[2048];
	ssize_t n;

	for (int tries = 0; tries < 8 && !job.http_settled; tries++) {
		n = recv(job.xfd.fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			return;             /* EAGAIN: wait next event    */
		}
		/* status line: HTTP/1.x <code> */
		if (n >= 12 && !memcmp(buf, "HTTP/", 5)) {
			int code = 0;
			if (sscanf(buf + 5, "%*s %d", &code) >= 1 && code > 0)
				job.c->health.http.code = code;
		}
		if (job.c->health.http.code > 0) {
			http_chain_settle(true);
			return;
		}
		if (n < (ssize_t)sizeof(buf) - 1)
			return;             /* more data later            */
	}
}

static void x_event(struct uloop_fd *u, unsigned int events)
{
	(void)u;
	(void)events;

	if (!job.http_phase) {
		unsigned char r[512];
		ssize_t n = recv(job.xfd.fd, r, sizeof(r), MSG_DONTWAIT);

		if (n > 12) {
			char ip[64];
			if (dns_parse_a(r, n, ip, sizeof(ip))) {
				snprintf(job.ip, sizeof(job.ip), "%s", ip);
				xfd_close();      /* resolver socket    */
				http_start(job.c, job.ip);
				return;
			}
			http_chain_settle(false);
			return;
		}
		if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
		    errno != EINTR) {
			http_chain_settle(false);   /* e.g. ECONNREFUSED */
			return;
		}
		return;                    /* keep waiting for the timer   */
	}

	if (job.conn_wait) {
		int err = 0;
		socklen_t el = sizeof(err);

		job.conn_wait = false;
		if (getsockopt(job.xfd.fd, SOL_SOCKET, SO_ERROR, &err,
			       &el) != 0 || err != 0) {
			http_chain_settle(false);
			return;
		}
		http_send_request(job.xfd.fd);
		return;
	}
	http_try_read();
}

static void x_timeout(struct uloop_timeout *t)
{
	(void)t;
	/* covers BOTH phases (dns budget, then http budget): the unfinished
	 * one fails to the timeout verdict */
	http_chain_settle(false);
}

static void http_start(struct sw_ctx *c, const char *ip)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	job.http_phase = true;
	job.conn_wait = false;
	if (fd < 0) {
		http_chain_settle(false);
		return;
	}
	if (bind_dev(fd, c->tun_if) != 0) {
		close(fd);
		http_chain_settle(false);
		return;
	}
	set_nonblocking(fd);
	job.xfd.fd = fd;

	struct sockaddr_in dst;
	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons(80);
	inet_pton(AF_INET, ip, &dst.sin_addr);

	/* overlap: master remaining budget caps this phase too (D2's
	 * SW_T_HTTP figure applies to the connect+response sum) */
	int remain = SW_MASTER_BUDGET_MS - (int)ms_since(&job.master_start);
	int budget = SW_T_HTTP * 1000;
	if (remain <= 0 || budget > remain)
		budget = remain;

	if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) == 0) {
		http_send_request(fd);
	} else if (errno == EINPROGRESS) {
		job.conn_wait = true;
		uloop_fd_add(&job.xfd, ULOOP_WRITE);
	} else {
		close(fd);
		job.xfd.fd = -1;
		http_chain_settle(false);
		return;
	}
	uloop_timeout_set(&job.x_to, budget);
}

static void dns_start(void)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);

	job.xfd.fd = -1;
	if (fd < 0) {
		http_chain_settle(false);
		return;
	}
	set_nonblocking(fd);
	job.xfd.fd = fd;
	job.xfd.cb = x_event;
	uloop_fd_add(&job.xfd, ULOOP_READ);
	dns_send();
	uloop_timeout_set(&job.x_to, SW_T_DNSQ * 1000);
}

static void master_timeout(struct uloop_timeout *t)
{
	(void)t;
	finish(true);              /* unsettled reporters stay "timeout"  */
}

/* ------------------------------------------------------------------ */
/* public API (saadway.h contract)                                      */
/* ------------------------------------------------------------------ */

int sw_health_run(struct sw_ctx *c, void (*cb)(struct sw_ctx *c, bool done))
{
	if (job.live)
		return -1;              /* a probe pair already runs   */

	memset(&job, 0, sizeof(job));
	job.c = c;
	job.cb = cb;
	job.ping_fd.fd = -1;
	job.xfd.fd = -1;

	/* verdict baseline: phases may only improve "timeout" */
	c->health.http.code = 0;
	c->health.ping_ok = 0;
	c->health.ping_latency_ms = 0;
	snprintf(c->health.ping_target, sizeof(c->health.ping_target), "%s",
		 SW_PING_TARGET);
	c->health.pending = true;
	c->health.done = false;

	gettimeofday(&job.master_start, NULL);
	job.live = true;

	job.ping_to.cb = ping_timeout;
	job.x_to.cb = x_timeout;
	job.master.cb = master_timeout;

	ping_start(c);
	dns_start();

	uloop_timeout_set(&job.master, SW_MASTER_BUDGET_MS);
	return 0;
}

bool sw_health_is_running(void)
{
	return job.live;
}

void sw_health_abort(void)
{
	if (!job.live)
		return;
	job.done_fired = true;      /* issuer is going away: no callback */
	finish(false);
}
