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
#define SIMPLE_PLUG_MINOR_VERSION	1

#define STARTUP_DELAY_MS		5000
#define DEF_SAMPLING_MS			10
#define DEF_VERIFY_MS			120000	/* 3 mins on average to fix */
#define HISTORY_SIZE			10
#define NUM_CORES			4

static struct delayed_work simple_plug_work;

static unsigned int simple_plug_active = 1;
static unsigned int min_cores = 1;
static unsigned int max_cores = NUM_CORES;
static unsigned int sampling_ms = DEF_SAMPLING_MS;
static unsigned int verify_ms = DEF_VERIFY_MS;

#ifdef CONFIG_SIMPLE_PLUG_STATS
static unsigned int reset_stats;
static unsigned int time_cores_running[NUM_CORES];
static unsigned int times_core_up[NUM_CORES];
static unsigned int times_core_down[NUM_CORES];

#define STATS(expr) (expr)
#else
#define STATS(expr) /* nop */
#endif

static unsigned int nr_avg;
static unsigned int nr_run_history[HISTORY_SIZE];
static unsigned int nr_last_i;

/* set n_until_verify to 1 to cause it to do a verification on the very
 * first invocation, this replaces the need for special startup checking
 * of cores.
 */
static unsigned int n_until_verify;
static bool verify_needed = false;
static unsigned int n_online;

module_param(simple_plug_active, uint, 0644);
module_param(min_cores, uint, 0644);
module_param(max_cores, uint, 0644);
module_param(sampling_ms, uint, 0644);
module_param(verify_ms, uint, 0644);

#ifdef CONFIG_SIMPLE_PLUG_STATS
module_param(reset_stats, uint, 0644);
module_param_array(time_cores_running, uint, NULL, 0444);
module_param_array(times_core_up, uint, NULL, 0444);
module_param_array(times_core_down, uint, NULL, 0444);
#endif

module_param(nr_avg, uint, 0444);
module_param_array(nr_run_history, uint, NULL, 0444);
module_param(n_online, uint, 0444);

#define FSHIFT_ONE	(1<<FSHIFT)

static unsigned __cpuinit desired_number_of_cores(void)
{
	int target_cores, up_cores, down_cores;
	int avg;

	nr_avg -= nr_run_history[nr_last_i];
	nr_avg += nr_run_history[nr_last_i] = avg_nr_running();
	nr_last_i = (nr_last_i + 1) % HISTORY_SIZE;

	/* Compute number of cores of average active work.
	 * To add core N we must have a load of at least N+0.5
	 * To remove core N we must have load of N-0.5 or less
	 */

	avg = nr_avg / HISTORY_SIZE;

	if (avg > FSHIFT_ONE/2) 
		up_cores = (avg - FSHIFT_ONE/2) >> FSHIFT;
	else
		up_cores = 1;

	down_cores = (avg + FSHIFT_ONE/2) >> FSHIFT;

	if (up_cores > n_online)
		target_cores = up_cores;
	else if (down_cores < n_online)
		target_cores = down_cores;
	else target_cores = n_online;

	if (target_cores > max_cores)
		return max_cores;
	else if (target_cores < min_cores)
		return min_cores;
	else
		return target_cores;
}

static void
set_max_frequency(int cpu)
{
	struct cpufreq_policy policy;
	int ret;

	if ((ret = cpufreq_get_policy(&policy, n_online)) == 0) {
		if ((ret = cpufreq_driver_target(&policy, policy.max, CPUFREQ_RELATION_L)) < 0)
			pr_info(PR_NAME "failed to target freq=%d for cpu%d.\n", policy.max, n_online);
	} else
		pr_info(PR_NAME "failed to get policy for cpu%d, ret=%d.\n", n_online, ret);
}

static bool cpu_state_is_not_valid(int cpu)
{
	return (cpu < n_online && ! cpu_online(cpu)) ||
	       (cpu >= n_online && cpu_online(cpu));
}

