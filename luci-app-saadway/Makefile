# OpenWrt package Makefile for the LuCI SaadWay app (lifecycle page only).
#
# Constitution Principle V (strict dual-UI decoupling):
#   This view exposes exactly: service status, enable/disable toggle, and the
#   "Open Dashboard" link. It calls only saadway.mgmt.* through LuCI's own
#   authenticated ubus session. No configuration UI lives here (FR-022).

include $(TOPDIR)/rules.mk

LUCI_TITLE:=SaadWay service status/enable and dashboard link
LUCI_DEPENDS:=+saadway +luci-base
LUCI_ARCH:=all

MAINTAINER:=SaadWay Maintainers <saadway@invalid.example>
PKG_LICENSE:=Apache-2.0

include $(TOPDIR)/feeds/luci/luci.mk

# htdocs/luci-static/resources/view/saadway/dashboard.js is installed by
# the shared luci.mk logic (define ending $(LUCI_NAME)); the root/ tree —
# including root/usr/share/rpcd/acl.d/luci-app-saadway.json (session ACL for
# this view's two mgmt.* calls; upstream luci-base.json grants no ubus
# wildcard for third-party objects) — is installed the same way.
