include $(TOPDIR)/rules.mk

# Name, version and release number
# The name and version of your package are used to define the variable to point to the build directory of your package: $(PKG_BUILD_DIR)
PKG_NAME:=lxi-tools
PKG_VERSION:=2.7
PKG_RELEASE:=1

PKG_BUILD_DEPENDS:=liblxi lua readline

# Source settings (i.e. where to find the source codes)
# This is a custom variable, used below
#SOURCE_DIR:=/home/fzs/dev/wrt_packages/mesonexempl/helloworld/src

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)
#  complete url: https://github.com/lxi-tools/lxi-tools/archive/refs/tags/v2.7.tar.gz
PKG_SOURCE:=v$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://github.com/lxi-tools/lxi-tools/archive/refs/tags
PKG_HASH:=6196980e82be2d143aa7f52e8e4612866b570cfce225d7d61698d2eeb1bf8a00

PKG_URL:=https://github.com/lxi-tools/lxi-tools

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/meson.mk
include $(INCLUDE_DIR)/nls.mk

# Package definition; instructs on how and where our package will appear in the overall configuration menu ('make menuconfig')
define Package/lxi-tools
  SECTION:=examples
  CATEGORY:=Examples
  TITLE:=lxi-tools
  DEPENDS:=+liblxi +liblua +libreadline +libtirpc
endef

# Package description; a more verbose description on what our package does
define Package/lxi-tools/description
  lxi-tools package from
endef

#define Build/Prepare
#		mkdir -p $(PKG_BUILD_DIR)
#		cp $(SOURCE_DIR)/build/* $(PKG_BUILD_DIR)
#endef

MESON_ARGS += -Dgui=false -Dbashcompletiondir="no"

#define Package/liblxi/configure
#	$(call Build/Configure/Default)
#endef
#
#
#define Package/liblxi/compile
#	$(call Build/Compile/Default)
#endef

define Package/lxi-tools/install
		$(INSTALL_DIR) $(1)/usr/bin/
		$(INSTALL_BIN)  $(PKG_INSTALL_DIR)/usr/bin/* $(1)/usr/bin
endef

$(eval $(call BuildPackage,lxi-tools))
