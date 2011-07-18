/*
 * GPS low power control via GPIO
 *
 * Copyright (C) 2011 Eduardo Jos� Tagle <ejtagle@tutopia.com>
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

#include <linux/gpio.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/regulator/consumer.h>
#include "board-shuttle.h"
#include "gpio-names.h"


struct shuttle_pm_gps_data {
	struct regulator *regulator[2];
	int pre_resume_state;
	int state;
#ifdef CONFIG_PM
	int keep_on_in_suspend;
#endif
};

/* Power control */
static void __shuttle_pm_gps_toggle_radio(struct device *dev, unsigned int on)
{
	struct shuttle_pm_gps_data *gps_data = dev_get_drvdata(dev);

	/* Avoid turning it on or off if already in that state */
	if (gps_data->state == on)
		return;
	
	if (on) {

		regulator_enable(gps_data->regulator[0]);
		regulator_enable(gps_data->regulator[1]);
	
		/* 3G/GPS power on sequence */
		shuttle_3g_gps_poweron();

	} else {
	
		shuttle_3g_gps_poweroff();
				
		regulator_disable(gps_data->regulator[1]);
		regulator_disable(gps_data->regulator[0]);
	}
	
	/* store new state */
	gps_data->state = on;
}


static ssize_t shuttle_gps_read(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct shuttle_pm_gps_data *gps_data = dev_get_drvdata(dev);
	int ret = 0;

	if (!strcmp(attr->attr.name, "power_on") ||
	    !strcmp(attr->attr.name, "pwron")) {
		ret = gps_data->state;
#ifdef CONFIG_PM
	} else if (!strcmp(attr->attr.name, "keep_on_in_suspend")) {
		ret = gps_data->keep_on_in_suspend;
#endif
	}
	if (ret)
		return strlcpy(buf, "1\n", 3);
	else
		return strlcpy(buf, "0\n", 3);
}

static ssize_t shuttle_gps_write(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	struct shuttle_pm_gps_data *gps_data = dev_get_drvdata(dev);
	unsigned long on = simple_strtoul(buf, NULL, 10);

	if (!strcmp(attr->attr.name, "power_on") ||
	    !strcmp(attr->attr.name, "pwron")) {
		__shuttle_pm_gps_toggle_radio(dev,on);
#ifdef CONFIG_PM
	} else if (!strcmp(attr->attr.name, "keep_on_in_suspend")) {
		gps_data->keep_on_in_suspend = on;
#endif
	}
	return count;
}

#ifdef CONFIG_PM
static int shuttle_pm_gps_suspend(struct platform_device *pdev,
				pm_message_t state)
{
	struct shuttle_pm_gps_data *gps_data = dev_get_drvdata(&pdev->dev);
	
	gps_data->pre_resume_state = gps_data->state;
	if (!gps_data->keep_on_in_suspend)
		__shuttle_pm_gps_toggle_radio(&pdev->dev,0);
	else
		dev_warn(&pdev->dev, "keeping gps ON during suspend\n");
	return 0;
}

static int shuttle_pm_gps_resume(struct platform_device *pdev)
{
	struct shuttle_pm_gps_data *gps_data = dev_get_drvdata(&pdev->dev);
	__shuttle_pm_gps_toggle_radio(&pdev->dev,gps_data->pre_resume_state);
	return 0;
}

static DEVICE_ATTR(keep_on_in_suspend, 0644, shuttle_gps_read, shuttle_gps_write);
#else
#define shuttle_pm_gps_suspend	NULL
#define shuttle_pm_gps_resume	NULL
#endif

static DEVICE_ATTR(power_on, 0644, shuttle_gps_read, shuttle_gps_write);

static struct attribute *shuttle_gps_sysfs_entries[] = {
	&dev_attr_power_on.attr,
#ifdef CONFIG_PM
	&dev_attr_keep_on_in_suspend.attr,
#endif
	NULL
};

static struct attribute_group shuttle_gps_attr_group = {
	.name	= NULL,
	.attrs	= shuttle_gps_sysfs_entries,
};

static int __init shuttle_pm_gps_probe(struct platform_device *pdev)
{
	struct regulator *regulator[2];
	struct shuttle_pm_gps_data *gps_data;
	
	gps_data = kzalloc(sizeof(*gps_data), GFP_KERNEL);
	if (!gps_data) {
		dev_err(&pdev->dev, "no memory for context\n");
		return -ENOMEM;
	}
	
	dev_set_drvdata(&pdev->dev, gps_data);

	regulator[0] = regulator_get(&pdev->dev, "avdd_usb_pll");
	if (IS_ERR(regulator[0])) {
		dev_err(&pdev->dev, "unable to get regulator for usb pll\n");
		kfree(gps_data);
		dev_set_drvdata(&pdev->dev, NULL);
		return -ENODEV;
	}
	gps_data->regulator[0] = regulator[0];

	regulator[1] = regulator_get(&pdev->dev, "avdd_usb");
	if (IS_ERR(regulator[1])) {
		dev_err(&pdev->dev, "unable to get regulator for usb\n");
		regulator_put(regulator[0]);
		gps_data->regulator[0] = NULL;
		kfree(gps_data);
		dev_set_drvdata(&pdev->dev, NULL);
		return -ENODEV;
	}

	gps_data->regulator[1] = regulator[1];
	
	/* Init io pins */
	shuttle_3g_gps_init();

	dev_info(&pdev->dev, "GPS power management driver loaded\n");
	
	return sysfs_create_group(&pdev->dev.kobj,
				  &shuttle_gps_attr_group);
}

static int shuttle_pm_gps_remove(struct platform_device *pdev)
{
	struct shuttle_pm_gps_data *gps_data = dev_get_drvdata(&pdev->dev);

	sysfs_remove_group(&pdev->dev.kobj, &shuttle_gps_attr_group);
	
	if (!gps_data)
		return 0;
	
	if (gps_data->regulator[0] && gps_data->regulator[1])
		__shuttle_pm_gps_toggle_radio(&pdev->dev, 0);

	if (gps_data->regulator[0]) 
		regulator_put(gps_data->regulator[0]);
		
	if (gps_data->regulator[1]) 
		regulator_put(gps_data->regulator[1]);

	kfree(gps_data);
	return 0;
}

static struct platform_driver shuttle_pm_gps_driver = {
	.probe		= shuttle_pm_gps_probe,
	.remove		= shuttle_pm_gps_remove,
	.suspend	= shuttle_pm_gps_suspend,
	.resume		= shuttle_pm_gps_resume,
	.driver		= {
		.name		= "shuttle-pm-gps",
	},
};

static int __devinit shuttle_pm_gps_init(void)
{
	return platform_driver_register(&shuttle_pm_gps_driver);
}

static void shuttle_pm_gps_exit(void)
{
	platform_driver_unregister(&shuttle_pm_gps_driver);
}

module_init(shuttle_pm_gps_init);
module_exit(shuttle_pm_gps_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Eduardo Jos� Tagle <ejtagle@tutopia.com>");
MODULE_DESCRIPTION("Shuttle 3G / GPS power management");
