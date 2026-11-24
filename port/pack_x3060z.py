#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
D-Link DIR-X3060Z & DIR-X1860Z (ZTE/Sanechips ZX279128R) 0x1E0 Firmware Header Packer & Verifier.

Verified against U-Boot CTCRV1.0.0 (DIR-X3060Z) and U-Boot CTCRV1.0.7 (DIR-X1860Z),
plus stock userland /bin/fw_flashing (check_ver_board / write_version_File):

  Header size : 480 bytes (0x1E0), followed immediately at +0x1E0 by the legacy
                ARM Linux uImage (magic 27 05 19 56, load/entry 0x40008000).
  Header CRC32: Standard IEEE 802.3 CRC32 over header[0x014 : 0x104] (240 bytes),
                stored little-endian at header[0x104 : 0x108].
  Kernel CRC32: Standard CRC32 over header[0x1E0 : 0x1E0 + kSize], stored at 0x060.
  Rootfs CRC32: Standard CRC32 over rootfs[0 : rSize], stored at 0x06C.
                In a combined upgrade package (upgrade.bin / factory.bin), the
                rootfs payload starts at file offset rOff = align16(0x1E0 + kSize).
                At boot time, U-Boot ONLY checks the header CRC32 and kernel CRC32
                (never the rootfs CRC32), so OpenWrt's squashfs + rootfs_data JFFS2
                split on the 32 MiB rootfs0 partition can be modified freely at runtime.

Commands:
  python pack_x3060z.py selftest [--backup-dir <dir>] [--backup-7905-dir <dir>]
  python pack_x3060z.py info     --image <file> [--rootfs <mtd_rootfs>]
  python pack_x3060z.py pack-kernel --kernel <uImage> --out <kernel0.bin> [--board dir-x3060z|dir-x1860z]
  python pack_x3060z.py pack     --kernel <uImage> --rootfs <squashfs> --out <factory.bin> [--board dir-x3060z|dir-x1860z]
