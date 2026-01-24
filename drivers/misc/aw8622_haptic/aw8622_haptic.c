/*
 *  PWM haptic driver
 *
 *  Copyright (C) 2017 Collabora Ltd.
 *
 *
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under  the terms of the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the License, or (at your
 *  option) any later version.
 */

#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/of_gpio.h>
#include <linux/dma-mapping.h>
#include <linux/time64.h>

#include <mt-plat/mtk_pwm_hal_pub.h>
#include <mt-plat/mtk_pwm.h>
#include <mt-plat/mt6785/include/mach/mtk_pwm_hal.h>
#include <linux/workqueue.h>
#include "aw8622_haptic.h"

static char *aw8622_pwm_gpio_cfg[] = {"haptic_gpio_aw8622_default", "haptic_gpio_aw8622_set"};
static u64 aw8622_dma_mask = DMA_BIT_MASK(32);

static struct pwm_spec_config aw8622_pwm_old_mode_config = {
	.pwm_no = 0,
	.mode = PWM_MODE_OLD,
	.clk_div = CLK_DIV1,
	.clk_src = PWM_CLK_OLD_MODE_BLOCK,
	.pmic_pad = 0,

	.PWM_MODE_OLD_REGS.IDLE_VALUE = IDLE_FALSE,
	.PWM_MODE_OLD_REGS.WAVE_NUM = 0,
	.PWM_MODE_OLD_REGS.DATA_WIDTH = 1000,
	.PWM_MODE_OLD_REGS.GDURATION = 0,
	.PWM_MODE_OLD_REGS.THRESH = 500,

	.PWM_MODE_FIFO_REGS.IDLE_VALUE = 0,
	.PWM_MODE_FIFO_REGS.GUARD_VALUE = 0,
	.PWM_MODE_FIFO_REGS.GDURATION = 0,
	.PWM_MODE_FIFO_REGS.WAVE_NUM = 0,
};

static void aw8622_hw_off_work(struct work_struct *delay_work) {
	struct aw8622_haptic *haptic = NULL;
	struct delayed_work *p_delayed_work = NULL;

	p_delayed_work = container_of(delay_work, struct delayed_work, work);
	haptic = container_of(p_delayed_work, struct aw8622_haptic, hw_off_work);

	pr_debug("%s\n", __func__);

	mutex_lock(&haptic->mutex_lock);
	if (haptic->is_actived) {
		pr_debug("%s is active hw off failed\n", __func__);
		mutex_unlock(&haptic->mutex_lock);
		return;
	}

	if (!haptic->is_actived) {
		pr_debug("%s hw off success\n", __func__);
		gpio_set_value(haptic->hwen_gpio, 0);
		udelay(1000);
		pr_debug("%s pwm call mt_pwm_disable\n", __func__);
		mt_pwm_disable(haptic->pwm_ch, aw8622_pwm_old_mode_config.pmic_pad);
		haptic->is_power_on = false;
	}
	mutex_unlock(&haptic->mutex_lock);
}

static void aw8622_switch_pwm_gpio_mode(struct aw8622_haptic *haptic, int mode)
{
	struct pinctrl_state *pins_state = NULL;

	if (mode >= (ARRAY_SIZE(aw8622_pwm_gpio_cfg))) {
		pr_err("%s() invalid parameter mode=%d\n", __func__, mode);
		return;
	}

	if (IS_ERR(haptic->ppinctrl_pwm)) {
		pr_err("%s() ppinctrl_haptic:%p err:%ld\n",
				__func__, haptic->ppinctrl_pwm, PTR_ERR(haptic->ppinctrl_pwm));
		return;
	}

	pins_state = pinctrl_lookup_state(haptic->ppinctrl_pwm, aw8622_pwm_gpio_cfg[mode]);
	if (IS_ERR(pins_state)) {
		pr_err("%s() pinctrl_lookup_state failed for mode=%d\n", __func__, mode);
		return;
	}

	pinctrl_select_state(haptic->ppinctrl_pwm, pins_state);
	pr_debug("%s() switched to mode:%d\n", __func__, mode);
}

