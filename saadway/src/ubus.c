/*
 * ubus.c — ubus object `saadway`, exact RPC surface of contracts/api.md
 * (T007 mgmt.*, T016 profile.*, T021 sec.*, T025 steer.*, T035 mon.*,
 * T039 maint.*)
 *
 * Constitution Principle V: exclusively authenticated API surface — the
 * uhttpd ACL (files/rpc-acl.json) + ubus sessions enforce access BEFORE
 * the daemon; every response redacts secret material (FR-021).
 *
 * Transport notes:
 *  - payloads accumulate in a libubox blob_buf; handlers reply with one
 *    message + a clear ubus status code.
 *  - Validation failures return 400-class codes with a `detail` string
 *    carrying the API's E_* reject codes (reply payload + status).
 *  - profile.activate (@SC-001 ≤10 s) and mon.health_test (@SC-003 ≤5 s)
 *    use deferred replies: both are async internally and must never block
 *    the poll loop (Principle II).
 */
#define _GNU_SOURCE
#include "saadway.h"

#include <json-c/json.h>
#include <libubox/blobmsg.h>
#include <stdlib.h>
#include <time.h>

static struct blob_buf bb;

struct htest {                    /* deferred state for mon.health_test */
	struct ubus_context *ctx;
	/* T061: upstream libubus API — the deferred handle is a plain
	 * struct ubus_request_data value; ubus_defer_request() is void. */
	struct ubus_request_data dreq;
	bool dreq_active;               /* deferred reply in flight         */
};

static struct htest g_htest;

/* ------------------------------------------------------------------ */
/* helpers                                                              */
/* ------------------------------------------------------------------ */

static const char *conn_str(int s)
{
	switch (s) {
	case SW_CONN_UP: return "up";
	case SW_CONN_CONNECTING: return "connecting";
	case SW_CONN_CONTAINED: return "contained";
	case SW_CONN_COOLDOWN: return "cooldown";
	default: return "down";
	}
}

/* desired-policy snapshot; exported: main.c reuses it on reload/boot */
void sw_policy_apply_now(struct sw_ctx *c)
{
	if (c->st.conn_status == SW_CONN_UP)
		sw_policy_apply(c, c->st.kill_switch, c->tun_if, true);
	else if (c->fo.cooldown_active && c->st.kill_switch)
		sw_policy_apply(c, true, NULL, false);
	else
		sw_policy_apply_direct(c);
}

static void add_profile_obj(struct sw_profile *p, bool active,
			    const char *status)
{
	void *t = blobmsg_open_table(&bb, NULL);

	blobmsg_add_string(&bb, "name", p->name);
	blobmsg_add_string(&bb, "kind",
			   p->kind == SW_KIND_OPENVPN ? "openvpn" : "ikev2");
	blobmsg_add_bool(&bb, "enabled", p->enabled);
	blobmsg_add_u32(&bb, "order", (uint32_t)p->order);
	blobmsg_add_string(&bb, "secret_ref", p->secret_ref);
	blobmsg_add_bool(&bb, "active", active);
	blobmsg_add_string(&bb, "status", status);
	blobmsg_close_table(&bb, t);
}

static const char *profile_status(struct sw_ctx *c, struct sw_profile *p)
{
	if (c->active == p)
		return "active";
	if (!p->enabled)
		return "disabled";
	return "ready";
}

/* error reply: detail payload + 400-class code (api.md error envelope) */
static int reply_error(struct ubus_context *ctx, struct ubus_request_data *req,
		       int code, const char *detail)
{
	blob_buf_init(&bb, 0);
	blobmsg_add_string(&bb, "detail", detail ? detail : "internal error");
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	return code;                      /* ubus transport status code */
}

static void arg_missing(struct ubus_object *obj)
{
	(void)obj;
}

/* numeric argument (T062): ubus delivers JSON numbers as
 * BLOBMSG_TYPE_INT32, but a client may send a numeric string — accept
 * both, and never read a value out of a wrong-typed attribute (the
 * methods carry no upstream policy table, so ubus performs no type
 * validation for us). */
static bool arg_int(struct blob_attr *msg, const char *name, long *out)
{
	struct blob_attr *a = blobmsg_find_name_typed(msg, name,
						      BLOBMSG_TYPE_INT32);
	const char *s;
	char *end = NULL;
	long v;

	if (a) {
		*out = (long)blobmsg_get_u32(a);
		return true;
	}
	a = blobmsg_find_name_typed(msg, name, BLOBMSG_TYPE_STRING);
	if (!a)
		return false;
	s = blobmsg_get_string(a);
	if (!s)
		return false;
	v = strtol(s, &end, 10);
	if (!end || *end != '\0')
		return false;
	*out = v;
	return true;
}

/* json string array → blobmsg string array (backup.import report) */
static void json_array_into(struct blob_buf *b, struct json_object *arr,
			    const char *name, const char *k1, const char *k2)
{
	void *a = blobmsg_open_array(b, name);
	int n = arr ? json_object_array_length(arr) : 0;

	for (int i = 0; i < n; i++) {
		struct json_object *e = json_object_array_get_idx(arr, i);

		if (json_object_get_type(e) == json_type_object) {
			void *t = blobmsg_open_table(b, NULL);
			struct json_object *o1 = json_object_object_get(e, k1);
			struct json_object *o2 = json_object_object_get(e, k2);

			if (o1)
				blobmsg_add_string(b, k1,
					json_object_get_string(o1));
			if (o2)
				blobmsg_add_string(b, k2,
					json_object_get_string(o2));
			blobmsg_close_table(b, t);
		} else {
			blobmsg_add_string(b, NULL, json_object_get_string(e));
		}
	}
	blobmsg_close_array(b, a);
}

