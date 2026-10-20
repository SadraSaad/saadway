# saadway/src/Makefile — plain source-tree build (Phase 13 / T058)
#
# The OpenWrt package Makefile keeps NO custom Build/Compile and delegates
# here: OpenWrt's default Build/Compile runs
#     $(MAKE) -C $(PKG_BUILD_DIR) \
#         CFLAGS="$(TARGET_CFLAGS) … $(TARGET_CPPFLAGS) …" \
#         LDFLAGS="$(TARGET_LDFLAGS)" CROSS="$(TARGET_CROSS)"
# so every toolchain variable below uses ?= — the cross values always win.
# T060: upstream passes CROSS but NOT CC, so CC must default to the cross
# compiler (a host `cc` sees neither the target sysroot nor the staged
# headers); the staging include path arrives inside CFLAGS via
# TARGET_CPPFLAGS, and $(CPPFLAGS) is honored explicitly as well.
# The host unit suite lives in unit/ (own Makefile, dev-only) and is never
# part of this build tree.

# GNU make PREDEFINES CC (built-in default "cc"), and `CC ?= …` does NOT
# override a built-in default — so `?=` alone would keep the HOST compiler
# (upstream passes CROSS but never CC), which sees neither the target
# sysroot nor the staged headers. The origin guard substitutes the
# toolchain compiler only while CC still comes from make's built-in
# default; an explicitly provided CC (command line / environment, e.g. the
# SDK) always wins.
ifeq ($(origin CC),default)
CC := $(CROSS)gcc
endif
ifeq ($(origin CFLAGS),default)
CFLAGS := -O2
endif
# LDFLAGS/CPPFLAGS: no built-in values to fight — upstream passes LDFLAGS
# on the command line; CPPFLAGS stays empty unless a caller provides it.

MODULES:= backup counters dns failover health log main policy steering \
	tunnel-ikev2 tunnel-openvpn ubus uci-state
OBJS:= $(MODULES:%=%.o)

all: saadway

saadway: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lubus -lubox -luci -ljson-c -lpthread

%.o: %.c saadway.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -std=c11 -I. -c -o $@ $<

clean:
	rm -f $(OBJS) saadway

.PHONY: all clean