static int aw8622_state_init(struct aw8622_haptic *haptic) {
	aw8622_switch_pwm_gpio_mode(haptic, HAPTIC_GPIO_AW8622_SET);
	haptic->is_power_on = false;
	haptic->is_actived = false;
	haptic->is_hwen = false;
	haptic->duration = 10;
	haptic->frequency = DEFAULT_FREQUENCY;
	return 0;
}

static int aw8622_update_pwm_frequency(struct aw8622_haptic *haptic)
{
	int err = 0;
	unsigned int data_width, thresh;
	static unsigned int last_freq = 0;

	if (haptic->frequency == 0)
		return -EINVAL;

	if (haptic->frequency == last_freq)
		return 0;

	pr_debug("%s frequency=%u Hz\n", __func__, haptic->frequency);

	mt_pwm_disable(aw8622_pwm_old_mode_config.pwm_no, aw8622_pwm_old_mode_config.pmic_pad);
	mt_pwm_clk_sel_hal(aw8622_pwm_old_mode_config.pwm_no, CLK_26M);

	aw8622_pwm_old_mode_config.clk_div = CLK_DIV1;
	data_width = HAPTIC_PWM_OLD_MODE_CLOCK / haptic->frequency;
	thresh = data_width / 2;

	aw8622_pwm_old_mode_config.PWM_MODE_OLD_REGS.DATA_WIDTH = data_width;
	aw8622_pwm_old_mode_config.PWM_MODE_OLD_REGS.THRESH = thresh;

	err = pwm_set_spec_config(&aw8622_pwm_old_mode_config);
	if (err < 0) {
		dev_err(haptic->dev, "%s pwm_set_spec_config failed\n", __func__);
		last_freq = 0;
	} else {
		last_freq = haptic->frequency;
	}

	return err;
}

static int aw8622_set_pwm_default_state(struct aw8622_haptic *haptic)
{
	return aw8622_update_pwm_frequency(haptic);
}

static void aw8622_haptic_stop(struct aw8622_haptic *haptic)
{
	aw8622_set_pwm_default_state(haptic);
	haptic->is_actived = false;
}

static int aw8622_play_wave(struct aw8622_haptic *haptic)
{
	pr_debug("%s enter\n", __func__);

	if (haptic->is_power_on) {
		mt_pwm_disable(aw8622_pwm_old_mode_config.pwm_no, aw8622_pwm_old_mode_config.pmic_pad);
	}

	aw8622_update_pwm_frequency(haptic);

	if (!haptic->is_power_on) {
		gpio_set_value(haptic->hwen_gpio, 1);
		haptic->is_power_on = true;
	}

	hrtimer_start(&haptic->timer,
				  ktime_set(haptic->duration / MSEC_PER_SEC,
							(haptic->duration % MSEC_PER_SEC) * NSEC_PER_MSEC),
				  HRTIMER_MODE_REL);
	return 0;
}

static enum hrtimer_restart aw8622_haptic_timer_func(struct hrtimer *timer)
{
	struct aw8622_haptic *haptic = container_of(timer, struct aw8622_haptic, timer);

	pr_debug("%s enter\n", __func__);
	queue_work(haptic->aw8622_wq, &haptic->stop_play_work);

	return HRTIMER_NORESTART;
}

static void aw8622_haptic_play_work(struct work_struct *work)
{
	struct aw8622_haptic *haptic = container_of(work, struct aw8622_haptic, play_work);
	int ret = 0;

	pr_debug("%s enter\n", __func__);

	if (haptic->is_actived) {
		ret = aw8622_play_wave(haptic);
		if (ret < 0) {
			pr_err("%s aw8622_play_wave failed\n", __func__);
			queue_work(haptic->aw8622_wq, &haptic->stop_play_work);
		}
	}
}

static void aw8622_haptic_stop_play_work(struct work_struct *work)
{
	struct aw8622_haptic *haptic = container_of(work, struct aw8622_haptic, stop_play_work);

	pr_debug("%s enter\n", __func__);

	if (!haptic->is_actived) {
		dev_err(haptic->dev, "%s logic error\n", __func__);
	}

	mutex_lock(&haptic->mutex_lock);
	cancel_delayed_work_sync(&haptic->hw_off_work);
	queue_delayed_work(haptic->aw8622_wq, &haptic->hw_off_work, 30 * HZ);

	aw8622_haptic_stop(haptic);
	mutex_unlock(&haptic->mutex_lock);
}

