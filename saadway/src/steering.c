/*
 * steering.c - traffic steering engine (T023, T024, T026)
 *
 * Evaluation chain (research D4, clarification Q1-A):
 *     device-direct > domain-bypass > IP-range auto-bypass > implicit tunnel
 * Implemented declaratively: policy.c renders the mark_rules chain in this
 * exact priority order; steering.c owns the bundled CIDR set (FR-010, kept
 * offline in /etc/saadway/bypass-cidr.txt) and the bypass-domain DNS
 * exception (research D5 nuance: bypass domains resolve via the WAN
 * resolver — their queries are forwarded by the tunnel dnsmasq instance to
 * the main/LAN resolver at 127.0.0.1#53 — while tunneled traffic stays on
 * the tunnel resolver).
 */
#define _GNU_SOURCE
#include "saadway.h"

#include <sys/stat.h>

static char **range_cids;
static int n_range_cids;

/*
 * The bundled local/country/banking bypass CIDR set (FR-010): shipped
 * offline with the package, refreshed between releases. Line format: one
 * CIDR per line, '#' comment lines / trailing comments.
 */
int sw_steering_init(struct sw_ctx *c)
{
	(void)c;
	size_t len = 0;
	char *buf = sw_util_slurp(SW_BYPASS_FILE, &len);
	if (!buf) {
		sw_warn("steering: bundled CIDR set missing at %s", SW_BYPASS_FILE);
		return -1;
	}
	for (char *p = buf; p && *p; ) {
		char line[128];
		char *eol = strchr(p, '\n');
		size_t l = eol ? (size_t)(eol - p) : strlen(p);
		if (l >= sizeof(line))
			l = sizeof(line) - 1;
		memcpy(line, p, l);
		line[l] = 0;
		p = eol ? eol + 1 : NULL;

		/* strip comment */
		char *hash = strchr(line, '#');
		if (hash)
			*hash = 0;
		char *tok = strtok(line, " \t\r");
		if (!tok)
			continue;
		if (!sw_valid_cidr4(tok, true)) {
			sw_warn("steering: skipping invalid CIDR '%s'", tok);
			continue;
		}
		range_cids = realloc(range_cids,
				     sizeof(char *) * (size_t)(n_range_cids + 1));
		if (!range_cids)
			break;
		range_cids[n_range_cids++] = strdup(tok);
	}
	free(buf);
	sw_info("steering: %d bypass CIDRs loaded from bundled set", n_range_cids);
	return 0;
}

int sw_steering_count_ranges(void)
{
	return n_range_cids;
}

void sw_steering_free(void)
{
	for (int i = 0; i < n_range_cids; i++)
		free(range_cids[i]);
	free(range_cids);
	range_cids = NULL;
	n_range_cids = 0;
}

/*
 * mark_rules chain content in EXACT evaluation priority order
 * (unit-test target for the Q1-A precedence chain).
 */
void sw_steering_mark_rules(struct sw_strbuf *sb, struct sw_ctx *c)
{
	/* 1) device policies: direct has absolute priority (Q1-A).
	 *    explicit 'tunneled' devices re-assert the default mark too. */
	for (struct sw_device_rule *d = c->st.devices; d; d = d->next) {
		int mark = d->ptype == SW_POLICY_DIRECT ? SW_MARK_DIRECT
							: SW_MARK_TUNNEL;
		if (d->match_type == SW_MTYPE_IP)
			sw_sb_appendf(sb, "\tip saddr %s meta mark set 0x%02lx\n",
				      d->match_value, (unsigned long)mark);
		else
			sw_sb_appendf(sb, "\tether saddr %s meta mark set 0x%02lx\n",
				      d->match_value, (unsigned long)mark);
	}

	/* 2) domain bypass (destination IPs learned via dnsmasq nftset) */
	sw_sb_append(sb, "\tip daddr @bypass_domains meta mark set 0x02\n");
	/* 3) bundled IP-range auto bypass (FR-010) */
	sw_sb_append(sb, "\tip daddr @bypass_ranges meta mark set 0x04\n");
	/* 4) implicit tunnel default (Q1-A): everything else tunneled */
	sw_sb_append(sb, "\tmeta mark set 0x08\n");
}

void sw_steering_mark_ranges(struct sw_strbuf *sb)
{
	for (int i = 0; i < n_range_cids; i++)
		sw_sb_appendf(sb, "%s%s", i ? ", " : "", range_cids[i]);
}

/* T026: bypass-domain DNS exception for the dnsmasq tunnel instance */
void sw_steering_dnsconf_append(struct sw_strbuf *sb, struct sw_ctx *c)
{
	for (struct sw_domain_rule *d = c->st.domains; d; d = d->next) {
		sw_sb_appendf(sb, "\nserver=/%s/127.0.0.1#53", d->domain);
		sw_sb_appendf(sb, "\nnftset=/%s/4#inet#saadway#bypass_domains",
			      d->domain);
	}
}