/* ------------------------------------------------------------------ */
/* mgmt.* (T007)                                                        */
/* ------------------------------------------------------------------ */

static int h_mgmt_status(struct ubus_context *ctx, struct ubus_object *obj,
			 struct ubus_request_data *req, const char *method,
			 struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;

	(void)obj;
	(void)method;
	arg_missing(obj);
	(void)msg;
	blob_buf_init(&bb, 0);
	blobmsg_add_bool(&bb, "enabled", c->st.enabled);
	blobmsg_add_bool(&bb, "running", c->st.enabled);
	blobmsg_add_string(&bb, "connection_status",
			   conn_str(c->st.conn_status));
	blobmsg_add_string(&bb, "active_profile", c->st.active_profile);
	blobmsg_add_string(&bb, "current_if", c->tun_if[0] ? c->tun_if : "-");
	blobmsg_add_u32(&bb, "uptime_s", (uint32_t)time(NULL));
	/* T053 (FR-020): expose the configured session TTL; the SPA requests
	 * this value at session.login so the UCI option actually governs the
	 * inactivity expiry (data-model §DashboardSession). */
	blobmsg_add_u32(&bb, "session_timeout",
			(uint32_t)c->st.session_timeout);
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	return UBUS_STATUS_OK;
}

static int h_mgmt_set(struct ubus_context *ctx, struct ubus_object *obj,
		      struct ubus_request_data *req, const char *method,
		      struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;
	struct blob_attr *v;

	(void)obj;
	(void)method;
	arg_missing(obj);
	v = blobmsg_find_name_typed(msg, "enabled", BLOBMSG_TYPE_BOOL);
	if (!v)
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX");
	bool enabled = blobmsg_get_bool(v);

	c->st.enabled = enabled;
	sw_uci_persist_state(c);
	if (!enabled) {
		sw_tunnel_cancel_children(c);
		sw_deactivate_all(c, true);
		sw_failover_reset(c);
		sw_dns_stop();
		sw_policy_apply_now(c);   /* direct or contained baseline */
	}
	blob_buf_init(&bb, 0);
	blobmsg_add_bool(&bb, "applied", true);
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	return UBUS_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* profile.* (T016)                                                     */
/* ------------------------------------------------------------------ */

static int h_profile_list(struct ubus_context *ctx, struct ubus_object *obj,
			  struct ubus_request_data *req, const char *method,
			  struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;

	(void)obj;
	(void)method;
	arg_missing(obj);
	(void)msg;
	blob_buf_init(&bb, 0);
	void *arr = blobmsg_open_array(&bb, "profiles");
	for (struct sw_profile *p = c->st.profiles; p; p = p->next)
		add_profile_obj(p, c->active == p, profile_status(c, p));
	blobmsg_close_array(&bb, arr);
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	return UBUS_STATUS_OK;
}

static int h_profile_import_openvpn(struct ubus_context *ctx,
				    struct ubus_object *obj,
				    struct ubus_request_data *req,
				    const char *method,
				    struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;
	struct blob_attr *nv, *ov, *cv;
	const char *name, *b64;
	char detail[128] = "";
	char *warnings = NULL;

	(void)obj;
	(void)method;
	arg_missing(obj);
	if (!blobmsg_find_name_typed(msg, "name", BLOBMSG_TYPE_STRING) ||
	    !blobmsg_find_name_typed(msg, "ovpn_b64", BLOBMSG_TYPE_STRING))
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX");
	name = blobmsg_get_string(nv);
	b64 = blobmsg_get_string(ov);
	const char *user = NULL, *pass = NULL;

	if (msg && (cv = blobmsg_find_name_typed(msg, "credentials",
						 BLOBMSG_TYPE_TABLE))) {
		struct blob_attr *u = blobmsg_find_name_typed(cv, "username",
							     BLOBMSG_TYPE_STRING);
		struct blob_attr *pw = blobmsg_find_name_typed(cv, "password",
							      BLOBMSG_TYPE_STRING);

		user = u ? blobmsg_get_string(u) : NULL;
		pass = pw ? blobmsg_get_string(pw) : NULL;
	}
	int rc = sw_ovpn_import(c, name, b64, user, pass, detail,
				sizeof(detail), &warnings);

	if (rc != UBUS_STATUS_OK) {
		free(warnings);
		return reply_error(ctx, req, rc, detail);
	}

	struct sw_profile *p = sw_uci_profile_find(c, name);

	if (!p) {
		free(warnings);
		return reply_error(ctx, req, UBUS_STATUS_UNKNOWN_ERROR,
				   "internal: profile vanished");
	}
	blob_buf_init(&bb, 0);
	void *t = blobmsg_open_table(&bb, "profile");

	add_profile_obj(p, false, "ready");
	blobmsg_close_table(&bb, t);
	if (warnings && warnings[0])
		blobmsg_add_string(&bb, "warnings", warnings);
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	free(warnings);
	return UBUS_STATUS_OK;
}

/* proposal strings are rendered into swanctl.conf verbatim: charset cap */
static bool proposal_valid(const char *s)
{
	for (const char *q = s; q && *q; q++)
		if (!(*q >= 'a' && *q <= 'z') && *q != '-' && *q != '_')
			return false;
	return true;
}

static int h_profile_create_ikev2(struct ubus_context *ctx,
				  struct ubus_object *obj,
				  struct ubus_request_data *req,
				  const char *method,
				  struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;
	struct blob_attr *nv = NULL, *hv = NULL, *uv = NULL, *pv = NULL;
	struct blob_attr *rv, *lv, *cmv, *piv, *pev;

	(void)obj;
	(void)method;
	arg_missing(obj);
	if (!blobmsg_find_name_typed(msg, "name", BLOBMSG_TYPE_STRING) ||
	    !blobmsg_find_name_typed(msg, "hostname", BLOBMSG_TYPE_STRING) ||
	    !blobmsg_find_name_typed(msg, "username", BLOBMSG_TYPE_STRING) ||
	    !blobmsg_find_name_typed(msg, "password", BLOBMSG_TYPE_STRING))
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX: name/hostname/username/"
				   "password required");
	const char *name = blobmsg_get_string(nv);
	const char *host = blobmsg_get_string(hv);
	const char *user = blobmsg_get_string(uv);
	const char *pass = blobmsg_get_string(pv);
	rv = msg ? blobmsg_find_name_typed(msg, "remote_id", BLOBMSG_TYPE_STRING) : NULL;
	lv = msg ? blobmsg_find_name_typed(msg, "local_id", BLOBMSG_TYPE_STRING) : NULL;
	cmv = msg ? blobmsg_find_name_typed(msg, "cert_mode", BLOBMSG_TYPE_STRING) : NULL;
	piv = msg ? blobmsg_find_name_typed(msg, "proposal_ike", BLOBMSG_TYPE_STRING) : NULL;
	pev = msg ? blobmsg_find_name_typed(msg, "proposal_esp", BLOBMSG_TYPE_STRING) : NULL;
	const char *rid = rv ? blobmsg_get_string(rv) : "same-as-hostname";
	const char *lid = lv ? blobmsg_get_string(lv) : "same-as-username";
	const char *cert = cmv ? blobmsg_get_string(cmv) : "from-server";
	const char *pik = piv ? blobmsg_get_string(piv) :
				"aes256gcm16-prfsha256-ecp384-ecp521";
	const char *pes = pev ? blobmsg_get_string(pev) : "aes256gcm16";

	if (!sw_valid_name(name))
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX: invalid name");
	if (sw_uci_name_taken(c, name))
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_DUPLICATE_NAME");
	if (!sw_valid_domain(host))
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX: invalid hostname");
	if (!proposal_valid(pik))
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX: proposal_ike");
	if (!proposal_valid(pes))
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX: proposal_esp");
	if (*cert && strcmp(cert, "from-server"))
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX: cert_mode (spec v1 "
				   "from-server only)");

	if (sw_secret_write(name, "eap.txt", pass) != 0)
		return reply_error(ctx, req, UBUS_STATUS_UNKNOWN_ERROR,
				   "E_SECRET_WRITE_FAILED");

	struct sw_profile *p = calloc(1, sizeof(*p));

	if (!p)
		return reply_error(ctx, req, UBUS_STATUS_UNKNOWN_ERROR,
				   "internal");
	snprintf(p->name, sizeof(p->name), "%s", name);
	p->kind = SW_KIND_IKEV2;
	p->enabled = true;
	p->if_id = sw_next_if_id(c);
	sw_profile_compute_ifname(p);
	snprintf(p->hostname, sizeof(p->hostname), "%s", host);
	snprintf(p->username, sizeof(p->username), "%s", user);
	snprintf(p->remote_id, sizeof(p->remote_id), "%s", rid);
	snprintf(p->local_id, sizeof(p->local_id), "%s", lid);
	snprintf(p->cert_mode, sizeof(p->cert_mode), "%s",
		 cert[0] ? cert : "from-server");
	snprintf(p->proxy, sizeof(p->proxy), "off");
	snprintf(p->proposal_ike, sizeof(p->proposal_ike), "%s", pik);
	snprintf(p->proposal_esp, sizeof(p->proposal_esp), "%s", pes);
	snprintf(p->secret_ref, sizeof(p->secret_ref),
		 "secrets/%s/eap.txt", name);

	if (sw_uci_profile_add(c, p) != SW_UCI_OK) {
		free(p);
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_DUPLICATE_NAME");
	}
	blob_buf_init(&bb, 0);
	void *t = blobmsg_open_table(&bb, "profile");

	add_profile_obj(p, false, "ready");
	blobmsg_close_table(&bb, t);
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	return UBUS_STATUS_OK;
}

