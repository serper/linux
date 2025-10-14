// SPDX-License-Identifier: GPL-2.0
/*
 * sunxi-g2d-m2m - Skeleton Allwinner G2D memory-to-memory driver
 *
 * Minimal skeleton to register a platform driver and parse basic resources.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/interrupt.h>

struct sunxi_g2d {
    void __iomem *regs;
    struct clk *clk;
    int irq;
    struct device *dev;
};

static int sunxi_g2d_probe(struct platform_device *pdev)
{
    struct sunxi_g2d *g2d;
    struct resource *res;

    g2d = devm_kzalloc(&pdev->dev, sizeof(*g2d), GFP_KERNEL);
    if (!g2d)
        return -ENOMEM;

    g2d->dev = &pdev->dev;

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    g2d->regs = devm_ioremap_resource(&pdev->dev, res);
    if (IS_ERR(g2d->regs))
        return PTR_ERR(g2d->regs);

    g2d->clk = devm_clk_get(&pdev->dev, NULL);
    if (IS_ERR(g2d->clk))
        dev_warn(&pdev->dev, "clock not found or optional\n");
    else
        clk_prepare_enable(g2d->clk);

    g2d->irq = platform_get_irq(pdev, 0);
    if (g2d->irq >= 0)
        dev_info(&pdev->dev, "g2d irq %d registered (not hooked)\n", g2d->irq);

    platform_set_drvdata(pdev, g2d);

    dev_info(&pdev->dev, "sunxi g2d m2m probe ok\n");
    return 0;
}

static int sunxi_g2d_remove(struct platform_device *pdev)
{
    struct sunxi_g2d *g2d = platform_get_drvdata(pdev);

    if (g2d && g2d->clk && !IS_ERR(g2d->clk))
        clk_disable_unprepare(g2d->clk);

    dev_info(&pdev->dev, "sunxi g2d removed\n");
    return 0;
}

static const struct of_device_id sunxi_g2d_of_match[] = {
    { .compatible = "allwinner,sun8i-g2d", },
    { }
};
MODULE_DEVICE_TABLE(of, sunxi_g2d_of_match);

static struct platform_driver sunxi_g2d_driver = {
    .probe = sunxi_g2d_probe,
    .remove = sunxi_g2d_remove,
    .driver = {
        .name = "sunxi-g2d-m2m",
        .of_match_table = sunxi_g2d_of_match,
    },
};

module_platform_driver(sunxi_g2d_driver);

MODULE_AUTHOR("spuc");
MODULE_DESCRIPTION("Skeleton Allwinner Sunxi G2D M2M driver");
MODULE_LICENSE("GPL v2");
