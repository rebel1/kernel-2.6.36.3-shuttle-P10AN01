/*
 * arch/arm/mach-tegra/shuttle-pm-wlan.c
 *
 * Copyright (C) 2011 Eduardo José Tagle <ejtagle@tutopia.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

/* shuttle-pm-wlan.c
	Wlan is on SDIO bus and it is a AR6000 
*/
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <asm/mach-types.h>
#include <asm/gpio.h>
#include <asm/io.h>
#include <asm/setup.h>
#include <linux/if.h>
#include <linux/skbuff.h>
#include <linux/random.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/rfkill.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>

#include <mach/hardware.h>
#include <asm/mach-types.h>

#include "board-shuttle.h"
#include "gpio-names.h"

struct shuttle_pm_wlan_data {
	struct regulator *regulator[2];
	struct rfkill *rfkill;
#ifdef CONFIG_PM
	int pre_resume_state;
#endif
	int state;
};


/* Power control */
static void __shuttle_pm_wlan_toggle_radio(struct device *dev, unsigned int on)
{
	struct shuttle_pm_wlan_data *wlan_data = dev_get_drvdata(dev);

	/* Avoid turning it on if already on */
	if (wlan_data->state == on)
		return;
	
	if (on) {
		dev_info(dev, "WLAN adapter enabled\n");

		regulator_enable(wlan_data->regulator[0]);
		regulator_enable(wlan_data->regulator[1]);
	
		/* Wlan power on sequence */
		gpio_set_value(SHUTTLE_WLAN_RESET, 0); /* Assert reset */
		gpio_set_value(SHUTTLE_WLAN_POWER, 0); /* Powerdown */
		msleep(2);
		gpio_set_value(SHUTTLE_WLAN_POWER, 1); /* Powerup */
		msleep(2);
		gpio_set_value(SHUTTLE_WLAN_RESET, 1); /* Deassert reset */
		msleep(2);
		
	} else {
		dev_info(dev, "WLAN adapter disabled\n");
		
		gpio_set_value(SHUTTLE_WLAN_RESET, 0); /* Assert reset */
		gpio_set_value(SHUTTLE_WLAN_POWER, 0); /* Powerdown */
		
		regulator_disable(wlan_data->regulator[1]);
		regulator_disable(wlan_data->regulator[0]);
	}
	
	/* store new state */
	wlan_data->state = on;
	
}

static int shuttle_wlan_set_carddetect(struct device *dev,int val)
{
	dev_dbg(dev,"%s: %d\n", __func__, val);

	/* power module up or down based on needs */
	__shuttle_pm_wlan_toggle_radio(dev,val);
	
	/* notify the SDIO layer of the CD change */
	shuttle_wifi_set_cd(val);
	return 0;
} 

/* rfkill */
static int shuttle_wlan_set_radio_block(void *data, bool blocked)
{
	struct device *dev = data;
	
	dev_dbg(dev, "blocked %d\n", blocked);

	/* manage rfkill by 'inserting' or 'removing' the virtual adapter */
	return shuttle_wlan_set_carddetect(dev,!blocked);
}

static const struct rfkill_ops shuttle_wlan_rfkill_ops = {
    .set_block = shuttle_wlan_set_radio_block,
};

static ssize_t wlan_read(struct device *dev, struct device_attribute *attr,
		       char *buf)
{
	int ret = 0;
	struct shuttle_pm_wlan_data *wlan_data = dev_get_drvdata(dev);
	
	if (!strcmp(attr->attr.name, "power_on")) {
		if (wlan_data->state)
			ret = 1;
	} else if (!strcmp(attr->attr.name, "reset")) {
		if (wlan_data->state == 0)
			ret = 1;
	}

	if (!ret) {
		return strlcpy(buf, "0\n", 3);
	} else {
		return strlcpy(buf, "1\n", 3);
	}
}

static ssize_t wlan_write(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	unsigned long on = simple_strtoul(buf, NULL, 10);
	struct shuttle_pm_wlan_data *wlan_data = dev_get_drvdata(dev);

	if (!strcmp(attr->attr.name, "power_on")) {
		rfkill_set_sw_state(wlan_data->rfkill, on ? 1 : 0);
		__shuttle_pm_wlan_toggle_radio(dev, on);
	} else if (!strcmp(attr->attr.name, "reset")) {
		/* reset is low-active, so we need to invert */
		__shuttle_pm_wlan_toggle_radio(dev, !on);
	}

	return count;
}

static DEVICE_ATTR(power_on, 0644, wlan_read, wlan_write);
static DEVICE_ATTR(reset, 0644, wlan_read, wlan_write);

#ifdef CONFIG_PM
static int shuttle_wlan_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct shuttle_pm_wlan_data *wlan_data = dev_get_drvdata(&pdev->dev);

	dev_dbg(&pdev->dev, "suspending\n");

	wlan_data->pre_resume_state = wlan_data->state;
	__shuttle_pm_wlan_toggle_radio(&pdev->dev, 0);

	return 0;
}

