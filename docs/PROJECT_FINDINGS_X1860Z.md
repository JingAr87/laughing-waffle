# D-Link DIR-X1860Z (ZTE ZX279128R + MT7905DAN/MT7975DN) — Complete Hardware, Firmware & OpenWrt Porting Report

**Last Updated:** 2026-11-20  
**Device Under Test:** D-Link `DIR-X1860Z` (TM / Unifi Malaysia AX1800 ISP build)  
**Hardware Revision:** `V1.0`  
**Active Firmware (Slot 0 & Slot 1):** `V1.0.3.220121.164818` (`ZXIC DIR-X1860Z UNI V1.0.3.220121.164818`, built 2022-04-14)  
**Bootloader:** `U-Boot CTCRV1.0.7` (`20220414131149`, **Non-Secure U-Boot**)  
**Serial Number:** `RG6DL60Z<redacted>`  
**Base MACs (`/tagparam/paramtag`):** `a4:2a:95:xx:xx:30` through `a4:2a:95:xx:xx:34` (`a4:2a:95:xx:xx:35` in `MT7915_EEPROM.bin`)

---

## 1. Hardware Architecture & Comparison with DIR-X3060Z

| Component | D-Link `DIR-X1860Z` (AX1800) | D-Link `DIR-X3060Z` (AX3000) | Compatibility / Notes |
| :--- | :--- | :--- | :--- |
| **SoC** | ZTE / Sanechips `ZX279128R` (`cputype = 3`, dual Cortex-A9 @ ~1.0 GHz, soft-float) | Identical (`ZX279128R`, `cputype = 3`) | **100% Identical** (`sanechips` target) |
| **RAM** | 256 MiB DDR3 (`0x40000000`–`0x4fffffff`) | 256 MiB DDR3 (`0x40000000`–`0x4fffffff`) | **100% Identical** |
| **Flash** | 128 MiB 3.3V SPI-NAND (`0x08000000`, 2 KiB page, 128 KiB eraseblock) on `spifc@94406000` | 128 MiB 3.3V SPI-NAND on `spifc@94406000` | Same chip size & controller, **different MTD partition offsets** (see Section 2) |
| **Wi-Fi 6** | MediaTek **`MT7905DAN` + `MT7975DN`** (chip ID `0x7915`, `15 79` in `/wlan/MT7915_EEPROM.bin`, 3,778 B) | MediaTek **`MT7916AN` + `MT7976DN`** (chip ID `0x7916`, `16 79` in `/wlan/MT7916_EEPROM.bin`) | Both use **`kmod-mt7915e`** (`mt76`); X1860Z loads `kmod-mt7915-firmware`, X3060Z loads `kmod-mt7916-firmware` |
| **Ethernet PHYs** | 4× integrated GbE PHYs (`uni_phy` @ `0xc060b6e4` = `{0x0d, 0x0a, 0x0b, 0x0c}`, GIC IRQs 71–74) | 4× integrated GbE PHYs (`uni_phy` @ `0xc05d1a90` = `{0x0d, 0x0a, 0x0b, 0x0c}`, GIC IRQs 71–74) | **100% Identical** (`phy10..phy13` at MDIO `10..13`) |
| **GPIO LEDs & Buttons** | Board ID `0x46` (`F628R`) / `0x47` (`F428S`) @ `0xc05f42bc`: Outputs `6, 17, 53, 56..59`; Inputs `0, 62` | Board ID `0x47` (`F628S`) @ `0xc05ba580`: Outputs `6, 17, 41, 53, 56..59`; Inputs `0, 62` | **100% Identical primary LED, button, and PCIe reset GPIOs** |
| **Bootloader** | `U-Boot CTCRV1.0.7` (`non secure uboot`, `0x1E0` header + legacy `uImage` at `+0x1E0`) | `U-Boot CTCRV1.0.0` (`non secure uboot`, `0x1E0` header + legacy `uImage` at `+0x1E0`) | **100% Identical `0x1E0` header format & CRC32 algorithm** |

---

## 2. Complete 128 MiB SPI-NAND Flash Partition Table (`vmlinux.bin` @ `0xc0458638`)

