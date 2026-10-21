/*
 * uci-state.c - UCI state layer (T005, T017, T028)
 *
 * Reads/writes/validates every `saadway` UCI section type verbatim per
 * data-model.md and contracts/uci.md:
 *
 *   `service`             — unique named section, global state + tunables
 *   `saadway_profile`     — <=20, unique name (1-64), unique order, enums
 *   `saadway_domain_rule` — <=500, unique case-insensitive domain
 *   `saadway_device_rule` — <=253, match_value must be IPv4 CIDR or MAC,
 *                           policy in {direct, tunneled}
 *   `dashboard`           — language/session_timeout/authentication
 *
 * Principle IV: secrets are NEVER written to UCI — they live in 0600 files
 * under /etc/saadway/ referenced by UCI (secret_ref / ovpn_ref).
 * Principle VI: UCI is the only persistent state; runtime fields
 * (active_profile, connection_status, failover counters) are persisted back
 * so state survives reboot (FR-024).
 */
#define _GNU_SOURCE
#include "saadway.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>

static struct uci_context *uci;

/* forward decl: per-profile unique if_id loader (used by sw_uci_load) */
static int sw_next_if_id_p(struct sw_ctx *c, struct sw_profile *self);

/* ------------------------------------------------------------------ */
/* Validation helpers                                                   */
/* ------------------------------------------------------------------ */

bool sw_valid_name(const char *name)
{
	size_t n;
	if (!name || !(n = strlen(name)) || n > 64)
		return false;
	if (name[0] == '.' || name[0] == ' ' || name[n - 1] == ' ')
		return false;
	for (size_t i = 0; i < n; i++) {
		unsigned char ch = (unsigned char)name[i];
		if (ch < 0x21 || ch == '"' || ch == '\'' || ch == 0x7f)
			return false;
	}
	return true;
}

bool sw_valid_domain(const char *s)
{
	size_t n, labellen = 0;
	if (!s || !(n = strlen(s)) || n > 253)
		return false;
	for (size_t i = 0; i <= n; i++) {
		char c = (i == n) ? '.' : s[i];
		if (c == '.') {
			if (labellen == 0)
				return false;
			labellen = 0;
			continue;
		}
		if (c == '*') {                 /* wildcard zone allowed */
			if (labellen != 0)
				return false;
			labellen++;             /* as only first char */
			continue;
		}
		if (!isalnum((unsigned char)c) && c != '-' && c != '_' &&
		    !((unsigned char)c > 0x7f))
			return false;           /* no spaces/control */
		labellen++;
	}
	return true;
}

bool sw_valid_hostname(const char *s)
{
	if (!sw_valid_domain(s))
		return false;
	return true;
}

bool sw_valid_mac(const char *s)
{
	if (!s || strlen(s) != 17)
		return false;
	for (int i = 0; i < 17; i++) {
		if (i == 2 || i == 5 || i == 8 || i == 11 || i == 14) {
			if (s[i] != ':')
				return false;
			continue;
		}
		if (!isxdigit((unsigned char)s[i]))
			return false;
	}
	/* all-zero or broadcast treated as invalid rules */
	if (strncmp(s, "00:00:00:00:00:00", 17) == 0)
		return false;
	return true;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

bool sw_valid_cidr4(const char *s, bool hostpart_ok)
{
	unsigned int a = 0, b = 0, cc = 0, d = 0, pl = 32;
	int n;
	char rest[8];
	rest[0] = 0;
	n = sscanf(s, "%u.%u.%u.%u/%7s", &a, &b, &cc, &d, rest);
	if (n == 4) {
		/* plain host address */
	} else if (n == 5) {
		for (const char *c = rest; *c; c++)
			if (!isdigit((unsigned char)*c))
				return false;
		pl = (unsigned)strtoul(rest, NULL, 10);
		if (pl > 32)
			return false;
	} else
		return false;
	if (a > 255 || b > 255 || cc > 255 || d > 255)
		return false;
	if (n == 5 && !hostpart_ok)
		return false;
	return true;
}

/* ------------------------------------------------------------------ */
/* small utilities                                                      */
/* ------------------------------------------------------------------ */

void sw_sb_init(struct sw_strbuf *s)
{
	s->b = NULL;
	s->len = s->cap = 0;
	s->oom = false;
}

static void sb_grow(struct sw_strbuf *s, size_t extra)
{
	size_t need = s->len + extra + 1;
	if (need <= s->cap)
		return;
	size_t ncap = s->cap ? s->cap : 512;
	while (ncap < need)
		ncap *= 2;
	char *n = realloc(s->b, ncap);
	if (!n) {
		s->oom = true;
		return;
	}
	s->b = n;
	s->cap = ncap;
}

void sw_sb_append(struct sw_strbuf *s, const char *text)
{
	size_t l = strlen(text);
	sb_grow(s, l);
	if (s->oom)
		return;
	memcpy(s->b + s->len, text, l);
	s->len += l;
	s->b[s->len] = 0;
}

void sw_sb_appendf(struct sw_strbuf *s, const char *fmt, ...)
{
	char tmp[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	sw_sb_append(s, tmp);
}

void sw_sb_free(struct sw_strbuf *s)
{
	free(s->b);
	sw_sb_init(s);
}

char *sw_sb_release(struct sw_strbuf *s, size_t *len)
{
	if (s->oom) {
		sw_sb_free(s);
		return NULL;
	}
	if (len)
		*len = s->len;
	char *out = s->b ? s->b : strdup("");
	sw_sb_init(s);
	return out;
}

char *sw_util_slurp(const char *path, size_t *len)
{
	FILE *f;
	char *buf = NULL;
	long sz;
	f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	if (sz < 0 || sz > (4L << 20)) {         /* 4 MiB sanity bound */
		fclose(f);
		return NULL;
	}
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		fclose(f);
		free(buf);
		return NULL;
	}
	fclose(f);
	buf[sz] = 0;
	if (len)
		*len = (size_t)sz;
	return buf;
}

int sw_util_write_file_mode(const char *path, const char *data, size_t len,
			    mode_t mode)
{
	char tmp[SW_PATH_MAX + 8];
	int fd;
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0)
		return -1;
	fchmod(fd, mode);
	if (len && write(fd, data, len) != (ssize_t)len) {
		close(fd);
		unlink(tmp);
		return -1;
	}
	close(fd);
	if (rename(tmp, path) != 0) {           /* fsync-less best effort */
		unlink(tmp);
		return -1;
	}
	return 0;
}

