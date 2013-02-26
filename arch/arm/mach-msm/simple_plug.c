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

#define PR_NAME "simple_plug: "

#include <linux/earlysuspend.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/cpufreq.h>

#define SIMPLE_PLUG_MAJOR_VERSION	1
#define SIMPLE_PLUG_MINOR_VERSION	0

#define DEF_SAMPLING_MS			(10)
#define HISTORY_SIZE			10
#define NUM_CORES			4

static struct delayed_work first_work;
static struct delayed_work simple_plug_work;

static unsigned int simple_plug_active = 1;
static unsigned int min_cores = 1;
static unsigned int max_cores = NUM_CORES;
static unsigned int sampling_ms = DEF_SAMPLING_MS;

#ifdef CONFIG_SIMPLE_PLUG_STATS
static unsigned int reset_stats;
static unsigned int time_cores_running[NUM_CORES];
static unsigned int times_core_up[NUM_CORES];
static unsigned int times_core_down[NUM_CORES];
#endif

static unsigned int nr_avg;
static unsigned int nr_run_history[HISTORY_SIZE];
static unsigned int nr_last_i;

static unsigned int n_online;

module_param(simple_plug_active, uint, 0644);
module_param(min_cores, uint, 0644);
module_param(max_cores, uint, 0644);
module_param(sampling_ms, uint, 0644);

#ifdef CONFIG_SIMPLE_PLUG_STATS
module_param(reset_stats, uint, 0644);
module_param_array(time_cores_running, uint, NULL, 0444);
module_param_array(times_core_up, uint, NULL, 0444);
module_param_array(times_core_down, uint, NULL, 0444);
#endif

module_param(nr_avg, uint, 0444);
module_param_array(nr_run_history, uint, NULL, 0444);
module_param(n_online, uint, 0444);

static unsigned int desired_number_of_cores(void)
{
	int target_cores;
	int avg;

	nr_avg -= nr_run_history[nr_last_i];
	nr_avg += nr_run_history[nr_last_i] = avg_nr_running();
	nr_last_i = (nr_last_i + 1) % HISTORY_SIZE;

	/* Compute number of cores of average active work.
	 * If potentially decreasing the number of cores, truncate up
	 * If potentially increasing the number of cores, truncate down.
	 */

	avg = nr_avg / HISTORY_SIZE;
	target_cores = avg >> FSHIFT;
	if (target_cores < n_online)
		target_cores = (avg + (1<<FSHIFT)-1) >> FSHIFT;

	if (target_cores > max_cores)
		return max_cores;
	else if (target_cores < min_cores)
		return min_cores;
	else
		return target_cores;
}

static void
cpus_up_down(int nr_run_stat)
{
	BUG_ON(nr_run_stat < 1 || nr_run_stat > NUM_CORES);

#ifdef CONFIG_SIMPLE_PLUG_STATS
	time_cores_running[nr_run_stat-1]++;
#endif

	while(n_online < nr_run_stat) {
		struct cpufreq_policy policy;
		int ret;

		pr_debug(PR_NAME "starting cpu%d, want %d online\n", n_online, nr_run_stat);
#ifdef CONFIG_SIMPLE_PLUG_STATS
		times_core_up[n_online]++;
#endif
		cpu_up(n_online);

                if ((ret = cpufreq_get_policy(&policy, n_online)) == 0) {
		        if ((ret = cpufreq_driver_target(&policy, policy.max, CPUFREQ_RELATION_L)) < 0)
				pr_info(PR_NAME "failed to target freq=%d for cpu%d.\n", policy.max, n_online);
		} else
			pr_info(PR_NAME "failed to get policy for cpu%d, ret=%d.\n", n_online, ret);

		n_online++;
	}

	while(n_online > nr_run_stat) {
		n_online--;
		pr_debug(PR_NAME "unplugging cpu%d, want %d online\n", n_online, nr_run_stat);
#ifdef CONFIG_SIMPLE_PLUG_STATS
		times_core_down[n_online]++;
#endif
		cpu_down(n_online);
	}
}
     
