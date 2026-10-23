#
# OpenWrt-Package ethercat-weigh
#
# import feed (the whole owrt-packages repository is the feed):
#   echo "src-link fazs /my-path/owrt-packages" >> feeds.conf.default
#   ./scripts/feeds update fazs && ./scripts/feeds install ethercat-weigh
#
# Do not add ethercat-weigh/openwrt as a separate feed in addition,
# otherwise the package is defined twice.
#
# Required prerequisites:
# - Frontend built (web/dist)
# - Submodules checked out
#

include $(TOPDIR)/rules.mk

PKG_NAME:=ethercat-weigh
PKG_VERSION:=0.1.0
PKG_RELEASE:=1

PKG_MAINTAINER:=fazs <12977360+fazs@users.noreply.github.com>

EW_SOURCE_ROOT:=$(abspath $(CURDIR)/../..)

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

CMAKE_OPTIONS += -DEW_BUILD_TESTS=OFF

define Package/ethercat-weigh
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=EtherCAT EL3356 Weighing with Webinterface
  DEPENDS:=+libstdcpp
endef

define Package/ethercat-weigh/description
  EtherCAT-Master (SOEM) to use with EK1100 and EL3356-0010, reckognizes "valid" 
  measurement with user-defined parameters, shows temporary statistics.
  Provides web-interface on port 8080 and live updates through REST/WebSocket-API
endef

define Package/ethercat-weigh/conffiles
/etc/ethercat-weigh/config.json
endef

define Build/Prepare
	@if [ ! -f "$(EW_SOURCE_ROOT)/web/dist/index.html" ]; then \
		echo "web/dist missing: in repository; run 'cd web && npm ci && npm run build'"; \
		exit 1; \
	fi
	@if [ ! -f "$(EW_SOURCE_ROOT)/third_party/SOEM/soem/ethercat.h" ]; then \
		echo "third_party missing: run 'git submodule update --init'"; \
		exit 1; \
	fi
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) $(EW_SOURCE_ROOT)/CMakeLists.txt $(EW_SOURCE_ROOT)/src $(EW_SOURCE_ROOT)/tests \
		$(EW_SOURCE_ROOT)/third_party $(PKG_BUILD_DIR)/
	mkdir -p $(PKG_BUILD_DIR)/web
	$(CP) $(EW_SOURCE_ROOT)/web/dist $(PKG_BUILD_DIR)/web/
endef

define Package/ethercat-weigh/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/ethercat-weigh $(1)/usr/bin/
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/ethercat-weigh.init $(1)/etc/init.d/ethercat-weigh
	$(INSTALL_DIR) $(1)/etc/ethercat-weigh
	$(INSTALL_CONF) ./files/config.json $(1)/etc/ethercat-weigh/config.json
	$(INSTALL_DIR) $(1)/www/ethercat-weigh
	$(CP) $(PKG_BUILD_DIR)/web/dist/* $(1)/www/ethercat-weigh/
endef

$(eval $(call BuildPackage,ethercat-weigh))