All 12 flash partitions (plus appended `fdt.dtb`, `MT7915_EEPROM.bin`, `paramtag.bin`, and decrypted XML configs) have been dumped and verified in `e:\Antigravity\ZXIC\backup_7905\`:

| MTD | Name | Size | Eraseblock | Flash Offset (`0xc0458638`) | Filesystem / Format | Local Backup File |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `mtd0` | `whole flash` | `0x08000000` (128 MiB) | `0x20000` | `0x00000000` | Full chip span | Covered by `mtd1`–`mtd12` |
| `mtd1` | `u-boot` | `0x00200000` (2 MiB) | `0x20000` | `0x00000000` | `U-Boot CTCRV1.0.7` (`non secure uboot`) | `backup_7905/mtd1_uboot.bin` |
| `mtd2` | `parameter tags` | `0x00300000` (3 MiB) | `0x20000` | **`0x06800000`** | JFFS2 (`/tagparam/paramtag` MACs at `+0x36`) | `backup_7905/mtd2_tagparam.bin` |
| `mtd3` | `kernel0` | `0x00500000` (5 MiB) | `0x20000` | **`0x00200000`** | `0x1E0` header + `uImage` (`V1.0.3.220121`, **active**) | `backup_7905/mtd3_kernel0.bin` |
| `mtd4` | `kernel1` | `0x00500000` (5 MiB) | `0x20000` | **`0x00700000`** | `0x1E0` header + `uImage` (`V1.0.3.220121`, backup) | `backup_7905/mtd4_kernel1.bin` |
| `mtd5` | `usercfg` | `0x00800000` (8 MiB) | `0x20000` | **`0x00c00000`** | JFFS2 (`/userconfig`: `cfg/db_user_cfg.xml`, etc.) | `backup_7905/mtd5_usercfg.bin` |
| `mtd6` | `others` | `0x00100000` (1 MiB) | `0x20000` | **`0x01400000`** | Dual-slot selector (`0xFF` erased = default Slot 0) | `backup_7905/mtd6_others.bin` |
| `mtd7` | `wlan` | `0x00300000` (3 MiB) | `0x20000` | **`0x01500000`** | JFFS2 (`/wlan`: `MT7915_EEPROM.bin` 3,778 B caldata) | `backup_7905/mtd7_wlan.bin` |
| `mtd8` | `rootfs0` | `0x02000000` (32 MiB) | `0x20000` | **`0x01800000`** | JFFS2 (Slot 0 rootfs, currently **active** `/`) | `backup_7905/mtd8_rootfs0.bin` |
| `mtd9` | `rootfs1` | `0x02000000` (32 MiB) | `0x20000` | **`0x03800000`** | JFFS2 (Slot 1 rootfs, currently **inactive**) | `backup_7905/mtd9_rootfs1.bin` |
| `mtd10` | `framework` | `0x00800000` (8 MiB) | `0x20000` | `0x05800000` | JFFS2 (`/ipks`, Framework slot 0, active) | `backup_7905/mtd10_framework.bin` |
| `mtd11` | `framework1` | `0x00800000` (8 MiB) | `0x20000` | `0x06000000` | JFFS2 (Framework slot 1, inactive) | `backup_7905/mtd11_framework1.bin` |
| `mtd12` | `apps` | `0x01500000` (21 MiB) | `0x20000` | **`0x06b00000`** | UBIFS (`ubi0_0` mounted at `/ctcap` + overlays) | `backup_7905/mtd12_apps.bin` |

> **Why Dumping Before Flashing Was Critical:**
> On the `DIR-X1860Z`, `usercfg` (`0x00c00000`, 8 MiB), `others` (`0x01400000`, 1 MiB), and `wlan` (`0x01500000`, 3 MiB — holding `MT7915_EEPROM.bin`) sit between `kernel1` (`0x00700000`) and `rootfs0` (`0x01800000`). On the `DIR-X3060Z`, `rootfs0` starts at `0x00c00000`. Giving the `DIR-X1860Z` its own Device Tree ([zx279128r-dlink-dir-x1860z.dts](file:///e:/Antigravity/ZXIC/port/dts/zx279128r-dlink-dir-x1860z.dts)) prevents `rootfs` from overlapping `usercfg`/`others`/`wlan` and points `wlan` and `tagparam` at their exact flash offsets (`0x01500000` and `0x06800000`).

---

## 3. Bootloader (`U-Boot CTCRV1.0.7`) & `0x1E0` Header Verification

1. **Non-Secure Bootloader:**
   - `mtd1_uboot.bin` contains `non secure uboot` and uses `hdr_ofs = 0` (unsigned `0x1E0` header + legacy `uImage` at `+0x1E0`).
   - Live U-Boot environment at `mtd1_uboot.bin+0x180000` (`CRC = 0xd1bf0443`):
     ```text
     baudrate=115200
     bootcmd=setenv bootargs console=$(console) root=/dev/mtdblock8 ro rootfstype=jffs2  mem=$(memsize);bootm 0x440001e0;
     bootdelay=1
     console=ttyAMA0,115200n8
     ethact=eth0
     fullfile=upgrade.bin
     ipaddr=192.168.1.1
     serverip=192.168.1.100
     linuzfile=vmlinuz.bin
     loadaddr=0x44000000
     memsize=256M
     versioninfo=U-Boot CTCRV1.0.7 20220414131149 0x200000 0x0 0x83 0x82
     ```
2. **480-Byte (`0x1E0`) Header Fields & CRC32 Verification (`mtd3_kernel0.bin` & `mtd4_kernel1.bin`):**
   - `0x024..0x043`: Version string `"V1.0.3.220121.164818"`
   - `0x054..0x057`: Total payload size `0x017465b0`
   - `0x058..0x063`: Kernel `{size = 0x002463cf, offset = 0x1e0, crc32 = 0x7af7050f}` — **Verified `OK`**
   - `0x064..0x06F`: Rootfs `{size = 0x01500000, offset = 0x002465b0, crc32 = 0x5c37a3a6}` — **Verified `OK`** on both `mtd8_rootfs0.bin` and `mtd9_rootfs1.bin`
   - `0x070..0x07F`: Slot 0 descriptor `{0x00200000, 0x00500000, 0x01800000, 0x02000000}`
   - `0x080..0x08F`: Slot 1 descriptor `{0x00700000, 0x00500000, 0x03800000, 0x02000000}`
   - `0x090..0x0C3`: Product banner `"ZXIC DIR-X1860Z UNI V1.0.3.220121.164818"`
   - `0x104..0x107`: Header CRC32 = `CRC32(header[0x014 : 0x104]) = 0x32d9e79a` — **Verified `OK`**
   - `0x12C..0x14B`: Hardware version `"V1.0"`, Model `"DIR-X1860Z"`
   - **Packer Self-Test (`python port/pack_x3060z.py selftest`):** Reconstructs both Slot 0 and Slot 1 headers of the `DIR-X1860Z` (as well as both slots of the `DIR-X3060Z`) with **480 / 480 bytes identical**.

---

## 6. Live OpenWrt 6.18.55 Hardware Verification (`2026-11-20`)

Flashed `openwrt-sanechips-generic-dlink_dir-x1860z-squashfs-factory.bin` (`10,938,080` B) directly via the stock Web UI (`Management & Diagnosis -> System Management -> Software Upgrade`). Stock `/bin/fw_flashing` accepted the `0x1E0` container on the first attempt, wrote OpenWrt into **Slot 1** (`0x00700000` + `0x03800000`), preserved **Stock Slot 0** untouched, and booted directly into **OpenWrt Linux 6.18.55** with LuCI on `192.168.1.1`.

### Verified Working Subsystems
- **System & Overlay:** `D-Link DIR-X1860Z` (`dlink,dir-x1860z`), `Linux 6.18.55 SMP armv7l`, `24.9 MiB` JFFS2 `rootfs_data` overlay (`23.6 MiB` free).
- **Factory MACs:** Loaded from `/tagparam/paramtag` (`a4:2a:95:xx:xx:30` LAN, `a4:2a:95:xx:xx:31` WAN).
- **WAN & NAT:** DHCP client on physical WAN (`192.168.0.192/24` upstream), `ping 1.1.1.1` = `8.7 ms`, `0%` loss.
- **Dual-Band Wi-Fi 6 (`MT7905DAN` + `MT7975DN`):**
  - 2.4 GHz (`phy0-ap0`): Ch 1, **HE40**, `26 dBm`, scanning neighbors at `-43 dBm`, client association & DHCP verified.
  - 5 GHz (`phy1-ap0`): Ch 36, **HE80**, `25 dBm`, scanning neighbors at `-46 dBm`, live client verified at **`1020.6 MBit/s`** (`80 MHz HE-MCS 10`, 2x2 MIMO, `-50 dBm [-53, -53]`).

### Key Live-Hardware Discoveries Folded Into `port/`
1. **MT7905DAN EEPROM Byte `0x190` (`0xd2 -> 0x92`):** D-Link's factory `MT7915_EEPROM.bin` sets `0x190` (`MT_EE_WIFI_CONF0`) to `0xd2` (`MT_EE_BAND_SEL_DUAL`), causing `mt76_init_sband()` to register 5 GHz on `phy0` and initialize `phy0->chandef` to 5 GHz Ch 36. Starting 2.4 GHz then sent `MCU_EXT_CMD_SET_RX_PATH` (`0x00004eed`) for 5 GHz on `band_idx=0`, hanging the WM firmware. Patching byte `0x190` `0xd2 -> 0x92` (`MT_EE_BAND_SEL_2GHZ`) in `11-mt76-caldata` fixes 2.4 GHz while keeping D-Link's `0x194..0x197` (`28 00 00 15`) FEM/LNA calibration bytes (+51 dB RX sensitivity vs generic upstream EEPROM).
2. **MT7905DAN WFDMA0 RX Init Quirk (`sys_recovery=7`):** On `ZX279128R` + `MT7905DAN` (`0x7915`), the initial `mt7915e` probe leaves WFDMA0 RX (`queue 0` & `queue 3`) stalled (`0` RX frames) until a single L0 full recovery (`echo 7 > /sys/kernel/debug/ieee80211/phy0/mt76/sys_recovery`) runs. Added automatically to `port/base-files/sbin/e2631-wifi-on` right after binding `0x7915`.
3. **Physical GbE Port Mapping Verified (`DIR-X1860Z`):**
   - `gmac_port0` (`phy10`, MDIO `0x0a`) = physical **WAN**
   - `gmac_port1` (`phy11`, MDIO `0x0b`) = physical **LAN1**
   - `gmac_port2` (`phy12`, MDIO `0x0c`) = physical **LAN2**
   - `gmac_port3` (`phy13`, MDIO `0x0d`) = physical **LAN3**
4. **LEDs, PCIe Reset & Buttons Verified (`DIR-X1860Z` & `DIR-X3060Z`):**
   - **Power LED:** `&gpio0 6 GPIO_ACTIVE_HIGH` (`slot 53`, verified live).
   - **IE / Internet LED:** `&gpio1 1 GPIO_ACTIVE_HIGH` (`slot 4`, verified live).
   - **Wi-Fi LED:** `&gpio3 5 GPIO_ACTIVE_HIGH` (`flat GPIO 53`, `slot 61` in `bspdriver.ko` `hw_led_wlan_mtk`!). Previously stuck solid ON because `&pcie0` inherited E2631's `reset-gpios = <&gpio3 5 GPIO_ACTIVE_LOW>`.
   - **Actual PCIe Reset Pins (`vmlinux.bin` @ `0x5062a6` / `0x4d567d`):** `pcie0` = **flat GPIO 52 (`&gpio3 4 GPIO_ACTIVE_LOW`)**, `pcie1` = **flat GPIO 54 (`&gpio3 6 GPIO_ACTIVE_LOW`)** on both `DIR-X1860Z` and `DIR-X3060Z`.
   - **LAN1–LAN3 + Globe (WAN) LEDs:** Hardware-driven by the integrated GbE PHYs (`gpio3 8..11`).
   - **Buttons:** **Reset** (`&gpio0 0 GPIO_ACTIVE_LOW`) and **WPS** (`&gpio3 14 GPIO_ACTIVE_LOW`) both verified live (`in hi` idle -> `in lo` pressed).

### Resolved UART Diagnostics (`2026-11-23`)
- **Intermittent Warm-Reboot / Early-Boot Hang & `___slab_alloc+0x36c` Panic — FIXED:**
  - **Root cause:** Enabling live ZTE `zx29` `earlycon` (`port/patches-6.18/900-amba-pl011-zte-earlycon.patch`) revealed that every early hang/panic was caused by U-Boot's `do_mcupg` initializing the integrated Ethernet switch, MACs, BMU (`0x92348000`), and DMA (`0x92350000`) with buffer pool `buf=0x41000000` on every boot and jumping to Linux without halting them. Incoming Ethernet frames during the first `0.62s` of kernel boot overwrote `kmem_cache_node` at physical `0x41000040` (`0xc1000040`), triggering `___slab_alloc+0x36c` panics or CPU 0 RCU stalls.
  - **Fix:** Added a `zx279128s_reserve()` hook in `port/files/arch/arm/mach-zte/zx279128s.c` (`mdesc->reserve`, before `early_ioremap_reset()` and slab/L2/SMP init) that disables all 5 MAC ports (`0x92200000 + p*0x40000`), clears `ZX_DMA_CTRL_EN` (`0x92350000`) and `ZX_BMU_CTRL` (`0x92348000`), resets PP (`0x92380000 = 2`), and holds `ZX_GLB_RESET = 0` (`0x92000008`) until `zx279128s-eth` probes.
  - **Verification:** Passed **6/6 consecutive boots (5/5 warm reboots, including a full post-Wi-Fi reboot)** with both WAN and LAN3 actively connected — zero panics, zero stalls, WAN `ping 1.1.1.1 = 8.6 ms` (`0%` loss).
- **`fw4` Flowtable Timing — FIXED:** Added automatic `(sleep 12; /etc/init.d/firewall reload)` in `e2631-wifi-on` and `network.globals.packet_steering=1` in `70_x3060z_defaults`; verified `phy0-ap0` and `phy1-ap0` automatically join `inet fw4 flowtable ft`.
- **Note (Direct Sanechips-to-Sanechips LAN Link):** Connecting a `ZTE F6600P` (`ZX279128S`) LAN port directly to a `DIR-X1860Z` (`ZX279128R`) LAN port showed no link LEDs on either end (same cable worked normally on a PC NIC — likely `Generic PHY` Auto-MDIX edge case).
