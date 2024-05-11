include $(TOPDIR)/rules.mk

# Name, version and release number
# The name and version of your package are used to define the variable to point to the build directory of your package: $(PKG_BUILD_DIR)
PKG_NAME:=liblxi
PKG_VERSION:=1.20
PKG_RELEASE:=1

#PKG_BUILD_DEPENDS:=meson/host

# Source settings (i.e. where to find the source codes)
# This is a custom variable, used below
#SOURCE_DIR:=/home/fzs/dev/wrt_packages/mesonexempl/helloworld/src

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

PKG_SOURCE:=v$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://github.com/lxi-tools/liblxi/archive/refs/tags
PKG_HASH:=4ee8dc2daea6bf581c1da32c51c4cb08e3f3b42d4c77d8a19777f5bbae93f57a

PKG_URL:=https://github.com/lxi-tools/liblxi

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/meson.mk
include $(INCLUDE_DIR)/nls.mk

# Package definition; instructs on how and where our package will appear in the overall configuration menu ('make menuconfig')
define Package/liblxi
  SECTION:=examples
  CATEGORY:=Examples
  TITLE:=liblxi
  DEPENDS:=+libavahi-client +avahi-daemon +libtirpc +libxml2-dev
endef

# Package description; a more verbose description on what our package does
define Package/liblxi/description
  liblxi package from Github LXI-Tools
endef

#define Build/Prepare
#		mkdir -p $(PKG_BUILD_DIR)
#		cp $(SOURCE_DIR)/build/* $(PKG_BUILD_DIR)
#endef

#define Build/Configure
#	$(call Build/Configure/Default)
#endef
#
#
#define Package/Compile
#	$(call Build/Compile/Default)
#endef

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include
	$(CP) $(PKG_BUILD_DIR)/src/lxi.h $(1)/usr/include/

	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/liblxi* $(1)/usr/lib

	$(INSTALL_DIR) $(1)/usr/lib/pkgconfig
	$(CP) $(PKG_INSTALL_DIR)/usr/lib/pkgconfig/liblxi.pc $(1)/usr/lib/pkgconfig
endef

define Package/liblxi/install
	$(INSTALL_DIR) $(1)/usr/lib/
	$(CP)  $(PKG_INSTALL_DIR)/usr/lib/liblxi.so $(1)/usr/lib/
	$(CP)  $(PKG_INSTALL_DIR)/usr/lib/liblxi.so.* $(1)/usr/lib/
endef

$(eval $(call BuildPackage,liblxi))
