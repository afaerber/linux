/*
 * linux/drivers/video/backlight/pwm_bl.c
 *
 * simple PWM based backlight control, board code has to setup
 * 1) pin configuration so PWM waveforms can output
 * 2) platform_data being correctly configured
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/fb.h>
#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/pwm.h>
#include <linux/pwm_backlight.h>
#include <linux/slab.h>
#include <linux/of_gpio.h>

struct pwm_bl_gpio {
	unsigned int gpio;
	enum of_gpio_flags flags;
};

struct pwm_bl_data {
	struct pwm_device	*pwm;
	struct device		*dev;
	unsigned int		period;
	unsigned int		lth_brightness;
	unsigned int		*levels;
	unsigned int		num_gpios;
	struct pwm_bl_gpio	*gpios;
	int			(*notify)(struct device *,
					  int brightness);
	void			(*notify_after)(struct device *,
					int brightness);
	int			(*check_fb)(struct device *, struct fb_info *);
	void			(*exit)(struct device *);
};

static int pwm_backlight_update_status(struct backlight_device *bl)
{
	struct pwm_bl_data *pb = bl_get_data(bl);
	int brightness = bl->props.brightness;
	int max = bl->props.max_brightness;

	if (bl->props.power != FB_BLANK_UNBLANK ||
	    bl->props.fb_blank != FB_BLANK_UNBLANK ||
	    bl->props.state & BL_CORE_FBBLANK)
		brightness = 0;

	if (pb->notify)
		brightness = pb->notify(pb->dev, brightness);

	if (pb->pwm) {
		if (brightness == 0) {
			pwm_config(pb->pwm, 0, pb->period);
			pwm_disable(pb->pwm);
		} else {
			int duty_cycle;

			if (pb->levels) {
				duty_cycle = pb->levels[brightness];
				max = pb->levels[max];
			} else {
				duty_cycle = brightness;
			}

			duty_cycle = pb->lth_brightness +
			     (duty_cycle * (pb->period - pb->lth_brightness) / max);
			pwm_config(pb->pwm, duty_cycle, pb->period);
			pwm_enable(pb->pwm);
		}
	}

	if (pb->notify_after)
		pb->notify_after(pb->dev, brightness);

	return 0;
}

static int pwm_backlight_get_brightness(struct backlight_device *bl)
{
	return bl->props.brightness;
}

static int pwm_backlight_check_fb(struct backlight_device *bl,
				  struct fb_info *info)
{
	struct pwm_bl_data *pb = bl_get_data(bl);

	return !pb->check_fb || pb->check_fb(pb->dev, info);
}

static const struct backlight_ops pwm_backlight_ops = {
	.update_status	= pwm_backlight_update_status,
	.get_brightness	= pwm_backlight_get_brightness,
	.check_fb	= pwm_backlight_check_fb,
};

#ifdef CONFIG_OF
static int pwm_backlight_dt_notify(struct device *dev, int brightness)
{
	struct backlight_device *bl = dev_get_drvdata(dev);
	struct pwm_bl_data *pb = bl_get_data(bl);
	int i;

	if (brightness) {
		for (i = 0; i < pb->num_gpios; i++) {
			if (pb->gpios[i].flags == OF_GPIO_ACTIVE_LOW)
				gpio_set_value(pb->gpios[i].gpio, 0);
			else
				gpio_set_value(pb->gpios[i].gpio, 1);
		}
		return 0;
	}

	/* de-assert gpios in reverse order, in case this is important */
	for (i = pb->num_gpios - 1; i >= 0; i--) {
		if (pb->gpios[i].flags == OF_GPIO_ACTIVE_LOW)
			gpio_set_value(pb->gpios[i].gpio, 1);
		else
			gpio_set_value(pb->gpios[i].gpio, 0);
	}
	return 0;
}

static void pwm_backlight_dt_exit(struct pwm_bl_data *pb)
{
	int i;

	for (i = 0; i < pb->num_gpios; i++)
		gpio_free(pb->gpios[i].gpio);
}

