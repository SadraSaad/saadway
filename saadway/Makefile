# OpenWrt package Makefile for the SaadWay core service package
#
# Constitution Section 2 compliance:
#   */fw4/nftables (firewall4), procd, ubus, uci only — no iptables/ipset/fw3.
#   * No banned runtimes (Node.js, full CPython, JVM). The service is a plain
#     C11 binary against musl, plus ash glue scripts under files/.
#
# Build-time verification items (docs/verify-2512.md):
#   T-verify-1: uhttpd-mod-ubus presence + ACL syntax in 25.12 feeds
#   T-verify-2: strongSwan package naming/versions (eap-mschapv2, gcm, xfrm)
#   T-verify-3: fw4 include strategy (this package intentionally owns the
#               standalone `table inet saadway` via atomic `nft -f` render;
#               see src/policy.c and files/core.nft)

include $(TOPDIR)/rules.mk

PKG_NAME:=saadway
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_MAINTAINER:=SaadWay Maintainers <saadway@invalid.example>
PKG_LICENSE:=GPL-2.0-only
PKG_LICENSE_FILES:=COPYING

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)
PKG_CONFIG_DEPENDS:= \
	CONFIG_PACKAGE_openvpn-openssl \
	CONFIG_PACKAGE_strongswan \
	CONFIG_PACKAGE_strongswan-swanctl \

include $(INCLUDE_DIR)/package.mk

define Package/saadway
	SECTION:=net
	CATEGORY:=Network
	TITLE:=SaadWay VPN tunnel & routing dashboard (core service)
	DEPENDS:= \
		+libubus +libubox +libuci +libblobmsg-json \
		+libjson-c \
		+firewall4 +kmod-nft-core +kmod-nft-nat \
		+procd +uci +ubus +dnsmasq-full \
		+ip-full \
		+openvpn-openssl \
		+strongswan +strongswan-swanctl \
		+strongswan-mod-eap-mschapv2 +strongswan-mod-gcm \
		+kmod-xfrm-interface \
		+uhttpd +uhttpd-mod-ubus +rpcd
endef

define Package/saadway/description
	Resident C daemon (saadwayd) providing multi-profile OpenVPN/IKEv2 client
	tunnel management with atomic nftables policy application, automatic
	failover with cool-down retry, zero-leak kill switch, forced-tunnel DNS
	and per-device/domain/IP traffic steering. Ships the standalone
	Persian-first RTL dashboard SPA served by uhttpd.
endef

define Package/saadway/conffiles
/etc/config/saadway
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	# T054: daemon sources/headers only — src/unit/ (host test Makefile,
	# shims, test sources, host ELF binaries) must never enter
	# PKG_BUILD_DIR (constitution Gate 2 footprint)
	$(CP) ./src/*.c ./src/*.h $(PKG_BUILD_DIR)/
endef

SAADWAY_MODULES:= main uci-state log ubus policy tunnel-openvpn \
	tunnel-ikev2 failover steering dns health counters backup
SAADWAY_OBJECTS:=$(patsubst %,$(PKG_BUILD_DIR)/%.o,$(SAADWAY_MODULES))

# T054: flags / sources / libs strictly separated; only libraries staged by
# DEPENDS are linked (T049 removed libmbedtls from DEPENDS — the daemon has
# no mbedtls consumer, so the stale -lmbedtls -lmbedcrypto broke clean-SDK
# link builds). LDFLAGS come after the object list, libs last. _GNU_SOURCE
# is intentionally NOT here: every module defines it in-source (a -D flag
# emits 13 redefinition warnings per build).
SAADWAY_CFLAGS:=$(TARGET_CFLAGS) -std=c11 -Os -I$(PKG_BUILD_DIR)
SAADWAY_LIBS:=-lubus -lubox -luci -ljson-c

$(PKG_BUILD_DIR)/%.o: $(PKG_BUILD_DIR)/%.c $(PKG_BUILD_DIR)/saadway.h
	$(TARGET_CC) $(SAADWAY_CFLAGS) -c -o $@ $<

define Build/Compile
	$(TARGET_CC) \
		-o $(PKG_BUILD_DIR)/saadwayd \
		$(SAADWAY_OBJECTS) \
		$(TARGET_LDFLAGS) \
		$(SAADWAY_LIBS)
endef

define Package/saadway/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/config $(1)/etc/init.d \
		$(1)/etc/saadway/secrets $(1)/etc/saadway/run/openvpn \
		$(1)/etc/saadway/swanctl \
		$(1)/usr/share/rpcd/acl.d $(1)/usr/libexec/saadway \
		$(1)/www/saadway
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/saadwayd $(1)/usr/sbin/saadwayd
	$(INSTALL_DATA) ./files/saadway.conf $(1)/etc/config/saadway
	$(INSTALL_BIN) ./files/saadway.init $(1)/etc/init.d/saadway
	$(INSTALL_DATA) ./files/core.nft $(1)/etc/saadway/core.nft
	$(INSTALL_DATA) ./files/rpc-acl.json $(1)/usr/share/rpcd/acl.d/saadway.json
	$(INSTALL_DATA) ./files/bypass-cidr.txt $(1)/etc/saadway/bypass-cidr.txt
	$(INSTALL_BIN) ./files/saadway-ovpn-updown $(1)/usr/libexec/saadway/ovpn-updown
	$(INSTALL_BIN) ./files/saadway-provision $(1)/usr/libexec/saadway/provision
	# standalone dashboard SPA (uhttpd serves /www as docroot)
	$(CP) ./files/htdocs/saadway/* $(1)/www/saadway/
	# uci-defaults: uhttpd /ubus wiring + LAN-only binding
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(CP) ./files/uci-defaults/* $(1)/etc/uci-defaults/
endef

$(eval $(call BuildPackage,saadway))