static void __cpuinit cpus_up_down(int desired_n_online)
{
	int cpu;

	BUG_ON(desired_n_online < 1 || desired_n_online > NUM_CORES);

	STATS(time_cores_running[desired_n_online-1]++);

	if (n_online == desired_n_online)
		return;

	for (cpu = NUM_CORES-1; cpu >= 0; cpu--) {
		if (cpu_state_is_not_valid(cpu)) {
			pr_info(PR_NAME "cpu%d state was externally changed, scheduling verify", cpu);
			verify_needed = true;
			n_until_verify = (verify_ms+sampling_ms-1)/sampling_ms;
			BUG_ON(n_until_verify == 0);
			return;
		}

		if (cpu >= n_online && cpu < desired_n_online) {
			pr_debug(PR_NAME "starting cpu%d, want %d online\n", cpu, desired_n_online);
			STATS(times_core_up[cpu]++);
			cpu_up(cpu);
			set_max_frequency(cpu);
		} else if (cpu >= desired_n_online && cpu < n_online) {
			pr_debug(PR_NAME "unplugging cpu%d, want %d online\n", cpu, desired_n_online);
			STATS(times_core_down[cpu]++);
			cpu_down(cpu);
		}
	}

	n_online = desired_n_online;
}
     
static void __cpuinit verify_cores(unsigned desired_n_cores)
{
	int cpu;

	for (cpu = 0; cpu < NUM_CORES; cpu++) {
		if (cpu < desired_n_cores && ! cpu_online(cpu)) {
			pr_info(PR_NAME "re-plugging cpu%d that someone took offline.\n", cpu);
			cpu_up(cpu);
		} else if (cpu >= desired_n_cores && cpu_online(cpu)) {
			pr_info(PR_NAME "unplugging cpu%d that we want offline\n", cpu);
			cpu_down(cpu);
		}
	}

	n_online = desired_n_cores;
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
		if (verify_needed) {
			if (--n_until_verify == 0) {
				verify_cores(cores);
				verify_needed = false;
			}
		} else {
			cpus_up_down(cores);
		}
	}

	schedule_delayed_work_on(0, &simple_plug_work,
		msecs_to_jiffies(sampling_ms));
}

#ifdef CONFIG_HAS_EARLYSUSPEND

static void __cpuinit simple_plug_early_suspend(struct early_suspend *handler)
{
	cancel_delayed_work_sync(&simple_plug_work);
	verify_cores(1);
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

	
	/* Ask it to run very soon to allow that ramp-up to happen
	 * and let's ask it to immediately verify_cores
	 */

	verify_needed = true;
	n_until_verify = 1;
	schedule_delayed_work_on(0, &simple_plug_work,
		msecs_to_jiffies(1));
}

static struct early_suspend simple_plug_early_suspend_struct_driver = {
	.level = EARLY_SUSPEND_LEVEL_DISABLE_FB + 10,
	.suspend = simple_plug_early_suspend,
	.resume = simple_plug_late_resume,
};
#endif	/* CONFIG_HAS_EARLYSUSPEND */

static int __init simple_plug_init(void)
{
	pr_info(PR_NAME "version %d.%d by crpalmer\n",
		 SIMPLE_PLUG_MAJOR_VERSION,
		 SIMPLE_PLUG_MINOR_VERSION);

#ifdef CONFIG_HAS_EARLYSUSPEND
	register_early_suspend(&simple_plug_early_suspend_struct_driver);
#endif

	/* hack-o-rama: there is a race with the PM module starting
	 * and this starting.  If you try to turn cores off before the PM
	 * is initialized, it will crash.  The race window seems to be in the
	 * order 10s of ms, so 5 seconds gives tons of time for it to
	 * resolve itself.
	 */
	
	INIT_DELAYED_WORK(&simple_plug_work, simple_plug_work_fn);
	schedule_delayed_work_on(0, &simple_plug_work, msecs_to_jiffies(STARTUP_DELAY_MS));

	return 0;
}

MODULE_AUTHOR("Christopher R. Palmer <crpalmer@gmail.com>");
MODULE_DESCRIPTION("'simple_plug' - An simple cpu hotplug driver for "
	"Low Latency Frequency Transition capable processors");
MODULE_LICENSE("GPL");

late_initcall(simple_plug_init);
