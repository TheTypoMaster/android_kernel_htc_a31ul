/* Copyright (c) 2012-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irq.h>

#include <mach/msm_iomap.h>
#include <mach/gpiomux.h>
#include "gpio-msm-common.h"

enum {
	GPIO_IN_BIT  = 0,
	GPIO_OUT_BIT = 1
};

enum {
	INTR_STATUS_BIT = 0,
};

enum {
	GPIO_OE_BIT = 9,
};

enum {
	INTR_ENABLE_BIT        = 0,
	INTR_POL_CTL_BIT       = 1,
	INTR_DECT_CTL_BIT      = 2,
	INTR_RAW_STATUS_EN_BIT = 4,
	INTR_TARGET_PROC_BIT   = 5,
	INTR_DIR_CONN_EN_BIT   = 8,
};

enum {
	DC_GPIO_SEL_BIT = 0,
	DC_POLARITY_BIT	= 8,
};

static unsigned subsys_id = 4;
#define INTR_RAW_STATUS_EN BIT(INTR_RAW_STATUS_EN_BIT)
#define INTR_ENABLE        BIT(INTR_ENABLE_BIT)
#define INTR_POL_CTL_HI    BIT(INTR_POL_CTL_BIT)
#define INTR_DIR_CONN_EN   BIT(INTR_DIR_CONN_EN_BIT)
#define DC_POLARITY_HI     BIT(DC_POLARITY_BIT)

#define INTR_TARGET_PROC_APPS    (subsys_id << INTR_TARGET_PROC_BIT)
#define INTR_TARGET_PROC_NONE    (7 << INTR_TARGET_PROC_BIT)

#define INTR_DECT_CTL_LEVEL      (0 << INTR_DECT_CTL_BIT)
#define INTR_DECT_CTL_POS_EDGE   (1 << INTR_DECT_CTL_BIT)
#define INTR_DECT_CTL_NEG_EDGE   (2 << INTR_DECT_CTL_BIT)
#define INTR_DECT_CTL_DUAL_EDGE  (3 << INTR_DECT_CTL_BIT)
#define INTR_DECT_CTL_MASK       (3 << INTR_DECT_CTL_BIT)

#define GPIO_CONFIG(gpio)        (MSM_TLMM_BASE + 0x1000 + (0x10 * (gpio)))
#define GPIO_IN_OUT(gpio)        (MSM_TLMM_BASE + 0x1004 + (0x10 * (gpio)))
#define GPIO_INTR_CFG(gpio)      (MSM_TLMM_BASE + 0x1008 + (0x10 * (gpio)))
#define GPIO_INTR_STATUS(gpio)   (MSM_TLMM_BASE + 0x100c + (0x10 * (gpio)))
#define GPIO_DIR_CONN_INTR(intr) (MSM_TLMM_BASE + 0x2800 + (0x04 * (intr)))

static inline void set_gpio_bits(unsigned n, void __iomem *reg)
{
	__raw_writel_no_log(__raw_readl_no_log(reg) | n, reg);
}

static inline void clr_gpio_bits(unsigned n, void __iomem *reg)
{
	__raw_writel_no_log(__raw_readl_no_log(reg) & ~n, reg);
}

unsigned __msm_gpio_get_inout(unsigned gpio)
{
	return __raw_readl_no_log(GPIO_IN_OUT(gpio)) & BIT(GPIO_IN_BIT);
}

void __msm_gpio_set_inout(unsigned gpio, unsigned val)
{
	__raw_writel_no_log(val ? BIT(GPIO_OUT_BIT) : 0, GPIO_IN_OUT(gpio));
}

void __msm_gpio_set_config_direction(unsigned gpio, int input, int val)
{
	if (input) {
		clr_gpio_bits(BIT(GPIO_OE_BIT), GPIO_CONFIG(gpio));
	} else {
		__msm_gpio_set_inout(gpio, val);
		set_gpio_bits(BIT(GPIO_OE_BIT), GPIO_CONFIG(gpio));
	}
}

void __msm_gpio_set_polarity(unsigned gpio, unsigned val)
{
	if (val)
		clr_gpio_bits(INTR_POL_CTL_HI, GPIO_INTR_CFG(gpio));
	else
		set_gpio_bits(INTR_POL_CTL_HI, GPIO_INTR_CFG(gpio));
}

unsigned __msm_gpio_get_intr_status(unsigned gpio)
{
	return __raw_readl_no_log(GPIO_INTR_STATUS(gpio)) &
					BIT(INTR_STATUS_BIT);
}

void __msm_gpio_set_intr_status(unsigned gpio)
{
	__raw_writel_no_log(0, GPIO_INTR_STATUS(gpio));
}

unsigned __msm_gpio_get_intr_config(unsigned gpio)
{
	return __raw_readl_no_log(GPIO_INTR_CFG(gpio));
}

void __msm_gpio_set_intr_cfg_enable(unsigned gpio, unsigned val)
{
	unsigned cfg;

	cfg = __raw_readl_no_log(GPIO_INTR_CFG(gpio));
	if (val) {
		cfg &= ~INTR_DIR_CONN_EN;
		cfg |= INTR_ENABLE;
	} else {
		cfg &= ~INTR_ENABLE;
	}
	__raw_writel_no_log(cfg, GPIO_INTR_CFG(gpio));
}

unsigned  __msm_gpio_get_intr_cfg_enable(unsigned gpio)
{
	return __msm_gpio_get_intr_config(gpio) & INTR_ENABLE;
}

void __msm_gpio_set_intr_cfg_type(unsigned gpio, unsigned type)
{
	unsigned cfg;

	cfg = INTR_RAW_STATUS_EN | INTR_TARGET_PROC_APPS;
	__raw_writel_no_log(cfg, GPIO_INTR_CFG(gpio));
	cfg &= ~INTR_DECT_CTL_MASK;
	if (type == IRQ_TYPE_EDGE_RISING)
		cfg |= INTR_DECT_CTL_POS_EDGE;
	else if (type == IRQ_TYPE_EDGE_FALLING)
		cfg |= INTR_DECT_CTL_NEG_EDGE;
	else if (type == IRQ_TYPE_EDGE_BOTH)
		cfg |= INTR_DECT_CTL_DUAL_EDGE;
	else
		cfg |= INTR_DECT_CTL_LEVEL;

	if (type & IRQ_TYPE_LEVEL_LOW)
		cfg &= ~INTR_POL_CTL_HI;
	else
		cfg |= INTR_POL_CTL_HI;

	__raw_writel_no_log(cfg, GPIO_INTR_CFG(gpio));
	udelay(5);
}

void __msm_gpio_set_subsys_id(unsigned id)
{
	subsys_id = id;
}

void __gpio_tlmm_config(unsigned config)
{
	unsigned flags;
	unsigned gpio = GPIO_PIN(config);

	flags = ((GPIO_DIR(config) << 9) & (0x1 << 9)) |
		((GPIO_DRVSTR(config) << 6) & (0x7 << 6)) |
		((GPIO_FUNC(config) << 2) & (0xf << 2)) |
		((GPIO_PULL(config) & 0x3));
	__raw_writel_no_log(flags, GPIO_CONFIG(gpio));
}

void __msm_gpio_install_direct_irq(unsigned gpio, unsigned irq,
					unsigned int input_polarity)
{
	unsigned cfg;

	set_gpio_bits(BIT(GPIO_OE_BIT), GPIO_CONFIG(gpio));
	cfg = __raw_readl_no_log(GPIO_INTR_CFG(gpio));
	cfg &= ~(INTR_TARGET_PROC_NONE | INTR_RAW_STATUS_EN | INTR_ENABLE);
	cfg |= INTR_TARGET_PROC_APPS | INTR_DIR_CONN_EN;
	__raw_writel_no_log(cfg, GPIO_INTR_CFG(gpio));

	cfg = gpio;
	if (input_polarity)
		cfg |= DC_POLARITY_HI;
	__raw_writel_no_log(cfg, GPIO_DIR_CONN_INTR(irq));
}

#if defined(CONFIG_HTC_POWER_DEBUG) && defined(CONFIG_PINCTRL_MSM_TLMM_V3)
void __msm_gpio_get_dump_info(unsigned gpio, struct  msm_gpio_dump_info *data)
{
        unsigned flags;

        flags = __raw_readl(GPIO_CONFIG(gpio));
        data->pull = flags & 0x3;
        data->func_sel = (flags >> 2) & 0xf;
        data->drv = (flags >> 6) & 0x7;
        data->dir = (flags >> 9) & 0x1;

        if (data->dir)
                data->value = (__raw_readl(GPIO_IN_OUT(gpio)) >> 1) & 0x1;
        else {
                data->value = __raw_readl(GPIO_IN_OUT(gpio)) & 0x1;
                data->int_en = __raw_readl(GPIO_INTR_CFG(gpio)) & 0x1;
                if (data->int_en)
                        data->int_owner = (__raw_readl(GPIO_INTR_CFG(gpio)) >> 5) & 0x7;
        }
}
#endif