static void unplug_other_cores(void)
{
	int cpu;

	for (cpu = 1; cpu < NUM_CORES; cpu++) {
		if (cpu_online(cpu)) {
			pr_debug(PR_NAME "unplugging cpu%d\n", cpu);
			cpu_down(cpu);
		}
	}
	n_online = 1;
}

static void __cpuinit simple_plug_work_fn(struct work_struct *work)
{
#ifdef CONFIG_SIMPLE_PLUG_STATS
	if (reset_stats) {
		reset_stats = 0;
		memset(time_cores_running, 0, sizeof(time_cores_running));
		memset(times_core_up, 0, sizeof(times_core_up));
		memset(times_core_down, 0, sizeof(times_core_down));
	}
#endif
	
	if (simple_plug_active == 1) {
		int cores = desired_number_of_cores();
		cpus_up_down(cores);
	}

	schedule_delayed_work_on(0, &simple_plug_work,
		msecs_to_jiffies(sampling_ms));
}


static void __cpuinit first_work_fn(struct work_struct *work)
{
	/* Get the CPUs into a known good state so we don't leave
	 * random cores online after booting.
	 */
	if (! cpu_online(0)) {
		pr_debug(PR_NAME "bringing cpu0 online\n");
		cpu_up(0);
	}

	unplug_other_cores();

	simple_plug_work_fn(work);
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void simple_plug_early_suspend(struct early_suspend *handler)
{
	cancel_delayed_work_sync(&simple_plug_work);
	unplug_other_cores();
}

static void __cpuinit simple_plug_late_resume(struct early_suspend *handler)
{
	int i;
	unsigned almost_2 = (2 << FSHIFT) - 1;

	/* setup a state which will let a second cpu come online very
	 * easily if there is much startup load (which there usually is)
	 */

	for (i = 0; i < HISTORY_SIZE; i++) {
		nr_run_history[i] = almost_2;
	}
	nr_avg = almost_2 * HISTORY_SIZE;

	
	/* Ask it to run very soon to allow that ramp-up to happen */

	schedule_delayed_work_on(0, &simple_plug_work,
		msecs_to_jiffies(1));
}

static struct early_suspend simple_plug_early_suspend_struct_driver = {
	.level = EARLY_SUSPEND_LEVEL_DISABLE_FB + 10,
	.suspend = simple_plug_early_suspend,
	.resume = simple_plug_late_resume,
};
#endif	/* CONFIG_HAS_EARLYSUSPEND */

int __init simple_plug_init(void)
{
	int cpu;

	pr_info(PR_NAME "version %d.%d by crpalmer\n",
		 SIMPLE_PLUG_MAJOR_VERSION,
		 SIMPLE_PLUG_MINOR_VERSION);

	for (cpu = 0; cpu < NUM_CORES; cpu++)
		if (cpu_online(cpu))
			pr_info(PR_NAME "cpu%d is online\n", cpu);

	INIT_DELAYED_WORK(&first_work, first_work_fn);
	INIT_DELAYED_WORK(&simple_plug_work, simple_plug_work_fn);

#ifdef CONFIG_HAS_EARLYSUSPEND
	register_early_suspend(&simple_plug_early_suspend_struct_driver);
#endif

	/* hack-o-rama: there is a race with the PM module starting
	 * and this starting.  If you try to turn cores off before the PM
	 * is initialized, it will crash.  The race window seems to be in the
	 * order 10s of ms, so 5 seconds gives tons of time for it to
	 * resolve itself.
	 */
	
	schedule_delayed_work_on(0, &first_work, msecs_to_jiffies(5000)); // sampling_ms));

	return 0;
}

MODULE_AUTHOR("Christopher R. Palmer <crpalmer@gmail.com>");
MODULE_DESCRIPTION("'simple_plug' - An simple cpu hotplug driver for "
	"Low Latency Frequency Transition capable processors");
MODULE_LICENSE("GPL");

late_initcall(simple_plug_init);