static int h_profile_update(struct ubus_context *ctx, struct ubus_object *obj,
			    struct ubus_request_data *req, const char *method,
			    struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;
	struct blob_attr *nv, *pv;

	(void)obj;
	(void)method;
	arg_missing(obj);
		nv = blobmsg_find_name_typed(msg, "name", BLOBMSG_TYPE_STRING);
	pv = blobmsg_find_name_typed(msg, "patch", BLOBMSG_TYPE_TABLE);
	if (!nv || !pv)
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX");
	const char *name = blobmsg_get_string(nv);
	struct sw_profile *p = sw_uci_profile_find(c, name);

	if (!p)
		return reply_error(ctx, req, UBUS_STATUS_NOT_FOUND,
				   "unknown profile");
	bool was_active = (c->active == p);
	struct blob_attr *v;

	if ((v = blobmsg_find_name_typed(pv, "enabled", BLOBMSG_TYPE_BOOL)))
		p->enabled = blobmsg_get_bool(v);
	if ((v = blobmsg_find_name_typed(pv, "hostname", BLOBMSG_TYPE_STRING)) &&
	    p->kind == SW_KIND_IKEV2) {
		const char *s = blobmsg_get_string(v);

		if (!sw_valid_domain(s))
			return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
					   "E_BAD_SYNTAX: hostname");
		snprintf(p->hostname, sizeof(p->hostname), "%s", s);
	}
	if ((v = blobmsg_find_name_typed(pv, "username", BLOBMSG_TYPE_STRING)) &&
	    p->kind == SW_KIND_IKEV2)
		snprintf(p->username, sizeof(p->username), "%s",
			 blobmsg_get_string(v));
	if ((v = blobmsg_find_name_typed(pv, "remote_id", BLOBMSG_TYPE_STRING)) &&
	    p->kind == SW_KIND_IKEV2)
		snprintf(p->remote_id, sizeof(p->remote_id), "%s",
			 blobmsg_get_string(v));
	if ((v = blobmsg_find_name_typed(pv, "local_id", BLOBMSG_TYPE_STRING)) &&
	    p->kind == SW_KIND_IKEV2)
		snprintf(p->local_id, sizeof(p->local_id), "%s",
			 blobmsg_get_string(v));
	if ((v = blobmsg_find_name_typed(pv, "cert_mode", BLOBMSG_TYPE_STRING)) &&
	    p->kind == SW_KIND_IKEV2) {
		const char *s = blobmsg_get_string(v);

		if (strcmp(s, "from-server"))
			return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
					   "E_BAD_SYNTAX: cert_mode");
		snprintf(p->cert_mode, sizeof(p->cert_mode), "%s", s);
	}
	if ((v = blobmsg_find_name_typed(pv, "proposal_ike", BLOBMSG_TYPE_STRING)) &&
	    p->kind == SW_KIND_IKEV2) {
		const char *s = blobmsg_get_string(v);

		if (!proposal_valid(s))
			return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
					   "E_BAD_SYNTAX: proposal_ike");
		snprintf(p->proposal_ike, sizeof(p->proposal_ike), "%s", s);
	}
	if ((v = blobmsg_find_name_typed(pv, "proposal_esp", BLOBMSG_TYPE_STRING)) &&
	    p->kind == SW_KIND_IKEV2) {
		const char *s = blobmsg_get_string(v);

		if (!proposal_valid(s))
			return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
					   "E_BAD_SYNTAX: proposal_esp");
		snprintf(p->proposal_esp, sizeof(p->proposal_esp), "%s", s);
	}
	/* secrets replaced, never echoed (FR-021) */
	if ((v = blobmsg_find_name_typed(pv, "password", BLOBMSG_TYPE_STRING))) {
		const char *s = blobmsg_get_string(v);

		if (p->kind == SW_KIND_IKEV2 &&
		    sw_secret_write(name, "eap.txt", s) != 0)
			return reply_error(ctx, req, UBUS_STATUS_UNKNOWN_ERROR,
					   "E_SECRET_WRITE_FAILED");
		if (p->kind == SW_KIND_OPENVPN) {
			struct blob_attr *u = blobmsg_find_name_typed(pv, "username", BLOBMSG_TYPE_STRING);
			const char *uu = u ? blobmsg_get_string(u) : NULL;
			char auth[640];

			if (!uu || strlen(uu) + strlen(s) + 3 >= sizeof(auth))
				return reply_error(ctx, req,
					UBUS_STATUS_INVALID_ARGUMENT,
					"E_BAD_SYNTAX: credentials");
			snprintf(auth, sizeof(auth), "%s\n%s\n", uu, s);
			if (sw_secret_write(name, "auth.txt", auth) != 0)
				return reply_error(ctx, req,
					UBUS_STATUS_UNKNOWN_ERROR,
					"E_SECRET_WRITE_FAILED");
		}
	}

	if (sw_uci_profile_update(c, p) != SW_UCI_OK)
		return reply_error(ctx, req, UBUS_STATUS_UNKNOWN_ERROR,
				   "internal: UCI persist failed");

	/* an active tunnel must pick the patch up: switch to the same
	 * profile (activation engine tears down + respawns atomically)  */
	if (was_active) {
		sw_policy_apply_now(c);
		if (sw_activate_profile(c, p, NULL) != 0)
			return reply_error(ctx, req, UBUS_STATUS_UNKNOWN_ERROR,
					   "activation failed");
	}
	blob_buf_init(&bb, 0);
	void *t = blobmsg_open_table(&bb, "profile");

	add_profile_obj(p, was_active, profile_status(c, p));
	blobmsg_close_table(&bb, t);
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	return UBUS_STATUS_OK;
}