static int aw8622_hwen_init(struct aw8622_haptic *haptic)
{
	int ret = 0;
	struct device_node *node = haptic->dev->of_node;

	haptic->hwen_gpio = of_get_named_gpio(node, "hwen-gpio", 0);
	if ((!gpio_is_valid(haptic->hwen_gpio))) {
		dev_err(haptic->dev, "%s: dts don't provide hwen-gpio\n", __func__);
		return -EINVAL;
	}

	ret = gpio_request(haptic->hwen_gpio, "aw8622-hwen");
	if (ret) {
		dev_err(haptic->dev, "%s: unable to request gpio [%d]\n", __func__, haptic->hwen_gpio);
		return ret;
	}

	ret = gpio_direction_output(haptic->hwen_gpio, 0);
	if (ret) {
		gpio_free(haptic->hwen_gpio);
		dev_err(haptic->dev, "%s: unable to set direction of gpio\n", __func__);
		return ret;
	}
	return ret;
}

static int aw8622_parse_devicetree_info(struct aw8622_haptic *haptic)
{
	int err = 0;

	err = of_property_read_u32(haptic->dev->of_node, "center_freq", &haptic->center_freq);
	if (err < 0) {
		dev_warn(haptic->dev, "get center_freq failed, using default\n");
		haptic->center_freq = DEFAULT_FREQUENCY;
	}

	err = of_property_read_u32(haptic->dev->of_node, "default_pwm_freq", &haptic->default_pwm_freq);
	if (err < 0) {
		dev_warn(haptic->dev, "get default_pwm_freq failed, using default\n");
		haptic->default_pwm_freq = 26000;
	}

	err = of_property_read_u32(haptic->dev->of_node, "pwm_ch", &haptic->pwm_ch);
	if (err < 0) {
		dev_err(haptic->dev, "get pwm_ch failed\n");
		return -EINVAL;
	}

	haptic->ppinctrl_pwm = devm_pinctrl_get(haptic->dev);
	if (IS_ERR(haptic->ppinctrl_pwm)) {
		pr_notice("%s() cannot find pinctrl! ptr_err:%ld.\n",
				  __func__, PTR_ERR(haptic->ppinctrl_pwm));
		err = PTR_ERR(haptic->ppinctrl_pwm);
	}

	pr_debug("%s dt info def_pwm_freq=%uHz center_freq=%u\n",
			__func__, haptic->default_pwm_freq, haptic->center_freq);
	pr_debug("%s dt info pwm_ch=%u\n", __func__, haptic->pwm_ch);

	return err;
}

/* Sysfs attributes */
static ssize_t aw8622_activate_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct aw8622_haptic *haptic = dev_get_drvdata(dev);
	return snprintf(buf, PAGE_SIZE, "%d\n", haptic->is_actived);
}

static ssize_t aw8622_activate_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct aw8622_haptic *haptic = dev_get_drvdata(dev);
	unsigned int val = 0;
	int rc = 0;

	rc = kstrtouint(buf, 0, &val);
	if (rc < 0)
		return rc;

	pr_debug("%s: value=%d\n", __func__, val);

	mutex_lock(&haptic->mutex_lock);
	if (val == 1) {
		if (!haptic->is_actived) {
			haptic->is_actived = true;
			queue_work(haptic->aw8622_wq, &haptic->play_work);
		}
	} else {
		if (haptic->is_actived) {
			if (hrtimer_try_to_cancel(&haptic->timer) > 0) {
				dev_info(haptic->dev, "%s Manually stop haptic\n", __func__);
				queue_work(haptic->aw8622_wq, &haptic->stop_play_work);
			}
		}
	}
	mutex_unlock(&haptic->mutex_lock);

	return count;
}

