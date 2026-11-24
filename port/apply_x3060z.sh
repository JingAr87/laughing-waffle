#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Apply the D-Link DIR-X3060Z & DIR-X1860Z OpenWrt port overlay on top of the
# sanechips target tree (Zlion-Y/openwrt-sanechips-mirror + zte-e2631-openwrt base patches).
#
# Usage (inside Linux or WSL2):
#   1. First apply the base sanechips / E2631 kernel patches to your OpenWrt tree:
#        ./repos/zte-e2631-openwrt-main/apply.sh openwrt
#   2. Then layer the DIR-X3060Z / DIR-X1860Z board files, DTS, and 0x1E0 packer:
#        ./port/apply_x3060z.sh openwrt

set -eu

TREE="${1:-openwrt}"
HERE="$(cd "$(dirname "$0")" && pwd)"

[ -d "$TREE/target/linux/sanechips" ] || {
	echo "ERROR: $TREE/target/linux/sanechips not found." >&2
	echo "Clone https://github.com/Zlion-Y/openwrt-sanechips-mirror (branch pr-zte-zxhn-h3600) first." >&2
	exit 1
}

echo "[1/7] Copying DIR-X3060Z & DIR-X1860Z Device Trees..."
mkdir -p "$TREE/target/linux/sanechips/dts"
cp "$HERE/dts/zx279128r-dlink-dir-x3060z.dts" \
   "$TREE/target/linux/sanechips/dts/zx279128r-dlink-dir-x3060z.dts"
cp "$HERE/dts/zx279128r-dlink-dir-x1860z.dts" \
   "$TREE/target/linux/sanechips/dts/zx279128r-dlink-dir-x1860z.dts"

echo "[2/7] Installing ZX279128S/R kernel fixes (U-Boot eth DMA quiesce, ZTE earlycon, eth TX/RX perf)..."
mkdir -p "$TREE/target/linux/sanechips/patches-6.18"
mkdir -p "$TREE/target/linux/sanechips/files/arch/arm/mach-zte"
mkdir -p "$TREE/target/linux/sanechips/files/drivers/net/ethernet/zte"
cp "$HERE/patches-6.18/900-amba-pl011-zte-earlycon.patch" \
   "$TREE/target/linux/sanechips/patches-6.18/900-amba-pl011-zte-earlycon.patch"
cp "$HERE/patches-6.18/901-arm-compressed-zx279128s-quiesce-eth.patch" \
   "$TREE/target/linux/sanechips/patches-6.18/901-arm-compressed-zx279128s-quiesce-eth.patch"
cp "$HERE/files/arch/arm/mach-zte/zx279128s.c" \
   "$TREE/target/linux/sanechips/files/arch/arm/mach-zte/zx279128s.c"
cp "$HERE/files/drivers/net/ethernet/zte/zx279128s-eth.h" \
   "$TREE/target/linux/sanechips/files/drivers/net/ethernet/zte/zx279128s-eth.h"
cp "$HERE/files/drivers/net/ethernet/zte/zx279128s-main.c" \
   "$TREE/target/linux/sanechips/files/drivers/net/ethernet/zte/zx279128s-main.c"

echo "[3/7] Removing E2631-only flash/LED/userdata/Chinese scripts..."
BF="$TREE/target/linux/sanechips/base-files"
rm -f \
	"$BF/etc/init.d/e2631-trial" \
	"$BF/etc/init.d/e2631-userdata" \
	"$BF/etc/init.d/e2631-leds" \
	"$BF/etc/init.d/e2631-hb" \
	"$BF/etc/diag.sh" \
	"$BF/lib/preinit/79_e2631_userdata" \
	"$BF/lib/upgrade/platform.sh" \
	"$BF/etc/uci-defaults/99_e2631_lang" \
	"$BF/etc/uci-defaults/61_e2631_timezone" \
	"$BF/etc/uci-defaults/62_e2631_netune_cron" \
	"$BF/etc/config/fstab"

echo "[4/7] Copying D-Link DIR-X3060Z / DIR-X1860Z base-files (caldata, MAC loader, Wi-Fi bind, sysupgrade, slot switcher)..."
cp -r "$HERE/base-files/." "$BF/"
chmod +x "$BF/etc/hotplug.d/firmware/11-mt76-caldata"
chmod +x "$BF/etc/board.d/01_leds"
chmod +x "$BF/etc/board.d/02_network"
chmod +x "$BF/etc/init.d/e2631-wifi-eeprom"
chmod +x "$BF/etc/uci-defaults/70_x3060z_defaults"
chmod +x "$BF/sbin/e2631-wifi-on"
chmod +x "$BF/sbin/x3060z-fwslot"
chmod 0644 "$BF/lib/upgrade/platform.sh"
chmod 0644 "$BF/etc/config/wireless"

# Update e2631-wifi-boot default fallback SSID and Malaysia country code
if [ -f "$BF/sbin/e2631-wifi-boot" ]; then
	sed -i 's/SSID=E2631/SSID=OpenWrt-ZXIC/g' "$BF/sbin/e2631-wifi-boot"
	sed -i 's/E2631-OpenWrt/OpenWrt-ZXIC/g' "$BF/sbin/e2631-wifi-boot"
fi
if [ -f "$BF/sbin/e2631-wifi-ap" ]; then
	sed -i 's/country=CN/country=MY/g' "$BF/sbin/e2631-wifi-ap"
	sed -i 's/E2631-OpenWrt/OpenWrt-ZXIC/g' "$BF/sbin/e2631-wifi-ap"
fi

echo "[5/7] Installing DIR-X3060Z / DIR-X1860Z 0x1E0 firmware packer (tools/pack_x3060z.py)..."
mkdir -p "$TREE/tools"
cp "$HERE/pack_x3060z.py" "$TREE/tools/pack_x3060z.py"
chmod +x "$TREE/tools/pack_x3060z.py"

echo "[6/7] Wiring image/Makefile.x3060z into target/linux/sanechips/image/Makefile..."
cp "$HERE/image/Makefile.x3060z" "$TREE/target/linux/sanechips/image/Makefile.x3060z"
IMG="$TREE/target/linux/sanechips/image/Makefile"
if ! grep -q 'Makefile.x3060z' "$IMG"; then
	sed -i 's|^$(eval $(call BuildImage))$|include $(TOPDIR)/target/linux/sanechips/image/Makefile.x3060z\n\n$(eval $(call BuildImage))|' "$IMG"
fi

echo "[7/7] Setting kernel command line for Slot rootfs (mtdblock3 on both X3060Z and X1860Z)..."
CFG="$TREE/target/linux/sanechips/config-6.18"
if [ -f "$CFG" ]; then
	sed -i -e '/^CONFIG_CMDLINE_BOOL=y$/d' -e '/^CONFIG_CMDLINE_FORCE=y$/d' \
	       -e '/^CONFIG_CMDLINE=/d' "$CFG"
	cat >> "$CFG" << 'EOF'
CONFIG_CMDLINE_BOOL=y
CONFIG_CMDLINE="console=ttyAMA0,115200n8 earlycon root=/dev/mtdblock3 rootfstype=squashfs,jffs2 ro rootwait"
CONFIG_CMDLINE_FORCE=y
EOF
fi

echo "Done! Select Target System: 'Sanechips ZX279128S' -> Target Profiles: 'D-Link DIR-X1860Z V1.0' & 'D-Link DIR-X3060Z V1.0'"