int sw_util_mkdirs(const char *path, mode_t mode)
{
	char tmp[SW_PATH_MAX];
	char *p;
	int rc = 0;
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (mkdir(tmp, mode) != 0 && errno != EEXIST)
				rc = -1;
			*p = '/';
		}
	}
	if (mkdir(tmp, mode) != 0 && errno != EEXIST)
		rc = -1;
	return rc;
}

static const char b64t[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int sw_util_b64_encode(const unsigned char *in, size_t inlen, char **out)
{
	size_t olen = ((inlen + 2) / 3) * 4;
	char *buf = malloc(olen + 1);
	size_t i, o = 0;
	if (!buf)
		return -1;
	for (i = 0; i < inlen; i += 3) {
		unsigned v = (unsigned)in[i] << 16;
		if (i + 1 < inlen) v |= in[i + 1] << 8;
		if (i + 2 < inlen) v |= in[i + 2];
		buf[o++] = b64t[(v >> 18) & 63];
		buf[o++] = b64t[(v >> 12) & 63];
		buf[o++] = (i + 1 < inlen) ? b64t[(v >> 6) & 63] : '=';
		buf[o++] = (i + 2 < inlen) ? b64t[v & 63] : '=';
	}
	buf[o] = 0;
	*out = buf;
	return 0;
}

int sw_util_b64_decode(const char *in, unsigned char **out, size_t *outlen)
{
	size_t inlen = strlen(in);
	size_t cap = inlen / 4 * 3 + 3;
	unsigned char *buf = malloc(cap);
	unsigned acc = 0, accbits = 0;
	size_t o = 0;
	if (!buf)
		return -1;
	for (size_t i = 0; in[i]; i++) {
		char c = in[i];
		int v;
		if (c == '\r' || c == '\n' || c == ' ' || c == '\t')
			continue;
		if (c == '=')
			break;
		v = (strchr(b64t, c)) ? (int)(strchr(b64t, c) - b64t) : -1;
		if (v < 0) {
			free(buf);
			return 1;               /* bad syntax */
		}
		acc = (acc << 6) | (unsigned)v;
		accbits += 6;
		if (accbits >= 8) {
			accbits -= 8;
			buf[o++] = (unsigned char)((acc >> accbits) & 0xFF);
		}
	}
	if (o == 0) {
		free(buf);
		return 1;
	}
	buf[o] = 0;
	*out = buf;
	*outlen = o;
	return 0;
}

void sw_util_string_list_add(char ***list, int *n, const char *s)
{
	if (!list || !s)
		return;
	for (int i = 0; i < *n; i++)
		if ((*list)[i] && strcmp((*list)[i], s) == 0)
			return;
	*list = realloc(*list, sizeof(char *) * (size_t)(*n + 1));
	if (!*list)
		return;
	(*list)[*n] = strdup(s);
	(*n)++;
}

static uint32_t fnv1a32(const char *s)
{
	uint32_t h = 2166136261u;
	while (*s) {
		h ^= (unsigned char)*s++;
		h *= 16777619u;
	}
	return h;
}

void sw_util_normalize_name(char *dst, size_t dstlen, const char *src)
{
	size_t i = 0, o = 0;
	while (src[i] && o < dstlen - 1) {
		unsigned char c = (unsigned char)src[i++];
		if (isalnum(c))
			dst[o++] = (char)tolower(c);
		else if (c == '-' || c == '_') {
			/* read o BEFORE the increment: `dst[o++] = (char)(o ? c
			 * : 'a')` modified and read o without a sequence point
			 * (undefined behaviour). */
			char keep = o ? c : 'a';

			dst[o++] = keep;
		}
	}
	while (o < 4)
		dst[o++] = 'a';                 /* avoid too-short ifnames */
	dst[o] = 0;
}

/* ------------------------------------------------------------------ */
/* UCI plumbing (string-lookup API style, like uci CLI; reliable)        */
/* ------------------------------------------------------------------ */

static struct uci_package *pkg;

static int u_lookup(struct uci_ptr *p, const char *path)
{
	return uci_lookup_ptr(uci, p, (char *)path, true);
}

static int u_setval(const char *sec, const char *opt, const char *val)
{
	struct uci_ptr p;
	char path[256];
	if (!sec || !opt)
		return -1;
	snprintf(path, sizeof(path), "saadway.%s.%s", sec, opt);
	if (u_lookup(&p, path) != 0 || !p.p || !p.s)
		return -1;
	p.value = val;
	return uci_set(uci, &p);
}

static int u_add_section(const char *sec, const char *type)
{
	struct uci_ptr p;
	char path[256];
	snprintf(path, sizeof(path), "saadway.%s", sec);
	if (u_lookup(&p, path) != 0 || !p.p)
		return -1;
	/* section must not yet exist */
	if (p.s)
		return -1;
	p.value = type;
	return uci_set(uci, &p);
}

static int u_del_section(const char *sec)
{
	struct uci_ptr p;
	char path[256];
	snprintf(path, sizeof(path), "saadway.%s", sec);
	if (u_lookup(&p, path) != 0 || !p.p || !p.s)
		return -1;
	p.value = NULL;
	return uci_delete(uci, &p);
}

static bool u_section_exists(const char *sec)
{
	struct uci_ptr p;
	char path[256];
	snprintf(path, sizeof(path), "saadway.%s", sec);
	if (u_lookup(&p, path) != 0)
		return false;
	return p.s != NULL;
}

static void u_commit(void)
{
	if (!pkg)
		return;
	uci_save(uci, pkg);
	uci_commit(uci, &pkg, false);
}

static const char *u_get(const char *sec, const char *opt)
{
	if (!pkg || !sec || !opt)
		return NULL;
	struct uci_section *s = NULL;
	struct uci_element *e;
	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *t = uci_to_section(e);
		if (t->e.name && !strcmp(t->e.name, sec)) {
			s = t;
			break;
		}
	}
	if (!s)
		return NULL;
	return uci_lookup_option_string(uci, s, opt);
}

