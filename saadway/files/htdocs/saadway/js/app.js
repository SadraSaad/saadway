/* app.js — dashboard application logic (T029/T036).
 * Views render against the contract payload shapes ONLY (contracts/api.md).
 * Every privileged interaction calls SwApi.saadway(...) with the session
 * token handled inside js/api.js. Rendering escapes all server strings.
 */

"use strict";

(function () {
	var T = function (k, s) { return window.I18n.t(k, s); };
	var $ = function (sel) { return document.querySelector(sel); };
	var esc = function (s) {
		return String(s === undefined || s === null ? "" : s)
			.replace(/&/g, "&amp;").replace(/</g, "&lt;")
			.replace(/>/g, "&gt;").replace(/"/g, "&quot;");
	};

	var poll = { stats: null, status: null, logs: null };
	var logFollow = true;
	var lastLogCursor = 0;
	var rxHist = [], txHist = [];

	function toast(msg, isErr) {
		var el = $("#toast");
		el.textContent = msg;
		el.className = "toast" + (isErr ? " err" : "");
		setTimeout(function () { el.classList.add("hidden"); }, 3500);
	}

	/* ---------------------------------------------------------------- */
	/* session / global header                                          */
	/* ---------------------------------------------------------------- */

	var CONN_I18N = {
		up: "conn_up", down: "conn_down", connecting: "conn_connecting",
		contained: "conn_contained", cooldown: "conn_cooldown",
	};

	function render_status(st) {
		var badge = $("#conn-badge");
		var cs = st.connection_status || "down";
		badge.className = "badge " + (cs === "up" ? "up"
			: (cs === "down" ? "down" : "connecting"));
		badge.textContent = T(CONN_I18N[cs] || "conn_down") +
			(st.active_profile ? " — " + st.active_profile : "") +
			(st.current_if && st.current_if !== "-" ?
				" (" + esc(st.current_if) + ")" : "");
	}

	function refresh_status() {
		return SwApi.saadway("mgmt.status").then(function (r) {
			if (r.ok && r.data) render_status(r.data);
		});
	}

	function start_status_poll() {
		stop_poll("status");
		refresh_status();
		poll.status = setInterval(refresh_status, 5000);
	}

	var SwApp = {
		on_session_lost: function () {
			/* forced re-login on denial codes (api.js flag) */
			show_login(true);
		},
	};
	window.SwApp = SwApp;

	/* ---------------------------------------------------------------- */
	/* login view                                                       */
	/* ---------------------------------------------------------------- */

	function show_login(expired) {
		["#view-app"].forEach(function (s) { $(s).classList.add("hidden"); });
		$("#view-login").classList.remove("hidden");
		$("#btn-logout").classList.add("hidden");
		$("#conn-badge").classList.add("hidden");
		if (expired) {
			var e = $("#login-err");
			e.textContent = T("session_expired");
			e.classList.remove("hidden");
		}
	}

	function show_app() {
		$("#view-login").classList.add("hidden");
		$("#view-app").classList.remove("hidden");
		$("#btn-logout").classList.remove("hidden");
		$("#conn-badge").classList.remove("hidden");
		start_status_poll();
		switch_tab("profiles");
	}

	function on_login(ev) {
		ev.preventDefault();
		$("#login-err").classList.add("hidden");
		var u = $("#login-user").value, p = $("#login-pass").value;
		SwApi.login(u, p).then(function (r) {
			if (r.ok) {
				$("#login-pass").value = "";     /* pwd never persisted */
				show_app();
			} else {
				var e = $("#login-err");
				e.textContent = T("login_failed");
				e.classList.remove("hidden");
			}
		});
	}

	function on_logout() {
		stop_all_polls();
		SwApi.logout().then(function () { show_login(false); });
	}

	/* ---------------------------------------------------------------- */
	/* tabs                                                             */
	/* ---------------------------------------------------------------- */

	function switch_tab(name) {
		document.querySelectorAll(".tab").forEach(function (el) {
			el.classList.toggle("active", el.getAttribute("data-tab") === name);
		});
		["profiles", "steering", "security", "monitoring", "maintenance"]
			.forEach(function (p) { $("#panel-" + p).hidden = p !== name; });
		var r = {
			profiles: tab_profiles,
			steering: tab_steering,
			security: tab_security,
			monitoring: tab_monitoring,
			maintenance: tab_maintenance,
		}[name];
		(r || function () {})();
	}

	/* ---------------------------------------------------------------- */
	/* Profiles (US1)                                                   */
	/* ---------------------------------------------------------------- */

	function status_chip(status) {
		var cls = status === "up" ? "ok" : (status === "connecting" ||
			status === "cooldown" || status === "contained" ? "warn" : "err");
		return '<span class="chip ' + cls + '">' + esc(status || "down") +
			"</span>";
	}

	function tab_profiles() {
		stop_poll("stats", "logs");
		SwApi.saadway("profile.list").then(function (r) {
			if (!r.ok) { toast(T("save_error"), true); return; }
			var rows = "";
			(r.data.profiles || []).forEach(function (p) {
				rows += "<tr><td>" + esc(p.name) + (p.active ?
					' <span class="chip ok">' + esc(T("active_pin")) + "</span>" : "") +
					"</td><td>" +
					esc(T(p.kind === "ikev2" ? "kind_ikev2" : "kind_openvpn")) +
					"</td><td>" + status_chip(p.status) + " · " +
					esc(p.enabled ? T("profile_enabled") : T("profile_disabled")) +
					"</td><td>";
				rows += '<button class="btn small" data-act="activate" data-name="' +
					esc(p.name) + '">' + esc(T("activate")) + "</button> ";
				rows += '<button class="btn small danger" data-act="delete" data-name="' +
					esc(p.name) + '">' + esc(T("delete_profile")) + "</button>";
				rows += "</td></tr>";
			});
			rows = rows || '<tr><td colspan="4" class="muted">' +
				esc(T("empty")) + "</td></tr>";
			var tbl = "<table class=\"tbl\"><thead><tr>" +
				"<th>" + esc(T("col_name")) + "</th><th>" + esc(T("col_kind")) +
				"</th><th>" + esc(T("col_status")) + "</th><th>" +
				esc(T("col_actions")) + "</th></tr></thead><tbody>" + rows +
				"</tbody></table>";
			$("#panel-profiles").innerHTML =
				'<div class="card"><h2>' + esc(T("profiles_title")) + "</h2>" + tbl +
				'<div class="actions"><button class="btn" data-act="deactivate">' +
				esc(T("deactivate")) + "</button></div></div>" +
				'<div class="card"><h3>' + esc(T("import_title")) + "</h3>" +
				'<label>' + esc(T("import_name")) + "</label>" +
				'<input id="imp-name" autocomplete="off" spellcheck="false">' +
				"<label>" + esc(T("import_file")) + "</label>" +
				'<input id="imp-file" type="file" accept=".ovpn,text/plain">' +
				"<label>" + esc(T("import_user")) + "</label>" +
				'<input id="imp-user" autocomplete="off">' +
				"<label>" + esc(T("import_pass")) + "</label>" +
				'<input id="imp-pass" type="password">' +
				'<button class="btn primary" data-act="import">' + esc(T("import_btn")) +
				"</button></div>" +
				'<div class="card"><h3>' + esc(T("ike_title")) + "</h3>" +
				"<label>" + esc(T("ike_name")) + "</label>" +
				'<input id="ike-name" autocomplete="off" spellcheck="false">' +
				"<label>" + esc(T("ike_hostname")) + "</label>" +
				'<input id="ike-host" autocomplete="off" spellcheck="false" dir="ltr">' +
				"<label>" + esc(T("ike_username")) + "</label>" +
				'<input id="ike-user" autocomplete="off" dir="ltr">' +
				"<label>" + esc(T("ike_password")) + "</label>" +
				'<input id="ike-pass" type="password" dir="ltr">' +
				'<button class="btn primary" data-act="ike-create">' +
				esc(T("ike_create")) + "</button></div>";
		});
	}

	function file_to_b64(file) {
		return new Promise(function (resolve, reject) {
			var fr = new FileReader();
			fr.onload = function () {
				var m = String(fr.result).split(",");
				resolve(m.length > 1 ? m[1] : "");
			};
			fr.onerror = function () { reject(fr.error); };
			fr.readAsDataURL(file);
		});
	}

	function panel_click(ev) {
		var btn = ev.target.closest("button[data-act]");
		if (!btn) return;
		var act = btn.getAttribute("data-act");
		var panel = btn.closest("section");
		var pid = panel ? panel.id : "";

		if (act === "activate") {
			SwApi.saadway("profile.activate", { name: btn.getAttribute("data-name") })
				.then(function (r) {
					if (r.ok) { toast("switched_to: " + (r.data &&
						r.data.switched_to || "")); tab_profiles(); refresh_status(); }
					else toast(err_of(r, T), true);
				});
			return;
		}
		if (act === "deactivate") {
			SwApi.saadway("profile.deactivate").then(function (r) {
				if (r.ok) { tab_profiles(); refresh_status(); }
				else toast(err_of(r, T), true);
			});
			return;
		}
		if (act === "delete") {
			var nm = btn.getAttribute("data-name");
			if (!window.confirm(T("confirm_delete", { name: nm }))) return;
			SwApi.saadway("profile.delete", { name: nm }).then(function (r) {
				if (r.ok) tab_profiles();
				else toast(err_of(r, T), true);
			});
			return;
		}
		if (act === "import") {
			var f = $("#imp-file").files[0];
			var nm = $("#imp-name").value.trim();
			if (!f || !nm) { toast(T("save_error"), true); return; }
			file_to_b64(f).then(function (b64) {
				var msg = { name: nm, ovpn_b64: b64 };
				var u = $("#imp-user").value, pw = $("#imp-pass").value;
				if (u) msg.credentials = { username: u, password: pw };
				SwApi.saadway("profile.import_openvpn", msg).then(function (r) {
					if (r.ok) {
						toast("import: " + nm + (r.data && r.data.warnings ?
							" · " + esc(r.data.warnings.join(", ")) : ""), false);
						tab_profiles();
					} else toast(err_of(r, T), true);
				});
			}).catch(function () { toast(T("save_error"), true); });
			return;
		}
		if (act === "ike-create") {
			var im = {
				name: $("#ike-name").value.trim(),
				hostname: $("#ike-host").value.trim(),
				username: $("#ike-user").value,
				password: $("#ike-pass").value,
			};
			if (!im.name || !im.hostname) { toast(T("save_error"), true); return; }
			SwApi.saadway("profile.create_ikev2", im).then(function (r) {
				if (r.ok) { tab_profiles(); }
				else toast(err_of(r, T), true);
			});
			return;
		}
	}

	function err_of(r) {
		if (r.error && r.error.message)
			return String(r.error.message);
		if (r.data && r.data.detail)
			return String(r.data.detail);
		return T("save_error");
	}

	/* ---------------------------------------------------------------- */
	/* Steering (US3)                                                   */
	/* ---------------------------------------------------------------- */

	function tab_steering() {
		stop_poll("stats", "logs");
		SwApi.saadway("steer.list").then(function (r) {
			if (!r.ok) { toast(T("save_error"), true); return; }
			var d = r.data;
			var rows = (d.domains || []).map(function (dom) {
				return '<tr><td>' + esc(dom) + "</td><td>" +
					'<button class="btn small danger" data-act="dom-del" data-dom="' +
					esc(dom) + '">' + esc(T("remove")) + "</button></td></tr>";
			}).join("") || '<tr><td colspan="2" class="muted">' +
				esc(T("empty")) + "</td></tr>";

			var drows = (d.devices || []).map(function (dev) {
				var pol = T("policy_" + (dev.policy || "implicit"));
				return "<tr><td>" + esc(dev.match_value) + " (" + esc(dev.match_type) +
					")</td><td>" + esc(pol) + "</td><td>" +
					'<button class="btn small danger" data-act="dev-del" data-mt="' +
					esc(dev.match_type) + '" data-mv="' + esc(dev.match_value) +
					'">' + esc(T("remove")) + "</button></td></tr>";
			}).join("") || '<tr><td colspan="3" class="muted">' +
				esc(T("empty")) + "</td></tr>";

			$("#panel-steering").innerHTML =
				'<div class="card"><h2>' + esc(T("steer_title")) + "</h2>" +
				'<h3>' + esc(T("domains_head")) + "</h3>" +
				'<table class="tbl"><tbody>' + rows + "</tbody></table>" +
				'<input id="new-domain" placeholder="' + esc(T("domain_ph")) +
				'" spellcheck="false" autocomplete="off"> ' +
				'<button class="btn" data-act="dom-add">' +
				esc(T("add_domain")) + "</button>" +
				"<h3>" + esc(T("devices_head")) + "</h3>" +
				'<table class="tbl"><thead><tr><th>' + esc(T("col_name")) +
				"</th><th>" + esc(T("col_status")) + "</th><th></th></tr></thead>" +
				"<tbody>" + drows + "</tbody></table>" +
				'<select id="dev-type">' +
				'<option value="ip">IP</option>' +
				'<option value="mac">MAC</option>' +
				"</select> " +
				'<input id="dev-value" placeholder="' + esc(T("device_ph_value")) +
				'" spellcheck="false" autocomplete="off" dir="ltr"> ' +
				'<select id="dev-policy">' +
				'<option value="direct">' + esc(T("policy_direct")) + "</option>" +
				'<option value="tunneled">' + esc(T("policy_tunneled")) + "</option>" +
				"</select> " +
				'<button class="btn" data-act="dev-set">' + esc(T("set")) + "</button>" +
				"<h3>" + esc(T("iprange_head")) + "</h3>" +
				'<label class="switch"><input type="checkbox" id="ipr-toggle"' +
				(d.iprange_bypass ? " checked" : "") +
				' data-act="ipr"><span class="slider"></span></label>' +
				"</div>";
		});
	}

	/* ---------------------------------------------------------------- */
	/* Security (US2)                                                   */
	/* ---------------------------------------------------------------- */

	function tab_security() {
		stop_poll("stats", "logs");
		SwApi.saadway("sec.status").then(function (r) {
			if (!r.ok) { toast(T("save_error"), true); return; }
			$("#panel-security").innerHTML =
				'<div class="card"><h2>' + esc(T("sec_title")) + "</h2>" +
				"<h3>" + esc(T("kill_switch")) + "</h3>" +
				'<p class="muted">' + esc(T("kill_switch_help")) + "</p>" +
				'<label class="switch"><input type="checkbox" id="ks-toggle"' +
				(r.data.kill_switch ? " checked" : "") +
				' data-act="ks"><span class="slider"></span></label>' +
				(r.data.contained ? ' <span class="chip warn">' +
					esc(T("conn_contained")) + "</span>" : "") +
				"<h3>" + esc(T("dns_head")) + "</h3>" +
				(dns_status_rows(r.data)) +
				"</div>";
		});
	}

	function dns_status_rows(s) {
		return '<div class="kv"><dt>' + esc(T("dns_forced")) + "</dt><dd>" +
			(s.dns_forced ? "✓" : "—") + "</dd><dt>" + esc(T("dns_resolver_if")) +
			"</dt><dd>" + esc(s.resolver_if || "-") + "</dd><dt>" +
			esc(T("dns_leak_risk")) + "</dt><dd>" +
			esc(s.leak_risk === "low" ? T("leak_low") : T("leak_unknown")) +
			"</dd></div>";
	}

	function toggle_ks(on) {
		SwApi.saadway("sec.kill_switch.set", { enabled: on }).then(function (r) {
			if (r.ok) { toast(T("applied")); refresh_status(); }
			else toast(T("save_error"), true);
		});
	}

	/* ---------------------------------------------------------------- */
	/* Monitoring (US5)                                                 */
	/* ---------------------------------------------------------------- */

	function tab_monitoring() {
		stop_poll("stats", "logs");
		rxHist = []; txHist = [];
		lastLogCursor = 0;
		logFollow = true;
		$("#panel-monitoring").innerHTML =
			'<div class="card"><h2>' + esc(T("bw_title")) + "</h2>" +
			'<canvas id="bw-chart"></canvas>' +
			'<div style="display:flex;gap:1rem" class="muted">' +
			'<span><span style="color:#4f8cff">●</span> ' + esc(T("rx")) +
			"</span><span><span style=\"color:#2fbf71\">●</span> " +
			esc(T("tx")) + "</span><span id=\"bw-if\"></span></div></div>" +
			'<div class="card"><h3>' + esc(T("health_title")) + "</h3>" +
			'<p class="muted">' + esc(T("health_hint")) + "</p>" +
			'<button class="btn primary" data-act="health-run">' +
			esc(T("health_run")) + "</button>" +
			'<div id="health-out" class="kv"></div></div>' +
			'<div class="card"><h3>' + esc(T("logs_head")) + "</h3>" +
			'<button class="btn small" id="btn-logtoggle">' +
			esc(T("logs_follow")) + "</button>" +
			'<div id="logview"></div></div>';

		document.querySelector("[data-act=health-run]").addEventListener(
			"click", run_health);
		document.querySelector("#btn-logtoggle").addEventListener(
			"click", toggle_log_follow);

		poll.stats = setInterval(poll_stats, 1000);
		poll.logs = setInterval(poll_logs, 2000);
	}

	function stop_poll() {
		for (var i = 0; i < arguments.length; i++) {
			var k = arguments[i];
			if (poll[k]) { clearInterval(poll[k]); poll[k] = null; }
		}
	}

	function stop_all_polls() { stop_poll("status", "stats", "logs"); }

	function poll_stats() {
		SwApi.saadway("mon.stats").then(function (r) {
			if (!r.ok || !r.data) return;
			rxHist.push(r.data.rx_bps); txHist.push(r.data.tx_bps);
			if (rxHist.length > 60) rxHist.shift();
			if (txHist.length > 60) txHist.shift();
			var cv = $("#bw-chart");
			if (cv && !cv.closest("section").hidden) {
				window.SwChart.make(cv, {
					series: [
						{ values: rxHist, color: "#4f8cff" },
						{ values: txHist, color: "#2fbf71" },
					],
				});
			}
			var lbl = $("#bw-if");
			if (lbl && r.data.iface) lbl.textContent = r.data.iface;
		});
	}

	function run_health() {
		var out = $("#health-out");
		out.innerHTML = '<dt>' + esc(T("health_running")) + "</dt>";
		SwApi.saadway("mon.health_test").then(function (r) {
			if (!r.ok) {
				out.innerHTML = '<dt>' + esc(err_of(r)) + "</dt>";
				return;
			}
			var d = r.data;
			var ping = d.ping, http = d.http;
			var pl = (ping && typeof ping.latency_ms === "number") ?
				ping.latency_ms + " " + esc(T("latency_ms")) : esc(T("timeout"));
			var hc = (http && typeof http.code === "number") ?
				"http " + http.code + (http.code === 204 ? " ✓" : "") :
				esc(T("timeout"));
			out.innerHTML =
				"<dt>" + esc(T("health_ping")) + " (" +
				esc((ping && ping.target) || "1.1.1.1") + ")</dt><dd>" + pl +
				"</dd><dt>" + esc(T("health_http")) + "</dt><dd>" + hc + "</dd>";
		});
	}

	function toggle_log_follow() {
		logFollow = !logFollow;
		$("#btn-logtoggle").textContent =
			logFollow ? T("logs_follow") : T("logs_pause");
	}

	function level_class(p) {
		return p >= 3 ? "lvl-err" : p === 2 ? "lvl-warn" :
			(p === 0 ? "lvl-debug" : "lvl-info");
	}

	function poll_logs() {
		if (!logFollow) return;
		SwApi.saadway("mon.logs", lastLogCursor ? { cursor: String(lastLogCursor), lines: 30 } : { lines: 30 })
		.then(function (r) {
			if (!r.ok || !r.data) return;
			lastLogCursor = r.data.next_cursor || lastLogCursor;
			var view = $("#logview");
			if (!view) return;
			(r.data.entries || []).forEach(function (e) {
				var d = new Date((e.ts || 0) * 1000);
				var stamp = d ? d.toISOString().slice(11, 19) : "--:--:--";
				var div = document.createElement("div");
				div.className = level_class(e.level | 0);
				div.textContent = "[" + stamp + "] " + String(e.msg || "");
				view.appendChild(div);
				while (view.childNodes.length > 300) view.removeChild(view.firstChild);
				view.scrollTop = view.scrollHeight;
			});
		});
	}

	/* ---------------------------------------------------------------- */
	/* Maintenance (US6)                                                */
	/* ---------------------------------------------------------------- */

	function tab_maintenance() {
		stop_poll("stats", "logs");
		$("#panel-maintenance").innerHTML =
			'<div class="card"><h3>' + esc(T("backup_export")) + "</h3>" +
			'<label class="switch"><input type="checkbox" id="bk-cred"' +
			'><span class="slider"></span></label> ' +
			esc(T("backup_include_credentials")) +
			'<br><button class="btn primary" data-act="bk-export">' +
			esc(T("backup_export")) + "</button></div>" +
			'<div class="card"><h3>' + esc(T("backup_import")) + "</h3>" +
			'<input id="bk-file" type="file" accept="application/json,.json">' +
			'<br><button class="btn primary" data-act="bk-import">' +
			esc(T("backup_import")) + "</button>" +
			'<div id="bk-report"></div></div>' +
			'<div class="card"><h3>' + esc(T("cooldown_head")) + "</h3>" +
			"<label>" + esc(T("cooldown_seconds")) + "</label>" +
			'<input id="cd-sec" type="number" min="0" max="86400" value="300">' +
			"<label>" + esc(T("cooldown_attempts")) + "</label>" +
			'<input id="cd-att" type="number" min="1" max="10" value="3">' +
			'<button class="btn" data-act="cd-set">' + esc(T("set")) + "</button></div>";
	}

	function download_json(obj, name) {
		var blob = new Blob([JSON.stringify(obj, null, 1)],
			{ type: "application/json" });
		var a = document.createElement("a");
		a.href = URL.createObjectURL(blob);
		a.download = name;
		a.click();
		URL.revokeObjectURL(a.href);
	}

	function panel_events(ev) {
		var btn = ev.target.closest("[data-act]");
		if (btn) {
			var act = btn.getAttribute("data-act");
			if (act === "ks") { toggle_ks(btn.checked); return; }
			if (act === "ipr") {
				SwApi.saadway("steer.iprange.set", { enabled: btn.checked })
					.then(function (r) {
						if (r.ok) toast(T("applied")); else toast(T("save_error"), true);
					});
				return;
			}
			if (act === "dom-add") {
				var dom = $("#new-domain").value.trim();
				if (!dom) return;
				SwApi.saadway("steer.domain.add", { domain: dom }).then(function (r) {
					if (r.ok) tab_steering();
					else toast(err_of(r), true);
				});
				return;
			}
			if (act === "dom-del") {
				SwApi.saadway("steer.domain.remove",
					{ domain: btn.getAttribute("data-dom") })
					.then(function (r) { r.ok ? tab_steering() : toast(err_of(r), true); });
				return;
			}
			if (act === "dev-set") {
				SwApi.saadway("steer.device.set", {
					match_type: $("#dev-type").value,
					match_value: $("#dev-value").value.trim(),
					policy: $("#dev-policy").value,
				}).then(function (r) { r.ok ? tab_steering() : toast(err_of(r), true); });
				return;
			}
			if (act === "dev-del") {
				SwApi.saadway("steer.device.remove", {
					match_type: btn.getAttribute("data-mt"),
					match_value: btn.getAttribute("data-mv"),
				}).then(function (r) { r.ok ? tab_steering() : toast(err_of(r), true); });
				return;
			}
			if (act === "health-run") { run_health(); return; }
			if (act === "bk-export") {
				SwApi.saadway("maint.backup.export",
					{ include_credentials: $("#bk-cred").checked })
					.then(function (r) {
						if (!r.ok) { toast(T("save_error"), true); return; }
						var txt = atob(r.data.json_b64);
						/* decode UTF-8 payload bytes safely */
						var bytes = new Uint8Array(txt.length);
						for (var i = 0; i < txt.length; i++)
							bytes[i] = txt.charCodeAt(i);
						var blob = new Blob([bytes],
							{ type: "application/json" });
						var a = document.createElement("a");
						a.href = URL.createObjectURL(blob);
						a.download = "saadway-backup.json";
						a.click();
						URL.revokeObjectURL(a.href);
					});
				return;
			}
			if (act === "bk-import") {
				var f = $("#bk-file").files[0];
				if (!f) { toast(T("save_error"), true); return; }
				file_to_b64(f).then(function (b64) {
					SwApi.saadway("maint.backup.import", { json_b64: b64 })
						.then(function (r) {
							if (!r.ok) { toast(err_of(r), true); return; }
							render_bk_report(r.data);
						});
				});
				return;
			}
			if (act === "cd-set") {
				SwApi.saadway("maint.cooldown.set", {
					seconds: parseInt($("#cd-sec").value, 10) || 0,
					attempts: parseInt($("#cd-att").value, 10) || undefined,
				}).then(function (r) {
					if (r.ok) toast(T("applied")); else toast(T("save_error"), true);
				});
				return;
			}
			return;
		}
		/* clicked row buttons (profiles table) */
		panel_click(ev);
	}

	function render_bk_report(d) {
		var list = function (arr, f1, f2) {
			return (arr || []).map(function (x) {
				return "<li>" + esc(x[f1] !== undefined ? x[f1] : JSON.stringify(x)) +
					(f2 && x[f2] !== undefined ?
						" — " + esc(x[f2]) : "") + "</li>";
			}).join("") || "<li>—</li>";
		};
		$("#bk-report").innerHTML =
			"<h3>" + esc(T("restored")) + "</h3><ul>" +
			list(d.restored, "item") + "</ul><h3>" +
			esc(T("needs_attention")) + "</h3><ul>" +
			list(d.needs_attention, "item", "reason") + "</ul><h3>" +
			esc(T("errors")) + "</h3><ul>" +
			list(d.errors, "item", "reason") + "</ul>";
	}

	/* ---------------------------------------------------------------- */
	/* boot                                                             */
	/* ---------------------------------------------------------------- */

	function boot() {
		window.I18n.init(function () {
			document.querySelectorAll(".tab").forEach(function (el) {
				el.addEventListener("click", function () {
					switch_tab(el.getAttribute("data-tab"));
				});
			});
			$("#lang-switch").addEventListener("change", function (ev) {
				window.I18n.set(ev.target.value, function () { });
			});
			$("#login-form").addEventListener("submit", on_login);
			$("#btn-logout").addEventListener("click", on_logout);
			document.body.addEventListener("click", panel_events);
			/* session restore: probe with a status call; 401 → login view */
			if (SwApi.is_logged_in()) show_app();
			else {
				SwApi.saadway("mgmt.status").then(function (r) {
					if (r.ok) show_app(); else show_login(false);
				});
			}
		});
	}

	if (document.readyState === "loading")
		document.addEventListener("DOMContentLoaded", boot);
	else boot();
})();
