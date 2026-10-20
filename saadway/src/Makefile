# saadway/src/Makefile — plain source-tree build (Phase 13 / T058)
#
# The OpenWrt package Makefile keeps NO custom Build/Compile and delegates
# here: OpenWrt's default Build/Compile runs
#     $(MAKE) -C $(PKG_BUILD_DIR) \
#         CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)"
# so every toolchain variable below uses ?= — the cross values always win.
# The host unit suite lives in unit/ (own Makefile, dev-only) and is never
# part of this build tree.

CC      ?= cc
CFLAGS  ?= -O2
LDFLAGS ?=

MODULES:= backup counters dns failover health log main policy steering \
	tunnel-ikev2 tunnel-openvpn ubus uci-state
OBJS:= $(MODULES:%=%.o)

all: saadway

saadway: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lubus -lubox -luci -ljson-c -lpthread

%.o: %.c saadway.h
	$(CC) $(CFLAGS) -std=c11 -I. -c -o $@ $<

clean:
	rm -f $(OBJS) saadway

.PHONY: all clean
