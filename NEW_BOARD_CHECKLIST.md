# New ZX279128S / ZX279128R Board Feasibility & OpenWrt Porting Checklist

Use this checklist when evaluating any new ZTE / Sanechips `ZX279128S` or `ZX279128R` router or ONT (e.g., ZTE `F6600P`, D-Link `DIR-X*`, ZTE `E2631`/`H3600`) for OpenWrt (`sanechips` target, Linux 6.18+).

---

## Stage 1 — Access & Non-Destructive Backup (Do First!)
- [ ] **Gain read-only shell or UART access** (3.3V TTL `115200 8N1`; never connect the `3.3V` pin between host and board).
  - Never probe `/proc/zteinfo/*` or `/dev/gpio*` on a live stock kernel (can trigger watchdog reboots).
- [ ] **Dump all MTD partitions** (`cat /proc/mtd` -> dump every `/dev/mtdblockN` over `tcpsvd` or `tftp` into a board-specific `backup_<board>/` folder excluded by `.gitignore`).
- [ ] **Save live telemetry:** `/proc/cpuinfo`, `/proc/cmdline`, `/proc/interrupts`, `/sys/bus/pci/devices/*`, `dmesg`, factory calibration files (`/wlan/*EEPROM*` or `caldata`), and `/tagparam/paramtag` (factory MACs/serial).

## Stage 2 — Feasibility & Secure Boot Gate (Critical on ZTE ONTs!)
- [ ] **Check U-Boot for Secure Boot:** Inspect the dumped `u-boot` MTD partition (`strings` / UART boot banner) for `non secure uboot` vs `secure uboot` / RSA/signature verification (`verify_sig`, `rsa_verify`, `efuse`).
  - If **Secure Boot is active**, unsigned kernels/images will be rejected by U-Boot — stop and document the block before attempting any flash.
- [ ] **Check SoC & RAM/Flash specs:** Confirm `ZX279128S` / `ZX279128R` (`cputype=3`, ARMv7 Cortex-A9 soft-float, no VFP/NEON), RAM size (`256 MiB` vs `512 MiB`), and SPI-NAND geometry (`128 MiB` vs `256 MiB`, page/eraseblock size).
- [ ] **Check Bootloader Container Format:** Confirm whether U-Boot uses the standard ZTE 480-byte (`0x1E0`) slot header + legacy `uImage` (`bootm 0x440001e0`), and verify dual-slot switching via `others` (`BootImageNum=0x00000000` vs `0x00000001`).
- [ ] **GPON/PON caveat (ONTs like `F6600P`):** OpenWrt `sanechips` has no proprietary ZTE GPON/OMCI driver. OpenWrt turns an ONT into a pure Ethernet router / Wi-Fi 6 AP while keeping Stock in the alternate flash slot for rollback.

## Stage 3 — Hardware Reverse-Engineering (Offline from Dumps)
- [ ] **SPI-NAND Partition Offsets:** Extract exact byte offsets from `vmlinux.bin` + `/proc/mtd` (do **not** assume offsets match another board — e.g., `DIR-X1860Z` places `usercfg`/`others`/`wlan` at `0x00c00000` where `DIR-X3060Z` puts `rootfs0`!).
  - Map the target OpenWrt slot's `rootfs` to `/dev/mtdblock3` in the DTS (leaving the stock slot `read-only`) so the shared `root=/dev/mtdblock3` kernel cmdline works cleanly.
- [ ] **Ethernet PHY Map:** Locate `uni_phy` in `vmlinux.bin` (typically MDIO `0x0a..0x0d` for 4x integrated GbE PHYs) and verify physical chassis labels (`wan`, `lan1`..`lan3`) once booted.
- [ ] **Wi-Fi Chip & Calibration EEPROM:**
  - Identify PCIe vendor/device IDs (`0x14c3:0x7915` = `MT7905DAN`/`MT7915`, `0x14c3:0x7906` = `MT7916AN`, plus secondary ext-HIF `0x7916`/`0x790a` which must stay unbound in single-HIF mode on ZX279128).
  - Verify EEPROM magic (`15 79` vs `16 79`) and byte `0x190` (`MT_EE_WIFI_CONF0`): on `MT7905DAN`, `0xd2` (`DUAL`) must be patched to `0x92` (`2G-only` on `phy0`) while keeping factory FEM/LNA bytes (`0x194..0x197`), plus one-shot `sys_recovery=7` after binding `0x7915` to wake WFDMA0 RX.
- [ ] **LEDs, Buttons & PCIe Reset GPIOs:**
  - Dump the board GPIO table from `vmlinux.bin` + disassemble `hw_led_wlan_mtk` in `bspdriver.ko` to identify active-high vs active-low LED pins and button pins (`gpio-keys-polled` is mandatory — IRQ mode locks up RCU).
  - Search `vmlinux.bin` for `pcie0 wifi reset pin` / `pcie1 wifi reset pin` (e.g., `&gpio3 4` / `&gpio3 6` on D-Link vs `&gpio3 5` on ZTE E2631 — getting this wrong can steal the Wi-Fi LED pin!).

## Stage 4 — Build, Pre-Flight & Safe Bring-Up
- [ ] **Author board DTS** (`port/dts/zx279128s-<vendor>-<model>.dts`) and add board profile to `port/pack_x3060z.py` + `port/image/Makefile.x3060z`.
- [ ] **Run packer self-test:** Confirm 480/480 byte-exact reconstruction against the board's dumped stock slot headers.
- [ ] **Pre-flight verify built images:** Check `uImage` load address (`0x48008000` for `initramfs.uImage`, `0x40008000` for flash `factory.bin`), appended DTB model & `rootfs` offset, SquashFS `hsqs` magic, and `0x1E0` CRCs.
- [ ] **Bring-up:** Prefer UART TFTP RAM-boot (`tftp 0x44000000 initramfs.uImage; bootm 0x44000000`) first; verify Ethernet, Wi-Fi TX+RX (`iwinfo scan` + `rx-queues`), factory MACs, LEDs, and buttons before flashing the alternate slot.
