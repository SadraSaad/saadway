# OpenWrt package Makefile for the LuCI SaadWay app (lifecycle page only).
#
# Constitution Principle V (strict dual-UI decoupling):
#   This view exposes exactly: service status, enable/disable toggle, and the
#   "Open Dashboard" link. It calls only saadway.mgmt.* through LuCI's own
#   authenticated ubus session. No configuration UI lives here (FR-022).
#
# T064: this is a STANDALONE package definition. The previous version
# delegated to the luci feed's own luci.mk include — a path that exists
# only inside a full OpenWrt source tree with the luci feed checked out.
# In an SDK drop-in (<sdk>/package/luci-app-saadway/) that include
# resolved to nothing, BuildPackage was never evaluated, the metadata
# scanner did not register the package, and the build failed with
# "No rule to make target 'package/luci-app-saadway/compile'".
#
# The package therefore uses the plain SDK machinery only:
#   include $(INCLUDE_DIR)/package.mk  →  an explicit Package/… block
#   $(eval $(call BuildPackage,luci-app-saadway))
# and installs its three assets explicitly:
#   * the view            → /www/luci-static/resources/view/saadway/
#   * its session ACL     → /usr/share/rpcd/acl.d/
#   * its menu entry      → /usr/share/luci/menu.d/
#
# The ACL group ("luci-app-saadway") is what makes the view's two
# saadway.mgmt.* calls work — upstream luci-base.json grants no ubus
# wildcard for third-party objects (docs/verify-2512.md §10, T048).
# The menu entry is what makes the page reachable at all: LuCI2's client
# router builds its route tree from /usr/share/luci/menu.d/*.json, so a
# view that no menu entry references is not reachable through the UI
# (FR-022, quickstart Scenario 4).
#
# DEPENDS: only the daemon. An SDK staging tree may not carry the luci
# feed, and the app is inert on a device without LuCI anyway — pull
# luci-base from an image package list when building a full image.

# T065: the version metadata MUST be declared at the TOP LEVEL, before
# include $(INCLUDE_DIR)/package.mk — upstream's BuildPackage evaluates
# $(Package/Default) BEFORE $(Package/luci-app-saadway), and Package/Default
# is what derives the package-scoped VERSION field from PKG_VERSION /
# PKG_RELEASE. Declaring them inside the package define is too late: the
# derivation already ran with an empty PKG_VERSION, VERSION stayed empty
# and the build aborted with
#   "Package/luci-app-saadway is missing the VERSION field".
# The core package declares these the same way (saadway/Makefile) and is
# why it packages as saadway-1.0.0-r1.apk.
PKG_NAME:=luci-app-saadway
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/package.mk

define Package/luci-app-saadway
	SECTION:=luci
	CATEGORY:=LuCI
	TITLE:=SaadWay service status/enable and dashboard link
	VERSION:=$(PKG_VERSION)-r$(PKG_RELEASE)
	DEPENDS:=+saadway
	PKGARCH:=all
	MAINTAINER:=SaadWay Maintainers <saadway@invalid.example>
	PKG_LICENSE:=Apache-2.0
endef

# T065: this package ships FILES only — no compiled sources. The default
# Build/Compile would run $(MAKE) -C $(PKG_BUILD_DIR) where no Makefile
# exists, and the default Build/Configure would look for a ./configure
# script that does not exist. Both are therefore explicitly empty.
define Build/Configure
endef

define Build/Compile
endef

define Package/luci-app-saadway/install
	$(INSTALL_DIR) $(1)/www/luci-static/resources/view/saadway
	$(INSTALL_DATA) ./htdocs/luci-static/resources/view/saadway/dashboard.js \
		$(1)/www/luci-static/resources/view/saadway/
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./root/usr/share/rpcd/acl.d/luci-app-saadway.json \
		$(1)/usr/share/rpcd/acl.d/
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d
	$(INSTALL_DATA) ./root/usr/share/luci/menu.d/luci-app-saadway.json \
		$(1)/usr/share/luci/menu.d/
endef

$(eval $(call BuildPackage,luci-app-saadway))
