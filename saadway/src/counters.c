/*
 * counters.c — 1 Hz RX/TX throughput sampler (T034, FR-016)
 *
 * Reads /sys/class/net/<tunnel-if>/statistics/{rx,tx}_bytes for the
 * active tunnel interface and converts the delta into bps (Principle
 * III: counters bind to the materialized L3 interface).  Pure stdio —
 * no fork, no syscalls beyond open/read; main.c timers call
 * sw_counters_tick() every stats_interval seconds.
 */
#define _GNU_SOURCE
#include "saadway.h"

#include <stdio.h>
#include <string.h>

static uint64_t read_bytes(const char *ifname, const char *leaf)
{
	char path[128];
	char b[64];
	FILE *f;

	snprintf(path, sizeof(path),
		 "/sys/class/net/%s/statistics/%s", ifname, leaf);
	f = fopen(path, "r");
	if (!f)
		return 0;              /* interface gone — report zero */
	size_t n = fread(b, 1, sizeof(b) - 1, f);
	fclose(f);
	if (n == 0)
		return 0;
	b[n] = 0;
	return strtoull(b, NULL, 10);
}

void sw_counters_tick(struct sw_ctx *c)
{
	time_t now = time(NULL);

	if (!c->tun_if[0]) {
		c->ctr.rx_bps = 0;
		c->ctr.tx_bps = 0;
		c->ctr.last_rx = 0;
		c->ctr.last_tx = 0;
		c->ctr.last_ts = now;
		return;
	}
	uint64_t rx = read_bytes(c->tun_if, "rx_bytes");
	uint64_t tx = read_bytes(c->tun_if, "tx_bytes");

	if (c->ctr.last_ts && now > c->ctr.last_ts) {
		long dt = (long)(now - c->ctr.last_ts);
		/* bytes → bps: saturating delta avoids wrap artifacts */
		uint64_t drx = rx >= c->ctr.last_rx ? rx - c->ctr.last_rx : 0;
		uint64_t dtx = tx >= c->ctr.last_tx ? tx - c->ctr.last_tx : 0;

		c->ctr.rx_bps = (long)((drx * 8 + (uint64_t)dt / 2) /
				       (uint64_t)dt);
		c->ctr.tx_bps = (long)((dtx * 8 + (uint64_t)dt / 2) /
				       (uint64_t)dt);
	}
	c->ctr.last_rx = rx;
	c->ctr.last_tx = tx;
	c->ctr.last_ts = now;
}
