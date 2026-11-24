# D-Link DIR-X3060Z (ZTE ZX279128R) — Complete Hardware, Firmware & OpenWrt Porting Report

**Last Updated:** 2026-11-19  
**Device Under Test:** D-Link `DIR-X3060Z` (TM / Unifi Malaysia ISP build)  
**Hardware Revision:** `V1.0`  
**Active Firmware:** `DIR-X3060Z_240719_1.0.6` (`V1.0.6.240719.095339`, built 2024-07-24)  
**Inactive Slot Firmware:** `V1.0.2.220522.164210` (built 2022-08-08)  
**Bootloader:** `U-Boot CTCRV1.0.0` (`20220808184253`, **Non-Secure U-Boot**)  
**Serial Number:** `RG6DLAX3<redacted>`  
**Base MACs (`/tagparam/paramtag`):** `c8:78:7d:xx:xx:04` through `c8:78:7d:xx:xx:08`

---

## 1. Hardware Architecture Summary

| Component | Part Number / Block | Details | OpenWrt / Linux Driver Status |
| :--- | :--- | :--- | :--- |
| **SoC** | ZTE / Sanechips `ZX279128R` (`cputype = 3`, `128s` family) | Dual-core 32-bit ARM Cortex-A9 (`0xc09` rev 1) @ ~1.0 GHz (`BogoMIPS ~1993`), ARMv7-A (no VFP/NEON — requires soft-float `arm_cortex-a9`), PL310 L2 cache `@0x00c00000`, GIC `@0x00801000`, Global Timer `@0x00800200` | Supported via `sanechips` target (`openwrt#25469` / `Zlion-Y/zte-e2631-openwrt`) |
| **RAM** | ESMT `M15T2G16128A` | 256 MiB DDR3 (`0x40000000`–`0x4fffffff`, size `0x10000000`) | Fully supported |
| **Flash** | ESMT `F50L1G41LB` | 128 MiB 3.3V Quad-SPI NAND (`0x08000000`, 2 KiB page, 128 KiB eraseblock, on-die ECC) on controller `spifc@94406000` (`zte,ZX27912x-spifc`) | Supported in U-Boot (`SPI NAND F50L1G41LB 128MiB 3,3V`) and OpenWrt `spi-nand` |
| **Wi-Fi 6** | MediaTek `MT7916AN` + `MT7976DN/DM` | Filogic 630 DBDC 2.4 GHz / 5 GHz AX3000 over PCIe (`pcie@10000000`, bus `0000:01:00.0` at `0x20000000`) | Supported upstream via `mt76` (`mt7915e`) using factory `/wlan/MT7916_EEPROM.bin` |
| **Ethernet / Switch** | Integrated 4× GbE PHYs (`gephy128s@9b000000`) + Switch/PPE (`0x92000000`) | MDIO controller `mdio@9a101000` (`zte,zx279128s-mdio`), 4 GPHY register banks (`0x9b000000`..`0x9b300000`, GIC IRQs 71–74), PPE/NPP/TM/IDM @ `0x92000000`..`0x92380000` | Supported via `zx279128s-eth` + `zx279128s-ppe` in `Zlion-Y/zte-e2631-openwrt` |
| **UART** | `serial0` (`ttyAMA0`) | PL011 UART `@0x94404000`, `115200 8N1`, 4-pin unpopulated through-hole header (`VDD`, `GND`, `TX`, `RX` — **3.3V CMOS logic only; never connect `VDD` or ±12V RS-232**) | Supported via `zte-pl011-uart` |

---

## 2. Complete 128 MiB SPI-NAND Flash Partition Table (`/proc/mtd`) & Backups

