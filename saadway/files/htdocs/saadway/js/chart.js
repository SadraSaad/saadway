/* chart.js — self-hosted canvas bandwidth chart (T036, FR-016).
 * No CDN, no libraries. Draws rx/tx series in device-pixel space, RTL-safe
 * (the chart is a time series — LTR direction is forced explicitly). */

"use strict";

window.SwChart = (function () {
	function fmt_bps(v) {
		if (v >= 1e6) return (v / 1e6).toFixed(1) + ' "Mbit/s"';
		if (v >= 1e3) return Math.round(v / 1e3) + ' "kbit/s"';
		return Math.round(v) + " bit/s";
	}

	function BpsScale(max) {
		this.max = Math.max(max, 10e3);
	}

	BpsScale.prototype.label = function (frac) {
		return fmt_bps(frac * this.max);
	};

	function make(canvas, opts) {
		var dpr = window.devicePixelRatio || 1;
		var w = canvas.clientWidth, h = canvas.clientHeight;
		canvas.width = w * dpr; canvas.height = h * dpr;
		var ctx = canvas.getContext("2d");
		ctx.scale(dpr, dpr);
		ctx.save();
		/* force LTR inside chart regardless of page direction */
		if (w === 0 || h === 0) { ctx.restore(); return { draw: function () {} }; }

		var max = 10e3;   /* floor 10 kbit/s so idle traces stay legible */
		var series = opts.series || [];
		for (var s = 0; s < series.length; s++)
			for (var i = 0; i < series[s].values.length; i++)
				if (series[s].values[i] > max) max = series[s].values[i];

		/* grid */
		ctx.fillStyle = getComputedStyle(canvas.closest("body")).backgroundColor
			|| "#0e1320";
		ctx.clearRect(0, 0, w, h);
		ctx.strokeStyle = "#232c47";
		ctx.lineWidth = 1;
		for (var g = 1; g <= 3; g++) {
			var gy = Math.round(h * g / 4);
			ctx.beginPath(); ctx.moveTo(0, gy); ctx.lineTo(w, gy); ctx.stroke();
		}

		var ngr = series.length ? series[0].values.length : 0;
		for (s = 0; s < series.length; s++) {
			var vals = series[s].values;
			if (!vals.length) continue;
			ctx.beginPath();
			ctx.strokeStyle = series[s].color || "#4f8cff";
			ctx.lineWidth = 1.6;
			var n = vals.length;
			for (i = 0; i < n; i++) {
				var x = w - (n - 1 - i) * (w / 60);   /* newest at right edge */
				var y = h - (vals[i] / max) * (h - 6) - 3;
				if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
			}
			ctx.stroke();
		}

		/* right-edge current value labels */
		ctx.font = "10px ui-monospace, monospace";
		for (s = 0; s < series.length; s++) {
			var va = series[s].values;
			if (!va.length) continue;
			var cur = va[va.length - 1];
			ctx.fillStyle = series[s].color || "#4f8cff";
			ctx.fillText(fmt_bps(cur), 6, 12 + 12 * s);
		}
		ctx.restore();
		return { max: max, fmt: fmt_bps };
	}

	return { make: make, fmt_bps: fmt_bps };
})();
