# D-Link DIR-X3060Z OpenWrt Build & Testing Guide

## 1. Directory Contents (`e:\Antigravity\ZXIC\port\`)

- `dts/zx279128r-dlink-dir-x3060z.dts` — OpenWrt Device Tree for the D-Link DIR-X3060Z (HW V1.0).
- `pack_x3060z.py` — Standalone Python tool that builds and verifies the 480-byte (`0x1E0`) DIR-X3060Z header + `uImage` + `squashfs` firmware container (proven 480/480 byte-exact against both Slot 0 and Slot 1 flash backups).
- `base-files/etc/hotplug.d/firmware/11-mt76-caldata` — Extracts `MT7916_EEPROM.bin` (`0x7916`) from the `wlan` JFFS2 partition (`0x05400000`) into `/lib/firmware/mediatek/mt7916_eeprom.bin`.
- `base-files/etc/board.d/02_network` — Configures `lan1..lan3` + `wan` and loads the factory LAN/WAN MAC addresses (`c8:78:7d:xx:xx:04` / `05`) from `/tagparam/paramtag` in the `tagparam` JFFS2 partition (`0x04c00000`).
- `base-files/sbin/x3060z-fwslot` — On-device utility to inspect and toggle the active boot slot (`BootImageNum=0x00000000` for Slot 0 OpenWrt vs `BootImageNum=0x00000001` for Slot 1 Stock firmware) inside the `others` partition (`0x05300000`).
- `image/Makefile.x3060z` — OpenWrt image build recipe producing `initramfs-kernel.bin`, `factory.bin`, `kernel0.bin`, `rootfs0.bin`, and `sysupgrade.bin`.
- `apply_x3060z.sh` — Script to apply this port onto an OpenWrt build tree.

---

## 2. Building in Linux / WSL2

```bash
# 1. Clone the Sanechips ZX279128S OpenWrt base tree
git clone --branch pr-zte-zxhn-h3600 https://github.com/Zlion-Y/openwrt-sanechips-mirror openwrt
git -C openwrt checkout db8b8d4130457dbe46558a7cea872dd2dae539e6

# 2. Apply the base ZX279128S kernel/target patches from our portable repo copy
./repos/zte-e2631-openwrt-main/apply.sh openwrt

# 3. Apply the D-Link DIR-X3060Z board overlay and 0x1E0 packer
./port/apply_x3060z.sh openwrt

# 4. Update feeds and select D-Link DIR-X3060Z
cd openwrt
./scripts/feeds update -a
./scripts/feeds install -a
make defconfig
# Set target device to dlink_dir-x3060z (and enable ramdisk/initramfs image)
cat >> .config << 'EOF'
CONFIG_TARGET_sanechips=y
CONFIG_TARGET_sanechips_generic=y
CONFIG_TARGET_sanechips_generic_DEVICE_dlink_dir-x3060z=y
CONFIG_TARGET_ROOTFS_INITRAMFS=y
CONFIG_TARGET_INITRAMFS_COMPRESSION_LZMA=y
EOF
make defconfig
make -j$(nproc)
```

---

## 3. Zero-Risk RAM Boot Testing (Once Your 3.3V USB-TTL Adapter Arrives)

> **Hardware Warning:** Only connect `GND`, `TX`, and `RX` between the router's 4-pin UART header (`J1`) and a **3.3V USB-to-TTL** adapter (`115200 8N1`). Do **not** connect the `VDD` (`3.3V`) pin, and never connect to a native PC 9-pin RS-232 port.

1. Set your PC's Ethernet adapter static IP to `192.168.1.100/24` and start a TFTP server hosting `openwrt-sanechips-generic-dlink_dir-x3060z-initramfs-kernel.bin` renamed to `test.bin`.
2. Power on the router and press any key at `Hit any key to stop autoboot: 1` to enter the `ZXIC #` U-Boot prompt.
3. Load and boot the OpenWrt initramfs image purely in RAM (touches zero bytes of flash):
   ```text
   tftp 0x44000000 test.bin
   bootm 0x44000000
   ```
4. Verify in the running OpenWrt RAM session:
   - `cat /proc/mtd` matches all 13 hardware partitions (`u-boot`, `kernel`, `kernel1`, `rootfs`, `rootfs1`, `tagparam`, `usercfg`, `others`, `wlan`, `middleware`, `framework`, `framework1`, `apps`).
   - `ip link` shows `lan1..lan3` and `wan` with the factory MAC address `c8:78:7d:xx:xx:04` / `05`.
   - `dmesg | grep mt7915e` shows the MediaTek Filogic 630 (`MT7916AN`) loading `/lib/firmware/mediatek/mt7916_eeprom.bin` from `wlan`.

---

## 4. Dual-Slot Installation into Slot 0 (Keeping Stock Firmware in Slot 1)

Once the RAM initramfs test succeeds, you can install OpenWrt into **Slot 0** (`kernel` @ `0x00200000` + `rootfs` @ `0x00c00000`) while keeping your stock firmware (`V1.0.6.240719.095339`) untouched in **Slot 1** (`kernel1` @ `0x00700000` + `rootfs1` @ `0x02c00000`):

- **Option A — From U-Boot TFTP (`downver`):**
  Place `openwrt-sanechips-generic-dlink_dir-x3060z-squashfs-factory.bin` on your TFTP server (`192.168.1.100`) as `upgrade.bin`, and at the `ZXIC #` prompt run:
  ```text
  downver version 1
  ```
  *(In U-Boot's `downver` command, index `1` is Slot 0 [`mtd3` + `mtd5`], and index `2` is Slot 1 [`mtd4` + `mtd6`]).*
- **Option B — From the OpenWrt Initramfs RAM Session:**
  ```sh
  mtd write kernel0.bin kernel
  mtd write rootfs0.bin rootfs
  x3060z-fwslot switch 0
  reboot
  ```
- **Rolling Back to Stock Firmware (Slot 1) at Any Time:**
  - From OpenWrt SSH: `x3060z-fwslot switch 1 && reboot`
  - Or from U-Boot serial console: `setenv bootcmd "setenv bootargs console=ttyAMA0,115200n8 root=/dev/mtdblock6 ro rootfstype=jffs2 mem=256M;bootm 0x440001e0"; saveenv; reset`
