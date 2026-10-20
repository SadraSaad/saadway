#!/bin/sh
# pkg-metadata-audit.sh — host-side emulation of the OpenWrt package
# metadata contract (T065, Gate 1/2 evidence).
#
# The SDK aborts a package with
#   "Package/<name> is missing the VERSION field"
# when the package-scoped VERSION is empty — and upstream derives that
# field from PKG_VERSION/PKG_RELEASE inside Package/Default, which
# BuildPackage evaluates BEFORE the package-specific define. Declaring
# the version fields inside the package define is therefore too late.
#
# This script reproduces upstream's evaluation order and field check with
# a stub rules.mk/package.mk, so the failure surfaces on the build host
# instead of in an SDK build. It also enforces the file-only-package
# rule: a package that ships no sources must define Build/Configure and
# Build/Compile as empty blocks (the defaults would run make in a
# directory without a Makefile / look for a ./configure script).
#
# Usage: tests/pkg-metadata-audit.sh [pkg-dir ...]
#   default: every package directory in the repo (saadway, luci-app-saadway)
#
# Exit: 0 = all packages valid, 1 = at least one violation

set -u

REPO=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

die() {
	echo "pkg-metadata-audit: $1" >&2
	exit 2
}

command -v make >/dev/null 2>&1 || die "make not present"

# ---------------------------------------------------------------------
# stub of the OpenWrt build system (upstream semantics, openwrt@main):
#   include/package-defaults.mk -> Package/Default derives VERSION
#   include/package.mk         -> BuildPackage evaluates Default FIRST,
#                                 then the package define, then checks
#                                 TITLE/CATEGORY/SECTION/VERSION
# ---------------------------------------------------------------------
mkdir -p "$TMP/sdk"
cat > "$TMP/sdk/rules.mk" <<'EOF'
# stub of the OpenWrt top-level rules.mk (host-side metadata audit)
EOF
cat > "$TMP/sdk/package.mk" <<'EOF'
# stub of include/package.mk + include/package-defaults.mk
INSTALL_DIR:=install -d
INSTALL_DATA:=install -m644
PKG_MAINTAINER:=
PKG_SOURCE_URL:=
PKG_UNPACK:=

define Package/Default
  SECTION:=opt
  CATEGORY:=Extra packages
  MAINTAINER:=$(PKG_MAINTAINER)
  ifneq ($(PKG_VERSION),)
    ifneq ($(PKG_RELEASE),)
      VERSION:=$(PKG_VERSION)-r$(PKG_RELEASE)
    else
      VERSION:=$(PKG_VERSION)
    endif
  else
    VERSION:=$(PKG_RELEASE)
  endif
  PKGARCH:=$(ARCH)
endef

define BuildPackage
  $(eval $(call Package/Default))
  $(eval $(call Package/$(1)))
  $(foreach FIELD, TITLE CATEGORY SECTION VERSION,
    ifeq ($($(FIELD)),)
      $$(error Package/$(1) is missing the $(FIELD) field)
    endif
  )
  $(info AUDIT_BUILD_PACKAGE:$(1))
endef
EOF
cat > "$TMP/probe.mk" <<'EOF'
audit-probe:
	@echo "AUDIT_VERSION=$(VERSION)"
	@echo "AUDIT_PKG_VERSION=$(PKG_VERSION)"
	@echo "AUDIT_PKG_RELEASE=$(PKG_RELEASE)"
	@echo "AUDIT_COMPILE_STATE=$(if $(filter undefined,$(origin Build/Compile)),undefined,$(if $(strip $(Build/Compile)),nonempty,empty))"
	@echo "AUDIT_CONFIGURE_STATE=$(if $(filter undefined,$(origin Build/Configure)),undefined,$(if $(strip $(Build/Configure)),nonempty,empty))"
	@echo "AUDIT_HAS_SOURCES=$(if $(strip $(PKG_SOURCE_URL)),yes,no)"
	@echo "AUDIT_HAS_PREPARE=$(if $(strip $(Build/Prepare)),yes,no)"
EOF

if [ "$#" -gt 0 ]; then
	PKGS="$*"
else
	PKGS=$(find "$REPO" -maxdepth 2 -name Makefile -type f \
		-path '*/saadway/Makefile' -o -maxdepth 2 -name Makefile -type f \
		-path '*/luci-app-saadway/Makefile' 2>/dev/null | sed 's|/Makefile$||' | sort)
fi
[ -n "$PKGS" ] || die "no package directories found"

rc=0
for p in $PKGS; do
	[ -f "$p/Makefile" ] || die "no Makefile in $p"
	name=$(basename "$p")
	printf '%-20s ' "$name"

	out=$(make -f "$p/Makefile" -f "$TMP/probe.mk" \
		TOPDIR="$TMP/sdk" INCLUDE_DIR="$TMP/sdk" ARCH=all \
		audit-probe 2>&1)
	if [ $? -ne 0 ]; then
		echo "FAIL (package metadata rejected by the emulated build system)"
		echo "$out" | sed 's/^/    /' >&2
		rc=1
		continue
	fi

	ver=$(echo "$out" | sed -n 's/^AUDIT_VERSION=//p')
	pv=$(echo "$out" | sed -n 's/^AUDIT_PKG_VERSION=//p')
	pr=$(echo "$out" | sed -n 's/^AUDIT_PKG_RELEASE=//p')
	ce=$(echo "$out" | sed -n 's/^AUDIT_COMPILE_STATE=//p')
	gre=$(echo "$out" | sed -n 's/^AUDIT_CONFIGURE_STATE=//p')
	srcs=$(echo "$out" | sed -n 's/^AUDIT_HAS_SOURCES=//p')
	prep=$(echo "$out" | sed -n 's/^AUDIT_HAS_PREPARE=//p')

	problem=""
	[ -n "$ver" ] || problem="VERSION is empty"
	if [ "$srcs" = "no" ] && [ "$prep" = "no" ]; then
		# a package that ships files only must define both blocks as EMPTY:
		# "undefined" would fall back to upstream's default (make in a
		# directory without a Makefile), "nonempty" would try to build
		# something that does not exist
		[ "$ce" = "empty" ] || problem="$problem; Build/Compile is '$ce' (must be an empty block)"
		[ "$gre" = "empty" ] || problem="$problem; Build/Configure is '$gre' (must be an empty block)"
	fi
	if [ -n "$problem" ]; then
		echo "FAIL ($problem)"
		rc=1
		continue
	fi

	echo "OK (version=$ver, pkg=$pv-$pr)"
done

if [ $rc -ne 0 ]; then
	echo "pkg-metadata-audit: FAIL — at least one package violates the build-system metadata contract" >&2
	exit 1
fi
echo "pkg-metadata-audit: OK — all packages satisfy the OpenWrt metadata contract"
exit 0