static int pwm_backlight_dt_init(struct device *dev, struct pwm_bl_data *pb)
{
	int i, j, ret;

	/* request gpios and drive in the inactive state */
	for (i = 0; i < pb->num_gpios; i++) {
		char gpio_name[32];
		unsigned long flags;
		if (pb->gpios[i].flags == OF_GPIO_ACTIVE_LOW)
			flags = GPIOF_OUT_INIT_LOW;
		else
			flags = GPIOF_OUT_INIT_HIGH;
		snprintf(gpio_name, 32, "%s.%d", dev_name(dev), i);
		ret = gpio_request_one(pb->gpios[i].gpio, flags, gpio_name);
		if (ret) {
			dev_err(dev, "gpio #%d request failed\n", i);
			goto gpio_err;
		}
	}
	return 0;

 gpio_err:
	for (j = 0; j < i; j++)
		gpio_free(pb->gpios[j].gpio);
	return ret;
}

static int pwm_backlight_parse_dt(struct device *dev,
				  struct platform_pwm_backlight_data *data,
				  struct pwm_bl_data *pb)
{
	struct device_node *node = dev->of_node;
	struct property *prop;
	int length;
	u32 value;
	int ret, i, num_gpios;
	size_t gpiosize;

	if (!node)
		return -ENODEV;

	memset(data, 0, sizeof(*data));

	/* determine the number of brightness levels */
	prop = of_find_property(node, "brightness-levels", &length);
	if (!prop)
		return -EINVAL;

	data->max_brightness = length / sizeof(u32);

	/* read brightness levels from DT property */
	if (data->max_brightness > 0) {
		size_t size = sizeof(*data->levels) * data->max_brightness;

		data->levels = devm_kzalloc(dev, size, GFP_KERNEL);
		if (!data->levels)
			return -ENOMEM;

		ret = of_property_read_u32_array(node, "brightness-levels",
						 data->levels,
						 data->max_brightness);
		if (ret < 0)
			return ret;

		ret = of_property_read_u32(node, "default-brightness-level",
					   &value);
		if (ret < 0)
			return ret;

		data->dft_brightness = value;
		data->max_brightness--;
	}

	/* read gpios from DT property */
	num_gpios = of_gpio_count(node);
	if (num_gpios == -ENOENT)
		return 0;	/* no 'gpios' property present */
	if (num_gpios < 0) {
		dev_err(dev, "invalid DT node: gpios\n");
		return -EINVAL;
	}
	gpiosize = sizeof(struct pwm_bl_gpio) * num_gpios;
	pb->gpios = devm_kzalloc(dev, gpiosize, GFP_KERNEL);
	if (!pb->gpios)
		return -ENOMEM;
	for (i = 0; i < num_gpios; i++) {
		int gpio;
		enum of_gpio_flags flags;
		gpio = of_get_gpio_flags(node, i, &flags);
		pb->gpios[i].gpio = (unsigned int)gpio;
		pb->gpios[i].flags = flags;
	}
	pb->num_gpios = (unsigned int)num_gpios;
	pb->notify = pwm_backlight_dt_notify;

	return pwm_backlight_dt_init(dev, pb);
}

static struct of_device_id pwm_backlight_of_match[] = {
	{ .compatible = "pwm-backlight" },
	{ }
};

MODULE_DEVICE_TABLE(of, pwm_backlight_of_match);
#else
static int pwm_backlight_parse_dt(struct device *dev,
				  struct platform_pwm_backlight_data *data,
				  struct pwm_bl_data *pb)
{
	return -ENODEV;
}
static void pwm_backlight_dt_exit(struct pwm_bl_data *pb) {}
#endif

