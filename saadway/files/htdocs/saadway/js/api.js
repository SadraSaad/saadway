/* api.js — JSON-RPC /ubus client with session lifecycle (T030).
 *
 * Transport (contracts/api.md): POST /ubus, JSON-RPC 2.0 envelope
 *   { jsonrpc: "2.0", id: <n>, method: "call",
 *     params: ["<session-token>", "<object>", "<method>", <msg>] }
 *
 * session LOGIN uses the special all-zero unauthenticated token, exactly as
 * mandate by uhttpd-mod-ubus/rpcd:
 *   params: ["00000000000000000000000000000000", "session", "login",
 *            {username, password, timeout}]
 * The response carries ubus_rpc_session — the ONLY place the token lives is
 * RAM (never stored, never logged, never in URL; FR-020/FR-021).
 *
 * Inactivity expiry: rpcd touches a session on each authorized call; the
 * dashboard additionally requests the configured TTL at login — the
 * `session_timeout` field of `mgmt.status` (dashboard.session_timeout,
 * 1800 s default; the pre-status first login uses the 1800 s fallback).
 * On-device mapping (docs/verify-2512.md, T-verify-6, Phase 15/T061):
 * over the legacy POST /ubus path of uhttpd-mod-ubus, failures surface
 * as result arrays with a nonzero ubus status code — 6
 * (UBUS_STATUS_PERMISSION_DENIED) on wrong login credentials: rpcd's
 * rpc_handle_login returns exactly that, and the upstream ubusmsg.h enum
 * is 0-based (PERMISSION_DENIED=6, TIMEOUT=7, UNKNOWN_ERROR=9,
 * CONNECTION_FAILED=10); the modern {error:{code,...}} envelope is used
 * by uhttpd itself for pre-call rejections (-32002 "Access denied",
 * -32000 "Object not found", -32003 request timeout). The client treats
 * any nonzero code as an error and denial-class codes as re-login
 * triggers (session loss → login screen); 7 (TIMEOUT) is a transport
 * condition and must NOT trigger a re-login; 9/17/401/-1 are defensive
 * forward-compat slots.
 */

"use strict";

window.SwApi = (function () {
	var ZERO_TOKEN = "00000000000000000000000000000000";
	var OBJ = "saadway";

	var token = null;     /* current ubus session (RAM only) */
	var rpcId = 0;

	/* T053: the login TTL is the configured dashboard.session_timeout, learned
	 * from any successful mgmt.status (daemon already clamps it ≥300 s);
	 * LOGIN_TIMEOUT_FALLBACK covers the pre-status first login (conf default). */
	var LOGIN_TIMEOUT_FALLBACK = 1800;
	var session_timeout = 0;   /* 0 = not yet learned from mgmt.status */

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

	function isDenied(code) {
		/* PERMISSION_DENIED-ish codes across servers; T-verify-6/T061 —
		 * 6 = UBUS_STATUS_PERMISSION_DENIED (upstream 0-based enum;
		 * rpcd's login failure). 7 (TIMEOUT) is transport and stays
		 * OUT of the re-login trigger set. */
		return code === 6 || code === 9 || code === 17 || code === 401 ||
			code === -1;
	}

	function call(object, method, msg) {
		var body = {
			jsonrpc: "2.0",
			id: ++rpcId,
			method: "call",
			params: [token, object, method, msg || {}],
		};
		return post(body).then(function (resp) {
			var res = extractResult(resp);
			/* T053: keep the login TTL coherent with the UCI option —
			 * any successful mgmt.status tells us the configured value
			 * (already clamped daemon-side, ≥300 s). */
			if (res.ok && res.data && isFinite(res.data.session_timeout) &&
			    object === OBJ && method === "mgmt.status") {
				session_timeout = res.data.session_timeout;
			}
			if (!res.ok && isDenied(res.code) && token !== null) {
				/* session expired/replaced → force re-login flow */
				token = null;
				res.stale = true;
				if (window.SwApp && window.SwApp.on_session_lost) {
					window.SwApp.on_session_lost();
				}
			}
			return res;
		});
	}

	function login(username, password) {
		var body = {
			jsonrpc: "2.0",
			id: ++rpcId,
			method: "call",
			params: [ZERO_TOKEN, "session", "login", {
				username: String(username || ""),
				password: String(password || ""),
				/* T053: configured TTL once learned; the first
				 * (pre-status) session uses the conf default. */
				timeout: session_timeout || LOGIN_TIMEOUT_FALLBACK,
			}],
		};
		return post(body).then(function (resp) {
			var res = extractResult(resp);
			if (res.ok && res.data && res.data.ubus_rpc_session) {
				token = res.data.ubus_rpc_session;   /* RAM only */
				return { ok: true };
			}
			token = null;
			return { ok: false };
		});
	}

	function logout() {
		if (token === null) return Promise.resolve({ ok: true });
		var t0 = token;
		token = null;
		return post({
			jsonrpc: "2.0", id: ++rpcId, method: "call",
			params: [t0, "session", "destroy", { ubus_rpc_session: t0 }],
		}).then(function () { return { ok: true }; });
	}

	function is_logged_in() { return token !== null; }

	function saadway(method, msg) { return call(OBJ, method, msg); }

	return {
		call: call, login: login, logout: logout,
		is_logged_in: is_logged_in, saadway: saadway,
	};
})();
