#!/bin/sh
# Paket ethercat-weigh im OpenWrt-SDK bauen und auf dem Zielgerät installieren.
#
#   scripts/deploy.sh            Tests, Frontend, Feeds, Paket bauen, hochladen, installieren, prüfen
#
# Umgebung:
#   OPENWRT_DIR   SDK-/Build-Verzeichnis (Default: ../../openwrt-sdk-25.12.6-mediatek-filogic_...
#                 relativ zu ethercat-weigh/, d. h. neben owrt-packages)
#   EW_TARGET     SSH-Ziel (Default: root@waage)
#   EW_VERBOSE=1  make mit V=s
#   EW_FEEDS_ALL=1  alle Feeds aktualisieren (Default: nur fazs)
#   EW_NO_TESTS=1   Host-Build mit Unit-Tests überspringen
set -eu

EW_DIR=$(cd "$(dirname "$0")/.." && pwd)
OPENWRT_DIR=${OPENWRT_DIR:-$EW_DIR/../../openwrt-sdk-25.12.6-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64}
EW_TARGET=${EW_TARGET:-root@192.168.128.231}
MAKEFILE=$EW_DIR/openwrt/ethercat-weigh/Makefile
EW_VERBOSE=1
step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

version=$(sed -n 's/^PKG_VERSION:=//p' "$MAKEFILE")
release=$(sed -n 's/^PKG_RELEASE:=//p' "$MAKEFILE")
pkg=ethercat-weigh-$version-r$release.apk
echo "ethercat-weigh $version-r$release -> $EW_TARGET"

if [ -z "${EW_NO_TESTS:-}" ]; then
    step "Unit-Tests (Host-Build in build/host)"
    cmake -S "$EW_DIR" -B "$EW_DIR/build/host" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$EW_DIR/build/host" -j"$(nproc)" --target ew_tests
    "$EW_DIR/build/host/ew_tests"
fi

step "Frontend bauen"
(cd "$EW_DIR/web" && npm run build)

cd "$OPENWRT_DIR"
export STAGING_DIR="$OPENWRT_DIR/staging_dir/toolchain-aarch64_cortex-a53_gcc-14.3.0_musl/"
if [ -n "${EW_FEEDS_ALL:-}" ]; then
    step "Alle Feeds aktualisieren"
    ./scripts/feeds update -a
else
    step "Feed fazs aktualisieren"
    ./scripts/feeds update fazs
fi

step "Pakete des Feeds fazs installieren"
./scripts/feeds install -a -p fazs
make defconfig >/dev/null

step "Paket bauen"
# Änderungen in src/ und web/ erkennt das SDK nicht (nur den Paketordner): immer neu vorbereiten
make package/ethercat-weigh/clean >/dev/null
make -j"$(nproc)" package/ethercat-weigh/compile ${EW_VERBOSE:+V=s}

apk=$(find bin/packages -path '*/fazs/*' -name "$pkg" | head -n 1)
[ -n "$apk" ] || { echo "Paket $pkg nicht gefunden" >&2; exit 1; }

step "Hochladen: $apk"
scp "$apk" "$EW_TARGET:/root/"

step "Installieren auf $EW_TARGET"
# Lokal gebautes Paket ist nicht signiert -> --allow-untrusted
ssh "$EW_TARGET" "set -e
  /etc/init.d/ethercat-weigh stop || true
  apk add --allow-untrusted /root/$pkg
  /etc/init.d/ethercat-weigh start"

step "Dienst prüfen"
# Läuft der Prozess nach einigen Sekunden noch mit derselben PID (kein Respawn) und antwortet die API?
ssh "$EW_TARGET" 'sleep 3; p1=$(pidof ethercat-weigh); sleep 3; p2=$(pidof ethercat-weigh)
  if [ -z "$p1" ] || [ "$p1" != "$p2" ]; then
    echo "ethercat-weigh läuft nicht stabil (PID \"$p1\" -> \"$p2\")" >&2
    logread -e ethercat-weigh | tail -n 20 >&2
    exit 1
  fi
  conf=/etc/ethercat-weigh/config.json
  port=$(jsonfilter -i $conf -e @.http.port 2>/dev/null || echo 8080)
  bind=$(jsonfilter -i $conf -e @.http.bind 2>/dev/null || echo 0.0.0.0)
  [ "$bind" = 0.0.0.0 ] && bind=127.0.0.1
  info=$(wget -q -T 5 -O - "http://$bind:$port/api/info") || { echo "/api/info antwortet nicht" >&2; exit 1; }
  echo "PID $p1, /api/info: $info"'

step "Fertig: $pkg auf $EW_TARGET installiert"