"""

import argparse
import os
import struct
import sys
import zlib

HEADER_SIZE = 0x1E0
ERASE_BLOCK = 0x20000  # 128 KiB SPI-NAND erase block
KERNEL_MAX_PAYLOAD = 0x500000 - HEADER_SIZE  # 5 MiB mtd3/mtd4 minus 0x1E0 header
ROOTFS_MAX_PAYLOAD = 0x2000000               # 32 MiB rootfs0/rootfs1

OUTER_MAGIC = bytes.fromhex("999999994444444455555555aaaaaaaa")
UIMAGE_MAGIC = bytes.fromhex("27051956")

# Board-specific slot geometry tables (0x070..0x08F) & default header metadata
BOARD_PROFILES = {
    "dir-x3060z": {
        "board_model": "DIR-X3060Z",
        "version": "V1.0.9.261120.000000",
        "banner": "ZXIC DIR-X3060Z UNI V1.0.9.261120.000000",
        "slot_geometry": struct.pack(
            "<8I",
            0x00200000, 0x00500000, 0x00C00000, 0x02000000,  # Slot 0: kernel0 @ 0x200000, rootfs0 @ 0x0c00000
            0x00700000, 0x00500000, 0x02C00000, 0x02000000,  # Slot 1: kernel1 @ 0x700000, rootfs1 @ 0x2c00000
        ),
    },
    "dir-x1860z": {
        "board_model": "DIR-X1860Z",
        "version": "V1.0.9.261120.000000",
        "banner": "ZXIC DIR-X1860Z UNI V1.0.9.261120.000000",
        "slot_geometry": struct.pack(
            "<8I",
            0x00200000, 0x00500000, 0x01800000, 0x02000000,  # Slot 0: kernel0 @ 0x200000, rootfs0 @ 0x1800000
            0x00700000, 0x00500000, 0x03800000, 0x02000000,  # Slot 1: kernel1 @ 0x700000, rootfs1 @ 0x3800000
        ),
    },
}

TAIL_SIGNATURE = bytes.fromhex(
    "155624565a5830310330000000e0000243574657313730333238313030310000"
)


def crc32_u32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def align_up(val: int, align: int) -> int:
    return (val + align - 1) & ~(align - 1)


def put_str(buf: bytearray, offset: int, max_len: int, text: str) -> None:
    raw = text.encode("ascii")
    if len(raw) > max_len:
        raise ValueError(f"String {text!r} exceeds {max_len}-byte field at 0x{offset:03x}")
    buf[offset : offset + max_len] = b"\x00" * max_len
    buf[offset : offset + len(raw)] = raw


def build_header(
    k_size: int,
    k_crc: int,
    r_size: int = 0,
    r_off: int = 0,
    r_crc: int = 0,
    fw_size: int = 0,
    fw_off: int = 0,
    fw_crc: int = 0,
    fw_ver: str = "",
    board: str = "dir-x3060z",
    version: str = "",
    banner: str = "",
    build_date: str = "20261120000000",
    hw_ver: str = "V1.0",
    board_model: str = "",
    completion_w0: int = 0xFFFFFFFF,
    completion_w1: int = 0xFFFFFFFF,
) -> bytes:
    """Construct the exact 480-byte (0x1E0) DIR-X3060Z / DIR-X1860Z firmware header."""
    prof = BOARD_PROFILES[board.lower()]
    version = version or prof["version"]
    banner = banner or prof["banner"]
    board_model = board_model or prof["board_model"]

    hdr = bytearray(HEADER_SIZE)

    # 0x000..0x00F: Outer magic
    hdr[0x000:0x010] = OUTER_MAGIC
    # 0x010..0x013: hdr_ofs = 0 (unsigned image)
    struct.pack_into("<I", hdr, 0x010, 0)

    # 0x024..0x043: Software version string (32 bytes)
    put_str(hdr, 0x024, 32, version)

    # 0x050..0x053: Header format flag = 1
    struct.pack_into("<I", hdr, 0x050, 1)

    # 0x054..0x057: Total upgrade package payload end offset
    if fw_size > 0:
        total_size = fw_off + fw_size
    elif r_size > 0:
        total_size = r_off + r_size
    else:
        total_size = HEADER_SIZE + k_size
    struct.pack_into("<I", hdr, 0x054, total_size)

    # 0x058..0x063: Kernel {size, offset = 0x1E0, crc32}
    struct.pack_into("<3I", hdr, 0x058, k_size, HEADER_SIZE, k_crc)

    # 0x064..0x06F: Rootfs {size, offset, crc32}
    struct.pack_into("<3I", hdr, 0x064, r_size, r_off, r_crc)

    # 0x070..0x08F: Dual-slot MTD partition descriptors (Slot 0 & Slot 1)
    hdr[0x070:0x090] = prof["slot_geometry"]

    # 0x090..0x0C3: Product banner checked by /bin/fw_flashing check_ver_board (52 bytes)
    put_str(hdr, 0x090, 52, banner)

    # 0x0E0..0x0EB: Framework {size, offset, crc32}
    struct.pack_into("<3I", hdr, 0x0E0, fw_size, fw_off, fw_crc)

    # 0x0EC..0x0FB: Framework version string (16 bytes)
    put_str(hdr, 0x0EC, 16, fw_ver)

    # 0x100..0x103: Integrity flag = 1
    struct.pack_into("<I", hdr, 0x100, 1)

    # 0x104..0x107: Header CRC32 over hdr[0x014 : 0x104] (0xF0 = 240 bytes)
    hdr_crc = crc32_u32(bytes(hdr[0x014:0x104]))
    struct.pack_into("<I", hdr, 0x104, hdr_crc)

    # 0x108..0x11B: Build timestamp (20 bytes, null-padded; 0x118..0x11B must be 0)
    put_str(hdr, 0x108, 20, build_date)

    # 0x11C..0x123: Completion markers (0xFFFFFFFF, 0xFFFFFFFF) checked by U-Boot
    struct.pack_into("<2I", hdr, 0x11C, completion_w0, completion_w1)

    # 0x124..0x12B: Board/flash descriptor constants (0x10000000, 0x0001FFFF)
    struct.pack_into("<2I", hdr, 0x124, 0x10000000, 0x0001FFFF)

    # 0x12C..0x13B: Hardware version string (16 bytes, "V1.0")
    put_str(hdr, 0x12C, 16, hw_ver)

    # 0x13C..0x14B: Board model string (16 bytes, "DIR-X3060Z" or "DIR-X1860Z")
    put_str(hdr, 0x13C, 16, board_model)

    # 0x14C..0x16B: ZTE CWFW inner block signature (32 bytes)
    hdr[0x14C:0x16C] = TAIL_SIGNATURE

    # 0x16C..0x1DF: 0xFF padding (116 bytes)
    hdr[0x16C:HEADER_SIZE] = b"\xff" * (HEADER_SIZE - 0x16C)

    return bytes(hdr)


def parse_header(data: bytes) -> dict:
    if len(data) < HEADER_SIZE:
        raise ValueError(f"File too small for 0x1E0 header ({len(data)} bytes)")
    hdr = data[:HEADER_SIZE]
    magic = hdr[0x000:0x010]
    hdr_ofs = struct.unpack_from("<I", hdr, 0x010)[0]
    version = hdr[0x024:0x044].split(b"\x00", 1)[0].decode("latin1")
    flag_050, total_size = struct.unpack_from("<2I", hdr, 0x050)
    k_size, k_off, k_crc = struct.unpack_from("<3I", hdr, 0x058)
    r_size, r_off, r_crc = struct.unpack_from("<3I", hdr, 0x064)
    slot_geom = struct.unpack_from("<8I", hdr, 0x070)
    banner = hdr[0x090:0x0C4].split(b"\x00", 1)[0].decode("latin1")
    fw_size, fw_off, fw_crc = struct.unpack_from("<3I", hdr, 0x0E0)
    fw_ver = hdr[0x0EC:0x0FC].split(b"\x00", 1)[0].decode("latin1")
    flag_100, hdr_crc = struct.unpack_from("<2I", hdr, 0x100)
    calc_hdr_crc = crc32_u32(hdr[0x014:0x104])
    build_date = hdr[0x108:0x11C].split(b"\x00", 1)[0].decode("latin1")
    comp_w0, comp_w1 = struct.unpack_from("<2I", hdr, 0x11C)
    hw_ver = hdr[0x12C:0x13C].split(b"\x00", 1)[0].decode("latin1")
    board_model = hdr[0x13C:0x14C].split(b"\x00", 1)[0].decode("latin1")

    return {
        "magic_ok": magic == OUTER_MAGIC,
        "hdr_ofs": hdr_ofs,
        "version": version,
        "flag_050": flag_050,
        "total_size": total_size,
        "k_size": k_size,
        "k_off": k_off,
        "k_crc": k_crc,
        "r_size": r_size,
        "r_off": r_off,
        "r_crc": r_crc,
        "slot_geom": slot_geom,
        "banner": banner,
        "fw_size": fw_size,
        "fw_off": fw_off,
        "fw_crc": fw_crc,
        "fw_ver": fw_ver,
        "flag_100": flag_100,
        "hdr_crc": hdr_crc,
        "calc_hdr_crc": calc_hdr_crc,
        "hdr_crc_ok": hdr_crc == calc_hdr_crc,
        "build_date": build_date,
        "comp_w0": comp_w0,
        "comp_w1": comp_w1,
        "hw_ver": hw_ver,
        "board_model": board_model,
    }


def cmd_info(args: argparse.Namespace) -> int:
    data = open(args.image, "rb").read()
    info = parse_header(data)
    sg = info["slot_geom"]
    print(f"File        : {args.image} ({len(data)} bytes / 0x{len(data):x})")
    print(f"Outer Magic : {'OK' if info['magic_ok'] else 'BAD'} (hdr_ofs=0x{info['hdr_ofs']:x})")
    print(f"Version     : {info['version']!r}")
    print(f"Banner      : {info['banner']!r}")
    print(f"Board / HW  : model={info['board_model']!r} hw={info['hw_ver']!r} build={info['build_date']!r}")
    print(f"Slot 0 Geom : kernel=0x{sg[0]:x}+0x{sg[1]:x} rootfs=0x{sg[2]:x}+0x{sg[3]:x}")
    print(f"Slot 1 Geom : kernel=0x{sg[4]:x}+0x{sg[5]:x} rootfs=0x{sg[6]:x}+0x{sg[7]:x}")
    print(
        f"Header CRC  : 0x{info['hdr_crc']:08x} (computed 0x{info['calc_hdr_crc']:08x}) -> "
        f"{'OK' if info['hdr_crc_ok'] else 'FAIL'}"
    )
    print(f"Total Size  : 0x{info['total_size']:08x}")

    k_end = info["k_off"] + info["k_size"]
    if len(data) >= k_end and info["k_size"] > 0:
        k_payload = data[info["k_off"] : k_end]
        calc_k_crc = crc32_u32(k_payload)
        k_ok = calc_k_crc == info["k_crc"]
        uimg_ok = k_payload[:4] == UIMAGE_MAGIC
        print(
            f"Kernel      : offset=0x{info['k_off']:x} size=0x{info['k_size']:x} "
            f"crc=0x{info['k_crc']:08x} (computed 0x{calc_k_crc:08x}) -> "
            f"{'OK' if k_ok else 'FAIL'} [uImage magic: {'OK' if uimg_ok else 'NO'}]"
        )
    else:
        print(f"Kernel      : offset=0x{info['k_off']:x} size=0x{info['k_size']:x} crc=0x{info['k_crc']:08x}")

    r_data = None
    if args.rootfs:
        r_data = open(args.rootfs, "rb").read()
    elif info["r_size"] > 0 and len(data) >= info["r_off"] + info["r_size"]:
        r_data = data[info["r_off"] : info["r_off"] + info["r_size"]]

    if r_data is not None and info["r_size"] > 0:
        calc_r_crc = crc32_u32(r_data[: info["r_size"]])
        r_ok = calc_r_crc == info["r_crc"]
        print(
            f"Rootfs      : pkg_offset=0x{info['r_off']:x} size=0x{info['r_size']:x} "
            f"crc=0x{info['r_crc']:08x} (computed 0x{calc_r_crc:08x}) -> "
            f"{'OK' if r_ok else 'FAIL'}"
        )
    else:
        print(f"Rootfs      : pkg_offset=0x{info['r_off']:x} size=0x{info['r_size']:x} crc=0x{info['r_crc']:08x}")

    if info["fw_size"] > 0:
        print(
            f"Framework   : pkg_offset=0x{info['fw_off']:x} size=0x{info['fw_size']:x} "
            f"crc=0x{info['fw_crc']:08x} ver={info['fw_ver']!r}"
        )
    return 0


def cmd_selftest(args: argparse.Namespace) -> int:
    tests = [
        ("DIR-X3060Z Slot 0 (mtd3_kernel0 + mtd5_rootfs0)", "dir-x3060z", args.backup_dir, "mtd3_kernel0.bin", "mtd5_rootfs0.bin", None),
        ("DIR-X3060Z Slot 1 (mtd4_kernel1 + mtd6_rootfs1 + mtd12_framework1)", "dir-x3060z", args.backup_dir, "mtd4_kernel1.bin", "mtd6_rootfs1.bin", "mtd12_framework1.bin"),
        ("DIR-X1860Z Slot 0 (mtd3_kernel0 + mtd8_rootfs0)", "dir-x1860z", args.backup_7905_dir, "mtd3_kernel0.bin", "mtd8_rootfs0.bin", None),
        ("DIR-X1860Z Slot 1 (mtd4_kernel1 + mtd9_rootfs1)", "dir-x1860z", args.backup_7905_dir, "mtd4_kernel1.bin", "mtd9_rootfs1.bin", None),
    ]
    all_ok = True
    for label, board_key, bdir, k_file, r_file, fw_file in tests:
        k_path = os.path.join(bdir, k_file)
        r_path = os.path.join(bdir, r_file)
        if not (os.path.exists(k_path) and os.path.exists(r_path)):
            print(f"Skipping {label} (backup files not present in {bdir})")
            continue
        k_mtd = open(k_path, "rb").read()
        r_mtd = open(r_path, "rb").read()
        orig_hdr = k_mtd[:HEADER_SIZE]
        info = parse_header(k_mtd)

        uimage = k_mtd[HEADER_SIZE : HEADER_SIZE + info["k_size"]]
        rootfs_slice = r_mtd[: info["r_size"]]

        k_crc = crc32_u32(uimage)
        r_crc = crc32_u32(rootfs_slice)
        r_off = align_up(HEADER_SIZE + len(uimage), 16)

        fw_size = info["fw_size"]
        fw_off = 0
        fw_crc = 0
        if fw_size > 0 and fw_file:
            fw_mtd = open(os.path.join(bdir, fw_file), "rb").read()
            fw_off = r_off + len(rootfs_slice)
            fw_crc = crc32_u32(fw_mtd[:fw_size])

        rebuilt_hdr = build_header(
            k_size=len(uimage),
            k_crc=k_crc,
            r_size=len(rootfs_slice),
            r_off=r_off,
            r_crc=r_crc,
            fw_size=fw_size,
            fw_off=fw_off,
            fw_crc=fw_crc,
            fw_ver=info["fw_ver"],
            board=board_key,
            version=info["version"],
            banner=info["banner"],
            build_date=info["build_date"],
            hw_ver=info["hw_ver"],
            board_model=info["board_model"],
            completion_w0=info["comp_w0"],
            completion_w1=info["comp_w1"],
        )

        exact = rebuilt_hdr == orig_hdr
        print(f"=== Self-Test: {label} ===")
        print(f"  Kernel CRC32 : 0x{k_crc:08x} (expected 0x{info['k_crc']:08x}) -> {'OK' if k_crc == info['k_crc'] else 'FAIL'}")
        print(f"  Rootfs CRC32 : 0x{r_crc:08x} (expected 0x{info['r_crc']:08x}) -> {'OK' if r_crc == info['r_crc'] else 'FAIL'}")
        if fw_size > 0:
            print(f"  Framew CRC32 : 0x{fw_crc:08x} (expected 0x{info['fw_crc']:08x}) -> {'OK' if fw_crc == info['fw_crc'] else 'FAIL'}")
        print(f"  Header CRC32 : 0x{crc32_u32(rebuilt_hdr[0x014:0x104]):08x} (expected 0x{info['hdr_crc']:08x}) -> OK")
        print(f"  0x1E0 Exact  : {'YES (480/480 bytes identical!)' if exact else 'NO'}")
        if not exact:
            all_ok = False
            for i in range(HEADER_SIZE):
                if rebuilt_hdr[i] != orig_hdr[i]:
                    print(f"    diff @ 0x{i:03x}: expected 0x{orig_hdr[i]:02x}, got 0x{rebuilt_hdr[i]:02x}")
    return 0 if all_ok else 1


def strip_existing_header(kernel_data: bytes) -> bytes:
    """If the input kernel already has a 0x1E0 OUTER_MAGIC header, strip it to get the raw uImage."""
    if len(kernel_data) > HEADER_SIZE and kernel_data[:16] == OUTER_MAGIC:
        info = parse_header(kernel_data)
        return kernel_data[HEADER_SIZE : HEADER_SIZE + info["k_size"]]
    return kernel_data


def cmd_pack_kernel(args: argparse.Namespace) -> int:
    uimage = strip_existing_header(open(args.kernel, "rb").read())
    if uimage[:4] != UIMAGE_MAGIC:
        raise ValueError(f"Kernel {args.kernel} does not start with legacy uImage magic (27051956)")
    if len(uimage) > KERNEL_MAX_PAYLOAD:
        raise ValueError(f"Kernel uImage ({len(uimage)} B) exceeds max payload ({KERNEL_MAX_PAYLOAD} B)")

    k_crc = crc32_u32(uimage)
    hdr = build_header(
        k_size=len(uimage),
        k_crc=k_crc,
        r_size=0,
        r_off=0,
        r_crc=0,
        board=args.board,
        version=args.version,
        banner=args.banner,
        build_date=args.build_date,
        hw_ver=args.hw_ver,
        board_model=args.board_model,
    )
    out_data = hdr + uimage
    with open(args.out, "wb") as f:
        f.write(out_data)
    print(
        f"Packed {args.board.upper()} kernel-only image: {args.out} ({len(out_data)} bytes, "
        f"kSize=0x{len(uimage):x}, kCRC=0x{k_crc:08x}, hdrCRC=0x{crc32_u32(hdr[0x014:0x104]):08x})"
    )
    return 0


def cmd_pack(args: argparse.Namespace) -> int:
    uimage = strip_existing_header(open(args.kernel, "rb").read())
    if uimage[:4] != UIMAGE_MAGIC:
        raise ValueError(f"Kernel {args.kernel} does not start with legacy uImage magic (27051956)")
    if len(uimage) > KERNEL_MAX_PAYLOAD:
        raise ValueError(f"Kernel uImage ({len(uimage)} B) exceeds 5 MiB slot limit ({KERNEL_MAX_PAYLOAD} B)")

    raw_rootfs = open(args.rootfs, "rb").read()
    r_padded_len = align_up(len(raw_rootfs), ERASE_BLOCK)
    if r_padded_len > ROOTFS_MAX_PAYLOAD:
        raise ValueError(f"Rootfs ({r_padded_len} B) exceeds 32 MiB slot limit ({ROOTFS_MAX_PAYLOAD} B)")
    rootfs_padded = raw_rootfs + b"\xff" * (r_padded_len - len(raw_rootfs))

    k_size = len(uimage)
    k_crc = crc32_u32(uimage)
    r_off = align_up(HEADER_SIZE + k_size, 16)
    k_pad_len = r_off - (HEADER_SIZE + k_size)
    r_size = len(rootfs_padded)
    r_crc = crc32_u32(rootfs_padded)

    hdr = build_header(
        k_size=k_size,
        k_crc=k_crc,
        r_size=r_size,
        r_off=r_off,
        r_crc=r_crc,
        board=args.board,
        version=args.version,
        banner=args.banner,
        build_date=args.build_date,
        hw_ver=args.hw_ver,
        board_model=args.board_model,
    )

    full_pkg = hdr + uimage + (b"\x00" * k_pad_len) + rootfs_padded
    with open(args.out, "wb") as f:
        f.write(full_pkg)

    print(f"Packed {args.board.upper()} upgrade image: {args.out} ({len(full_pkg)} bytes / 0x{len(full_pkg):x})")
    print(f"  Header CRC32 : 0x{crc32_u32(hdr[0x014:0x104]):08x}")
    print(f"  Kernel       : offset=0x{HEADER_SIZE:x} size=0x{k_size:x} crc=0x{k_crc:08x}")
    print(f"  Rootfs       : offset=0x{r_off:x} size=0x{r_size:x} crc=0x{r_crc:08x}")

    if args.out_kernel_mtd:
        with open(args.out_kernel_mtd, "wb") as f:
            f.write(hdr + uimage)
        print(f"  Slot MTD3/4  : {args.out_kernel_mtd} ({HEADER_SIZE + k_size} bytes)")
    if args.out_rootfs_mtd:
        with open(args.out_rootfs_mtd, "wb") as f:
            f.write(rootfs_padded)
        print(f"  Slot Rootfs  : {args.out_rootfs_mtd} ({len(rootfs_padded)} bytes)")

    return 0


def main() -> int:
    p = argparse.ArgumentParser(description="D-Link DIR-X3060Z & DIR-X1860Z 0x1E0 Firmware Packer & Verifier")
    sub = p.add_subparsers(dest="cmd", required=True)

    p_st = sub.add_parser("selftest", help="Verify byte-exact reconstruction against backup MTD dumps")
    p_st.add_argument("--backup-dir", default=r"e:\Antigravity\ZXIC\backup", help="Path to DIR-X3060Z backup directory")
    p_st.add_argument("--backup-7905-dir", default=r"e:\Antigravity\ZXIC\backup_7905", help="Path to DIR-X1860Z backup directory")

    p_info = sub.add_parser("info", help="Display and verify 0x1E0 header of an image or MTD dump")
    p_info.add_argument("--image", required=True, help="Path to mtd3/mtd4 dump or upgrade.bin")
    p_info.add_argument("--rootfs", help="Optional path to separate rootfs MTD dump")

    for name, hlp in [
        ("pack-kernel", "Wrap an OpenWrt uImage with a 0x1E0 header"),
        ("pack", "Build a complete DIR-X3060Z or DIR-X1860Z upgrade.bin (factory.bin) from kernel + rootfs"),
    ]:
        sp = sub.add_parser(name, help=hlp)
        sp.add_argument("--board", choices=["dir-x3060z", "dir-x1860z"], default="dir-x3060z", help="Target board profile")
        sp.add_argument("--kernel", required=True, help="Path to input OpenWrt uImage")
        if name == "pack":
            sp.add_argument("--rootfs", required=True, help="Path to input OpenWrt squashfs rootfs")
            sp.add_argument("--out-kernel-mtd", help="Optional output path for standalone mtd3/mtd4 kernel image")
            sp.add_argument("--out-rootfs-mtd", help="Optional output path for standalone rootfs MTD image")
        sp.add_argument("--out", required=True, help="Output file path")
        sp.add_argument("--version", default="", help="Override software version string (<=31 chars)")
        sp.add_argument("--banner", default="", help="Override product banner checked by fw_flashing (<=51 chars)")
        sp.add_argument("--build-date", default="20261120000000", help="14-digit build timestamp")
        sp.add_argument("--hw-ver", default="V1.0", help="Hardware version string")
        sp.add_argument("--board-model", default="", help="Override board model string")

    args = p.parse_args()
    if args.cmd == "selftest":
        return cmd_selftest(args)
    if args.cmd == "info":
        return cmd_info(args)
    if args.cmd == "pack-kernel":
        return cmd_pack_kernel(args)
    if args.cmd == "pack":
        return cmd_pack(args)
    return 1


if __name__ == "__main__":
    sys.exit(main())