static int h_profile_delete(struct ubus_context *ctx, struct ubus_object *obj,
			    struct ubus_request_data *req, const char *method,
			    struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;
	struct blob_attr *nv, *fv;

	(void)obj;
	(void)method;
	arg_missing(obj);
		nv = blobmsg_find_name_typed(msg, "name", BLOBMSG_TYPE_STRING);
	if (!nv)
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX");
	const char *name = blobmsg_get_string(nv);
	struct sw_profile *p = sw_uci_profile_find(c, name);

	if (!p)
		return reply_error(ctx, req, UBUS_STATUS_NOT_FOUND,
				   "unknown profile");
	fv = msg ? blobmsg_find_name_typed(msg, "force", BLOBMSG_TYPE_BOOL) : NULL;
	if (c->active == p && !(fv && blobmsg_get_bool(fv)))
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_ACTIVE: force=true required");

	if (c->active == p) {
		/* disconnect FIRST: containment lands on the corpse */
		sw_tunnel_cancel_children(c);
		if (p->kind == SW_KIND_OPENVPN)
			sw_ovpn_stop(c, p, true);
		else
			sw_ikev2_stop(c, p, true);
		if (c->st.kill_switch)
			sw_policy_apply(c, true, NULL, false);
		else
			sw_policy_apply_direct(c);
		c->st.conn_status = c->st.kill_switch ? SW_CONN_CONTAINED :
							SW_CONN_DOWN;
		c->active = NULL;
		c->tun_if[0] = 0;
		c->st.active_profile[0] = 0;
		sw_uci_persist_state(c);
	}
	sw_uci_profile_delete(c, name);
	blob_buf_init(&bb, 0);
	blobmsg_add_bool(&bb, "deleted", true);
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	return UBUS_STATUS_OK;
}