static const char *u_lookup_opt(const char *sec, const char *opt)
{
	return u_get(sec, opt);
}


/* unique generated named section "p<hex>" */
static void u_gen_section(struct sw_ctx *c, char *out, size_t len, const char *base)
{
	uint32_t h = fnv1a32(base);
	(void)c;
	do {
		h = h * 16777619u + 41u;
		snprintf(out, len, "p%x", h & 0xffffffu);
	} while (u_section_exists(out));
}

static struct uci_section *u_find_section(const char *type, const char *opt,
					  const char *val)
{
	struct uci_element *e;
	if (!pkg)
		return NULL;
	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);
		if (strcmp(s->type, type) != 0)
			continue;
		if (!opt)
			return s;
		const char *got = uci_lookup_option_string(uci, s, opt);
		if (!got || !val)
			continue;
		if (strcasecmp(opt, "name") == 0 ? strcasecmp(got, val) == 0
						 : strcmp(got, val) == 0)
			return s;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* in-memory list helpers                                               */
/* ------------------------------------------------------------------ */

struct sw_profile *sw_uci_profile_find(struct sw_ctx *c, const char *name)
{
	struct sw_profile *p;
	for (p = c->st.profiles; p; p = p->next)
		if (strcmp(p->name, name) == 0)
			return p;
	return NULL;
}

bool sw_uci_name_taken(struct sw_ctx *c, const char *name)
{
	return sw_uci_profile_find(c, name) != NULL;
}

static void profile_free_list(struct sw_profile **list)
{
	while (*list) {
		struct sw_profile *p = *list;
		*list = p->next;
		free(p);
	}
}

static bool boolval(const char *s, bool dflt)
{
	if (!s)
		return dflt;
	return !(strcmp(s, "0") == 0 || strcasecmp(s, "false") == 0 ||
		 strcasecmp(s, "off") == 0 || strcmp(s, "") == 0);
}

static bool domain_taken(struct sw_ctx *c, const char *dom)
{
	struct sw_domain_rule *d;
	for (d = c->st.domains; d; d = d->next)
		if (strcasecmp(d->domain, dom) == 0)
			return true;
	return false;
}

static bool device_taken(struct sw_ctx *c, int mtype, const char *val)
{
	struct sw_device_rule *d;
	for (d = c->st.devices; d; d = d->next)
		if (d->match_type == mtype && strcmp(d->match_value, val) == 0)
			return true;
	return false;
}

static int n_prot(struct sw_ctx *c)
{
	int n = 0;
	struct sw_profile *p;
	for (p = c->st.profiles; p; p = p->next, n++)
		;
	return n;
}

static int n_domains(struct sw_ctx *c)
{
	int n = 0;
	struct sw_domain_rule *d;
	for (d = c->st.domains; d; d = d->next, n++)
		;
	return n;
}

static int n_devices(struct sw_ctx *c)
{
	int n = 0;
	struct sw_device_rule *d;
	for (d = c->st.devices; d; d = d->next, n++)
		;
	return n;
}


int sw_next_if_id(struct sw_ctx *c)
{
	uint32_t h = fnv1a32(c->st.active_profile[0] ? c->st.active_profile : "run");
	int id;
	for (;;) {
		bool free = true;
		struct sw_profile *p;
		h = h * 16777619u + 41u;
		id = 0x1000 + (int)(h & 0x7fffu);
		for (p = c->st.profiles; p; p = p->next)
			if (p->kind == SW_KIND_IKEV2 && p->if_id == id) {
				free = false;
				break;
			}
		if (free)
			return id;
	}
}

void sw_profile_compute_ifname(struct sw_profile *p)
{
	uint32_t h = fnv1a32(p->name);
	if (p->kind == SW_KIND_IKEV2)
		snprintf(p->ifname, sizeof(p->ifname), "xfrm%04x", h & 0xffff);
	else
		snprintf(p->ifname, sizeof(p->ifname), "saad%04x", h & 0xffff);
}

