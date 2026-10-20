/* luci-app-saadway dashboard.js — LuCI controller page (T031, FR-022, Gate 6)
 *
 * Constitution Principle V — STRICT dual-UI decoupling: this LuCI view
 * exposes EXACTLY three controls:
 *   1. service status display (saadway.mgmt.status, through LuCI's own
 *      authenticated ubus session — never a custom token),
 *   2. enable/disable toggle (saadway.mgmt.set → { enabled:bool }),
 *   3. "Open Dashboard" link to the standalone SPA (/saadway/).
 * No settings, no profile management, no steering UI — those live ONLY in
 * the standalone dashboard (research D7). If you need more here, refuses:
 * every line added beyond this violates FR-022.
 */
'use strict';

'require rpc';
'require view';
'require poll';
'require ui';

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

function renderRows(st) {
	var kv = [];
	kv.push(E('tr', {}, [
		E('td', { 'width': '33%' }, [ _('Service status') ]),
		E('td', {}, [ st.enabled ? _('Enabled') : _('Disabled') ]),
	]));
	kv.push(E('tr', {}, [
		E('td', {}, [ _('Connection status') ]),
		E('td', {}, [ statusText(st.connection_status) ]),
	]));
	kv.push(E('tr', {}, [
		E('td', {}, [ _('Active profile') ]),
		E('td', {}, [ String(st.active_profile || '-') ]),
	]));
	kv.push(E('tr', {}, [
		E('td', {}, [ _('Current interface') ]),
		E('td', {}, [ String(st.current_if || '-') ]),
	]));

	return kv;
}

return view.extend({
	handleToggle: function (ev, enabled) {
		return callSaadwaySet({ enabled: enabled })
			.then(L.bind(this.reload, this))
			.catch(function (e) { ui.addNotification(null, [ E('p', {}, [ String(e) ]) ], 'error'); });
	},

	reload: function () {
		var self = this;
		return self.render().then(function (frag) {
			var t = document.getElementById('saadway-view');
			if (t && frag) t.replaceWith(frag);
			return Promise.resolve(frag);
		});
	},

	render: function () {
		return callSaadwayStatus().then(L.bind(function (st) {
			st = st || {};
			var rows = renderRows(st);

			var chk = E('input', { 'type': 'checkbox',
				'onclick': ui.createHandlerFn(this, function (ev) {
				return this.handleToggle(ev, ev.target.checked);
			}) });
			if (st.enabled) chk.checked = true;

			var body = E('table', { 'class': 'table' }, rows);
			var dashUrl = 'http://' + window.location.hostname + '/saadway/';

			var view = E('div', { 'id': 'saadway-view' }, [
				E('h2', {}, [ _('SaadWay VPN Tunnel & Routing') ]),
				E('div', { 'class': 'cbi-map-descr' },
					[ _('Service lifecycle only — configuration lives in the standalone dashboard.') ]),
				body,
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
			]);

			return Promise.resolve(view);
		}, this));
	},

	handleSave: null,           /* FR-022: no config UI — apply/save forms */
	handleSaveApply: null,      /* are forbidden on the LuCI surface */
	handleReset: null,
});
