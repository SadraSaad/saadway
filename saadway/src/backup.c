/*
 * backup.c — portable configuration backup/restore (T038, FR-023/024/025)
 *
 * Schema v1 per contracts/backup-schema.json.  Export renders the entire
 * desired configuration; OpenVPN configs are re-inlined (secret file
 * references are folded back into <tag>…</tag> blocks) so the .ovpn text
 * is self-contained and the normal import pipeline re-derives identical
 * 0600 secret files on the target device (SC-004).  Credentials are
 * inlined ONLY when include_credentials=true — the restore then marks the
 * item restored; otherwise it lands in needs_attention, never in errors
 * (design: config present, secret depends on admin input).
 *
 * Import re-uses the existing mutators so all validation, dedup rules
 * (FR-025), if_id allocation and structural UCI rewriting happen exactly
 * as through the dashboard: a restore can never bypass any safety check.
 *
 * NOTE: import restores CONFIGURATION + desired-state metadata
 * (active_profile, kill switch).  Live tunnel spawning is intentionally
 * left to the reboot-resume engine (main.c T040) — the caller may trigger
 * the reactivation/re-apply (sw_reload) after a successful report.
 */
#define _GNU_SOURCE
#include "saadway.h"

#include <json-c/json.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* export                                                               */
/* ------------------------------------------------------------------ */

/* fold a secret reference line back into an inline block: map leaf names
 * to the same tags the import recognizes in tunnel-openvpn.c */
static const struct { const char *leaf, *tag, *dir; } leafmap[] = {
	{ "ca.pem",       "ca",        "ca"        },
	{ "cert.pem",     "cert",      "cert"      },
	{ "key.pem",      "key",       "key"       },
	{ "dh.pem",       "dh",        "dh"        },
	{ "tlsauth.key",  "tls-auth",  "tls-auth"  },
	{ "tlscrypt.key", "tls-crypt", "tls-crypt" },
};

/* rebuild self-contained .ovpn text from the normalized run config */
static char *rebuild_ovpn_text(const char *name, struct sw_strbuf *warn)
{
	char path[SW_PATH_MAX * 2];
	size_t len = 0;

	snprintf(path, sizeof(path), SW_RUN_DIR "/%s.conf", name);
	char *raw = sw_util_slurp(path, &len);
	if (!raw || !len) {
		if (warn)
			sw_warn("backup export: ovpn config %s unreadable",
				name);
		free(raw);
		return NULL;
	}

	struct sw_strbuf out;
	sw_sb_init(&out);
	for (char *p = raw; p && *p; ) {
		char *eol = strchr(p, '\n');
		size_t ll = eol ? (size_t)(eol - p) : strlen(p);
		char line[1024];

		if (ll >= sizeof(line))
			ll = sizeof(line) - 1;
		memcpy(line, p, ll);
		line[ll] = 0;

		/* "dir /etc/saadway/secrets/<name>/<leaf>" ?              */
		char dir[64] = "", leaf[64] = "";
		if (sscanf(line, "%63s /etc/saadway/secrets/%63[^/]/",
			   dir, leaf) == 2) {
			/* leave management lines verbatim otherwise      */
		}
		if (!strcmp(dir, "auth-user-pass")) {
			; /* credentials handled separately; drop the line */
		} else {
			const char *tag = NULL;
			if (leaf[0])
				for (size_t i = 0; i < sizeof(leafmap) /
						   sizeof(*leafmap); i++)
					if (!strcmp(leaf, leafmap[i].leaf)) {
						tag = leafmap[i].tag;
						break;
					}
			if (tag) {
				char sp[SW_PATH_MAX];
				snprintf(sp, sizeof(sp),
					 SW_SECRETS_DIR "/%s/%s", name, leaf);
				size_t slen = 0;
				char *sec = sw_util_slurp(sp, &slen);

				sw_sb_appendf(&out, "<%s>\n", tag);
				sw_sb_append(&out, sec ? sec : "");
				free(sec);
				sw_sb_appendf(&out, "</%s>\n", tag);
			} else {
				sw_sb_append(&out, line);
				sw_sb_append(&out, "\n");
			}
		}
		p = eol ? eol + 1 : NULL;
	}
	free(raw);
	return sw_sb_release(&out, NULL);
}