/* ------------------------------------------------------------------ */
/* full config load (parse + validate; empty/missing config = defaults) */
/* ------------------------------------------------------------------ */

static struct sw_profile *profile_alloc(void)
{
	struct sw_profile *p = calloc(1, sizeof(*p));
	if (!p)
		return NULL;
	p->kind = SW_KIND_OPENVPN;
	p->enabled = true;
	p->order = 0;
	p->if_id = -1;
	strcpy(p->remote_id, "same-as-hostname");
	strcpy(p->local_id, "same-as-username");
	strcpy(p->cert_mode, "from-server");
	strcpy(p->proxy, "off");
	strcpy(p->proposal_ike, "aes256gcm16-prfsha256-ecp384-ecp521");
	strcpy(p->proposal_esp, "aes256gcm16");
	return p;
}

/* sorted insert by order; ties → tail */
static void profile_insert(struct sw_ctx *c, struct sw_profile *p)
{
	struct sw_profile **pp = &c->st.profiles;
	while (*pp && (*pp)->order <= p->order)
		pp = &(*pp)->next;
	p->next = *pp;
	*pp = p;
}

static int load_section_profile(struct sw_ctx *c, struct uci_section *s)
{
	struct sw_profile *p = profile_alloc();
	const char *v;
	int n;

	if (!p)
		return -1;
	if (!(v = uci_lookup_option_string(uci, s, "name")) ||
	    !sw_valid_name(v)) {
		free(p);
		return 0;               /* skip silently-invalid legacy entry */
	}
	if (sw_uci_profile_find(c, v)) {
		sw_warn("uci: duplicate profile name '%s' ignored", v);
		free(p);
		return 0;
	}
	if (n_prot(c) >= SW_MAX_PROFILES) {
		sw_warn("uci: profile limit %d exceeded, ignoring '%s'",
			SW_MAX_PROFILES, v);
		free(p);
		return 0;
	}
	snprintf(p->name, sizeof(p->name), "%s", v);
	v = uci_lookup_option_string(uci, s, "kind");
	p->kind = (v && strcmp(v, "ikev2") == 0) ? SW_KIND_IKEV2 : SW_KIND_OPENVPN;
	p->enabled = boolval(uci_lookup_option_string(uci, s, "enabled"), true);
	v = uci_lookup_option_string(uci, s, "order");
	n = v ? atoi(v) : 0;
	p->order = (n < 1) ? n_prot(c) + 1 : n;
	if ((v = uci_lookup_option_string(uci, s, "secret_ref")) &&
	    strlen(v) < SW_PATH_MAX)
		snprintf(p->secret_ref, sizeof(p->secret_ref), "%s", v);
	if (p->kind == SW_KIND_OPENVPN) {
		if ((v = uci_lookup_option_string(uci, s, "ovpn_ref")) &&
		    strlen(v) < SW_PATH_MAX)
			snprintf(p->ovpn_ref, sizeof(p->ovpn_ref), "%s", v);
		if ((v = uci_lookup_option_string(uci, s, "remote_host")) &&
		    strlen(v) < 96)
			snprintf(p->remote_host, sizeof(p->remote_host), "%s", v);
	} else {
		if ((v = uci_lookup_option_string(uci, s, "hostname")))
			snprintf(p->hostname, sizeof(p->hostname), "%s", v);
		if ((v = uci_lookup_option_string(uci, s, "username")))
			snprintf(p->username, sizeof(p->username), "%s", v);
		if ((v = uci_lookup_option_string(uci, s, "remote_id")) && *v)
			snprintf(p->remote_id, sizeof(p->remote_id), "%s", v);
		if ((v = uci_lookup_option_string(uci, s, "local_id")) && *v)
			snprintf(p->local_id, sizeof(p->local_id), "%s", v);
		if ((v = uci_lookup_option_string(uci, s, "cert_mode")) && *v)
			snprintf(p->cert_mode, sizeof(p->cert_mode), "%s", v);
		if ((v = uci_lookup_option_string(uci, s, "proxy")) && *v)
			snprintf(p->proxy, sizeof(p->proxy), "%s", v);
		if ((v = uci_lookup_option_string(uci, s, "proposal_ike")) && *v)
			snprintf(p->proposal_ike, sizeof(p->proposal_ike), "%s", v);
		if ((v = uci_lookup_option_string(uci, s, "proposal_esp")) && *v)
			snprintf(p->proposal_esp, sizeof(p->proposal_esp), "%s", v);
		if (strstr(p->remote_id, "same-") != p->remote_id &&
		    !(isalnum((unsigned char)p->remote_id[0]) ||
		      p->remote_id[0] == '@' || p->remote_id[0] > 0x7e))
			strcpy(p->remote_id, "same-as-hostname");
		if (strcmp(p->cert_mode, "from-server") != 0)
			strcpy(p->cert_mode, "from-server");   /* spec v1 enum */
		if (strcmp(p->proxy, "off") != 0)
			strcpy(p->proxy, "off");               /* spec v1 enum */
	}
	profile_insert(c, p);
	return 0;
}

static int load_section_domain(struct sw_ctx *c, struct uci_section *s)
{
	struct sw_domain_rule *d, **pp;
	const char *v = uci_lookup_option_string(uci, s, "domain");

	if (!v || !sw_valid_domain(v))
		return 0;
	if (domain_taken(c, v)) {
		sw_warn("uci: duplicate domain rule '%s' ignored", v);
		return 0;
	}
	if (n_domains(c) >= SW_MAX_DOMAINS) {
		sw_warn("uci: domain rule limit %d reached", SW_MAX_DOMAINS);
		return 0;
	}
	d = calloc(1, sizeof(*d));
	if (!d)
		return -1;
	memcpy(d->domain, v, sizeof(d->domain) - 1);
	for (pp = &c->st.domains; *pp; pp = &(*pp)->next)
		;
	d->next = *pp;
	*pp = d;
	return 0;
}