static ssize_t aw8622_duration_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct aw8622_haptic *haptic = dev_get_drvdata(dev);
	return snprintf(buf, PAGE_SIZE, "%d\n", haptic->duration);
}

static ssize_t aw8622_duration_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct aw8622_haptic *haptic = dev_get_drvdata(dev);
	unsigned int val = 0;
	int rc = 0;

	rc = kstrtouint(buf, 0, &val);
	if (rc < 0)
		return rc;

	pr_debug("%s: duration=%d\n", __func__, val);

	if (val <= 0)
		return count;

	mutex_lock(&haptic->mutex_lock);
	haptic->duration = val;
	mutex_unlock(&haptic->mutex_lock);

	return count;
}

static ssize_t aw8622_frequency_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct aw8622_haptic *haptic = dev_get_drvdata(dev);
	return snprintf(buf, PAGE_SIZE, "%u\n", haptic->frequency);
}

static ssize_t aw8622_frequency_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct aw8622_haptic *haptic = dev_get_drvdata(dev);
	unsigned int val = 0;
	int rc = 0;

	rc = kstrtouint(buf, 0, &val);
	if (rc < 0)
		return rc;

	if (val < MIN_FREQUENCY || val > MAX_FREQUENCY) {
		dev_err(haptic->dev, "freq out of range (%d-%d Hz)\n",
				MIN_FREQUENCY, MAX_FREQUENCY);
		return -EINVAL;
	}

	pr_debug("%s: frequency=%u Hz\n", __func__, val);

	mutex_lock(&haptic->mutex_lock);
	haptic->frequency = val;

	if (haptic->is_power_on && !haptic->is_actived) {
		aw8622_update_pwm_frequency(haptic);
	}
	mutex_unlock(&haptic->mutex_lock);

	return count;
}

static ssize_t aw8622_hwen_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct aw8622_haptic *haptic = dev_get_drvdata(dev);
	return snprintf(buf, PAGE_SIZE, "%s\n",
					gpio_get_value(haptic->hwen_gpio) ? "enable" : "disable");
}

static ssize_t aw8622_hwen_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct aw8622_haptic *haptic = dev_get_drvdata(dev);
	unsigned int val = 0;
	int rc = 0;

	rc = kstrtouint(buf, 0, &val);
	if (rc < 0)
		return rc;

	pr_debug("%s: value=%d\n", __func__, val);

	if (val == 1) {
		gpio_set_value(haptic->hwen_gpio, 1);
		msleep(50);
	} else {
		gpio_set_value(haptic->hwen_gpio, 0);
	}
	return count;
}

static ssize_t aw8622_vendor_info_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "vendor: awinic aw8622\n");
}

static DEVICE_ATTR(activate, S_IWUSR | S_IRUGO, aw8622_activate_show, aw8622_activate_store);
static DEVICE_ATTR(duration, S_IWUSR | S_IRUGO, aw8622_duration_show, aw8622_duration_store);
static DEVICE_ATTR(frequency, S_IWUSR | S_IRUGO, aw8622_frequency_show, aw8622_frequency_store);
static DEVICE_ATTR(hwen, S_IWUSR | S_IRUGO, aw8622_hwen_show, aw8622_hwen_store);
static DEVICE_ATTR(info, S_IRUGO, aw8622_vendor_info_show, NULL);

static struct attribute *aw8622_vibrator_attributes[] = {
	&dev_attr_duration.attr,
	&dev_attr_activate.attr,
	&dev_attr_frequency.attr,
	&dev_attr_hwen.attr,
	&dev_attr_info.attr,
	NULL,
};

static struct attribute_group aw8622_vibrator_attribute_group = {
	.attrs = aw8622_vibrator_attributes
};

