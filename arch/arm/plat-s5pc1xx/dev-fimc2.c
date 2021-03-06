/* linux/arch/arm/plat-s5pc1xx/dev-fimc2.c
 *
 * Copyright 2009 Samsung Electronics
 *	Jinsung Yang <jsgood.yang@samsung.com>
 *	http://samsungsemi.com/
 *
 * S5PC1XX series device definition for fimc device 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/platform_device.h>

#include <mach/map.h>

#include <plat/fimc.h>
#include <plat/devs.h>
#include <plat/cpu.h>

static struct resource s3c_fimc_resource[] = {
	[0] = {
		.start = S5PC1XX_PA_FIMC2,
		.end   = S5PC1XX_PA_FIMC2 + SZ_1M - 1,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = IRQ_FIMC2,
		.end   = IRQ_FIMC2,
		.flags = IORESOURCE_IRQ,
	},
};

struct platform_device s3c_device_fimc2 = {
	.name		  = "s3c-fimc",
	.id		  = 2,
	.num_resources	  = ARRAY_SIZE(s3c_fimc_resource),
	.resource	  = s3c_fimc_resource,
};

static struct s3c_platform_fimc default_fimc2_data __initdata = {
	.srclk_name	= "dout_mpll",
	.clk_name	= "sclk_fimc",
	.clockrate	= 133000000,
	.line_length	= 1280,
	.nr_frames	= 4,
	.shared_io	= 0,
};

void __init s3c_fimc2_set_platdata(struct s3c_platform_fimc *pd)
{
	struct s3c_platform_fimc *npd;

	if (!pd)
		pd = &default_fimc2_data;

	npd = kmemdup(pd, sizeof(struct s3c_platform_fimc), GFP_KERNEL);
	if (!npd)
		printk(KERN_ERR "%s: no memory for platform data\n", __func__);
	else if (!npd->cfg_gpio)
		npd->cfg_gpio = s3c_fimc2_cfg_gpio;

	s3c_device_fimc2.dev.platform_data = npd;
}