static int load_section_device(struct sw_ctx *c, struct uci_section *s)
{
	struct sw_device_rule *d, **pp;
	const char *v = uci_lookup_option_string(uci, s, "match_value");
	const char *t = uci_lookup_option_string(uci, s, "match_type");
	const char *pol = uci_lookup_option_string(uci, s, "policy");
	int mtype, ptype;

	if (!v || !t || !pol)
		return 0;
	if (!strcmp(t, "ip"))
		mtype = SW_MTYPE_IP;
	else if (!strcmp(t, "mac"))
		mtype = SW_MTYPE_MAC;
	else
		return 0;
	if (!strcmp(pol, "direct"))
		ptype = SW_POLICY_DIRECT;
	else if (!strcmp(pol, "tunneled"))
		ptype = SW_POLICY_TUNNELED;
	else
		return 0;               /* reject values outside enum set */
	if (mtype == SW_MTYPE_IP ? !sw_valid_cidr4(v, true) : !sw_valid_mac(v)) {
		sw_warn("uci: invalid device match_value '%s' ignored", v);
		return 0;
	}
	if (device_taken(c, mtype, v)) {
		sw_warn("uci: duplicate device rule '%s' ignored", v);
		return 0;
	}
	if (n_devices(c) >= SW_MAX_DEVICES) {
		sw_warn("uci: device rule limit %d reached", SW_MAX_DEVICES);
		return 0;
	}
	d = calloc(1, sizeof(*d));
	if (!d)
		return -1;
	d->match_type = mtype;
	d->ptype = ptype;
	memcpy(d->match_value, v, sizeof(d->match_value) - 1);
	for (pp = &c->st.devices; *pp; pp = &(*pp)->next)
		;
	d->next = *pp;
	*pp = d;
	return 0;
}

static bool parse_conn_status(const char *s, int *out)
{
	if (!s || !*s)
		*out = SW_CONN_DOWN;
	else if (!strcmp(s, "up"))
		*out = SW_CONN_UP;
	else if (!strcmp(s, "down"))
		*out = SW_CONN_DOWN;
	else if (!strcmp(s, "connecting"))
		*out = SW_CONN_CONNECTING;
	else if (!strcmp(s, "contained"))
		*out = SW_CONN_CONTAINED;
	else if (!strcmp(s, "cooldown"))
		*out = SW_CONN_COOLDOWN;
	else
		*out = SW_CONN_DOWN;
	return true;
}

static void load_service(struct sw_ctx *c, struct uci_section *s)
{
	const char *v;
	int n;

	c->st.enabled = boolval(uci_lookup_option_string(uci, s, "enabled"), true);
	c->st.kill_switch = boolval(uci_lookup_option_string(uci, s, "kill_switch"), false);
	c->st.iprange_bypass = boolval(uci_lookup_option_string(uci, s, "iprange_bypass"), false);
	c->st.cycle_exhausted = boolval(uci_lookup_option_string(uci, s, "cycle_exhausted"), false);

	v = uci_lookup_option_string(uci, s, "active_profile");
	if (v && sw_valid_name(v))
		memcpy(c->st.active_profile, v, sizeof(c->st.active_profile) - 1);
	parse_conn_status(uci_lookup_option_string(uci, s, "connection_status"),
			  &c->st.conn_status);

	v = uci_lookup_option_string(uci, s, "failover_attempts");
	n = v ? atoi(v) : 3;
	c->st.failover_attempts = (n < 1) ? 1 : (n > 10) ? 10 : n;
	v = uci_lookup_option_string(uci, s, "failover_cooldown");
	n = v ? atoi(v) : 300;
	c->st.failover_cooldown = (n < 30) ? 30 : (n > 86400) ? 86400 : n;
	v = uci_lookup_option_string(uci, s, "probe_interval");
	n = v ? atoi(v) : 30;
	c->st.probe_interval = (n < 5) ? 5 : n;
	v = uci_lookup_option_string(uci, s, "stats_interval");
	n = v ? atoi(v) : 1;
	c->st.stats_interval = (n < 1) ? 1 : n;
	v = uci_lookup_option_string(uci, s, "run_attempts");
	n = v ? atoi(v) : 0;
	c->st.run_attempts = (n < 0) ? 0 : n;
}

static void load_dashboard(struct sw_ctx *c, struct uci_section *s)
{
	const char *v;
	int n;

	v = uci_lookup_option_string(uci, s, "language");
	if (v && (!strcmp(v, "fa") || !strcmp(v, "en")))
		memcpy(c->st.language, v, sizeof(c->st.language) - 1);
	v = uci_lookup_option_string(uci, s, "authentication");
	if (v && (!strcmp(v, "dedicated") || !strcmp(v, "system")))
		memcpy(c->st.auth_mode, v, sizeof(c->st.auth_mode) - 1);
	v = uci_lookup_option_string(uci, s, "session_timeout");
	n = v ? atoi(v) : 1800;
	c->st.session_timeout = (n < 300) ? 300 : n;
}