static int aw8622_haptic_probe(struct platform_device *pdev)
{
	struct aw8622_haptic *haptic;
	int err;

	pr_debug("%s enter\n", __func__);

	haptic = devm_kzalloc(&pdev->dev, sizeof(*haptic), GFP_KERNEL);
	if (!haptic)
		return -ENOMEM;

	haptic->dev = &pdev->dev;
	platform_set_drvdata(pdev, haptic);

	err = aw8622_parse_devicetree_info(haptic);
	if (err < 0) {
		dev_err(haptic->dev, "%s aw8622 parse devicetree info failed\n", __func__);
		return err;
	}

	err = aw8622_hwen_init(haptic);
	if (err) {
		dev_err(haptic->dev, "%s aw8622 hwen error\n", __func__);
		return err;
	}

	haptic->aw8622_wq = create_singlethread_workqueue("aw8622 vibrator work queue");
	if (!haptic->aw8622_wq) {
		dev_err(haptic->dev, "%s create workqueue error\n", __func__);
		return -ENOMEM;
	}

	INIT_WORK(&haptic->play_work, aw8622_haptic_play_work);
	INIT_WORK(&haptic->stop_play_work, aw8622_haptic_stop_play_work);
	INIT_DELAYED_WORK(&haptic->hw_off_work, aw8622_hw_off_work);

	hrtimer_init(&haptic->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	haptic->timer.function = aw8622_haptic_timer_func;
	mutex_init(&haptic->mutex_lock);

	haptic->dev->dma_mask = &aw8622_dma_mask;
	haptic->dev->coherent_dma_mask = aw8622_dma_mask;

	aw8622_pwm_old_mode_config.pwm_no = haptic->pwm_ch;

	aw8622_state_init(haptic);

	err = sysfs_create_group(&pdev->dev.kobj, &aw8622_vibrator_attribute_group);
	if (err < 0) {
		dev_info(&pdev->dev, "%s error creating sysfs attr files\n", __func__);
		destroy_workqueue(haptic->aw8622_wq);
		return err;
	}

	pr_debug("%s probe success (default frequency: %u Hz)\n", __func__, haptic->frequency);
	return 0;
}

static int aw8622_haptic_remove(struct platform_device *pdev)
{
	struct aw8622_haptic *haptic = platform_get_drvdata(pdev);

	sysfs_remove_group(&pdev->dev.kobj, &aw8622_vibrator_attribute_group);
	cancel_work_sync(&haptic->play_work);
	cancel_work_sync(&haptic->stop_play_work);
	cancel_delayed_work_sync(&haptic->hw_off_work);
	destroy_workqueue(haptic->aw8622_wq);
	hrtimer_cancel(&haptic->timer);
	gpio_free(haptic->hwen_gpio);

	return 0;
}

static int __maybe_unused aw8622_haptic_suspend(struct device *dev)
{
	struct aw8622_haptic *haptic = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&haptic->hw_off_work);
	mutex_lock(&haptic->mutex_lock);
	if (haptic->is_power_on) {
		gpio_set_value(haptic->hwen_gpio, 0);
		udelay(500);
		pr_debug("%s pwm call mt_pwm_disable\n", __func__);
		mt_pwm_disable(aw8622_pwm_old_mode_config.pwm_no, aw8622_pwm_old_mode_config.pmic_pad);
		haptic->is_power_on = false;
	}
	mutex_unlock(&haptic->mutex_lock);
	return 0;
}

static int __maybe_unused aw8622_haptic_resume(struct device *dev)
{
	pr_debug("%s\n", __func__);
	return 0;
}

static SIMPLE_DEV_PM_OPS(aw8622_haptic_pm_ops, aw8622_haptic_suspend, aw8622_haptic_resume);

#ifdef CONFIG_OF
static const struct of_device_id pwm_vibra_dt_match_table[] = {
	{ .compatible = "awinic,aw8622" },
	{},
};
MODULE_DEVICE_TABLE(of, pwm_vibra_dt_match_table);
#endif

static struct platform_driver aw8622_haptic_driver = {
	.probe	= aw8622_haptic_probe,
	.remove	= aw8622_haptic_remove,
	.driver	= {
		.name	= "awinic,aw8622-haptic",
		.pm	= &aw8622_haptic_pm_ops,
		.of_match_table = of_match_ptr(pwm_vibra_dt_match_table),
	},
};
module_platform_driver(aw8622_haptic_driver);

MODULE_AUTHOR("luofuhong@awinic.com");
MODULE_DESCRIPTION("aw8622 haptic driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("aw8622 haptic");
