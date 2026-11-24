# SPDX-License-Identifier: GPL-2.0-only
#
# OpenWrt sysupgrade handler for D-Link DIR-X1860Z & DIR-X3060Z (ZX279128R).
#
# Uses OpenWrt's standard sysupgrade-tar image (*-squashfs-sysupgrade.bin):
#   sysupgrade-<board>/CONTROL  -> board identity check
#   sysupgrade-<board>/kernel   -> 480-byte (0x1E0) header + legacy uImage -> mtd "kernel"
#   sysupgrade-<board>/root     -> SquashFS rootfs -> mtd "rootfs" (with -j backup preserve)
#
# Only the active OpenWrt slot ("kernel" + "rootfs") is written. U-Boot, the alternate
# Stock firmware slot ("kernel0"/"rootfs0" on DIR-X1860Z, "kernel1"/"rootfs1" on DIR-X3060Z),
# "others", "wlan" (RF calibration), and "tagparam" (factory MACs) are never modified.

. /lib/functions.sh
. /lib/functions/system.sh

REQUIRE_IMAGE_METADATA=1
RAMFS_COPY_BIN="tar dd sha256sum sed head tr cut"
RAMFS_COPY_DATA="/tmp/sysinfo/board_name"

x3060z_board_name() {
	local b
	b=$(board_name 2>/dev/null)
	[ -n "$b" ] || b=$(tr '\0' '\n' < /proc/device-tree/compatible 2>/dev/null | head -n 1)
	echo "$b"
}

# sha256 prefix (first 32 hex chars) of:
#   - 16-byte ZTE/D-Link 0x1E0 outer magic (99999999 44444444 55555555 aaaaaaaa)
#   - 4-byte legacy uImage magic (27 05 19 56)
#   - 4-byte little-endian SquashFS magic ("hsqs")
ZXIC_OUTER_MAGIC_SHA="3d25b5b9bf4079c6760449611963b6f1"
ZXIC_UIMAGE_MAGIC_SHA="45f39b4a3c7439d22f85c4e827db13e5"
ZXIC_SQUASHFS_MAGIC_SHA="53bff6664974960b06bcb260bd4b8349"

x3060z_get_board_dir() {
	local tar_file="$1"
	tar tf "$tar_file" 2>/dev/null | sed -n 's|/CONTROL$||p' | head -n 1
}

platform_check_image() {
	local tar_file="$1"
	local board expected_dir board_dir
	local k_magic u_magic r_magic

	[ "$#" -gt 1 ] && return 1

	board=$(x3060z_board_name)
	case "$board" in
	dlink,dir-x1860z|\
	dlink,dir-x3060z)
		;;
	*)
		echo "Sysupgrade is not supported on board '$board'"
		return 1
		;;
	esac

	if ! find_mtd_index kernel >/dev/null || ! find_mtd_index rootfs >/dev/null; then
		echo "Required MTD partitions 'kernel' and 'rootfs' not found"
		return 1
	fi

	board_dir=$(x3060z_get_board_dir "$tar_file")
	expected_dir="sysupgrade-$(echo "$board" | tr ',' '_')"
	if [ -z "$board_dir" ] || [ "$board_dir" != "$expected_dir" ]; then
		echo "Invalid sysupgrade archive: expected '$expected_dir', found '${board_dir:-<none>}'"
		return 1
	fi

	k_magic=$(tar xf "$tar_file" "${board_dir}/kernel" -O 2>/dev/null | dd bs=16 count=1 2>/dev/null | sha256sum | cut -c1-32)
	if [ "$k_magic" != "$ZXIC_OUTER_MAGIC_SHA" ]; then
		echo "Invalid kernel image: missing 0x1E0 ZTE/D-Link outer magic"
		return 1
	fi

	u_magic=$(tar xf "$tar_file" "${board_dir}/kernel" -O 2>/dev/null | dd bs=1 skip=480 count=4 2>/dev/null | sha256sum | cut -c1-32)
	if [ "$u_magic" != "$ZXIC_UIMAGE_MAGIC_SHA" ]; then
		echo "Invalid kernel image: missing legacy uImage magic at +0x1E0"
		return 1
	fi

	r_magic=$(tar xf "$tar_file" "${board_dir}/root" -O 2>/dev/null | dd bs=4 count=1 2>/dev/null | sha256sum | cut -c1-32)
	if [ "$r_magic" != "$ZXIC_SQUASHFS_MAGIC_SHA" ]; then
		echo "Invalid rootfs image: missing SquashFS magic (hsqs)"
		return 1
	fi

	return 0
}

platform_do_upgrade() {
	local tar_file="$1"
	local board_dir

	board_dir=$(x3060z_get_board_dir "$tar_file")
	[ -n "$board_dir" ] || {
		v "ERROR: unable to locate sysupgrade board directory in $tar_file"
		return 1
	}

	v "Flashing kernel (${board_dir}/kernel) to mtd 'kernel'..."
	if ! tar xOf "$tar_file" "${board_dir}/kernel" | mtd write - kernel; then
		v "ERROR: failed writing kernel partition"
		return 1
	fi

	if [ -n "$UPGRADE_BACKUP" ] && [ -f "$UPGRADE_BACKUP" ]; then
		v "Flashing rootfs (${board_dir}/root) to mtd 'rootfs' with config backup..."
		tar xOf "$tar_file" "${board_dir}/root" | mtd -j "$UPGRADE_BACKUP" write - rootfs
	else
		v "Flashing rootfs (${board_dir}/root) to mtd 'rootfs' (clean overlay)..."
		tar xOf "$tar_file" "${board_dir}/root" | mtd write - rootfs
	fi
}