static struct json_object *export_profile(struct sw_ctx *c,
					  struct sw_profile *p,
					  bool with_creds)
{
	struct json_object *o = json_object_new_object();
	struct json_object *ov = json_object_new_object();
	char buf[256];

	json_object_object_add(o, "name",
			       json_object_new_string(p->name));
	json_object_object_add(o, "kind", json_object_new_string(
			       p->kind == SW_KIND_OPENVPN ? "openvpn" :
							    "ikev2"));
	json_object_object_add(o, "enabled", json_object_new_boolean(p->enabled));
	json_object_object_add(o, "order", json_object_new_int(p->order));

	/* credentials (only with include_credentials):
	 *   ikev2 → password from secrets/<n>/eap.txt
	 *   openvpn → user/pass from secrets/<n>/auth.txt (u\n p\n)       */
	if (with_creds) {
		struct json_object *cr = NULL;

		if (p->kind == SW_KIND_IKEV2 && p->secret_ref[0]) {
			char path[SW_PATH_MAX];
			snprintf(path, sizeof(path), SW_BASE_DIR "/%s",
				 p->secret_ref);
			size_t l = 0;
			char *pw = sw_util_slurp(path, &l);

			if (pw && l) {
				while (l && (pw[l - 1] == '\n' ||
					     pw[l - 1] == '\r'))
					pw[--l] = 0;
				cr = json_object_new_object();
				json_object_object_add(cr, "username",
					json_object_new_string(p->username));
				json_object_object_add(cr, "password",
					json_object_new_string(pw));
			}
			free(pw);
		}
		if (p->kind == SW_KIND_OPENVPN && p->secret_ref[0]) {
			char path[SW_PATH_MAX];
			snprintf(path, sizeof(path), SW_BASE_DIR "/%s",
				 p->secret_ref);
			size_t l = 0;
			char *txt = sw_util_slurp(path, &l);

			if (txt && l) {
				txt[l ? l - (txt[l - 1] == '\n' ? 1 : 0) : 0]
					= 0;   /* trim final newline only */
				char *eol = strchr(txt, '\n');
				char pw[256] = {0}, us[128] = {0};

				if (eol) {
					snprintf(us, sizeof(us), "%.*s",
						 (int)(eol - txt), txt);
					snprintf(pw, sizeof(pw), "%.*s",
						 (int)(l - (size_t)(eol -
						 txt) - 1), eol + 1);
					char *nl = strchr(pw, '\n');
					if (nl)
						*nl = 0;
					cr = json_object_new_object();
					json_object_object_add(cr, "username",
						json_object_new_string(us));
					json_object_object_add(cr, "password",
						json_object_new_string(pw));
				}
			}
			free(txt);
		}
		if (cr)
			json_object_object_add(o, "credentials", cr);
	}

	/* openvpn side: self-contained ovpn text */
	if (p->kind == SW_KIND_OPENVPN) {
		char *text = rebuild_ovpn_text(p->name, NULL);

		if (text)
			json_object_object_add(ov, "ovpn_text",
					       json_object_new_string(text));
		free(text);
		json_object_object_add(o, "openvpn", ov);
	} else {
		json_object_object_add(ov, "hostname",
				       json_object_new_string(p->hostname));
		json_object_object_add(ov, "username",
				       json_object_new_string(p->username));
		json_object_object_add(ov, "remote_id",
				       json_object_new_string(p->remote_id));
		json_object_object_add(ov, "local_id",
				       json_object_new_string(p->local_id));
		json_object_object_add(ov, "cert_mode",
				       json_object_new_string(p->cert_mode));
		json_object_object_add(ov, "proxy",
				       json_object_new_string(p->proxy));
		json_object_object_add(ov, "proposal_ike",
				       json_object_new_string(
					       p->proposal_ike));
		json_object_object_add(ov, "proposal_esp",
				       json_object_new_string(
					       p->proposal_esp));
		json_object_object_add(o, "ikev2", ov);
	}
	(void)buf;
	(void)c;
	return o;
}

