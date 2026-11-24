// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Navid Ghahremani <ghahramani.navid@gmail.com>
 */

#include <asm/early_ioremap.h>
#include <asm/mach/arch.h>
#include <linux/bits.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/printk.h>

#define ZX_ETH_PHYS_BASE	0x92000000
#define ZX_ETH_GLB_RESET	0x000008
#define ZX_ETH_MAC_BASE		0x200000
#define ZX_ETH_PORT_STRIDE	0x040000
#define ZX_ETH_MAC_CTRL_EN	GENMASK(1, 0)
#define ZX_ETH_NUM_PORTS	5
#define ZX_ETH_TM_OFFSET	0x340000
#define ZX_ETH_TM_BUF_START	0x0f4
#define ZX_ETH_BMU_OFFSET	0x348000
#define ZX_ETH_DMA_OFFSET	0x350000
#define ZX_ETH_DMA_CTRL_EN	GENMASK(19, 16)
#define ZX_ETH_DMA_CTRL_BIT21	BIT(21)
#define ZX_ETH_PP_OFFSET	0x380000

static void __init zx279128s_reserve(void)
{
	void __iomem *dma, *bmu, *tm, *glb, *pp, *mac;
	u32 dma_val, bmu_val, buf_start;
	int i;

	/*
	 * Some ZTE/D-Link U-Boot builds (e.g. DIR-X1860Z do_mcupg) enable the
	 * integrated Ethernet switch, MACs, BMU, and DMA on every boot and jump
	 * to Linux without halting them. The switch continues writing received
	 * frames into U-Boot's buffer pool (0x41000000) without the CPU,
	 * corrupting early kernel memory (kmem_cache_node, etc.) before
	 * zx279128s-eth probes. Disable all 5 MACs, DMA, BMU, PP, and hold
	 * ZX_GLB_RESET low until zx279128s-eth runs zx_hw_reset().
	 */
	dma = early_ioremap(ZX_ETH_PHYS_BASE + ZX_ETH_DMA_OFFSET, 0x1000);
	bmu = early_ioremap(ZX_ETH_PHYS_BASE + ZX_ETH_BMU_OFFSET, 0x1000);
	tm  = early_ioremap(ZX_ETH_PHYS_BASE + ZX_ETH_TM_OFFSET, 0x1000);
	if (dma && bmu && tm) {
		dma_val = readl(dma);
		bmu_val = readl(bmu);
		buf_start = readl(tm + ZX_ETH_TM_BUF_START);
		if ((dma_val & ZX_ETH_DMA_CTRL_EN) || (bmu_val & BIT(0))) {
			pr_info("zx279128s: quiescing U-Boot eth switch/DMA (dma=0x%08x bmu=0x%08x buf=0x%08x)\n",
				dma_val, bmu_val, buf_start);

			for (i = 0; i < ZX_ETH_NUM_PORTS; i++) {
				mac = early_ioremap(ZX_ETH_PHYS_BASE + ZX_ETH_MAC_BASE +
						    i * ZX_ETH_PORT_STRIDE, 0x1000);
				if (mac) {
					writel(readl(mac) & ~ZX_ETH_MAC_CTRL_EN, mac);
					readl(mac);
					early_iounmap(mac, 0x1000);
				}
			}

			writel((dma_val & ~ZX_ETH_DMA_CTRL_EN) | ZX_ETH_DMA_CTRL_BIT21, dma);
			readl(dma);
			writel(0, bmu);
			readl(bmu);

			pp = early_ioremap(ZX_ETH_PHYS_BASE + ZX_ETH_PP_OFFSET, 0x1000);
			if (pp) {
				writel(2, pp);
				readl(pp);
				early_iounmap(pp, 0x1000);
			}

			glb = early_ioremap(ZX_ETH_PHYS_BASE, 0x1000);
			if (glb) {
				writel(0, glb + ZX_ETH_GLB_RESET);
				readl(glb + ZX_ETH_GLB_RESET);
				early_iounmap(glb, 0x1000);
			}
		}
	}
	if (tm)
		early_iounmap(tm, 0x1000);
	if (bmu)
		early_iounmap(bmu, 0x1000);
	if (dma)
		early_iounmap(dma, 0x1000);
}

static const char *const zx279128s_dt_compat[] __initconst = {
	"zte,zx279128s",
	NULL,
};

DT_MACHINE_START(ZX279128S, "ZTE zx279128s (Device Tree)")
	.dt_compat	= zx279128s_dt_compat,
	.reserve	= zx279128s_reserve,
	/* Keep the L2 cache setup of the boot loader and the device tree */
	.l2c_aux_val	= 0,
	.l2c_aux_mask	= ~0,
MACHINE_END
