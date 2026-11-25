// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sanechips / ZTE ZX279128S/R on-die thermal sensor driver (temp@94100034)
 *
 * Register layout and piecewise linearization formula verified from stock
 * ZX279128R/S Linux 4.1 drivers/tempsensor/temp_zx.c.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>

#define ZX_TEMP_CFG		0x00
#define  ZX_TEMP_CFG_PD		BIT(12)
#define  ZX_TEMP_CFG_EN		BIT(13)
#define  ZX_TEMP_CFG_SEL_MASK	GENMASK(17, 16)
#define ZX_TEMP_DATA		0x04
#define  ZX_TEMP_DATA_RAW_MASK	GENMASK(9, 0)
#define  ZX_TEMP_DATA_READY	BIT(12)
#define ZX_TEMP_INT		0x08
#define  ZX_TEMP_INT_MASK_ALL	0x3fffffff

#define ZX_TEMP_SAMPLES		8

struct zx_thermal {
	void __iomem *base;
	struct mutex lock;
};

static int zx_thermal_raw_to_mc(u32 raw)
{
	int t = ((int)(raw * 1550U) >> 10) - 1104;

	if (t >= 35 && t <= 44)
		t -= (t - 35) / 2;
	else if (t >= 45 && t <= 64)
		t = (t - 5) - (t - 45) / 4;
	else if (t > 64)
		t -= 9;

	if (t > 100)
		t = 100 + (t - 100) / 2;

	return t * 1000;
}

static int zx_thermal_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct zx_thermal *zxt = thermal_zone_device_priv(tz);
	u32 val, sum = 0;
	int i, count = 0;

	mutex_lock(&zxt->lock);

	/* Fast path: read current ADC value if already ready and non-zero */
	val = readl(zxt->base + ZX_TEMP_DATA);
	if ((val & ZX_TEMP_DATA_READY) && (val & ZX_TEMP_DATA_RAW_MASK)) {
		sum += val & ZX_TEMP_DATA_RAW_MASK;
		count++;
	}

	/* Trigger fresh samples by toggling EN/PD as in stock temp_zx.c */
	for (i = 0; i < ZX_TEMP_SAMPLES && count < ZX_TEMP_SAMPLES; i++) {
		u32 cfg = readl(zxt->base + ZX_TEMP_CFG);

		cfg &= ~ZX_TEMP_CFG_EN;
		writel(cfg, zxt->base + ZX_TEMP_CFG);
		usleep_range(500, 800);

		cfg |= ZX_TEMP_CFG_PD;
		writel(cfg, zxt->base + ZX_TEMP_CFG);
		usleep_range(500, 800);

		cfg &= ~ZX_TEMP_CFG_PD;
		writel(cfg, zxt->base + ZX_TEMP_CFG);
		usleep_range(500, 800);

		cfg |= ZX_TEMP_CFG_EN;
		writel(cfg, zxt->base + ZX_TEMP_CFG);
		usleep_range(1000, 1500);

		val = readl(zxt->base + ZX_TEMP_DATA) & ZX_TEMP_DATA_RAW_MASK;
		if (val) {
			sum += val;
			count++;
		}
	}

	mutex_unlock(&zxt->lock);

	if (!count)
		return -EAGAIN;

	*temp = zx_thermal_raw_to_mc(sum / count);
	return 0;
}

static const struct thermal_zone_device_ops zx_thermal_ops = {
	.get_temp = zx_thermal_get_temp,
};

static int zx_thermal_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct zx_thermal *zxt;
	struct thermal_zone_device *tz;
	u32 cfg, val;
	int ret;

	zxt = devm_kzalloc(dev, sizeof(*zxt), GFP_KERNEL);
	if (!zxt)
		return -ENOMEM;

	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	zxt->base = devm_ioremap(dev, res->start, resource_size(res));
	if (!zxt->base)
		return -ENOMEM;

	mutex_init(&zxt->lock);

	/* Power up and enable sensor */
	cfg = readl(zxt->base + ZX_TEMP_CFG);
	cfg &= ~(ZX_TEMP_CFG_PD | ZX_TEMP_CFG_SEL_MASK);
	writel(cfg, zxt->base + ZX_TEMP_CFG);
	usleep_range(1000, 1500);

	cfg |= ZX_TEMP_CFG_EN;
	writel(cfg, zxt->base + ZX_TEMP_CFG);

	/* Mask all threshold interrupts */
	writel(ZX_TEMP_INT_MASK_ALL, zxt->base + ZX_TEMP_INT);

	ret = readl_poll_timeout(zxt->base + ZX_TEMP_DATA, val,
				 val & ZX_TEMP_DATA_RAW_MASK, 1000, 50000);
	if (ret)
		dev_warn(dev, "initial sensor sample timed out (data=%#x)\n", val);

	tz = devm_thermal_of_zone_register(dev, 0, zxt, &zx_thermal_ops);
	if (IS_ERR(tz))
		return dev_err_probe(dev, PTR_ERR(tz),
				     "failed to register thermal zone\n");

	dev_info(dev, "ZX279128S/R thermal sensor registered\n");
	return 0;
}

static const struct of_device_id zx_thermal_of_match[] = {
	{ .compatible = "zxic,tempsensor" },
	{ }
};
MODULE_DEVICE_TABLE(of, zx_thermal_of_match);

static struct platform_driver zx_thermal_driver = {
	.probe = zx_thermal_probe,
	.driver = {
		.name = "zx279128s-thermal",
		.of_match_table = zx_thermal_of_match,
	},
};
module_platform_driver(zx_thermal_driver);

MODULE_DESCRIPTION("Sanechips ZX279128S/R Thermal Sensor Driver");
MODULE_LICENSE("GPL");