static void iso_utc(char *out, size_t cap)
{
	time_t now = time(NULL);
	struct tm tm;

	gmtime_r(&now, &tm);
	strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

int sw_backup_export(struct sw_ctx *c, bool include_credentials,
		     char **json_out)
{
	struct json_object *root = json_object_new_object(), *src, *svc;
	char created[40];

	if (!json_out)
		return UBUS_STATUS_UNKNOWN_ERROR;
	iso_utc(created, sizeof(created));

	json_object_object_add(root, "schema_version", json_object_new_int(1));
	json_object_object_add(root, "created",
			       json_object_new_string(created));
	src = json_object_new_object();
	{
		char model[128] = "";
		FILE *f = fopen("/tmp/sysinfo/model", "r");

		if (f) {
			size_t n = fread(model, 1, sizeof(model) - 1, f);
			fclose(f);
			while (n && (model[n - 1] == '\n' || model[n - 1] ==
				     '\r'))
				model[--n] = 0;
		}
		if (!model[0])
			snprintf(model, sizeof(model), "OpenWrt device");
		json_object_object_add(src, "model",
				       json_object_new_string(model));
	}
	json_object_object_add(root, "source", src);

	svc = json_object_new_object();
	json_object_object_add(svc, "enabled",
			       json_object_new_boolean(c->st.enabled));
	json_object_object_add(svc, "active_profile",
			       c->st.active_profile[0] ?
			       json_object_new_string(c->st.active_profile) :
			       json_object_new_null());
	json_object_object_add(svc, "kill_switch",
			       json_object_new_boolean(c->st.kill_switch));
	json_object_object_add(svc, "failover_attempts",
			       json_object_new_int(c->st.failover_attempts));
	json_object_object_add(svc, "failover_cooldown",
			       json_object_new_int(c->st.failover_cooldown));
	json_object_object_add(svc, "probe_interval",
			       json_object_new_int(c->st.probe_interval));
	json_object_object_add(svc, "stats_interval",
			       json_object_new_int(c->st.stats_interval));
	json_object_object_add(svc, "iprange_bypass",
			       json_object_new_boolean(c->st.iprange_bypass));
	{
		struct json_object *db = json_object_new_object();

		json_object_object_add(db, "language",
				       json_object_new_string(c->st.language));
		json_object_object_add(db, "session_timeout",
				       json_object_new_int(
					       c->st.session_timeout));
		json_object_object_add(svc, "dashboard", db);
	}
	json_object_object_add(root, "service", svc);

	/* profiles in failover order */
	{
		struct json_object *arr = json_object_new_array();

		for (struct sw_profile *p = c->st.profiles; p; p = p->next)
			json_object_array_add(arr, export_profile(
					       c, p, include_credentials));
		json_object_object_add(root, "profiles", arr);
	}
	{
		struct json_object *st = json_object_new_object();
		struct json_object *dom = json_object_new_array();
		struct json_object *dev = json_object_new_array();

		for (struct sw_domain_rule *d = c->st.domains; d;
		     d = d->next)
			json_object_array_add(dom,
					      json_object_new_string(
						      d->domain));
		for (struct sw_device_rule *r = c->st.devices; r;
		     r = r->next) {
			struct json_object *ro = json_object_new_object();

			json_object_object_add(ro, "match_type",
				json_object_new_string(r->match_type ==
				SW_MTYPE_IP ? "ip" : "mac"));
			json_object_object_add(ro, "match_value",
				json_object_new_string(r->match_value));
			json_object_object_add(ro, "policy",
				json_object_new_string(r->ptype ==
				SW_POLICY_DIRECT ? "direct" : "tunneled"));
			json_object_array_add(dev, ro);
		}
		json_object_object_add(st, "domains", dom);
		json_object_object_add(st, "devices", dev);
		json_object_object_add(root, "steering", st);
	}

	const char *s = json_object_to_json_string(root);
	char *out = s ? strdup(s) : NULL;
	json_object_put(root);
	if (!out)
		return UBUS_STATUS_UNKNOWN_ERROR;
	*json_out = out;
	return UBUS_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* import                                                               */
/* ------------------------------------------------------------------ */

static void add_item(struct json_object *arr, const char *item,
		     const char *reason)
{
	struct json_object *o = json_object_new_object();

	json_object_object_add(o, "item", json_object_new_string(item));
	json_object_object_add(o, "reason", json_object_new_string(reason));
	json_object_array_add(arr, o);
}

static void do_service_prefs(struct sw_ctx *c, struct json_object *svc,
			     struct json_object *restored)
{
	struct json_object *v;

	if ((v = json_object_object_get(svc, "enabled")))
		c->st.enabled = json_object_get_boolean(v);
	if ((v = json_object_object_get(svc, "kill_switch")))
		c->st.kill_switch = json_object_get_boolean(v);
	if ((v = json_object_object_get(svc, "iprange_bypass")))
		c->st.iprange_bypass = json_object_get_boolean(v);
	if ((v = json_object_object_get(svc, "failover_attempts"))) {
		int n = json_object_get_int(v);
		if (n >= 1 && n <= 10)
			c->st.failover_attempts = n;
	}
	if ((v = json_object_object_get(svc, "failover_cooldown"))) {
		int n = json_object_get_int(v);
		if (n >= 30 && n <= 86400)
			c->st.failover_cooldown = n;
	}
	if ((v = json_object_object_get(svc, "probe_interval"))) {
		int n = json_object_get_int(v);
		if (n >= 5)
			c->st.probe_interval = n;
	}
	if ((v = json_object_object_get(svc, "stats_interval"))) {
		int n = json_object_get_int(v);
		if (n >= 1)
			c->st.stats_interval = n;
	}
	sw_uci_persist_tunables(c);
	sw_uci_persist_state(c);

	if ((v = json_object_object_get(svc, "dashboard"))) {
		struct json_object *d;

		if ((d = json_object_object_get(v, "language"))) {
			const char *lang = json_object_get_string(d);
			if (lang && (!strcmp(lang, "fa") || !strcmp(lang, "en")))
				snprintf(c->st.language,
					 sizeof(c->st.language), "%s", lang);
		}
		if ((d = json_object_object_get(v, "session_timeout"))) {
			int n = json_object_get_int(d);
			if (n >= 300)
				c->st.session_timeout = n;
		}
		sw_uci_persist_dashboard(c);
	}
	json_object_array_add(restored, json_object_new_string("service"));
}

static void do_steering(struct sw_ctx *c, struct json_object *steer,
			struct json_object *restored,
			struct json_object *attention)
{
	struct json_object *v;

	if ((v = json_object_object_get(steer, "domains"))) {
		int ok = 0;
		int n = json_object_array_length(v);

		for (int i = 0; i < n; i++) {
			struct json_object *e = json_object_array_get_idx(v, i);
			const char *dom = json_object_get_string(e);

			if (!dom || !sw_valid_domain(dom))
				continue;
			if (sw_uci_domain_add(c, dom) == SW_UCI_OK)
				ok++;
			else
				add_item(attention, dom, "duplicate-merged");
		}
		if (ok)
			json_object_array_add(restored,
					      json_object_new_string(
						      "steering.domains"));
	}
	if ((v = json_object_object_get(steer, "devices"))) {
		int ok = 0;
		int n = json_object_array_length(v);

		for (int i = 0; i < n; i++) {
			struct json_object *e = json_object_array_get_idx(v, i);
			struct json_object *mt, *mv, *mp;
			const char *mtype, *value, *pol;

			mt = json_object_object_get(e, "match_type");
			mv = json_object_object_get(e, "match_value");
			mp = json_object_object_get(e, "policy");
			mtype = mt ? json_object_get_string(mt) : NULL;
			value = mv ? json_object_get_string(mv) : NULL;
			pol = mp ? json_object_get_string(mp) : NULL;
			if (!mtype || !value || !pol)
				continue;
			bool is_ip = !strcmp(mtype, "ip");
			if (!is_ip && strcmp(mtype, "mac"))
				continue;
			if (!is_ip ? !sw_valid_mac(value) :
				     !sw_valid_cidr4(value, true))
				continue;
			bool direct = !strcmp(pol, "direct");
			sw_uci_device_set(c, is_ip ? SW_MTYPE_IP :
					  SW_MTYPE_MAC, value,
					  direct ? SW_POLICY_DIRECT :
						   SW_POLICY_TUNNELED);
			ok++;
		}
		if (ok)
			json_object_array_add(restored,
					      json_object_new_string(
						      "steering.devices"));
	}
}

static void do_profiles(struct sw_ctx *c, struct json_object *profiles,
			struct json_object *restored,
			struct json_object *attention,
			struct json_object *errors)
{
	int n = json_object_array_length(profiles);

	for (int i = 0; i < n; i++) {
		struct json_object *e = json_object_array_get_idx(profiles, i);
		struct json_object *nv = json_object_object_get(e, "name");
		struct json_object *kv = json_object_object_get(e, "kind");
		struct json_object *ev = json_object_object_get(e, "enabled");
		struct json_object *crv = json_object_object_get(e,
								 "credentials");
		const char *name = nv ? json_object_get_string(nv) : NULL;
		const char *kind = kv ? json_object_get_string(kv) : NULL;
		bool enabled = ev ? json_object_get_boolean(ev) : true;
		char detail[256];

		if (!name || !sw_valid_name(name) || !kind) {
			add_item(errors, name ? name : "?", "E_BAD_SYNTAX");
			continue;
		}
		if (sw_uci_name_taken(c, name)) {
			add_item(errors, name, "E_DUPLICATE_NAME");
			continue;
		}

		char *user = NULL, *pass = NULL;
		if (crv) {
			struct json_object *u = json_object_object_get(crv,
								       "username");
			struct json_object *p = json_object_object_get(crv,
								       "password");

			user = u ? (char *)json_object_get_string(u) : NULL;
			pass = p ? (char *)json_object_get_string(p) : NULL;
		}

		if (!strcmp(kind, "openvpn")) {
			struct json_object *ov = json_object_object_get(e,
								        "openvpn");
			struct json_object *tv = ov ?
				json_object_object_get(ov, "ovpn_text") :
				NULL;
			const char *text = tv ?
				json_object_get_string(tv) : NULL;
			char *warnings = NULL;

			if (!text || !*text) {
				add_item(errors, name, "E_BAD_SYNTAX");
				continue;
			}
			char *b64 = NULL;
			if (sw_util_b64_encode((const unsigned char *)text,
					       strlen(text), &b64) != 0) {
				add_item(errors, name, "internal-b64");
				continue;
			}
			int rc = sw_ovpn_import(c, name, b64, user, pass,
						detail, sizeof(detail),
						&warnings);
			free(b64);
			if (rc != UBUS_STATUS_OK) {
				add_item(errors, name, detail);
				free(warnings);
				continue;
			}
			if (warnings && warnings[0]) {   /* auth needed?   */
				struct sw_profile *p = sw_uci_profile_find(
					c, name);
				if (p) {
					p->enabled = enabled;
					sw_uci_profile_update(c, p);
				}
				add_item(attention, name, "credentials-missing");
				free(warnings);
				continue;
			}
			struct sw_profile *p = sw_uci_profile_find(c, name);
			if (p) {
				p->enabled = enabled;
				sw_uci_profile_update(c, p);
			}
			json_object_array_add(restored,
					      json_object_new_string(name));
			free(warnings);
			continue;
		}

		if (!strcmp(kind, "ikev2")) {
			struct json_object *iv = json_object_object_get(e,
								        "ikev2");

			if (!iv) {
				add_item(errors, name, "E_BAD_SYNTAX");
				continue;
			}
			const char *hn = NULL, *un = NULL, *rid, *lid;
			struct json_object *h, *u2, *r, *l;

			h = json_object_object_get(iv, "hostname");
			u2 = json_object_object_get(iv, "username");
			hn = h ? json_object_get_string(h) : NULL;
			un = u2 ? json_object_get_string(u2) : NULL;
			if (!hn || !sw_valid_domain(hn) || !un ||
			    !strpbrk(un, "abcdefghijklmnopqrstuvwxyz"
				     "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")) {
				add_item(errors, name, "E_BAD_SYNTAX");
				continue;
			}
			r = json_object_object_get(iv, "remote_id");
			l = json_object_object_get(iv, "local_id");
			rid = r ? json_object_get_string(r) :
				  "same-as-hostname";
			lid = l ? json_object_get_string(l) :
				  "same-as-username";

			/* credentials FIRST: restore is all-or-nothing */
			bool have_pw = user && pass;
			if (have_pw && sw_secret_write(name, "eap.txt",
						       pass) != 0) {
				add_item(errors, name,
					 "E_SECRET_WRITE_FAILED");
				continue;
			}

			struct sw_profile tmp;

			memset(&tmp, 0, sizeof(tmp));
			snprintf(tmp.name, sizeof(tmp.name), "%s", name);
			tmp.kind = SW_KIND_IKEV2;
			tmp.enabled = enabled;
			tmp.if_id = sw_next_if_id(c);
			sw_profile_compute_ifname(&tmp);
			snprintf(tmp.hostname, sizeof(tmp.hostname), "%s", hn);
			snprintf(tmp.username, sizeof(tmp.username), "%s", un);
			snprintf(tmp.remote_id, sizeof(tmp.remote_id), "%s",
				 rid ? rid : "same-as-hostname");
			snprintf(tmp.local_id, sizeof(tmp.local_id), "%s",
				 lid ? lid : "same-as-username");
			snprintf(tmp.cert_mode, sizeof(tmp.cert_mode),
				 "from-server");
			snprintf(tmp.proxy, sizeof(tmp.proxy), "off");
			if (have_pw)
				snprintf(tmp.secret_ref,
					 sizeof(tmp.secret_ref),
					 "secrets/%s/eap.txt", name);

			struct sw_profile *p = calloc(1, sizeof(*p));

			if (!p) {
				add_item(errors, name, "internal");
				continue;
			}
			*p = tmp;

			if (sw_uci_profile_add(c, p) != SW_UCI_OK) {
				free(p);
				add_item(errors, name, "E_DUPLICATE_NAME");
				continue;
			}
			sw_uci_profile_renumber(c);
			if (have_pw)
				json_object_array_add(restored,
					json_object_new_string(name));
			else
				add_item(attention, name,
					 "credentials-missing");
			continue;
		}

		add_item(errors, name, "unsupported kind");
	}
}

int sw_backup_import(struct sw_ctx *c, const char *json, char **report_out)
{
	if (!json || !*json || !report_out)
		return UBUS_STATUS_INVALID_ARGUMENT;

	struct json_object *root = json_tokener_parse(json);

	if (!root || json_object_get_type(root) != json_type_object) {
		json_object_put(root);
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	struct json_object *restored = json_object_new_array();
	struct json_object *attention = json_object_new_array();
	struct json_object *errors = json_object_new_array();
	struct json_object *svc = json_object_object_get(root, "service");
	struct json_object *steer = json_object_object_get(root, "steering");
	struct json_object *prof = json_object_object_get(root, "profiles");

	if (svc)
		do_service_prefs(c, svc, restored);
	if (steer)
		do_steering(c, steer, restored, attention);
	if (prof && json_object_get_type(prof) == json_type_array)
		do_profiles(c, prof, restored, attention, errors);

	/* desired-state metadata: last profile re-activation happens at   */
	/* boot via main.c resume (or the caller may sw_reload() now)      */
	if (svc) {
		struct json_object *v = json_object_object_get(svc,
				"active_profile");
		struct json_object *ks = json_object_object_get(svc,
				"kill_switch");

		if (ks)
			c->st.kill_switch = json_object_get_boolean(ks);
		if (v) {
			const char *ap = json_object_get_string(v);

			if (ap && ap[0] && sw_uci_profile_find(c, ap)) {
				snprintf(c->st.active_profile,
					 sizeof(c->st.active_profile), "%s",
					 ap);
				sw_warn("backup: '%s' is the desired active"
					"profile; reactivation occurs at "
					"boot or on demand", ap);
			}
		}
		sw_uci_persist_state(c);
	}

	struct json_object *report = json_object_new_object();

	json_object_object_add(report, "restored", restored);
	json_object_object_add(report, "needs_attention", attention);
	json_object_object_add(report, "errors", errors);
	const char *s = json_object_to_json_string(report);
	char *out = s ? strdup(s) : NULL;
	json_object_put(report);
	json_object_put(root);
	if (!out)
		return UBUS_STATUS_UNKNOWN_ERROR;
	*report_out = out;
	return UBUS_STATUS_OK;
}