static int pwm_backlight_probe(struct platform_device *pdev)
{
	struct platform_pwm_backlight_data *data = pdev->dev.platform_data;
	struct platform_pwm_backlight_data defdata;
	struct backlight_properties props;
	struct backlight_device *bl;
	struct pwm_bl_data *pb;
	unsigned int max;
	int ret;

	pb = devm_kzalloc(&pdev->dev, sizeof(*pb), GFP_KERNEL);
	if (!pb) {
		dev_err(&pdev->dev, "no memory for state\n");
		return -ENOMEM;
	}

	if (!data) {
		ret = pwm_backlight_parse_dt(&pdev->dev, &defdata, pb);
		if (ret < 0) {
			dev_err(&pdev->dev, "failed to find platform data\n");
			return ret;
		}

		data = &defdata;
	}

	if (data->init) {
		ret = data->init(&pdev->dev);
		if (ret < 0)
			return ret;
	}

	if (data->levels) {
		max = data->levels[data->max_brightness];
		pb->levels = data->levels;
	} else
		max = data->max_brightness;

	if (pb->notify == NULL)	/* not using DT and its built-in notify() */
		pb->notify = data->notify;
	pb->notify_after = data->notify_after;
	pb->check_fb = data->check_fb;
	pb->exit = data->exit;
	pb->dev = &pdev->dev;

	pb->pwm = devm_pwm_get(&pdev->dev, NULL);
	if (IS_ERR(pb->pwm)) {
		pb->pwm = NULL;
		if (pb->gpios) {
			dev_info(&pdev->dev, "non-pwm, gpio-only mode\n");
			goto accept;
		}

		dev_err(&pdev->dev, "unable to request PWM, trying legacy API\n");

		pb->pwm = pwm_request(data->pwm_id, "pwm-backlight");
		if (IS_ERR(pb->pwm)) {
			dev_err(&pdev->dev, "unable to request legacy PWM\n");
			ret = PTR_ERR(pb->pwm);
			goto err_alloc;
		}
	}

	dev_dbg(&pdev->dev, "got pwm for backlight\n");

	/*
	 * The DT case will set the pwm_period_ns field to 0 and store the
	 * period, parsed from the DT, in the PWM device. For the non-DT case,
	 * set the period from platform data.
	 */
accept:
	if (pb->pwm && data->pwm_period_ns > 0)
		pwm_set_period(pb->pwm, data->pwm_period_ns);

	if (pb->pwm) {
		pb->period = pwm_get_period(pb->pwm);
		pb->lth_brightness = data->lth_brightness * (pb->period / max);
	}

	memset(&props, 0, sizeof(struct backlight_properties));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = data->max_brightness;
	bl = backlight_device_register(dev_name(&pdev->dev), &pdev->dev, pb,
				       &pwm_backlight_ops, &props);
	if (IS_ERR(bl)) {
		dev_err(&pdev->dev, "failed to register backlight\n");
		ret = PTR_ERR(bl);
		goto err_alloc;
	}

	if (data->dft_brightness > data->max_brightness) {
		dev_warn(&pdev->dev,
			 "invalid default brightness level: %u, using %u\n",
			 data->dft_brightness, data->max_brightness);
		data->dft_brightness = data->max_brightness;
	}

	bl->props.brightness = data->dft_brightness;
	platform_set_drvdata(pdev, bl);
	backlight_update_status(bl);

	return 0;

err_alloc:
	if (data->exit)
		data->exit(&pdev->dev);
	return ret;
}

static int pwm_backlight_remove(struct platform_device *pdev)
{
	struct backlight_device *bl = platform_get_drvdata(pdev);
	struct pwm_bl_data *pb = bl_get_data(bl);

	backlight_device_unregister(bl);
	pwm_config(pb->pwm, 0, pb->period);
	pwm_disable(pb->pwm);
	if (pb->exit)
		pb->exit(&pdev->dev);
	pwm_backlight_dt_exit(pb);
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int pwm_backlight_suspend(struct device *dev)
{
	struct backlight_device *bl = dev_get_drvdata(dev);
	struct pwm_bl_data *pb = bl_get_data(bl);

	if (pb->notify)
		pb->notify(pb->dev, 0);
	if (pb->pwm) {
		pwm_config(pb->pwm, 0, pb->period);
		pwm_disable(pb->pwm);
	}
	if (pb->notify_after)
		pb->notify_after(pb->dev, 0);
	return 0;
}

static int pwm_backlight_resume(struct device *dev)
{
	struct backlight_device *bl = dev_get_drvdata(dev);

	backlight_update_status(bl);
	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(pwm_backlight_pm_ops, pwm_backlight_suspend,
			 pwm_backlight_resume);

static struct platform_driver pwm_backlight_driver = {
	.driver		= {
		.name		= "pwm-backlight",
		.owner		= THIS_MODULE,
		.pm		= &pwm_backlight_pm_ops,
		.of_match_table	= of_match_ptr(pwm_backlight_of_match),
	},
	.probe		= pwm_backlight_probe,
	.remove		= pwm_backlight_remove,
};

module_platform_driver(pwm_backlight_driver);

MODULE_DESCRIPTION("PWM based Backlight Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:pwm-backlight");