static int h_profile_activate(struct ubus_context *ctx,
			      struct ubus_object *obj,
			      struct ubus_request_data *req,
			      const char *method, struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;
	struct blob_attr *nv;

	(void)obj;
	(void)method;
	arg_missing(obj);
		nv = blobmsg_find_name_typed(msg, "name", BLOBMSG_TYPE_STRING);
	if (!nv)
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX");
	struct sw_profile *p = sw_uci_profile_find(c, blobmsg_get_string(nv));

	if (!p)
		return reply_error(ctx, req, UBUS_STATUS_NOT_FOUND,
				   "unknown profile");
	/* deferred reply owned by the activation engine in main.c        */
	if (sw_activate_profile(c, p, req) != 0)
		return reply_error(ctx, req, UBUS_STATUS_UNKNOWN_ERROR,
				   "activation failed to start");
	return UBUS_STATUS_OK;    /* reply arrives via the deferred path   */
}

static int h_profile_deactivate(struct ubus_context *ctx,
				struct ubus_object *obj,
				struct ubus_request_data *req,
				const char *method, struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;

	(void)obj;
	(void)method;
	arg_missing(obj);
	(void)msg;
	sw_deactivate_all(c, true);
	blob_buf_init(&bb, 0);
	blobmsg_add_bool(&bb, "deactivated", true);
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	return UBUS_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* steer.* (T025)                                                       */
/* ------------------------------------------------------------------ */

static int h_steer_list(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;

	(void)obj;
	(void)method;
	arg_missing(obj);
	(void)msg;
	blob_buf_init(&bb, 0);
	void *dom = blobmsg_open_array(&bb, "domains");
	for (struct sw_domain_rule *d = c->st.domains; d; d = d->next)
		blobmsg_add_string(&bb, NULL, d->domain);
	blobmsg_close_array(&bb, dom);
	void *dev = blobmsg_open_array(&bb, "devices");
	for (struct sw_device_rule *r = c->st.devices; r; r = r->next) {
		void *t = blobmsg_open_table(&bb, NULL);

		blobmsg_add_string(&bb, "match_type",
				   r->match_type == SW_MTYPE_IP ? "ip" :
								  "mac");
		blobmsg_add_string(&bb, "match_value", r->match_value);
		blobmsg_add_string(&bb, "policy",
				   r->ptype == SW_POLICY_DIRECT ?
				   "direct" : "tunneled");
		blobmsg_close_table(&bb, t);
	}
	blobmsg_close_array(&bb, dev);
	blobmsg_add_bool(&bb, "iprange_bypass", c->st.iprange_bypass);
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	return UBUS_STATUS_OK;
}

static int h_steer_domain_add(struct ubus_context *ctx,
			      struct ubus_object *obj,
			      struct ubus_request_data *req,
			      const char *method, struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;
	struct blob_attr *v;

	(void)obj;
	(void)method;
	arg_missing(obj);
		v = blobmsg_find_name_typed(msg, "domain", BLOBMSG_TYPE_STRING);
	if (!v)
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX");
	const char *domain = blobmsg_get_string(v);

	if (!sw_valid_domain(domain))
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX: invalid domain");
	int rc = sw_uci_domain_add(c, domain);

	if (rc == SW_UCI_E_DUPLICATE_DOMAIN)
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_DUPLICATE");
	if (rc != SW_UCI_OK)
		return reply_error(ctx, req, UBUS_STATUS_UNKNOWN_ERROR,
				   "internal");
	/* steering value lands atomically, tunnel stays up (US3)         */
	sw_policy_apply_now(c);
	blob_buf_init(&bb, 0);
	blobmsg_add_bool(&bb, "added", true);
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	return UBUS_STATUS_OK;
}

static int h_steer_domain_remove(struct ubus_context *ctx,
				 struct ubus_object *obj,
				 struct ubus_request_data *req,
				 const char *method, struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;
	struct blob_attr *v;

	(void)obj;
	(void)method;
	arg_missing(obj);
		v = blobmsg_find_name_typed(msg, "domain", BLOBMSG_TYPE_STRING);
	if (!v)
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX");
	sw_uci_domain_remove(c, blobmsg_get_string(v));
	sw_policy_apply_now(c);
	blob_buf_init(&bb, 0);
	blobmsg_add_bool(&bb, "removed", true);
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	return UBUS_STATUS_OK;
}

static int h_steer_device_set(struct ubus_context *ctx,
			      struct ubus_object *obj,
			      struct ubus_request_data *req,
			      const char *method, struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;
	struct blob_attr *mv, *vv, *pv;

	(void)obj;
	(void)method;
	arg_missing(obj);
	mv = blobmsg_find_name_typed(msg, "match_type", BLOBMSG_TYPE_STRING);
	vv = blobmsg_find_name_typed(msg, "match_value", BLOBMSG_TYPE_STRING);
	if (!mv || !vv)
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX");
	const char *mt = blobmsg_get_string(mv);
	const char *val = blobmsg_get_string(vv);
	pv = msg ? blobmsg_find_name_typed(msg, "policy", BLOBMSG_TYPE_STRING) : NULL;
	const char *pol = pv ? blobmsg_get_string(pv) : "tunneled";
	bool is_ip = !strcmp(mt, "ip");

	if (strcmp(pol, "direct") && strcmp(pol, "tunneled"))
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX: policy");
	int ptype = strcmp(pol, "direct") ? SW_POLICY_TUNNELED :
					   SW_POLICY_DIRECT;

	if (is_ip ? !sw_valid_cidr4(val, true) : !sw_valid_mac(val))
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX: match_value");
	sw_uci_device_set(c, is_ip ? SW_MTYPE_IP : SW_MTYPE_MAC, val, ptype);
	sw_policy_apply_now(c);
	blob_buf_init(&bb, 0);
	blobmsg_add_bool(&bb, "set", true);
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	return UBUS_STATUS_OK;
}

static int h_steer_device_remove(struct ubus_context *ctx,
				 struct ubus_object *obj,
				 struct ubus_request_data *req,
				 const char *method, struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;
	struct blob_attr *mv, *vv;

	(void)obj;
	(void)method;
	arg_missing(obj);
	mv = blobmsg_find_name_typed(msg, "match_type", BLOBMSG_TYPE_STRING);
	vv = blobmsg_find_name_typed(msg, "match_value", BLOBMSG_TYPE_STRING);
	if (!mv || !vv)
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX");
	const char *mt = blobmsg_get_string(mv);
	const char *val = blobmsg_get_string(vv);

	if (strcmp(mt, "ip") && strcmp(mt, "mac"))
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX: match_type");
	sw_uci_device_remove(c, strcmp(mt, "ip") ? SW_MTYPE_MAC :
			     SW_MTYPE_IP, val);
	sw_policy_apply_now(c);
	blob_buf_init(&bb, 0);
	blobmsg_add_bool(&bb, "removed", true);
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	return UBUS_STATUS_OK;
}

static int h_steer_iprange_set(struct ubus_context *ctx,
			       struct ubus_object *obj,
			       struct ubus_request_data *req,
			       const char *method, struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;
	struct blob_attr *v;

	(void)obj;
	(void)method;
	arg_missing(obj);
		v = blobmsg_find_name_typed(msg, "enabled", BLOBMSG_TYPE_BOOL);
	if (!v)
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX");
	c->st.iprange_bypass = blobmsg_get_bool(v);
	sw_uci_persist_state(c);
	sw_policy_apply_now(c);
	blob_buf_init(&bb, 0);
	blobmsg_add_bool(&bb, "applied", true);
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	return UBUS_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* sec.* (T021)                                                         */
/* ------------------------------------------------------------------ */

static bool contained_now(struct sw_ctx *c)
{
	return c->st.conn_status == SW_CONN_CONTAINED ||
	       (c->st.kill_switch && c->st.conn_status != SW_CONN_UP);
}

static int h_sec_status(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method,
			struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;

	(void)obj;
	(void)method;
	arg_missing(obj);
	(void)msg;
	blob_buf_init(&bb, 0);
	blobmsg_add_bool(&bb, "kill_switch", c->st.kill_switch);
	blobmsg_add_bool(&bb, "dns_forced", sw_dns_running());
	blobmsg_add_bool(&bb, "contained", contained_now(c));
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	return UBUS_STATUS_OK;
}

static int h_sec_kill_switch_set(struct ubus_context *ctx,
				 struct ubus_object *obj,
				 struct ubus_request_data *req,
				 const char *method, struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;
	struct blob_attr *v;

	(void)obj;
	(void)method;
	arg_missing(obj);
		v = blobmsg_find_name_typed(msg, "enabled", BLOBMSG_TYPE_BOOL);
	if (!v)
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX");
	c->st.kill_switch = blobmsg_get_bool(v);   /* persisted (FR-012) */
	sw_uci_persist_state(c);
	sw_policy_apply_now(c);
	blob_buf_init(&bb, 0);
	blobmsg_add_bool(&bb, "applied", true);
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	return UBUS_STATUS_OK;
}

static int h_sec_dns_status(struct ubus_context *ctx, struct ubus_object *obj,
			    struct ubus_request_data *req,
			    const char *method, struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;

	(void)obj;
	(void)method;
	arg_missing(obj);
	(void)msg;
	blob_buf_init(&bb, 0);
	blobmsg_add_string(&bb, "resolver_if",
			   c->tun_if[0] ? c->tun_if : "-");
	blobmsg_add_string(&bb, "leak_risk",
			   (c->st.conn_status == SW_CONN_UP &&
			    sw_dns_running()) ? "low" : "unknown");
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	return UBUS_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* mon.* (T035)                                                         */
/* ------------------------------------------------------------------ */

static int h_mon_stats(struct ubus_context *ctx, struct ubus_object *obj,
		       struct ubus_request_data *req, const char *method,
		       struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;

	(void)obj;
	(void)method;
	arg_missing(obj);
	(void)msg;
	blob_buf_init(&bb, 0);
	blobmsg_add_u32(&bb, "ts", (uint32_t)time(NULL));
	blobmsg_add_u32(&bb, "rx_bps", (uint32_t)c->ctr.rx_bps);
	blobmsg_add_u32(&bb, "tx_bps", (uint32_t)c->ctr.tx_bps);
	blobmsg_add_string(&bb, "iface", c->tun_if[0] ? c->tun_if : "-");
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	return UBUS_STATUS_OK;
}

/* deferred machinery for mon.health_test (SC-003 ≤5 s) */
static void htest_reply(struct sw_ctx *c, bool done)
{
	(void)done;
	blob_buf_init(&bb, 0);
	{
		void *t = blobmsg_open_table(&bb, "ping");

		blobmsg_add_string(&bb, "target", c->health.ping_target);
		if (c->health.ping_ok == 1)
			blobmsg_add_u32(&bb, "latency_ms",
					(uint32_t)c->health.ping_latency_ms);
		else
			blobmsg_add_string(&bb, "latency_ms", "timeout");
		blobmsg_close_table(&bb, t);
	}
	{
		void *t = blobmsg_open_table(&bb, "http");

		if (c->health.http.code > 0)
			blobmsg_add_u32(&bb, "code",
					(uint32_t)c->health.http.code);
		else
			blobmsg_add_string(&bb, "code", "timeout");
		blobmsg_close_table(&bb, t);
	}
	/* the deferred request carries its own target identity */
	ubus_send_reply(g_htest.ctx, &g_htest.dreq, bb.head);
	blob_buf_free(&bb);
	ubus_complete_deferred_request(g_htest.ctx, &g_htest.dreq,
				       UBUS_STATUS_OK);
	g_htest.dreq_active = false;
}

static int h_mon_health_test(struct ubus_context *ctx,
			     struct ubus_object *obj,
			     struct ubus_request_data *req,
			     const char *method, struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;

	(void)obj;
	(void)method;
	arg_missing(obj);
	(void)msg;
	if (g_htest.dreq_active)
		return reply_error(ctx, req, UBUS_STATUS_UNKNOWN_ERROR,
				   "health test already running");
	/* T061: defer BEFORE starting the probe — the upstream deferral marks
	 * the request deferred, so the asynchronous reply can never race
	 * ahead of the deferral itself. */
	ubus_defer_request(ctx, req, &g_htest.dreq);
	g_htest.dreq_active = true;
	g_htest.ctx = ctx;
	if (sw_health_run(c, htest_reply) != 0) {
		sw_health_abort();
		g_htest.dreq_active = false;
		/* the request is already deferred: report the failure through
		 * the deferred completion, not a plain reply */
		ubus_complete_deferred_request(ctx, &g_htest.dreq,
					       UBUS_STATUS_UNKNOWN_ERROR);
		return UBUS_STATUS_OK;
	}
	return UBUS_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* mon.logs — cursor tail; scrubbing happens inside log.c ring          */
/* ------------------------------------------------------------------ */

static int h_mon_logs(struct ubus_context *ctx, struct ubus_object *obj,
		      struct ubus_request_data *req, const char *method,
		      struct blob_attr *msg)
{
	struct blob_attr *cv, *lv;
	long cursor = 0;
	int max = 20;
	struct sw_log_entry entries[64];
	long next = 0;

	(void)obj;
	(void)method;
	arg_missing(obj);
	{
		long lmax = 0;

		arg_int(msg, "cursor", &cursor);
		if (arg_int(msg, "lines", &lmax))
			max = (int)lmax;
	}
	if (max <= 0)
		max = 20;
	if (max > 64)
		max = 64;
	int n = sw_log_snapshot(entries, max, cursor, &next);

	blob_buf_init(&bb, 0);
	void *arr = blobmsg_open_array(&bb, "entries");

	for (int i = 0; i < n; i++) {
		void *t = blobmsg_open_table(&bb, NULL);

		blobmsg_add_u32(&bb, "ts", (uint32_t)entries[i].ts);
		blobmsg_add_u32(&bb, "level", (uint32_t)entries[i].prio);
		blobmsg_add_string(&bb, "msg", entries[i].msg);
		blobmsg_close_table(&bb, t);
	}
	blobmsg_close_array(&bb, arr);
	blobmsg_add_u32(&bb, "next_cursor", (uint32_t)next);
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	return UBUS_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* maint.* (T039)                                                       */
/* ------------------------------------------------------------------ */

static int h_maint_backup_export(struct ubus_context *ctx,
				 struct ubus_object *obj,
				 struct ubus_request_data *req,
				 const char *method, struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;
	struct blob_attr *icv = msg ? blobmsg_find_name_typed(msg,
						"include_credentials",
						BLOBMSG_TYPE_BOOL) : NULL;
	bool incl = icv ? blobmsg_get_bool(icv) : false;
	char *json = NULL;

	(void)obj;
	(void)method;
	arg_missing(obj);
	int rc = sw_backup_export(c, incl, &json);

	if (rc != UBUS_STATUS_OK)
		return reply_error(ctx, req, rc, "E_BAD_SYNTAX: export");
	char *b64 = NULL;

	if (sw_util_b64_encode((const unsigned char *)json, strlen(json),
			       &b64) != 0) {
		free(json);
		return reply_error(ctx, req, UBUS_STATUS_UNKNOWN_ERROR,
				   "internal-b64");
	}
	free(json);
	blob_buf_init(&bb, 0);
	blobmsg_add_string(&bb, "json_b64", b64);
	blobmsg_add_u32(&bb, "schema_version", 1);
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	free(b64);
	return UBUS_STATUS_OK;
}

static int h_maint_backup_import(struct ubus_context *ctx,
				 struct ubus_object *obj,
				 struct ubus_request_data *req,
				 const char *method, struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;
	struct blob_attr *jv;

	(void)obj;
	(void)method;
	arg_missing(obj);
		jv = blobmsg_find_name_typed(msg, "json_b64", BLOBMSG_TYPE_STRING);
	if (!jv)
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX");
	unsigned char *json = NULL;
	size_t jlen = 0;
	int rc = sw_util_b64_decode(blobmsg_get_string(jv), &json, &jlen);

	if (rc != 0 || !json || !jlen || jlen > (4u << 20))
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX: not valid base64/json");
	/* json-c strings must be NUL-terminated */
	char *jtext = malloc(jlen + 1);

	if (!jtext) {
		free(json);
		return reply_error(ctx, req, UBUS_STATUS_UNKNOWN_ERROR,
				   "internal");
	}
	memcpy(jtext, json, jlen);
	jtext[jlen] = 0;
	free(json);

	char *report = NULL;
	rc = sw_backup_import(c, jtext, &report);
	free(jtext);
	if (rc != UBUS_STATUS_OK)
		return reply_error(ctx, req, rc, report ? report :
				   "E_BAD_SYNTAX: import");

	/* faithful structure per api.md: rebuilt arrays of strings/objects */
	struct json_object *r = json_tokener_parse(report);

	free(report);
	blob_buf_init(&bb, 0);
	if (r) {
		struct json_object *a1 = json_object_object_get(r, "restored");
		struct json_object *a2 = json_object_object_get(r,
				"needs_attention");
		struct json_object *a3 = json_object_object_get(r, "errors");

		json_array_into(&bb, a1, "restored", NULL, NULL);
		json_array_into(&bb, a2, "needs_attention", "item", "reason");
		json_array_into(&bb, a3, "errors", "item", "reason");
	}
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	if (r)
		json_object_put(r);

	/* successful structural import → re-apply current desired policy  */
	sw_policy_apply_now(c);
	return UBUS_STATUS_OK;
}

static int h_maint_cooldown_set(struct ubus_context *ctx,
				struct ubus_object *obj,
				struct ubus_request_data *req,
				const char *method, struct blob_attr *msg)
{
	struct sw_ctx *c = global_ctx;
	long sec = 0, att = 0;

	(void)obj;
	(void)method;
	arg_missing(obj);
	if (!arg_int(msg, "seconds", &sec))
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX");

	if (sec < 30 || sec > 86400)
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX: seconds (30..86400)");
	if (!arg_int(msg, "attempts", &att))
		att = c->st.failover_attempts;

	if (att < 1 || att > 10)
		return reply_error(ctx, req, UBUS_STATUS_INVALID_ARGUMENT,
				   "E_BAD_SYNTAX: attempts (1..10)");
	c->st.failover_cooldown = sec;
	c->st.failover_attempts = att;
	sw_uci_persist_tunables(c);
	blob_buf_init(&bb, 0);
	blobmsg_add_bool(&bb, "applied", true);
	ubus_send_reply(ctx, req, bb.head);
	blob_buf_free(&bb);
	return UBUS_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* registration                                                         */
/* ------------------------------------------------------------------ */

static struct ubus_method saadway_methods[] = {
	UBUS_METHOD_NOARG("mgmt.status",        h_mgmt_status),
	UBUS_METHOD_NOARG("mgmt.set",                 h_mgmt_set),
	UBUS_METHOD_NOARG("profile.list",       h_profile_list),
	UBUS_METHOD_NOARG("profile.import_openvpn",    h_profile_import_openvpn),
	UBUS_METHOD_NOARG("profile.create_ikev2",      h_profile_create_ikev2),
	UBUS_METHOD_NOARG("profile.update",           h_profile_update),
	UBUS_METHOD_NOARG("profile.delete",           h_profile_delete),
	UBUS_METHOD_NOARG("profile.activate",         h_profile_activate),
	UBUS_METHOD_NOARG("profile.deactivate",  h_profile_deactivate),
	UBUS_METHOD_NOARG("steer.list",         h_steer_list),
	UBUS_METHOD_NOARG("steer.domain.add",         h_steer_domain_add),
	UBUS_METHOD_NOARG("steer.domain.remove",      h_steer_domain_remove),
	UBUS_METHOD_NOARG("steer.device.set",         h_steer_device_set),
	UBUS_METHOD_NOARG("steer.device.remove",      h_steer_device_remove),
	UBUS_METHOD_NOARG("steer.iprange.set",        h_steer_iprange_set),
	UBUS_METHOD_NOARG("sec.status",         h_sec_status),
	UBUS_METHOD_NOARG("sec.kill_switch.set",      h_sec_kill_switch_set),
	UBUS_METHOD_NOARG("sec.dns.status",     h_sec_dns_status),
	UBUS_METHOD_NOARG("mon.health_test",    h_mon_health_test),
	UBUS_METHOD_NOARG("mon.stats",          h_mon_stats),
	UBUS_METHOD_NOARG("mon.logs",                 h_mon_logs),
	UBUS_METHOD_NOARG("maint.backup.export",      h_maint_backup_export),
	UBUS_METHOD_NOARG("maint.backup.import",      h_maint_backup_import),
	UBUS_METHOD_NOARG("maint.cooldown.set",       h_maint_cooldown_set),
};

static struct ubus_object_type saadway_type =
	UBUS_OBJECT_TYPE("saadway", saadway_methods);

static struct ubus_object saadway_object = {
	.name      = "saadway",
	.type      = &saadway_type,
	.methods   = saadway_methods,
	.n_methods = sizeof(saadway_methods) / sizeof(*saadway_methods),
};

int sw_ubus_init(struct sw_ctx *c)
{
	return ubus_add_object(c->ubus, &saadway_object);
}
