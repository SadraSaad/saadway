/* api.js — JSON-RPC /ubus client, tokenless (T030, T079).
 *
 * Transport (contracts/api.md): POST /ubus, JSON-RPC 2.0 envelope
 *   { jsonrpc: "2.0", id: <n>, method: "call",
 *     params: ["<session-id>", "<object>", "<method>", <msg>] }
 *
 * T079 / spec FR-020 + FR-035 / constitution v1.2.0: the standalone
 * dashboard performs NO credential exchange. Every request carries rpcd's
 * DEFAULT (unauthenticated) session id — the all-zero id
 * "00000000000000000000000000000000", which is literally
 * RPC_DEFAULT_SESSION_ID in openwrt/rpcd (include/rpcd/session.h:35).
 * rpcd creates that default session once at startup with a NULL login
 * section (session.c:1396-1400) and grants ONLY the access group named
 * "unauthenticated" to it (session.c:944-946) — that group is shipped in
 * saadway/files/rpc-acl.json. uhttpd-mod-ubus checks every /ubus call
 * through rpcd's session.access with the request's params[0] as the
 * session id (openwrt/uhttpd ubus.c:280-297, 820-840), so the tokenless
 * envelope lands on exactly that grant set.
 *
 * There is therefore NO session token in this client: no session.login, no
 * ubus_rpc_session, no TTL, no token in the DOM, in a URL or in a log
 * line. The access control of the dashboard is the platform's network
 * boundary — the LAN-bound uhttpd listener plus the firewall's WAN input
 * policy (docs/verify-2512.md §7).
 *
 * Transport failure mapping (T-verify-6, Phase 15/T061): over the legacy
 * POST /ubus path of uhttpd-mod-ubus, failures surface as result arrays
 * with a nonzero ubus status code — 6 (UBUS_STATUS_PERMISSION_DENIED),
 * 7 (TIMEOUT, transport-only), 9 (UNKNOWN_ERROR) — while the modern
 * {error:{code,...}} envelope is used by uhttpd itself for pre-call
 * rejections (-32002 "Access denied", -32000 "Object not found", -32003
 * request timeout). The client treats any nonzero code as an error.
 */

"use strict";

window.SwApi = (function () {
	/* rpcd's default (unauthenticated) session id — see the header */
	var DEFAULT_SESSION_ID = "00000000000000000000000000000000";
	var OBJ = "saadway";

	var rpcId = 0;

	function post(payload) {
		return new Promise(function (resolve) {
			var xhr = new XMLHttpRequest();
			xhr.open("POST", "/ubus", true);
			xhr.setRequestHeader("Content-Type", "application/json");
			xhr.onreadystatechange = function () {
				if (xhr.readyState !== 4) return;
				if (xhr.status !== 200) {
					resolve({ transport: true, status: xhr.status });
					return;
				}
				var resp;
				try { resp = JSON.parse(xhr.responseText); }
				catch (e) {
					resolve({ transport: true, status: 0 });
					return;
				}
				resolve(resp);
			};
			xhr.send(JSON.stringify(payload));
		});
	}

	function extractResult(resp) {
		/* uhttpd-mod-ubus shape: {result: [code, payload?]} */
		if (resp && Object.prototype.hasOwnProperty.call(resp, "result")) {
			var r = resp.result;
			return {
				ok: Array.isArray(r) && r[0] === 0,
				code: Array.isArray(r) ? r[0] : -1,
				data: Array.isArray(r) && r.length > 1 ? r[1] : null,
			};
		}
		/* newer uhttpd error envelope: {error: {code, message}} */
		if (resp && resp.error) {
			return { ok: false, code: resp.error.code || -1, error: resp.error };
		}
		return { ok: false, code: -1 };
	}

	function call(object, method, msg) {
		var body = {
			jsonrpc: "2.0",
			id: ++rpcId,
			method: "call",
			params: [DEFAULT_SESSION_ID, object, method, msg || {}],
		};
		return post(body).then(function (resp) {
			return extractResult(resp);
		});
	}

	function saadway(method, msg) { return call(OBJ, method, msg); }

	/* T103: the transport's own error text is generic ("Access denied",
	 * "Invalid argument"); the daemon's own detail (the E_* codes) is
	 * preferred, and the raw ubus status is mapped to a readable
	 * fallback so a failure is never reported as an opaque save error.
	 * The ubus status codes are the upstream 0-based enum
	 * (openwrt/ubus ubusmsg.h). */
	var STATUS_TEXT = {
		1: "command failed",
		2: "invalid argument",
		3: "method not found",
		4: "not found",
		5: "unknown error",
		6: "permission denied",
		7: "timeout",
		8: "connection failed",
		9: "unknown error",
		10: "connection failed",
	};

	function describe(res) {
		if (!res) return null;
		if (res.error && res.error.message) return String(res.error.message);
		if (res.data && res.data.detail) return String(res.data.detail);
		if (res.code && STATUS_TEXT[res.code]) return STATUS_TEXT[res.code];
		return null;
	}

	return {
		call: call,
		saadway: saadway,
		describe: describe,
	};
})();