int sw_uci_load(struct sw_ctx *c)
{
	struct uci_element *e;
	struct uci_section *svc = NULL, *dash = NULL;

	if (!uci) {
		uci = uci_alloc_context();
		if (!uci)
			return SW_UCI_E_INTERNAL;
	}
	if (pkg) {
		uci_unload(uci, pkg);
		pkg = NULL;
	}
	if (uci_load(uci, "saadway", &pkg) != 0) {
		pkg = NULL;             /* empty config parses to defaults */
		sw_info("uci: empty/missing saadway config, defaults in use");
	}

	/* traverse in file order */
	if (pkg) {
		uci_foreach_element(&pkg->sections, e) {
			struct uci_section *s = uci_to_section(e);
			int rc;
			if (!strcmp(s->type, "saadway_profile"))
				rc = load_section_profile(c, s);
			else if (!strcmp(s->type, "saadway_domain_rule"))
				rc = load_section_domain(c, s);
			else if (!strcmp(s->type, "saadway_device_rule"))
				rc = load_section_device(c, s);
			else if (!strcmp(s->type, "service"))
				svc = s;
			else if (!strcmp(s->type, "dashboard"))
				dash = s;
			else
				continue;
			if (rc != 0)
				return SW_UCI_E_INTERNAL;
		}
	}
	if (svc)
		load_service(c, svc);
	if (dash)
		load_dashboard(c, dash);

	/* validate active_profile pointer (stale → cleared) */
	if (c->st.active_profile[0] && !sw_uci_profile_find(c, c->st.active_profile)) {
		sw_warn("uci: stale active_profile '%s' cleared",
			c->st.active_profile);
		c->st.active_profile[0] = 0;
	}

	/* ikev2 interfaces/if_ids are derived deterministically */
	for (struct sw_profile *p = c->st.profiles; p; p = p->next)
		if (p->kind == SW_KIND_IKEV2)
			p->if_id = sw_next_if_id_p(c, p);

	return SW_UCI_OK;
}

/* deterministic, unique-per-ikev2-profile if_id (data-model FR-003) */
static int sw_next_if_id_p(struct sw_ctx *c, struct sw_profile *self)
{
	uint32_t h = fnv1a32(self->name);
	int id, guard = 0;
	for (;;) {
		bool used = false;
		struct sw_profile *p;
		if (guard++ > 0x8000)
			id = 1;         /* exhausted: walk linearly */
		h = h * 16777619u + 41u;
		id = 0x1000 + (int)(h & 0x7fffu);
		for (p = c->st.profiles; p; p = p->next)
			if (p != self && p->kind == SW_KIND_IKEV2 &&
			    p->if_id == id) {
				used = true;
				break;
			}
		if (!used)
			return id;
	}
}

/* ------------------------------------------------------------------ */
/* UCI persistence: full section rewrite (Principle VI: UCI-only store) */
/* ------------------------------------------------------------------ */

/* delete every section of the given types (bounded to this file's scale) */
static void u_wipe_types(const char **types, int ntypes)
{
	const char *names[1280];        /* 20 profiles + 500 domains + 253 devices + slack */
	int n = 0, i;
	struct uci_element *e;

	for (i = 0; i < ntypes && n < (int)(sizeof(names) / sizeof(names[0])); i++) {
		uci_foreach_element(&pkg->sections, e) {
			struct uci_section *s = uci_to_section(e);
			if (!strcmp(s->type, types[i]) &&
			    n < (int)(sizeof(names) / sizeof(names[0])))
				names[n++] = strdup(e->name);
		}
	}
	for (i = 0; i < n; i++) {
		u_del_section(names[i]);
		free((char *)names[i]);
	}
}

static void write_profile_sections(struct sw_ctx *c)
{
	struct sw_profile *p;
	for (p = c->st.profiles; p; p = p->next) {
		char secname[32];
		u_gen_section(c, secname, sizeof(secname), p->name);
		if (u_add_section(secname, "saadway_profile") != 0)
			continue;
		u_setval(secname, "name", p->name);
		u_setval(secname, "kind",
			 p->kind == SW_KIND_IKEV2 ? "ikev2" : "openvpn");
		u_setval(secname, "enabled", p->enabled ? "1" : "0");
		char ord[12];
		snprintf(ord, sizeof(ord), "%d", p->order);
		u_setval(secname, "order", ord);
		if (p->secret_ref[0])
			u_setval(secname, "secret_ref", p->secret_ref);
		if (p->kind == SW_KIND_OPENVPN) {
			if (p->ovpn_ref[0])
				u_setval(secname, "ovpn_ref", p->ovpn_ref);
			if (p->remote_host[0])
				u_setval(secname, "remote_host", p->remote_host);
		} else {
			u_setval(secname, "hostname", p->hostname);
			u_setval(secname, "username", p->username);
			u_setval(secname, "remote_id", p->remote_id);
			u_setval(secname, "local_id", p->local_id);
			u_setval(secname, "cert_mode", p->cert_mode);
			u_setval(secname, "proxy", p->proxy);
			u_setval(secname, "proposal_ike", p->proposal_ike);
			u_setval(secname, "proposal_esp", p->proposal_esp);
		}
	}
}

static void write_domain_sections(struct sw_ctx *c)
{
	struct sw_domain_rule *d;
	for (d = c->st.domains; d; d = d->next) {
		char secname[32];
		u_gen_section(c, secname, sizeof(secname), d->domain);
		if (u_add_section(secname, "saadway_domain_rule") != 0)
			continue;
		u_setval(secname, "domain", d->domain);
		char created[32];
		snprintf(created, sizeof(created), "%ld", (long)time(NULL));
		u_setval(secname, "created", created);
	}
}

