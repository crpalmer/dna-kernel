/* Author: Christopher R. Palmer <crpalmer@gmail.com>
 *
 * Very loosely based on a version released by HTC that was
 * Copyright (c) 2012, Code Aurora Forum. All rights reserved.
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

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/msm_tsens.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/msm_tsens.h>
#include <linux/msm_thermal.h>
#include <mach/cpufreq.h>
#include <mach/perflock.h>
#include <linux/earlysuspend.h>

#define NOT_THROTTLED		0

static int enabled = 1;

static unsigned watch_temp_degC = 50;
static unsigned poll_ms = 1000;

static unsigned temp_hysteresis = 5;
static unsigned limit_temp_1_degC = 60;
static unsigned limit_temp_2_degC = 70;
static unsigned limit_temp_3_degC = 80;

static unsigned limit_freq_1 = 1350000;
static unsigned limit_freq_2 =  918000;
static unsigned limit_freq_3 =  384000;

module_param(watch_temp_degC, uint, 0644);
module_param(poll_ms, uint, 0644);

module_param(limit_temp_1_degC, uint, 0644);
module_param(limit_temp_2_degC, uint, 0644);
module_param(limit_temp_3_degC, uint, 0644);
module_param(limit_freq_1, uint, 0644);
module_param(limit_freq_2, uint, 0644);
module_param(limit_freq_3, uint, 0644);

static unsigned release_temperature = NOT_THROTTLED;
static unsigned limited_max_freq = MSM_CPUFREQ_NO_LIMIT;

module_param(release_temperature, uint, 0444);
module_param(limited_max_freq, uint, 0444);

static struct msm_thermal_data msm_thermal_info;
static struct delayed_work check_temp_work;

static int update_cpu_max_freq(int cpu, unsigned max_freq)
{
	int ret;

	ret = msm_cpufreq_set_freq_limits(cpu, MSM_CPUFREQ_NO_LIMIT, max_freq);
	if (ret)
		return ret;

	ret = cpufreq_update_policy(cpu);
	if (ret)
		return ret;

	if (max_freq != MSM_CPUFREQ_NO_LIMIT) {
		struct cpufreq_policy policy;

		if ((ret = cpufreq_get_policy(&policy, cpu)) == 0)
			ret = cpufreq_driver_target(&policy, max_freq, CPUFREQ_RELATION_L);
	}

	if (max_freq != MSM_CPUFREQ_NO_LIMIT)
		pr_info("msm_thermal: Limiting cpu%d max frequency to %d\n",
				cpu, max_freq);
	else
		pr_info("msm_thermal: Max frequency reset for cpu%d\n", cpu);

	return ret;
}

static void
update_all_cpus_max_freq_if_changed(unsigned max_freq)
{
	int cpu;
	int ret;

	if (max_freq == limited_max_freq)
		return;

#ifdef CONFIG_PERFLOCK_BOOT_LOCK
	release_boot_lock();
#endif

	limited_max_freq = max_freq;
	
	/* Update new limits */
	for_each_possible_cpu(cpu) {
		ret = update_cpu_max_freq(cpu, max_freq);
		if (ret)
			pr_warn("Unable to limit cpu%d max freq to %d\n",
					cpu, max_freq);
	}
}

static void
schedule_work_if_enabled(void)
{
	if (enabled)
		schedule_delayed_work(&check_temp_work,
			msecs_to_jiffies(poll_ms));
}

static unsigned
select_frequency(unsigned temp)
{

	if (temp >= limit_temp_3_degC) {
		release_temperature = limit_temp_3_degC - temp_hysteresis;
		return limit_freq_3;
	}

	if (release_temperature < limit_temp_2_degC && temp >= limit_temp_2_degC) {
		release_temperature = limit_temp_2_degC - temp_hysteresis;
		return limit_freq_2;
	}

	if (release_temperature < limit_temp_1_degC && temp >= limit_temp_1_degC) {
		release_temperature = limit_temp_1_degC - temp_hysteresis;
		return limit_freq_1;
	}

	if (temp > release_temperature)
		return limited_max_freq;

	release_temperature = NOT_THROTTLED;
	return MSM_CPUFREQ_NO_LIMIT;
}

static void check_temp_and_throttle_if_needed(struct work_struct *work)
{
	struct tsens_device tsens_dev;
	unsigned long temp_ul = 0;
	unsigned temp;
	unsigned max_freq;
	int ret;

	tsens_dev.sensor_num = msm_thermal_info.sensor_id;
	ret = tsens_get_temp(&tsens_dev, &temp_ul);
	if (ret) {
		pr_warn("msm_thermal: Unable to read TSENS sensor %d\n",
				tsens_dev.sensor_num);
		return;
	}

	temp = (unsigned) temp_ul;
	max_freq = select_frequency(temp);

	if (temp >= watch_temp_degC) {
		pr_info("msm_thermal: TSENS sensor %d is %u degC max-freq %u\n",
			tsens_dev.sensor_num, temp, max_freq);
	} else
		pr_debug("msm_thermal: TSENS sensor %d is %u degC\n",
				tsens_dev.sensor_num, temp);

	update_all_cpus_max_freq_if_changed(max_freq);
}

static void check_temp(struct work_struct *work)
{
	check_temp_and_throttle_if_needed(work);
	schedule_work_if_enabled();
}

static void disable_msm_thermal(void)
{
	int cpu = 0;

	/* make sure check_temp is no longer running */
	cancel_delayed_work(&check_temp_work);
	flush_scheduled_work();

	if (limited_max_freq == MSM_CPUFREQ_NO_LIMIT)
		return;

	for_each_possible_cpu(cpu) {
		update_cpu_max_freq(cpu, MSM_CPUFREQ_NO_LIMIT);
	}
}

static int set_enabled(const char *val, const struct kernel_param *kp)
{
	int ret;

	ret = param_set_bool(val, kp);
	if (!enabled)
		disable_msm_thermal();
	else
		schedule_work_if_enabled();

	pr_info("msm_thermal: enabled = %d\n", enabled);

	return ret;
}

static struct kernel_param_ops module_ops = {
	.set = set_enabled,
	.get = param_get_bool,
};

module_param_cb(enabled, &module_ops, &enabled, 0644);
MODULE_PARM_DESC(enabled, "enforce thermal limit on cpu");

#ifdef CONFIG_HAS_EARLYSUSPEND

static void msm_thermal_early_suspend(struct early_suspend *handler)
{
	cancel_delayed_work_sync(&check_temp_work);
}

static void __cpuinit msm_thermal_late_resume(struct early_suspend *handler)
{
	schedule_delayed_work_on(0, &check_temp_work,
		msecs_to_jiffies(poll_ms));
}

static struct early_suspend msm_thermal_early_suspend_struct_driver = {
	.level = EARLY_SUSPEND_LEVEL_DISABLE_FB + 10,
	.suspend = msm_thermal_early_suspend,
	.resume = msm_thermal_late_resume,
};
#endif	/* CONFIG_HAS_EARLYSUSPEND */
int __init msm_thermal_init(struct msm_thermal_data *pdata)
{
	BUG_ON(!pdata);
	BUG_ON(pdata->sensor_id >= TSENS_MAX_SENSORS);
	memcpy(&msm_thermal_info, pdata, sizeof(struct msm_thermal_data));

#ifdef CONFIG_HAS_EARLYSUSPEND
	register_early_suspend(&msm_thermal_early_suspend_struct_driver);
#endif

	enabled = 1;
	INIT_DELAYED_WORK(&check_temp_work, check_temp);
	schedule_delayed_work(&check_temp_work, 0);

	return 0;
}
