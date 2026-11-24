# OpenWrt for D-Link DIR-X1860Z & DIR-X3060Z (ZTE/Sanechips ZX279128R)

OpenWrt (`Linux 6.18.55`, `sanechips` target) board port, device trees, native `sysupgrade` handler, and `0x1E0` firmware container packer for the TM / Unifi Malaysia **D-Link DIR-X1860Z (HW V1.0, AX1800)** and **D-Link DIR-X3060Z (HW V1.0, AX3000)** Wi-Fi 6 routers.

> **Status:** **DIR-X1860Z** is **live-verified** on hardware (Stock Web UI flash → dual-band Wi-Fi 6, Gigabit WAN/LAN with hardware NAT flow offload, factory MACs, LEDs, buttons, Stock Slot 0 rollback, and 1-click LuCI `sysupgrade` with config preservation). **DIR-X3060Z** images are compiled and pre-flight verified (awaiting live hardware verification).

---

## Hardware Support Matrix

| Feature | D-Link DIR-X1860Z (HW V1.0) | D-Link DIR-X3060Z (HW V1.0) |
|---|---|---|
| **SoC / RAM / Flash** | ZX279128R (2× Cortex-A9 @ 1 GHz, soft-float) / 256 MiB / 128 MiB SPI-NAND | Same |
| **Wi-Fi 6 Radio** | MediaTek `MT7905DAN` + `MT7975DN` (AX1800, 2×2 HE40 + HE80) | MediaTek `MT7916AN` + `MT7976DN/DM` (AX3000) |
| **Ethernet** | 1× GbE WAN + 3× GbE LAN (MDIO `0xa..0xd`, HW NAT flow offload) | Same |
| **Bootloader** | U-Boot `CTCRV1.0.7` (non-secure, `0x1E0` CRC32 container) | U-Boot `CTCRV1.0.0` (non-secure, `0x1E0` CRC32 container) |
| **OpenWrt Flash Slot** | **Slot 1** (`0x00700000` + `0x03800000`, Stock Slot 0 preserved!) | **Slot 0** (`0x00200000` + `0x00c00000`, Stock Slot 1 preserved!) |
| **Writable Overlay** | ~24.9 MiB JFFS2 (`rootfs_data`) | ~24.9 MiB JFFS2 (`rootfs_data`) |
| **LuCI `sysupgrade`** | Supported & verified (preserves settings) | Supported |
| **LEDs & Buttons** | Power, Internet, Wi-Fi, LAN1–3, WAN + Reset & WPS verified | Mapped from `vmlinux.bin` + `bspdriver.ko` |

---

## Flashing Guide (DIR-X1860Z)

> **Warning:** Flashing third-party firmware carries inherent risk. Make sure you verify your hardware version (`DIR-X1860Z` HW `V1.0`) and ideally back up your flash first. On the `DIR-X1860Z`, upgrading from Stock Slot 0 writes OpenWrt into **Slot 1** and leaves **U-Boot (`mtd0`)** and **Stock Slot 0** untouched.

### 1. First-Time Install (from Stock Web UI)
1. Download `openwrt-sanechips-generic-dlink_dir-x1860z-squashfs-factory.bin` and verify its SHA256 against `SHA256SUMS.txt`.
2. Log in to the stock router Web UI (`Management & Diagnosis → System Management → Software Upgrade`).
3. Upload `*-squashfs-factory.bin` and click **Upgrade**. Wait for the router to flash and reboot (~60–90 seconds; if it stalls on the first warm reboot after stock flash, power-cycle once).
4. Connect via Ethernet (`LAN1`–`LAN3`) or open Wi-Fi (`OpenWrt-ZXIC` / `OpenWrt-ZXIC-5G`, which come up ~60 s after boot via deferred radio bind) and open **`http://192.168.1.1/`** (`root`, no password initially).
5. **Immediately** set a root password (**System → Administration**) and WPA2/WPA3 Wi-Fi passwords (**Network → Wireless**).

