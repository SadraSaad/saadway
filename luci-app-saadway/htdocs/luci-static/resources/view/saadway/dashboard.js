/* luci-app-saadway dashboard.js — the LuCI controller page (T031, FR-022, T077, T079)
 *
 * Constitution Principle V (v1.2.0) — the LuCI surface carries EXACTLY two
 * tabs:
 *   1. "Status & Control" — the service status display, the enable/disable
 *      toggle (saadway.mgmt.set) and the "Open Dashboard" link to the
 *      standalone SPA (/saadway/);
 *   2. "Dashboard Credentials" (تنظیمات ورود به داشبورد) — the form that
 *      manages the OPTIONAL rpcd session credential of the `saadway_login`
 *      mapping. Since the auth bypass of 2026-10-21 (spec FR-020/FR-035,
 *      constitution v1.2.0, research.md D11) the standalone dashboard
 *      itself logs in no more — it is authorized as rpcd's default
 *      (unauthenticated) session and its access control is the platform's
 *      network boundary (the LAN-bound uhttpd listener + the firewall's WAN
 *      input policy). The credential this tab manages is therefore the
 *      optional operator credential, not the dashboard's entry point.
 *      The tab stays bounded: no profile, steering or monitoring surface
 *      appears here.
 *
 * The credential change is ONE ubus call (saadway.mgmt.credentials.set): the
 * browser never writes UCI, never hashes the password and never sees the
 * stored secret (FR-021/FR-033).
 *
 * T079 (Phase 24) — the three rendering defects:
 *   - `'require view';` is mandatory: `view` is a BUILT-IN LuCI module
 *     (openwrt/luci modules/luci-base/.../luci.js:2243 → `view: View`),
 *     not a global — without the require, `view.extend` is undefined and the
 *     page dies with "TypeError: view.extend is not a function".
 *   - the form is bound to the SERVICE's own UCI coordinates: a form.Map is
 *     bound to exactly ONE uci config (form.js → CBIMap), so the map must
 *     carry the config the state actually lives in ('saadway') and the
 *     section that holds it ('service' — `config service 'service'` in
 *     saadway/files/saadway.conf). Binding 'luci'/'system' renders a
 *     section with no values ("This section contains no values yet").
 *   - the map's own load check is `callSessionAccess('uci', <config>,
 *     'write')` (CBIMap.load) — the app's ACL group must carry the uci
 *     scope for the 'saadway' config, which is granted in
 *     luci-app-saadway/root/usr/share/rpcd/acl.d/luci-app-saadway.json.
 */
'use strict';

'require view';
'require rpc';
'require form';
'require poll';
'require ui';
'require uci';

var callSaadwayStatus = rpc.declare({
	object: 'saadway',
	method: 'mgmt.status',
	expect: { '': {} },
});

var callSaadwaySet = rpc.declare({
	object: 'saadway',
	method: 'mgmt.set',
	params: [ 'enabled' ],
	expect: { '': {} },
});

var callSaadwayCredentialsSet = rpc.declare({
	object: 'saadway',
	method: 'mgmt.credentials.set',
	params: [ 'username', 'password' ],
	expect: { '': {} },
});

var TAB_STATUS = 'status';
var TAB_CREDS = 'credentials';
var POLL_INTERVAL = 5;

var CONN_LABELS = {
	up: _('<span style="color:#107c41">Tunnel up</span>'),
	down: _('<span style="color:#d43f3a">Down</span>'),
	connecting: _('Connecting…'),
	contained: _('Contained (kill switch)'),
	cooldown: _('Failover cooldown'),
};

function statusText(status) {
	return CONN_LABELS[status] || status;
}

function statusRows(st) {
	var kv = [];

	kv.push(E('tr', {}, [
		E('td', { 'width': '33%' }, [ _('Service status') ]),
		E('td', {}, [ st.enabled ? _('Enabled') : _('Disabled') ]),
	]));
	kv.push(E('tr', {}, [
		E('td', { 'width': '33%' }, [ _('Connection status') ]),
		E('td', {}, [ statusText(st.connection_status) ]),
	]));
	kv.push(E('tr', {}, [
		E('td', { 'width': '33%' }, [ _('Active profile') ]),
		E('td', {}, [ String(st.active_profile || '-') ]),
	]));
	kv.push(E('tr', {}, [
		E('td', { 'width': '33%' }, [ _('Current interface') ]),
		E('td', {}, [ String(st.current_if || '-') ]),
	]));

	return kv;
}

