/*
 * Author: Christopher R. Palmer <crpalmer@gmail.com>
 *
 * Driver framework copied from Paul Reioux aka Faux123 <reioux@gmail.com>'s
 * intelli_plug.
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
#include <linux/earlysuspend.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/module.h>

#define SIMPLE_PLUG_MAJOR_VERSION	1
#define SIMPLE_PLUG_MINOR_VERSION	0

#define DEF_SAMPLING_MS			(50)
#define HISTORY_SIZE			5
#define NUM_PROC			4

static DEFINE_MUTEX(simple_plug_mutex);

struct delayed_work simple_plug_work;

static unsigned int simple_plug_active = 1;

static bool suspended = false;

static unsigned int reset_stats;
static unsigned int time_cores_running[NUM_PROC];
static unsigned int times_core_up[NUM_PROC];
static unsigned int times_core_down[NUM_PROC];
static unsigned int nr_run_last_array[HISTORY_SIZE];
static unsigned int nr_last_i;
static unsigned int nr_avg;

module_param(simple_plug_active, uint, 0644);
module_param(nr_avg, uint, 0444);
module_param_array(nr_run_last_array, uint, NULL, 0444);

module_param(reset_stats, uint, 0644);
module_param_array(time_cores_running, uint, NULL, 0444);
module_param_array(times_core_up, uint, NULL, 0444);
module_param_array(times_core_down, uint, NULL, 0444);

static unsigned int calculate_thread_stats(void)
{
	int target_cores;

	nr_avg -= nr_run_last_array[nr_last_i];
	nr_avg += nr_run_last_array[nr_last_i] = avg_nr_running();
	nr_last_i = (nr_last_i + 1) % HISTORY_SIZE;

	target_cores = ((nr_avg>>FSHIFT) / HISTORY_SIZE);

	if (target_cores > NUM_PROC)
		return NUM_PROC;
	else if (target_cores < 1)
		return 1;
	else
		return target_cores;
}

static void
cpus_up_down(int nr_run_stat)
{
	int n_online = num_online_cpus();

	BUG_ON(nr_run_stat < 1 || nr_run_stat > NUM_PROC);

	time_cores_running[nr_run_stat-1]++;

	while(n_online < nr_run_stat) {
		times_core_up[n_online]++;
		cpu_up(n_online);
		n_online++;
	}

	while(n_online > nr_run_stat) {
		n_online--;
		times_core_down[n_online]++;
		cpu_down(n_online);
	}
}
     
static void __cpuinit simple_plug_work_fn(struct work_struct *work)
{
	unsigned int nr_run_stat;

	if (reset_stats) {
		reset_stats = 0;
		memset(time_cores_running, 0, sizeof(time_cores_running));
		memset(times_core_up, 0, sizeof(times_core_up));
		memset(times_core_down, 0, sizeof(times_core_down));
	}
	
	if (simple_plug_active == 1) {
		nr_run_stat = calculate_thread_stats();
		if (!suspended)
			cpus_up_down(nr_run_stat);
	}

	schedule_delayed_work_on(0, &simple_plug_work,
		msecs_to_jiffies(DEF_SAMPLING_MS));
}


#ifdef CONFIG_HAS_EARLYSUSPEND
static void simple_plug_early_suspend(struct early_suspend *handler)
{
	int i;
	
	cancel_delayed_work_sync(&simple_plug_work);

	mutex_lock(&simple_plug_mutex);
	suspended = true;
	mutex_unlock(&simple_plug_mutex);

	// put rest of the cores to sleep!
	for (i=1; i < NUM_PROC; i++) {
		if (cpu_online(i))
			cpu_down(i);
	}
}

static void __cpuinit simple_plug_late_resume(struct early_suspend *handler)
{
	mutex_lock(&simple_plug_mutex);
	suspended = false;
	mutex_unlock(&simple_plug_mutex);

	schedule_delayed_work_on(0, &simple_plug_work,
		msecs_to_jiffies(10));
}

static struct early_suspend simple_plug_early_suspend_struct_driver = {
	.level = EARLY_SUSPEND_LEVEL_DISABLE_FB + 10,
	.suspend = simple_plug_early_suspend,
	.resume = simple_plug_late_resume,
};
#endif	/* CONFIG_HAS_EARLYSUSPEND */

int __init simple_plug_init(void)
{
	/* We want all CPUs to do sampling nearly on same jiffy */
	int delay = msecs_to_jiffies(DEF_SAMPLING_MS);

	if (num_online_cpus() > 1)
		delay -= jiffies % delay;

	//pr_info("simple_plug: scheduler delay is: %d\n", delay);
	pr_info("simple_plug: version %d.%d by faux123\n",
		 SIMPLE_PLUG_MAJOR_VERSION,
		 SIMPLE_PLUG_MINOR_VERSION);

	INIT_DELAYED_WORK(&simple_plug_work, simple_plug_work_fn);
	schedule_delayed_work_on(0, &simple_plug_work, delay);

#ifdef CONFIG_HAS_EARLYSUSPEND
	register_early_suspend(&simple_plug_early_suspend_struct_driver);
#endif
	return 0;
}

MODULE_AUTHOR("Christopher R. Palmer <crpalmer@gmail.com>");
MODULE_DESCRIPTION("'intell_plug' - An simple cpu hotplug driver for "
	"Low Latency Frequency Transition capable processors");
MODULE_LICENSE("GPL");

late_initcall(simple_plug_init);
