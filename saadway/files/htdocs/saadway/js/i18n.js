/* i18n.js — dictionary loader + language persistence (T029, FR-023/Q4-A,
 * T087/FR-027).
 * Persian-first: default language "fa" (RTL). Preference persists in
 * localStorage only — never in URL parameters (no token/UI state leakage).
 * T087: the daemon-side `dashboard.language` option is the DEFAULT for a
 * browser without a stored preference (the SPA applies it via
 * set_default() before init()); the browser's own stored choice always
 * wins, so the two sources cannot drift. */

"use strict";

window.I18n = (function () {
	var SUPPORTED = ["fa", "en"];
	var DEFAULT_LANG = "fa";          /* Q4-A: Persian-first */
	var DICTS = { fa: null, en: null };
	var lang = DEFAULT_LANG;

	function persist(l) {
		try { localStorage.setItem("saadway.lang", l); }
		catch (e) { /* storage disabled — in-memory only */ }
	}

	function apply() {
		/* RTL for Persian, LTR otherwise (FR-027) */
		document.documentElement.lang = lang;
		document.documentElement.dir = lang === "fa" ? "rtl" : "ltr";
		/* walk any statically-rendered data-i18n nodes */
		var nodes = document.querySelectorAll("[data-i18n]");
		for (var i = 0; i < nodes.length; i++) {
			var k = nodes[i].getAttribute("data-i18n");
			nodes[i].textContent = t(k);
		}
	}

	function load(langCode, cb) {
		/* dynamic dictionary fetch — same origin only (CSP 'self') */
		var xhr = new XMLHttpRequest();
		xhr.open("GET", "i18n/" + langCode + ".json", true);
		xhr.onreadystatechange = function () {
			if (xhr.readyState === 4) {
				if (xhr.status === 200) {
					try {
						DICTS[langCode] = JSON.parse(xhr.responseText);
					} catch (e) { /* ignore malformed */ }
				}
				cb && cb();
			}
		};
		xhr.send();
	}

	function t(key, subst) {
		var d = DICTS[lang] || DICTS.fa || {};
		var s = d[key];
		if (s === undefined) s = key;      /* visible key = loud failure */
		if (subst) {
			Object.keys(subst).forEach(function (k2) {
				s = s.replace(new RegExp("\\{" + k2 + "\\}", "g"), subst[k2]);
			});
		}
		return s;
	}

	function get() { return lang; }

	/* the daemon-side default (mgmt.status → "language") */
	function set_default(l) {
		if (SUPPORTED.indexOf(l) >= 0) DEFAULT_LANG = l;
	}

	function set(l, cb) {
		if (SUPPORTED.indexOf(l) < 0) return;
		if (DICTS[l]) {
			lang = l; persist(l); apply(); cb && cb(); return;
		}
		load(l, function () {
			lang = l; persist(l); apply(); cb && cb();
		});
	}

	function init(cb) {
		var saved;
		try { saved = localStorage.getItem("saadway.lang"); }
		catch (e) { saved = null; }
		if (SUPPORTED.indexOf(saved) < 0) saved = DEFAULT_LANG;
		lang = saved;
		document.getElementById("lang-switch").value = lang;
		load(lang, function () {
			apply();
			cb && cb();
		});
	}

	return {
		init: init, set: set, get: get, t: t, set_default: set_default,
	};
})();