return view.extend({
	load: function () {
		return Promise.all([
			callSaadwayStatus(),
			uci.load('saadway')
		]);
	},

	render: function (data) {
		var st = data[0] || {};
		var self = this;

		/* The map is bound to the SERVICE's own config: 'saadway' carries
		 * the state this page shows (config 'service'), and the app's ACL
		 * group grants the uci scope for it. No option ever writes UCI
		 * (every write() below is overridden), so neither Save & Apply nor
		 * Revert can touch the daemon's state — the daemon owns
		 * /etc/config/saadway and commits every write itself, so the
		 * revert only discards an empty delta. */
		var m = new form.Map('saadway',
			_('SaadWay VPN Tunnel & Routing'),
			_('Service lifecycle and the standalone dashboard.'));

		var s = m.section(form.TypedSection, 'service', _('Service'));
		s.anonymous = true;
		s.addremove = false;

		s.tab(TAB_STATUS, _('Status & Control'));
		s.tab(TAB_CREDS, _('Dashboard Credentials'));

		/* ---------------- Tab 1: status, toggle, dashboard link ----- */
		var statusBox = null;

		var o = s.taboption(TAB_STATUS, form.DummyValue, '_saadway_status',
			_('Service status'));
		o.cfgvalue = function () { return ''; };
		o.write = function () {};
		o.renderWidget = function (section_id, option_id, cfgvalue) {
			statusBox = E('div', { 'class': 'cbi-value' }, self.statusContent(st));
			return statusBox;
		};

		/* ---------------- Tab 2: the credential form ---------------- */
		/* The username is intentionally NOT pre-filled: reading the
		 * /etc/config/rpcd mapping would need a uci scope the panel does
		 * not hold, and a failed read would break the page (FR-033 — at
		 * most the username may ever be revealed). */
		var user = s.taboption(TAB_CREDS, form.Value, 'saadway_username',
			_('New Username'));
		user.optional = true;
		user.write = function () {};        /* never UCI */
		user.cfgvalue = function () { return ''; };

		var pass = s.taboption(TAB_CREDS, form.Value, 'saadway_password',
			_('New Password'));
		pass.password = true;
		pass.optional = true;
		pass.write = function () {};

		var conf = s.taboption(TAB_CREDS, form.Value, 'saadway_password2',
			_('Password Confirmation'));
		conf.password = true;
		conf.optional = true;
		conf.write = function () {};
		conf.validate = function (section_id, value) {
			var p = pass.formvalue(section_id);

			if (!value && !p)
				return true;
			if (value !== p)
				return _('The passwords do not match.');
			return true;
		};

		/* The writer is the username field: forcewrite makes it apply on
		 * every Save & Apply. The whole change is ONE ubus call — the
		 * browser neither writes UCI nor hashes the password (FR-021). */
		user.forcewrite = true;
		user.write = function (section_id) {
			var newUser = user.formvalue(section_id);
			var newPass = pass.formvalue(section_id);
			var newConf = conf.formvalue(section_id);

			if (!newPass && !newConf) {
				if (newUser === user.cfgvalue(section_id))
					return null;         /* nothing to change */
				return Promise.reject(new Error(_('The password is required.')));
			}
			if (!newUser)
				return Promise.reject(new Error(_('The user name is required.')));
			if (!newPass)
				return Promise.reject(new Error(_('The password is required.')));
			if (newPass !== newConf)
				return Promise.reject(new Error(_('The passwords do not match.')));

			return callSaadwayCredentialsSet(newUser, newPass).then(function () {
				ui.addNotification(null,
					E('p', {}, [ _('The dashboard credentials were updated.') ]),
					'info');
				return true;
			}).catch(function (e) {
				return Promise.reject(new Error(String(e)));
			});
		};

		return m.render().then(L.bind(function (map) {
			self.statusBox = statusBox;
			self.state = st;
			poll.add(L.bind(self.pollStatus, self), POLL_INTERVAL);
			return map;
		}, this));
	},

	/* the status rows, the enable toggle and the dashboard link */
	statusContent: function (st) {
		var self = this;
		var chk = E('input', { 'type': 'checkbox',
			'onclick': ui.createHandlerFn(this, function (ev) {
				return self.handleToggle(ev, ev.target.checked);
			}) });

		if (st.enabled)
			chk.checked = true;

		var dashUrl = 'http://' + window.location.hostname + '/saadway/';

		return [
			E('table', { 'class': 'table' }, statusRows(st)),
			E('div', { 'class': 'control-group', 'style': 'margin:1em 0' }, [
				E('label', { 'class': 'switch' }, [
					E('span', {}, [ _('Service enabled') ]), chk,
				]),
			]),
			E('div', { 'class': 'control-group' }, [
				E('a', {
					'class': 'btn',
					'href': dashUrl,
					'target': '_blank',
					'rel': 'noopener',
				}, [ _('Open Dashboard') ]),
			]),
		];
	},

	handleToggle: function (ev, enabled) {
		return callSaadwaySet({ enabled: enabled })
			.then(L.bind(this.reload, this))
			.catch(function (e) {
				ui.addNotification(null, [ E('p', {}, [ String(e) ]) ], 'error');
			});
	},

	/* the live status refresh touches ONLY the status widget — the
	 * credential fields the administrator typed must survive it */
	pollStatus: function () {
		var self = this;

		return callSaadwayStatus().then(function (st) {
			st = st || {};
			self.state = st;
			if (self.statusBox)
				self.statusBox.replaceChildren.apply(self.statusBox,
					self.statusContent(st));
		}).catch(function () {
			/* a transient RPC failure must not break the page */
		});
	},

	reload: function () {
		return this.pollStatus();
	},
});