static void write_device_sections(struct sw_ctx *c)
{
	struct sw_device_rule *d;
	for (d = c->st.devices; d; d = d->next) {
		char secname[32];
		u_gen_section(c, secname, sizeof(secname), d->match_value);
		if (u_add_section(secname, "saadway_device_rule") != 0)
			continue;
		u_setval(secname, "match_type",
			 d->match_type == SW_MTYPE_IP ? "ip" : "mac");
		u_setval(secname, "match_value", d->match_value);
		u_setval(secname, "policy",
			 d->ptype == SW_POLICY_DIRECT ? "direct" : "tunneled");
	}
}

/* regenerate the structural part of the config: profiles/domains/devices */
static int u_rewrite_structural(struct sw_ctx *c)
{
	static const char *types[] = {
		"saadway_profile", "saadway_domain_rule", "saadway_device_rule"
	};
	if (!pkg)
		return SW_UCI_E_INTERNAL;
	u_wipe_types(types, 3);
	write_profile_sections(c);
	write_domain_sections(c);
	write_device_sections(c);
	u_commit();
	return SW_UCI_OK;
}

/* ------------------------------------------------------------------ */
/* profile mutations (T016 pathway; orders / uniqueness enforced)       */
/* ------------------------------------------------------------------ */

int sw_uci_profile_add(struct sw_ctx *c, struct sw_profile *p)
{
	if (!sw_valid_name(p->name))
		return SW_UCI_E_BOUNDS;
	if (sw_uci_name_taken(c, p->name))
		return SW_UCI_E_DUPLICATE_NAME;
	if (n_prot(c) >= SW_MAX_PROFILES)
		return SW_UCI_E_LIMIT;
	int max_order = 0;
	for (struct sw_profile *q = c->st.profiles; q; q = q->next)
		if (q->order > max_order)
			max_order = q->order;
	p->order = max_order + 1;
	p->next = NULL;
	profile_insert(c, p);
	if (p->kind == SW_KIND_IKEV2)
		p->if_id = sw_next_if_id_p(c, p);
	sw_profile_compute_ifname(p);
	return u_rewrite_structural(c);
}

int sw_uci_profile_update(struct sw_ctx *c, struct sw_profile *p)
{
	struct sw_profile *q = sw_uci_profile_find(c, p->name);
	if (!q)
		return SW_UCI_E_BOUNDS;
	int keep_order = q->order;
	*q = *p;
	q->order = keep_order;
	return u_rewrite_structural(c);
}

int sw_uci_profile_delete(struct sw_ctx *c, const char *name)
{
	struct sw_profile **pp = &c->st.profiles;
	while (*pp) {
		struct sw_profile *p = *pp;
		if (strcmp(p->name, name) == 0) {
			*pp = p->next;
			free(p);
			return u_rewrite_structural(c);
		}
		pp = &p->next;
	}
	return SW_UCI_E_BOUNDS;
}

int sw_uci_profile_renumber(struct sw_ctx *c)
{
	int i = 1;
	for (struct sw_profile *p = c->st.profiles; p; p = p->next)
		p->order = i++;
	return u_rewrite_structural(c);
}

struct sw_profile *sw_uci_profile_active(struct sw_ctx *c)
{
	return c->st.active_profile[0] ?
	       sw_uci_profile_find(c, c->st.active_profile) : NULL;
}

/* ------------------------------------------------------------------ */
/* steering mutations (T025 pathway; E_DUPLICATE / scale limits)        */
/* ------------------------------------------------------------------ */

int sw_uci_domain_add(struct sw_ctx *c, const char *domain)
{
	struct sw_domain_rule *d, **pp;

	if (!sw_valid_domain(domain))
		return SW_UCI_E_BOUNDS;
	if (domain_taken(c, domain))
		return SW_UCI_E_DUPLICATE_DOMAIN;
	if (n_domains(c) >= SW_MAX_DOMAINS)
		return SW_UCI_E_LIMIT;
	d = calloc(1, sizeof(*d));
	if (!d)
		return SW_UCI_E_INTERNAL;
	memcpy(d->domain, domain, sizeof(d->domain) - 1);
	for (pp = &c->st.domains; *pp; pp = &(*pp)->next)
		;
	d->next = *pp;
	*pp = d;
	return u_rewrite_structural(c);
}

int sw_uci_domain_remove(struct sw_ctx *c, const char *domain)
{
	struct sw_domain_rule **pp = &c->st.domains;
	while (*pp) {
		struct sw_domain_rule *d = *pp;
		if (strcasecmp(d->domain, domain) == 0) {
			*pp = d->next;
			free(d);
			return u_rewrite_structural(c);
		}
		pp = &d->next;
	}
	return SW_UCI_E_BOUNDS;
}

int sw_uci_device_set(struct sw_ctx *c, int mtype, const char *value, int pol)
{
	bool is_new = !device_taken(c, mtype, value);

	if (mtype == SW_MTYPE_IP ? !sw_valid_cidr4(value, true)
				 : !sw_valid_mac(value))
		return SW_UCI_E_BOUNDS;
	if (is_new && n_devices(c) >= SW_MAX_DEVICES)
		return SW_UCI_E_LIMIT;
	for (struct sw_device_rule *d = c->st.devices; d; d = d->next) {
		if (d->match_type == mtype && strcmp(d->match_value, value) == 0) {
			d->ptype = pol;
			return u_rewrite_structural(c);
		}
	}
	struct sw_device_rule *d = calloc(1, sizeof(*d));
	if (!d)
		return SW_UCI_E_INTERNAL;
	d->match_type = mtype;
	d->ptype = pol;
	memcpy(d->match_value, value, sizeof(d->match_value) - 1);
	struct sw_device_rule **pp = &c->st.devices;
	while (*pp)
		pp = &(*pp)->next;
	d->next = *pp;
	*pp = d;
	return u_rewrite_structural(c);
}

