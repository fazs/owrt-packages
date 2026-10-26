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
# prerequisites:
# - Frontend built (web/dist)
# - Submodules checked out
#

include $(TOPDIR)/rules.mk

PKG_NAME:=ethercat-weigh
PKG_VERSION:=0.2.1
PKG_RELEASE:=14

PKG_MAINTAINER:=Fabian <fabiannd@gmail.com>

# Quellen liegen im selben Repository (zwei Ebenen über diesem Makefile)
EW_SOURCE_ROOT:=$(abspath $(CURDIR)/../..)

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

# Version für CMake (EW_VERSION); PKG_VERSION ist die einzige Quelle
CMAKE_OPTIONS += -DEW_BUILD_TESTS=OFF -DEW_PKG_VERSION=$(PKG_VERSION)

define Package/ethercat-weigh
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=EtherCAT EL3356 Waegeauswertung mit Webinterface
  DEPENDS:=+libstdcpp
endef

define Package/ethercat-weigh/description
  EtherCAT-Master (SOEM) fuer EK1100 + EL3356-0010, Erkennung gueltiger
  Messungen (statische Waegung), temporaere Statistik, REST/WebSocket-API
  und Web-Frontend.
endef

define Package/ethercat-weigh/conffiles
/etc/ethercat-weigh/config.json
endef

define Build/Prepare
	@if [ ! -f "$(EW_SOURCE_ROOT)/web/dist/index.html" ]; then \
		echo "web/dist fehlt: im Repository 'cd web && npm ci && npm run build' ausfuehren"; \
		exit 1; \
	fi
	@if [ ! -f "$(EW_SOURCE_ROOT)/third_party/SOEM/soem/ethercat.h" ]; then \
		echo "third_party fehlt: 'git submodule update --init' ausfuehren"; \
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
