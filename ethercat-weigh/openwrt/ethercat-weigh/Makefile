#
# OpenWrt-Package für ethercat-weigh
#
# Einbindung als Feed (Pfad anpassen):
#   echo "src-link ew /pfad/zu/ethercat-weigh/openwrt" >> feeds.conf.default
#   ./scripts/feeds update ew && ./scripts/feeds install ethercat-weigh
#
# Voraussetzung: Frontend gebaut (web/dist), Submodules ausgecheckt.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=ethercat-weigh
PKG_VERSION:=0.1.0
PKG_RELEASE:=1

PKG_MAINTAINER:=fazs <12977360+fazs@users.noreply.github.com>

# Quellen liegen im selben Repository (zwei Ebenen über diesem Makefile)
EW_SOURCE_ROOT:=$(abspath $(CURDIR)/../..)

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

CMAKE_OPTIONS += -DEW_BUILD_TESTS=OFF

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
