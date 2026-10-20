/*
 * log.c - syslog-backed logging with secret scrubbing (T006, FR-021)
 *
 * Every emitted entry is scrubbed against a registry of secret values
 * (passwords, tokens, private keys) registered by the tunnel modules, and
 * additionally against a few obvious key=value patterns. A bounded
 * in-memory ring of recent entries backs the cursor-based `mon.logs` tail
 * (the same lines go to syslog; the ring never stores un-scrubbed text).
 */
#define _GNU_SOURCE
#include "saadway.h"

#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#define SW_RING_CAP 512
#define SW_MAX_SECRETS 64

struct sw_log_entry_priv {
	long seq;
	long ts;
	int prio;
	char msg[512];
};

static struct {
	int nsecrets;
	char *secrets[SW_MAX_SECRETS];
	struct sw_log_entry_priv ring[SW_RING_CAP];
	int head;
	int used;
	long seq;
} L;

static const struct {
	const char *key;
	size_t keylen;
} scrub_keys[] = {
	{ "password=", sizeof("password=") - 1 },
	{ "secret=",   sizeof("secret=") - 1   },
	{ "token=",    sizeof("token=") - 1    },
};

static void scrub(char *buf, size_t buflen)
{
	int i;

	for (i = 0; i < L.nsecrets && L.secrets[i]; i++) {
		const char *s = L.secrets[i];
		size_t sl = strlen(s);
		if (!sl)
			continue;
		for (;;) {
			char *p = strstr(buf, s);
			const char *repl = "[REDACTED]";
			size_t rl = strlen(repl), tail, pos;
			if (!p)
				break;
			pos = (size_t)(p - buf);
			tail = strlen(p + sl) + 1;      /* incl NUL */
			if (pos + rl + tail > buflen)
				break;                  /* too long: drop rest */
			memmove(p + rl, p + sl, tail);
			memcpy(p, repl, rl);
		}
	}
	for (i = 0; i < (int)(sizeof(scrub_keys) / sizeof(scrub_keys[0])); i++) {
		char *p = buf;
		while ((p = strstr(p, scrub_keys[i].key)) != NULL) {
			char *start = p + scrub_keys[i].keylen;
			char *end = start;
			while (*end && *end != ' ' && *end != '"' &&
			       *end != ',' && *end != '\r' && *end != '\n')
				end++;
			memset(start, '*', end - start);
			p = end;
		}
	}
}

void sw_log_init(void)
{
	openlog("saadwayd", LOG_PID, LOG_DAEMON);
	setlogmask(LOG_UPTO(LOG_DEBUG));
}

void sw_log(int prio, const char *fmt, ...)
{
	char msg[512];
	va_list ap;
	long now = (long)time(NULL);
	struct sw_log_entry_priv *e;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	scrub(msg, sizeof(msg));

	if (L.used == SW_RING_CAP) {
		L.head = (L.head + 1) % SW_RING_CAP;
		L.used--;
	}
	e = &L.ring[(L.head + L.used) % SW_RING_CAP];
	L.used++;
	memset(e, 0, sizeof(*e));
	e->seq = ++L.seq;
	e->ts = now;
	e->prio = prio;
	memcpy(e->msg, msg, sizeof(e->msg));
	syslog(prio, "%s", msg);
}

void sw_log_secret_register(const char *value)
{
	int i;
	if (!value || !*value || strlen(value) < 3)
		return;                 /* never mask innocent words */
	for (i = 0; i < L.nsecrets; i++)
		if (strcmp(L.secrets[i], value) == 0)
			return;
	if (L.nsecrets >= SW_MAX_SECRETS) {
		/* FIFO eviction keeps scrub set bounded and recent */
		free(L.secrets[0]);
		memmove(L.secrets, L.secrets + 1,
			sizeof(L.secrets[0]) * (SW_MAX_SECRETS - 1));
		L.nsecrets--;
	}
	L.secrets[L.nsecrets++] = strdup(value);
}

void sw_log_secret_unregister_all(void)
{
	int i;
	for (i = 0; i < L.nsecrets; i++) {
		free(L.secrets[i]);
		L.secrets[i] = NULL;
	}
	L.nsecrets = 0;
}

int sw_log_snapshot(struct sw_log_entry *out, int max, long cursor, long *next)
{
	int i, n = 0;

	for (i = 0; i < L.used && n < max; i++) {
		struct sw_log_entry_priv *e = &L.ring[(L.head + i) % SW_RING_CAP];
		if (cursor && e->seq <= cursor)
			continue;
		out[n].ts = e->ts;
		out[n].prio = e->prio;
		out[n].seq = e->seq;
		memcpy(out[n].msg, e->msg, sizeof(out[n].msg));
		n++;
	}
	*next = n ? out[n - 1].seq : cursor;
	return n;
}

void sw_log_shutdown(void)
{
	sw_log_secret_unregister_all();
	closelog();
}