static int shuttle_wlan_resume(struct platform_device *pdev)
{
	struct shuttle_pm_wlan_data *wlan_data = dev_get_drvdata(&pdev->dev);
	dev_dbg(&pdev->dev, "resuming\n");

	__shuttle_pm_wlan_toggle_radio(&pdev->dev, wlan_data->pre_resume_state);
	return 0;
}
#else
#define shuttle_wlan_suspend	NULL
#define shuttle_wlan_resume		NULL
#endif

static struct attribute *shuttle_wlan_sysfs_entries[] = {
	&dev_attr_power_on.attr,
	&dev_attr_reset.attr,
	NULL
};

static struct attribute_group shuttle_wlan_attr_group = {
	.name	= NULL,
	.attrs	= shuttle_wlan_sysfs_entries,
};

/* ----- Initialization/removal -------------------------------------------- */
static int __init shuttle_wlan_probe(struct platform_device *pdev)
{
	/* default-on for now */
	const int default_state = 1;
	
	struct rfkill *rfkill;
	struct regulator *regulator[2];
	struct shuttle_pm_wlan_data *wlan_data;
	int ret;

	wlan_data = kzalloc(sizeof(*wlan_data), GFP_KERNEL);
	if (!wlan_data) {
		dev_err(&pdev->dev, "no memory for context\n");
		return -ENOMEM;
	}
	dev_set_drvdata(&pdev->dev, wlan_data);

	regulator[0] = regulator_get(&pdev->dev, "vddio_wlan");
	if (IS_ERR(regulator[0])) {
		dev_err(&pdev->dev, "unable to get regulator 0\n");
		kfree(wlan_data);
		dev_set_drvdata(&pdev->dev, NULL);
		return -ENODEV;
	}

	wlan_data->regulator[0] = regulator[0];

	regulator[1] = regulator_get(&pdev->dev, "vcore_wifi");
	if (IS_ERR(regulator[1])) {
		dev_err(&pdev->dev, "unable to get regulator 1\n");
		regulator_put(regulator[0]);
		kfree(wlan_data);
		dev_set_drvdata(&pdev->dev, NULL);
		return -ENODEV;
	}
	wlan_data->regulator[1] = regulator[1];
	
	/* Init io pins */
	gpio_request(SHUTTLE_WLAN_POWER, "wlan_power");
	gpio_direction_output(SHUTTLE_WLAN_POWER, 0);

	gpio_request(SHUTTLE_WLAN_POWER, "wlan_reset");
	gpio_direction_output(SHUTTLE_WLAN_POWER, 0);
	
	rfkill = rfkill_alloc("ar6000", &pdev->dev, RFKILL_TYPE_WLAN,
							&shuttle_wlan_rfkill_ops, &pdev->dev);


	if (!rfkill) {
		dev_err(&pdev->dev, "Failed to allocate rfkill\n");
		regulator_put(regulator[1]);
		regulator_put(regulator[0]);
		kfree(wlan_data);
		dev_set_drvdata(&pdev->dev, NULL);
		return -ENOMEM;
	}
	wlan_data->rfkill = rfkill;
	
	rfkill_init_sw_state(rfkill, default_state);

	ret = rfkill_register(rfkill);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register rfkill\n");
		regulator_put(regulator[1]);
		regulator_put(regulator[0]);
		rfkill_destroy(rfkill);
		kfree(wlan_data);
		dev_set_drvdata(&pdev->dev, NULL);
		return ret;
	}

	dev_info(&pdev->dev, "WLAN RFKill driver loaded\n");
	
	return sysfs_create_group(&pdev->dev.kobj, &shuttle_wlan_attr_group);
}

static int shuttle_wlan_remove(struct platform_device *pdev)
{
	struct shuttle_pm_wlan_data *wlan_data = dev_get_drvdata(&pdev->dev);

	sysfs_remove_group(&pdev->dev.kobj, &shuttle_wlan_attr_group);

	if (wlan_data->rfkill) {
		rfkill_unregister(wlan_data->rfkill);
		rfkill_destroy(wlan_data->rfkill);
	}

	if (!wlan_data || !wlan_data->regulator[0] || !wlan_data->regulator[1])
		return 0;

	__shuttle_pm_wlan_toggle_radio(&pdev->dev, 0);
	
	regulator_put(wlan_data->regulator[0]);	
	regulator_put(wlan_data->regulator[1]);

	kfree(wlan_data);
	dev_set_drvdata(&pdev->dev, NULL);
	
	return 0;
}

static struct platform_driver shuttle_wlan_driver = {
	.probe		= shuttle_wlan_probe,
	.remove		= shuttle_wlan_remove,
	.suspend	= shuttle_wlan_suspend,
	.resume		= shuttle_wlan_resume,
	.driver		= {
		.name		= "shuttle-pm-wlan",
	},
};

static int __devinit shuttle_wlan_init(void)
{
	return platform_driver_register(&shuttle_wlan_driver);
}

static void shuttle_wlan_exit(void)
{
	platform_driver_unregister(&shuttle_wlan_driver);
}

module_init(shuttle_wlan_init);
module_exit(shuttle_wlan_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Eduardo José Tagle <ejtagle@tutopia.com>");
MODULE_DESCRIPTION("Shuttle WLAN power management");