### 2. Future OpenWrt Upgrades (1-Click LuCI `sysupgrade`)
1. Download `openwrt-sanechips-generic-dlink_dir-x1860z-squashfs-sysupgrade.bin`.
2. In LuCI, go to **System → Backup / Flash Firmware → Flash new firmware image**.
3. Upload `*-squashfs-sysupgrade.bin`, keep **Keep settings and retain the current configuration** checked, and click **Continue**.

### 3. Rolling Back to Stock Firmware (Slot 0)
Because Stock firmware remains intact in Slot 0 (`kernel0` + `rootfs0`), you can switch back at any time over SSH without reflashing:
```sh
x3060z-fwslot status         # Inspect both slots
mtd erase others && reboot   # Restore factory Slot 0 selector and boot Stock
```

---

## Known Notes
- **Deferred Wi-Fi Startup (~60 s):** `mt7915e` is bound outside early boot (`S95`) so SSH/LuCI always come up first. On `MT7905DAN`, `e2631-wifi-on` automatically runs a one-shot L0 full recovery (`sys_recovery=7`) to wake WFDMA0 RX on both bands, and `11-mt76-caldata` patches EEPROM byte `0x190` (`0xd2 → 0x92`) + 5 GHz MAC from `/tagparam/paramtag` on the fly.
- **Hardware Acceleration:** Wired WAN ↔ LAN NAT uses the ZX279128R hardware flow offload engine (~900+ Mbps). Wi-Fi ↔ Ethernet forwarding runs on the dual-core 1 GHz Cortex-A9 CPU (~440+ Mbps on 5 GHz 80 MHz 2×2 with `packet_steering=1` and `fw4` flowtable).
- **U-Boot `do_mcupg` Early-Boot DMA Fix:** Stock D-Link U-Boot runs `do_mcupg` on every boot and jumps to Linux without halting Ethernet switch/BMU/DMA (`buf=0x41000000`). Both `port/patches-6.18/901-arm-compressed-zx279128s-quiesce-eth.patch` (inside `decompress_kernel()`, before `zImage` decompresses) and `port/files/arch/arm/mach-zte/zx279128s.c` (`.reserve` safety backup) quiesce all 5 MACs, DMA, BMU, PP, and hold `ZX_GLB_RESET = 0` until `zx279128s-eth` probes (verified **12/12 consecutive warm + cold boots** with WAN + LAN connected).
- **ZTE `earlycon` Support:** `port/patches-6.18/900-amba-pl011-zte-earlycon.patch` enables live UART console output from `[ 0.000000]` using ZTE's `ZX_UART011_DR` / `ZX_UART011_FR` registers.

---

## Repository Structure
- [`ROADMAP.md`](ROADMAP.md) — Milestone tracker and release status.
- [`PROJECT_FINDINGS_X1860Z.md`](PROJECT_FINDINGS_X1860Z.md) — D-Link `DIR-X1860Z` hardware architecture, live OpenWrt bring-up, MT7905DAN fixes, and LED/GPIO mapping.
- [`PROJECT_FINDINGS.md`](PROJECT_FINDINGS.md) — D-Link `DIR-X3060Z` hardware architecture, SPI-NAND partition table, and bootloader analysis.
- [`NEW_BOARD_CHECKLIST.md`](NEW_BOARD_CHECKLIST.md) — Reusable feasibility & porting checklist for other `ZX279128S`/`ZX279128R` boards.
- [`port/`](port/) — OpenWrt target overlay and build tooling:
  - [`port/dts/`](port/dts/) — OpenWrt Device Trees for `DIR-X1860Z` and `DIR-X3060Z`.
  - [`port/pack_x3060z.py`](port/pack_x3060z.py) — Standalone `0x1E0` firmware header packer & verifier (`480/480` byte-exact self-test across all 4 slots).
  - [`port/base-files/`](port/base-files/) — Calibration loader (`11-mt76-caldata`), deferred Wi-Fi binder (`e2631-wifi-on`), factory MAC loader (`02_network`), LED triggers (`01_leds`), native `sysupgrade` (`lib/upgrade/platform.sh`), and slot switcher (`x3060z-fwslot`).
  - [`port/BUILD_GUIDE.md`](port/BUILD_GUIDE.md) — Linux/WSL2 build, UART TFTP RAM-boot, and flashing guide.