All 13 flash partitions (plus live DTB, Wi-Fi EEPROM, and decrypted XML configs) have been dumped and verified in `e:\Antigravity\ZXIC\backup\`:

| MTD | Name | Size | Eraseblock | Flash Offset (`0xc042d918`) | Filesystem / Format | Local Backup File |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `mtd0` | `whole flash` | `0x08000000` (128 MiB) | `0x20000` | `0x00000000` | Full chip span | Covered by `mtd1`–`mtd13` |
| `mtd1` | `u-boot` | `0x00200000` (2 MiB) | `0x20000` | `0x00000000` | `U-Boot CTCRV1.0.0` (`non secure uboot`) | `backup/mtd1_uboot.bin` |
| `mtd2` | `parameter tags` | `0x00200000` (2 MiB) | `0x20000` | `0x04c00000` | JFFS2 (`/tagparam`: `frw_set`, `paramtag` MACs at `+0x36`) | `backup/mtd2_tagparam.bin` |
| `mtd3` | `kernel0` | `0x00500000` (5 MiB) | `0x20000` | `0x00200000` | `0x1E0` ZTE header + legacy `uImage` (`V1.0.2.220522`) | `backup/mtd3_kernel0.bin` |
| `mtd4` | `kernel1` | `0x00500000` (5 MiB) | `0x20000` | `0x00700000` | `0x1E0` ZTE header + legacy `uImage` (`V1.0.6.240719`) | `backup/mtd4_kernel1.bin` |
| `mtd5` | `rootfs0` | `0x02000000` (32 MiB) | `0x20000` | `0x00c00000` | JFFS2 (Slot 0 rootfs, currently **inactive**) | `backup/mtd5_rootfs0.bin` |
| `mtd6` | `rootfs1` | `0x02000000` (32 MiB) | `0x20000` | `0x02c00000` | JFFS2 (Slot 1 rootfs, currently **active** `/`) | `backup/mtd6_rootfs1.bin` |
| `mtd7` | `usercfg` | `0x00500000` (5 MiB) | `0x20000` | `0x04e00000` | JFFS2 (`/userconfig`: `cfg/db_user_cfg.xml`, etc.) | `backup/mtd7_usercfg.bin` |
| `mtd8` | `others` | `0x00100000` (1 MiB) | `0x20000` | `0x05300000` | Raw ASCII boot state (`FrwPartion=0x1,...`) | `backup/mtd8_others.bin` |
| `mtd9` | `wlan` | `0x00300000` (3 MiB) | `0x20000` | `0x05400000` | JFFS2 (`/wlan`: `MT7916_EEPROM.bin` caldata) | `backup/mtd9_wlan.bin` |
| `mtd10` | `middleware` | `0x00100000` (1 MiB) | `0x20000` | `0x05700000` | JFFS2 (`/db_excp`) | `backup/mtd10_middleware.bin` |
| `mtd11` | `framework` | `0x00800000` (8 MiB) | `0x20000` | `0x05800000` | Framework slot 0 | `backup/mtd11_framework.bin` |
| `mtd12` | `framework1` | `0x00800000` (8 MiB) | `0x20000` | `0x06000000` | JFFS2 (`/ipks`, Framework slot 1) | `backup/mtd12_framework1.bin` |
| `mtd13` | `apps` | `0x01000000` (16 MiB) | `0x20000` | `0x06800000` | UBIFS (`ubi0_0` mounted at `/ctcap` + overlays) | `backup/mtd13_apps.bin` |

Additional extracted artifacts in `e:\Antigravity\ZXIC\backup\`:
* `fdt.dtb` & decompiled `dir_x3060z_live.dts` (live Device Tree from `/sys/firmware/fdt`)
* `vmlinux.bin` (6,117,696-byte uncompressed ARM Linux 4.1.25 kernel extracted from `mtd4_kernel1.bin`)
* `MT7916_EEPROM.bin` (655,360-byte MediaTek Filogic 630 RF calibration data) & `l1profile_7916_dual.dat`
* `db_user_cfg.xml` (161,408 B), `db_backup_cfg.xml` (161,408 B), `db_default_cfg.xml` (64,103 B) — plaintext XML databases
* `paramtag.bin` (232 B) & `env_param.bin` (96 B)
* `extracted_rootfs/` — unpacked `/kmodule/*.ko` (`bspdriver.ko`, `plat-zxic_128s.ko`, `tm_128s.ko`, `switch.ko`, `ffe.ko`, `mt7916_ap.ko`, etc.), `/etc/rcS.d/*`, `/bin/cspd`, `/lib/libleduserapi.so`

---

## 3. Web UI & System Audit Findings (`192.168.12.1`)

1. **ZTE HGW Platform Identity:**
   * Runs ZTE's `Mini web server 1.0.` / `/bin/cspd` with 52 `.gch` templates accessed via `/getpage.gch?pid=1002&nextpage=<page>.gch`.
2. **Authentication & Session Design Flaws:**
   * **No HTTP→HTTPS redirect** on port `80`.
   * **Client-side Base64 password encoding** (`b64.js`).
   * **Broken lockout math & uptime disclosure:** `index.html` computes `maxtime = Math.min(60, lock_time + 60 - current_uptime)`, leaking uptime unauthenticated; furthermore, server-side `POST /` accepts valid credentials even while `GET /` serves `SetDisabled()`.
   * **IP-bound session tracking (no cookies):** After login, the server tracks the authenticated session solely by client IP address (`Cookies: []`).
3. **Exposed TR-069 / ACS Credentials (`net_tr069_basic_t.gch`):**
   * Plain HTTP ACS URL (`http://acs.tm.com.my:8082/tmacstr069`) with reversible Base64 credentials embedded in the page source.
4. **Root Shell via `app_rac_conf_t.gch` (`Remote Access Control`):**
   * The authenticated page `app_rac_conf_t.gch` exposes toggles for `LANTelnet`, `LANSSH`, and `LANFTP` (disabled by default).
   * Setting `LANTelnet=1`, `LANSSH=1`, and `LANFTP=1` immediately starts FTP (`:21`), Dropbear SSH (`:22`), and Telnet (`:23`).
   * Logging into Telnet (`192.168.12.1:23`) with the web admin account (`tmadmin` / `Adm@<last4MAC>`) drops directly into an unconfined root shell (`root@zxic:/ #`).

---

## 4. Bootloader (`CTCRV1.0.0`), Dual-Slot Selection & Firmware Image Format

### 4.1 Non-Secure Bootloader & 480-Byte (`0x1E0`) Slot Header
* `mtd1_uboot.bin` is a **non-secure U-Boot** (`non secure uboot`) that boots standard legacy `uImage` kernels (`load = entry = 0x40008000`, uncompressed `zImage` payload) after verifying **CRC32 checksums only** (no RSA/ECDSA signature verification).
* Notice how `DIR-X3060Z` differs slightly from `ZTE E2631` in partition layout:
  * On `E2631`, `kernel+rootfs` are combined into a single 28 MiB `slotA` (`0x700000`) and `slotB` (`0x2300000`) with a `0x214`-byte package header (`0x20`-byte slot marker inside flash).
  * On **`DIR-X3060Z`**, `kernel` and `rootfs` live in **separate MTD partitions** (confirmed from `vmlinux.bin` `struct mtd_partition` array at `0xc042d918` and header `0x070..0x08F`):
    * **Slot 0:** `mtd3` (`kernel0` @ `0x00200000`, 5 MiB) + `mtd5` (`rootfs0` @ `0x00c00000`, 32 MiB)
    * **Slot 1:** `mtd4` (`kernel1` @ `0x00700000`, 5 MiB) + `mtd6` (`rootfs1` @ `0x02c00000`, 32 MiB)
    * *(Note: `mtd2` `parameter tags` is located at `0x04c00000` right after `rootfs1`, not at `0x00200000`!)*
  * Disassembly of `U-Boot CTCRV1.0.0` (`mtd1_uboot.bin` @ `0x24584..0x24820`) confirmed that **at boot time (`bootk`), U-Boot ONLY verifies the `0x1E0` header CRC32 (`0x104`) and the kernel CRC32 (`0x060`)**. It does **not** checksum `rootfs0`/`rootfs1` during boot, meaning OpenWrt's standard `squashfs` + `rootfs_data` JFFS2 overlay split works across the full 32 MiB `rootfs0` partition without invalidating any boot-time CRC.
  * Each kernel partition (`mtd3` and `mtd4`) begins directly with a **480-byte (`0x1E0`) ZTE version header**, followed immediately at **`+0x1E0`** by the legacy `uImage` (`27 05 19 56`):
    * `0x000..0x00F`: Outer magic `99999999 44444444 55555555 aaaaaaaa`
    * `0x010..0x013`: `0x00000000` (`hdr_ofs = 0`, unsigned)
    * `0x024..0x043`: Version string (e.g., `V1.0.6.240719.095339`)
    * `0x054..0x057`: Total combined payload size (`0x1cc2250` on Slot 1)
    * `0x058..0x063`: Kernel `{size, offset = 0x1e0, crc32}` — verified `CRC32(mtd4[0x1e0 : 0x1e0 + size]) == stored_crc` (`0x4b50329c`)
    * `0x064..0x06F`: Rootfs `{size, pkg_offset, crc32}` — `size = 0x1a60000`, `crc32 = 0xfab6b6fc`
    * `0x090..0x0C3`: Product banner (`ZXIC DIR-X3060Z UNI V1.0.3.240719.095339`)
    * `0x0E0..0x0EB`: Framework `{size, pkg_offset, crc32}` (`0x40000` on Slot 1, `0` on Slot 0)
    * `0x104..0x107`: Header CRC32 = `CRC32(header[0x014 : 0x104])` — verified `0xc985c3ee` (Slot 0) and `0xdd997033` (Slot 1)
    * `0x108..0x11B`: Build timestamp (`20240724161315`)
    * `0x12C..0x14B`: Hardware version (`V1.0`) & model name (`DIR-X3060Z`)

### 4.2 Dual-Slot Selector in `mtd8` (`others`) and `mtd2` (`parameter tags`)
* **`mtd8` (`others` @ `0x05300000`)** holds the primary U-Boot boot-slot state string at offset `0x00000`:
  ```text
  FrwPartion=0x00000001,Image1Status=0xffffffff,BootImageNum=0x00000001,
  ```
* **`mtd2` (`/tagparam/frw_set`)** mirrors the active slot index:
  ```text
  partion:1
  ```
* **Rootfs CRC32 Verified on Both Slots:**
  * Slot 0 (`mtd5_rootfs0.bin`, first `0x1520000` bytes): `CRC32 = 0x84d256e2` — **100% match** with `mtd3` header at `0x06C`.
  * Slot 1 (`mtd6_rootfs1.bin`, first `0x1a60000` bytes): `CRC32 = 0xfab6b6fc` — **100% match** with `mtd4` header at `0x06C`.
* **Live U-Boot Environment (`mtd1_uboot.bin` @ `0x180000`):**
  ```text
  baudrate=115200
  bootcmd=setenv bootargs console=$(console) root=/dev/mtdblock6 ro rootfstype=jffs2  mem=$(memsize);bootm 0x440001e0;
  bootdelay=1
  console=ttyAMA0,115200n8
  ethact=eth0
  fullfile=upgrade.bin
  ipaddr=192.168.1.1
  serverip=192.168.1.100
  linuzfile=vmlinuz.bin
  loadaddr=0x44000000
  memsize=256M
  versioninfo=U-Boot CTCRV1.0.0 20220808184253 0x700000 0x1 0x83 0x83
  ```
  * **No U-Boot Serial Password:** `Hit any key to stop autoboot: 1` (or `Hit 1 to upgrade software version` for one-key TFTP upgrade using client `192.168.1.1` and TFTP server `192.168.1.100`).
  * **Kernel Load Address:** U-Boot reads the selected 5 MiB kernel partition (`mtd3` or `mtd4`) into RAM at `loadaddr = 0x44000000` and runs `bootm 0x440001e0` (skipping the `0x1e0`-byte ZTE version header to land directly on the `uImage` header at `+0x1e0`).
* And U-Boot natively supports both `rootfstype=jffs2` and `rootfstype=squashfs` on `/dev/mtdblock5` (Slot 0) or `/dev/mtdblock6` (Slot 1), as confirmed by the strings in `mtd1_uboot.bin`:
  ```text
  setenv bootargs console=$(console) root=/dev/mtdblock5 ro rootfstype=jffs2
  setenv bootargs console=$(console) root=/dev/mtdblock5 ro rootfstype=squashfs
  setenv bootargs console=$(console) root=/dev/mtdblock6 ro rootfstype=jffs2
  setenv bootargs console=$(console) root=/dev/mtdblock6 ro rootfstype=squashfs
  ```

---

## 5. SoC Board Table, GPIO, LED & Button Mapping (`vmlinux.bin` + `bspdriver.ko`)

From disassembling `arch/arm/mach-zx27912x/zxic_board_128s.c` inside `vmlinux.bin` (`0xc00174bc`–`0xc0017814`) and `bspdriver.ko`:

1. **Active Board Profile in Kernel (`current_board`):**
   * `cputype` is `3` (`ZX279128S` / `ZX279128R` silicon family, confirmed by `S99modules` loading `plat-zxic_128s.ko` and `tm_128s.ko`).
   * In `zxic_board_128s.c` (`0xc0017538` / `0xc0017598` / `0xc0017614`), when `cputype == 3`, the default board profile selected is **Board ID `0x47` (`71`, `F628S`, `ZX279128R`)** at `0xc05b4564` (file `0x5ac564`), with optional 5-PHY / variant profiles `0x48` (`F428S`) and `0x4a` (`F628S_5phy`).
2. **GPIO Pin Table for Board `0x47` (`F628S` / `ZX279128R`, `p_gpio = 0xc05ba580`, 10 entries):**
   * Each 16-byte entry in `p_gpio[idx]` is `{flat_gpio, used, direction, init_val}`:
     * `idx 0`–`3` (LAN LEDs / muxed pins): **flat GPIO `56`, `57`, `58`, `59`**
     * `idx 17` (Input / Button): **flat GPIO `0`** (`dir = 0 / input`)
     * `idx 31` (Input / Button — Reset / WPS): **flat GPIO `62`** (`dir = 0 / input`) — *(on `0x48`/`0x4a`, `idx 16` = **flat GPIO `55`**, `idx 17` = **flat GPIO `5`**)*
     * `idx 53` (Output / LED): **flat GPIO `6`** (`dir = 1 / output`, `init = 1`)
     * `idx 57` (Output / LED): **flat GPIO `41`** (`dir = 1 / output`, `init = 0`)
     * `idx 61` (Output / LED): **flat GPIO `53`** (`dir = 1 / output`, `init = 0`)
     * `idx 65` (Output / LED — Power/Status): **flat GPIO `17`** (on `0x47`) or **flat GPIO `40` & `41`** (on `0x48`/`0x4a`, matching E2631's `gpio40`)
   * *(Note: as documented in `Zlion-Y/zte-e2631-openwrt`, reading the raw GPIO controller registers `0x94407000+0x40n` directly from userland while the vendor kernel is running can trigger a bus hang and watchdog reboot—which is very likely why your router rebooted/dropped Wi-Fi around the time we probed `/proc` and `/dev`!)*
