# DIR-X3060Z & DIR-X1860Z OpenWrt Port — Goal Marker & Roadmap

**Status:** Milestones M1–M9 (`DIR-X1860Z` v1.0 RC) Complete — **OpenWrt Linux 6.18.55 + Native `sysupgrade` Verified Live on `DIR-X1860Z`!**  
**Next Milestone:** M10 — UART Capture of Rare Warm-Reboot Stall & Public GitHub v1.0 Release

---

## Completed Milestones

- [x] **M1: Interface & System Audit (`DIR-X3060Z` & `DIR-X1860Z`)**
  - Audited web interface templates (`.gch` files) and configuration structure on both routers.
  - Enabled local maintenance services (`FTP`, `SSH`, `Telnet`) via `app_rac_conf_t.gch` on both routers.
- [x] **M2: Complete 128 MiB SPI-NAND Backups (`backup\` & `backup_7905\`)**
  - **`DIR-X3060Z` (`e:\Antigravity\ZXIC\backup\`):** Dumped all 13 MTD partitions (`mtd1`–`mtd13`), `fdt.dtb` -> `dir_x3060z_live.dts`, `MT7916_EEPROM.bin`, `paramtag.bin`, `vmlinux.bin`, XML configs, and `extracted_rootfs/`.
  - **`DIR-X1860Z` (`e:\Antigravity\ZXIC\backup_7905\`):** Dumped all 12 MTD partitions (`mtd1`–`mtd12`), `fdt.dtb` -> `dir_x1860z_live.dts`, `MT7915_EEPROM.bin` (3,778 B), `paramtag.bin`, `vmlinux.bin`, and XML configs with zero stability/Wi-Fi issues.
- [x] **M3: Bootloader (`CTCRV1.0.0` & `CTCRV1.0.7`) & Firmware Container Verification**
  - Confirmed `non secure uboot` on both routers (standard CRC32 validation only, no cryptographic signature check).
  - Decoded the 480-byte (`0x1E0`) slot header format on both routers (`kernel0` @ `0x00200000`, `kernel1` @ `0x00700000`).
  - Identified the critical partition offset difference between the two models (`rootfs0` is at `0x00c00000` on `DIR-X3060Z` vs `0x01800000` on `DIR-X1860Z`, where `0x00c00000..0x01800000` holds `usercfg`, `others`, and `wlan` on the `DIR-X1860Z`).
- [x] **M4: SoC Board Table, Ethernet PHY & GPIO/LED Mapping**
  - Confirmed identical internal GbE PHY MDIO map (`uni_phy` = `{0x0d, 0x0a, 0x0b, 0x0c}`, `phy10..phy13` at MDIO `10..13`) and identical primary GPIO pins (`6, 17, 53, 56..59` outputs; `0, 62` inputs) across both routers.
- [x] **M5: Author OpenWrt Device Trees & Unified Dual-Board Target Base-Files**
  - Created `e:\Antigravity\ZXIC\port\dts\zx279128r-dlink-dir-x3060z.dts` and `e:\Antigravity\ZXIC\port\dts\zx279128r-dlink-dir-x1860z.dts` (both mapping `rootfs` to `/dev/mtdblock3` so a single kernel commandline works on both).
  - Updated `11-mt76-caldata`, `e2631-wifi-eeprom`, and `e2631-wifi-on` to auto-detect `MT7916AN` (`0x7906`, `1679`) vs `MT7905DAN` (`0x7915`, `1579`).
  - Updated `02_network` (loads factory LAN/WAN MACs from `/tagparam/paramtag`) and `x3060z-fwslot` for both boards.
- [x] **M6: Write & Self-Test the Dual-Board `0x1E0` Firmware Packer (`e:\Antigravity\ZXIC\port\pack_x3060z.py`)**
  - Added `--board dir-x3060z | dir-x1860z` support to `pack_x3060z.py`.
  - Passed 100% byte-exact self-test (`480/480` header bytes identical) across **all 4 firmware slots** (`DIR-X3060Z` Slot 0 & Slot 1 + `DIR-X1860Z` Slot 0 & Slot 1).
- [x] **M7: Prepare OpenWrt Build Overlay & Guide (`e:\Antigravity\ZXIC\port\`)**
  - Updated `image/Makefile.x3060z`, `apply_x3060z.sh`, and `BUILD_GUIDE.md` to build images for both `dlink_dir-x3060z` and `dlink_dir-x1860z`.

- [x] **M8a: Compile Dual-Board OpenWrt 6.18.55 Images (`mint:~/zxic-openwrt` -> `e:\Antigravity\ZXIC\out\`)**
  - Compiled `openwrt-sanechips-generic-dlink_dir-x1860z-{initramfs.uImage,squashfs-factory.bin,squashfs-kernel0.bin,squashfs-rootfs0.bin,squashfs-sysupgrade.bin}` and `dlink_dir-x3060z-*` with `CONFIG_SOFT_FLOAT=y`, `CONFIG_CMDLINE_FORCE=y` (`root=/dev/mtdblock3`), `CONFIG_CACHE_L2X0_IO_LOCK=y`, LuCI, `wpad-mbedtls`, `kmod-nft-offload`, and both `kmod-mt7915-firmware` & `kmod-mt7916-firmware`.
  - Passed 100% pre-flight verification (`uImage` magic, `0x48008000` initramfs / `0x40008000` factory load addresses, appended DTB model & rootfs partition base, SquashFS `hsqs` magic, and `0x1E0` header/kernel/rootfs CRC32 checksums).

---

- [x] **M8b: Live Hardware Flash & Full Peripheral Verification on `DIR-X1860Z`**
  - Directly flashed `openwrt-sanechips-generic-dlink_dir-x1860z-squashfs-factory.bin` via Stock Web UI -> booted OpenWrt **Linux 6.18.55** into Slot 1 (`24.9 MiB` JFFS2 overlay) on the first try with Stock Slot 0 preserved untouched.
  - Verified factory MACs (`a4:2a:95:xx:xx:30` / `31`), WAN DHCP + internet (`ping 1.1.1.1` = `8.7 ms`), and dual-band Wi-Fi 6 (`MT7905DAN` + `MT7975DN`, 5 GHz verified at **`1020.6 MBit/s` 80 MHz HE-MCS 10 2x2**).
  - Diagnosed and fixed two MT7905DAN quirks (`0x190: 0xd2 -> 0x92` in `11-mt76-caldata` + automatic `sys_recovery=7` in `e2631-wifi-on`), corrected physical `DIR-X1860Z` GbE port labels in DTS (`phy10=wan`, `phy11=lan1`, `phy12=lan2`, `phy13=lan3`), enabled first-boot Wi-Fi, and added `kernel0` support to `x3060z-fwslot`.

---

- [x] **M9: Lean v1.0 RC Build, Native `sysupgrade`, LEDs/Buttons & Stock Rollback Verified (`DIR-X1860Z`)**
  - Verified live Stock Slot 0 rollback (`mtd erase others && reboot` -> Stock `V1.0.3.220121.164818`).
  - Verified all 7 front-panel LEDs (Power `&gpio0 6`, Internet `&gpio1 1`, Wi-Fi `&gpio3 5` `ACTIVE_HIGH`, plus hardware GbE PHY LEDs) and both buttons (Reset `&gpio0 0`, WPS `&gpio3 14`).
  - Implemented and live-verified 1-click LuCI `sysupgrade` (`lib/upgrade/platform.sh` with `RAMFS_COPY_DATA` + `mtd -j` config preservation), auto `fw4` Wi-Fi flowtable reload, `packet_steering=1`, and real 5 GHz factory MAC staging from `tagparam+0x66`.
  - Built lean public v1.0 RC images (`out/SHA256SUMS.txt`) and completed privacy/redaction sweep across all tracked docs.

---

- [x] **M10: UART Early-Boot Hang Root-Cause & Fix (`DIR-X1860Z` & `DIR-X3060Z`)**
  - Fixed ZTE `zx29` PL011 `earlycon` (`port/patches-6.18/900-amba-pl011-zte-earlycon.patch`) to capture live `[ 0.000000]` boot logs on UART.
  - Root-caused the intermittent early-boot hangs and `___slab_alloc+0x36c` panics to U-Boot's `do_mcupg` leaving the integrated Ethernet switch/BMU/DMA active (`buf=0x41000000`) across `Starting kernel ...`, overwriting `kmem_cache_node` (`0xc1000040`) whenever Ethernet frames arrived before `zx279128s-eth` probed.
  - Added full early switch/MAC/DMA/BMU/PP/`ZX_GLB_RESET` quiesce in `port/files/arch/arm/mach-zte/zx279128s.c` (`mdesc->reserve`) and verified **6/6 consecutive boots (5/5 warm reboots)** with both WAN and LAN connected.
  - Built final v1.0 release binaries (`factory.bin`, `sysupgrade.bin`, `initramfs.uImage`, `SHA256SUMS.txt`) for both `DIR-X1860Z` and `DIR-X3060Z`. *(Note: original `DIR-X3060Z` test unit suffered an unrelated hardware failure prior to flashing; `DIR-X3060Z` images remain built and pre-flight verified.)*