int sw_uci_device_remove(struct sw_ctx *c, int mtype, const char *value)
{
	struct sw_device_rule **pp = &c->st.devices;
	while (*pp) {
		struct sw_device_rule *d = *pp;
		if (d->match_type == mtype && strcmp(d->match_value, value) == 0) {
			*pp = d->next;
			free(d);
			return u_rewrite_structural(c);
		}
		pp = &d->next;
	}
	return SW_UCI_E_BOUNDS;
}

/* ------------------------------------------------------------------ */
/* runtime-state persistence (T017, FR-024: survives reboot)            */
/* ------------------------------------------------------------------ */

static void service_opt(struct sw_ctx *c, const char *opt, const char *val)
{
	u_setval("service", opt, val);
}

int sw_uci_persist_state(struct sw_ctx *c)
{
	char buf[32];

	if (!pkg)
		return SW_UCI_E_INTERNAL;
	service_opt(c, "enabled", c->st.enabled ? "1" : "0");
	service_opt(c, "active_profile", c->st.active_profile);
	static const char *conn_names[] = {
		[SW_CONN_DOWN] = "down", [SW_CONN_UP] = "up",
		[SW_CONN_CONNECTING] = "connecting", [SW_CONN_CONTAINED] = "contained",
		[SW_CONN_COOLDOWN] = "cooldown",
	};
	service_opt(c, "connection_status", conn_names[c->st.conn_status]);
	snprintf(buf, sizeof(buf), "%d", c->st.run_attempts);
	service_opt(c, "run_attempts", buf);
	service_opt(c, "cycle_exhausted", c->st.cycle_exhausted ? "1" : "0");
	u_commit();
	return SW_UCI_OK;
}

int sw_uci_persist_tunables(struct sw_ctx *c)
{
	char buf[32];
	if (!pkg)
		return SW_UCI_E_INTERNAL;
	service_opt(c, "kill_switch", c->st.kill_switch ? "1" : "0");
	snprintf(buf, sizeof(buf), "%d", c->st.failover_attempts);
	service_opt(c, "failover_attempts", buf);
	snprintf(buf, sizeof(buf), "%d", c->st.failover_cooldown);
	service_opt(c, "failover_cooldown", buf);
	snprintf(buf, sizeof(buf), "%d", c->st.probe_interval);
	service_opt(c, "probe_interval", buf);
	snprintf(buf, sizeof(buf), "%d", c->st.stats_interval);
	service_opt(c, "stats_interval", buf);
	service_opt(c, "iprange_bypass", c->st.iprange_bypass ? "1" : "0");
	u_commit();
	return SW_UCI_OK;
}

int sw_uci_persist_dashboard(struct sw_ctx *c)
{
	char buf[32];
	if (!pkg)
		return SW_UCI_E_INTERNAL;
	if (!u_find_section("dashboard", NULL, NULL))
		u_add_section("dashboard", "dashboard");
	u_setval("dashboard", "language", c->st.language);
	u_setval("dashboard", "authentication", c->st.auth_mode);
	snprintf(buf, sizeof(buf), "%d", c->st.session_timeout);
	u_setval("dashboard", "session_timeout", buf);
	u_commit();
	return SW_UCI_OK;
}

/* ------------------------------------------------------------------ */
/* secrets (Principle IV): 0600 files under /etc/saadway/secrets/<prof> */
/* ------------------------------------------------------------------ */

int sw_secret_write(const char *profile, const char *leaf, const char *data)
{
	char dir[SW_PATH_MAX + 8], path[SW_PATH_MAX * 2];
	mode_t um;

	if (!profile || !leaf || !data ||
	    strchr(profile, '/') || strchr(profile, '.') || !profile[0] ||
	    strchr(leaf, '/'))
		return -1;
	snprintf(dir, sizeof(dir), "%s/%s", SW_SECRETS_DIR, profile);
	um = umask(077);			/* 0600 for files, 0700 for dirs */
	sw_util_mkdirs(dir, 0700);
	snprintf(path, sizeof(path), "%s/%s", dir, leaf);
	int rc = sw_util_write_file_mode(path, data, strlen(data), 0600);
	sw_util_mkdirs(SW_RUNSTATE_DIR, 0700);
	umask(um);
	if (rc == 0)
		sw_info("secret file written: /etc/saadway/secrets/%s/%s",
			profile, leaf);
	return rc;
}

int sw_secret_check(const char *rel_ref)
{
	char path[SW_PATH_MAX * 2];
	struct stat st;

	if (!rel_ref || rel_ref[0] == '/' || strstr(rel_ref, ".."))
		return -1;
	snprintf(path, sizeof(path), SW_BASE_DIR "/%s", rel_ref);
	if (stat(path, &st) != 0) {
		sw_err("secret missing: %s (activation refused)", path);
		return -1;
	}
	if ((st.st_mode & 0777) != 0600) {
		sw_warn("secret %s had mode %o; repairing to 0600", path,
			st.st_mode & 0777);
		if (chmod(path, 0600) != 0)
			return -1;
	}
	return 0;
}
