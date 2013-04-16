/*
 *  drivers/cpufreq/cpufreq_simpledemand.c
 *
 *  Copyright (C) 2013 Christopher R. Palmer <crpalmer@gmail.com>
 *
 *  Based slights on ondemand by
 *
 *  Copyright (C)  2001 Russell King
 *            (C)  2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
 *                      Jun Nakajima <jun.nakajima@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define SD	"simpledemand"
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/mutex.h>
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/workqueue.h>
#include <linux/slab.h>

#define DEF_FREQUENCY_DOWN_DIFFERENTIAL		(10)
#define DEF_FREQUENCY_UP_THRESHOLD		(80)
#define DEF_SAMPLING_DOWN_FACTOR		(1)
#define MAX_SAMPLING_DOWN_FACTOR		(100000)
#define MICRO_FREQUENCY_DOWN_DIFFERENTIAL	(3)
#define MICRO_FREQUENCY_UP_THRESHOLD		(95)
#define MICRO_FREQUENCY_MIN_SAMPLE_RATE		(10000)
#define MIN_FREQUENCY_UP_THRESHOLD		(11)
#define MAX_FREQUENCY_UP_THRESHOLD		(100)
#define MIN_FREQUENCY_DOWN_DIFFERENTIAL		(1)
#define DBS_SYNC_FREQ				(702000)
#define DBS_OPTIMAL_FREQ			(1296000)

/*
 * The polling frequency of this governor depends on the capability of
 * the processor. Default polling frequency is 1000 times the transition
 * latency of the processor. The governor will work on any processor with
 * transition latency <= 10mS, using appropriate sampling
 * rate.
 * For CPUs with transition latency > 10mS (mostly drivers with CPUFREQ_ETERNAL)
 * this governor will not work.
 * All times here are in uS.
 */
#define MIN_SAMPLING_RATE_RATIO			(2)

static unsigned int min_sampling_rate;

#define LATENCY_MULTIPLIER			(1000)
#define MIN_LATENCY_MULTIPLIER			(100)
#define TRANSITION_LATENCY_LIMIT		(10 * 1000 * 1000)

/* Sampling types */
enum {DBS_NORMAL_SAMPLE, DBS_SUB_SAMPLE};

#define HISTORY_SIZE				10
	
typedef struct {
	cputime64_t	last;
	unsigned	avg;
	unsigned	sum;
	unsigned	values[HISTORY_SIZE];
	int		idx;
} history_t;

struct cpu_state {
	history_t	idle;
	history_t	wall;
	struct cpufreq_policy *policy;
	struct cpufreq_frequency_table *freq_table;
};

static DEFINE_PER_CPU(struct cpu_state, per_cpu_state);

static DEFINE_MUTEX(shared_mutex);
static struct {
	cpumask_t	active;
	int		n_active;
	struct delayed_work work;
} shared;

static struct tunables {
	unsigned int sampling_rate;
	unsigned int up_threshold;
	unsigned int up_threshold_multi_core;
	unsigned int down_differential;
	unsigned int down_differential_multi_core;
	unsigned int optimal_freq;
	unsigned int up_threshold_any_cpu_load;
	unsigned int sync_freq;
	unsigned int sampling_down_factor;
	int          powersave_bias;
	unsigned int io_is_busy;
} tunables = {
	.up_threshold_multi_core = DEF_FREQUENCY_UP_THRESHOLD,
	.up_threshold = DEF_FREQUENCY_UP_THRESHOLD,
	.sampling_down_factor = DEF_SAMPLING_DOWN_FACTOR,
	.down_differential = DEF_FREQUENCY_DOWN_DIFFERENTIAL,
	.down_differential_multi_core = MICRO_FREQUENCY_DOWN_DIFFERENTIAL,
	.up_threshold_any_cpu_load = DEF_FREQUENCY_UP_THRESHOLD,
	.powersave_bias = 0,
	.sync_freq = DBS_SYNC_FREQ,
	.optimal_freq = DBS_OPTIMAL_FREQ,
	.sampling_rate = (MICRO_FREQUENCY_MIN_SAMPLE_RATE * 5)
};

static void history_init(history_t *h, unsigned value, cputime64_t cur)
{
	int i;

	h->last = cur;
	h->idx = 0;
	h->sum = value * HISTORY_SIZE;
	h->avg = value;
	for (i = 0; i < HISTORY_SIZE; i++)
		h->values[i] = value;
}

static void history_update(history_t *h, cputime64_t abs_value)
{
	unsigned value = abs_value - h->last;

	h->last = abs_value;
	h->sum -= h->values[h->idx];
	h->values[h->idx] = value;
	h->sum += h->values[h->idx];
	h->idx = (h->idx + 1) % HISTORY_SIZE;

	h->avg = (h->sum + HISTORY_SIZE/2) / HISTORY_SIZE;
}

static inline u64 get_cpu_idle_time_jiffy(unsigned int cpu, u64 *wall)
{
	u64 idle_time;
	u64 cur_wall_time;
	u64 busy_time;

	cur_wall_time = jiffies64_to_cputime64(get_jiffies_64());

	busy_time  = kcpustat_cpu(cpu).cpustat[CPUTIME_USER];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SYSTEM];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_IRQ];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SOFTIRQ];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_STEAL];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_NICE];

	idle_time = cur_wall_time - busy_time;
	if (wall)
		*wall = jiffies_to_usecs(cur_wall_time);

	return jiffies_to_usecs(idle_time);
}

static inline cputime64_t get_cpu_idle_time(unsigned int cpu, cputime64_t *wall)
{
	u64 idle_time = get_cpu_idle_time_us(cpu, NULL);

	if (idle_time == -1ULL)
		return get_cpu_idle_time_jiffy(cpu, wall);
	else
		idle_time += get_cpu_iowait_time_us(cpu, wall);

	return idle_time;
}

static void init_cpu(int cpu, struct cpufreq_policy *policy)
{
	struct cpu_state *cpu_state = &per_cpu(per_cpu_state, cpu);
	cputime64_t wall;

	cpu_state->policy = policy;
	cpu_state->freq_table = cpufreq_frequency_get_table(cpu);

	history_init(&cpu_state->idle, 0, get_cpu_idle_time(cpu, &wall));
	history_init(&cpu_state->wall, 0, wall);
}

static int delay(void)
{
	return usecs_to_jiffies(tunables.sampling_rate);
}

static inline void schedule_work_locked(void)
{
	schedule_delayed_work(&shared.work, delay());
}

static inline void cancel_work_locked(void)
{
	cancel_delayed_work_sync(&shared.work);
}

#if 0

/*
 * Find right freq to be set now with powersave_bias on.
 * Returns the freq_hi to be used right now and will set freq_hi_jiffies,
 * freq_lo, and freq_lo_jiffies in percpu area for averaging freqs.
 */
static unsigned int powersave_bias_target(struct cpufreq_policy *policy,
					  unsigned int freq_next,
					  unsigned int relation)
{
	unsigned int freq_req, freq_avg;
	unsigned int freq_hi, freq_lo;
	unsigned int index = 0;
	unsigned int jiffies_total, jiffies_hi, jiffies_lo;
	int freq_reduc;
	struct cpu_state *cpu_state = &per_cpu(per_cpu_state,
						   policy->cpu);

	if (!cpu_state->freq_table) {
		cpu_state->freq_lo = 0;
		cpu_state->freq_lo_jiffies = 0;
		return freq_next;
	}

	cpufreq_frequency_table_target(policy, cpu_state->freq_table, freq_next,
			relation, &index);
	freq_req = cpu_state->freq_table[index].frequency;
	freq_reduc = freq_req * tunables.powersave_bias / 1000;
	freq_avg = freq_req - freq_reduc;

	/* Find freq bounds for freq_avg in freq_table */
	index = 0;
	cpufreq_frequency_table_target(policy, cpu_state->freq_table, freq_avg,
			CPUFREQ_RELATION_H, &index);
	freq_lo = cpu_state->freq_table[index].frequency;
	index = 0;
	cpufreq_frequency_table_target(policy, cpu_state->freq_table, freq_avg,
			CPUFREQ_RELATION_L, &index);
	freq_hi = cpu_state->freq_table[index].frequency;

	/* Find out how long we have to be in hi and lo freqs */
	if (freq_hi == freq_lo) {
		cpu_state->freq_lo = 0;
		cpu_state->freq_lo_jiffies = 0;
		return freq_lo;
	}
	jiffies_total = usecs_to_jiffies(tunables.sampling_rate);
	jiffies_hi = (freq_avg - freq_lo) * jiffies_total;
	jiffies_hi += ((freq_hi - freq_lo) / 2);
	jiffies_hi /= (freq_hi - freq_lo);
	jiffies_lo = jiffies_total - jiffies_hi;
	cpu_state->freq_lo = freq_lo;
	cpu_state->freq_lo_jiffies = jiffies_lo;
	cpu_state->freq_hi_jiffies = jiffies_hi;
	return freq_hi;
}

static void freq_increase(struct cpufreq_policy *p, unsigned int freq)
{
	if (tunables.powersave_bias)
		freq = powersave_bias_target(p, freq, CPUFREQ_RELATION_H);
	else if (p->cur == p->max)
		return;

	__cpufreq_driver_target(p, freq, tunables.powersave_bias ?
			CPUFREQ_RELATION_L : CPUFREQ_RELATION_H);
}

static void check_cpu(struct cpu_state *this_cpu_state)
{
	/* Extrapolated load of this CPU */
	unsigned int load_at_max_freq = 0;
	unsigned int max_load_freq;
	/* Current load across this CPU */
	unsigned int cur_load = 0;
	unsigned int max_load_other_cpu = 0;
	struct cpufreq_policy *policy;
	unsigned int j;

	this_cpu_state->freq_lo = 0;
	policy = this_cpu_state->cur_policy;

	/*
	 * Every sampling_rate, we check, if current idle time is less
	 * than 20% (default), then we try to increase frequency
	 * Every sampling_rate, we look for a the lowest
	 * frequency which can sustain the load while keeping idle time over
	 * 30%. If such a frequency exist, we try to decrease to this frequency.
	 *
	 * Any frequency increase takes it to the maximum frequency.
	 * Frequency reduction happens at minimum steps of
	 * 5% (default) of current frequency
	 */

	/* Get Absolute Load - in terms of freq */
	max_load_freq = 0;

	for_each_cpu(j, policy->cpus) {
		struct cpu_state *j_cpu_state;
		cputime64_t cur_wall_time, cur_idle_time, cur_iowait_time;
		unsigned int idle_time, wall_time, iowait_time;
		unsigned int load_freq;
		int freq_avg;

		j_cpu_state = &per_cpu(per_cpu_state, j);

		cur_idle_time = get_cpu_idle_time(j, &cur_wall_time);
		cur_iowait_time = get_cpu_iowait_time(j, &cur_wall_time);

		wall_time = (unsigned int)
			(cur_wall_time - j_cpu_state->prev_cpu_wall);
		j_cpu_state->prev_cpu_wall = cur_wall_time;

		idle_time = (unsigned int)
			(cur_idle_time - j_cpu_state->prev_cpu_idle);
		j_cpu_state->prev_cpu_idle = cur_idle_time;

		iowait_time = (unsigned int)
			(cur_iowait_time - j_cpu_state->prev_cpu_iowait);
		j_cpu_state->prev_cpu_iowait = cur_iowait_time;

		if (tunables.ignore_nice) {
			u64 cur_nice;
			unsigned long cur_nice_jiffies;

			cur_nice = kcpustat_cpu(j).cpustat[CPUTIME_NICE] -
					 j_cpu_state->prev_cpu_nice;
			/*
			 * Assumption: nice time between sampling periods will
			 * be less than 2^32 jiffies for 32 bit sys
			 */
			cur_nice_jiffies = (unsigned long)
					cputime64_to_jiffies64(cur_nice);

			j_cpu_state->prev_cpu_nice = kcpustat_cpu(j).cpustat[CPUTIME_NICE];
			idle_time += jiffies_to_usecs(cur_nice_jiffies);
		}

		/*
		 * For the purpose of ondemand, waiting for disk IO is an
		 * indication that you're performance critical, and not that
		 * the system is actually idle. So subtract the iowait time
		 * from the cpu idle time.
		 */

		if (tunables.io_is_busy && idle_time >= iowait_time)
			idle_time -= iowait_time;

		if (unlikely(!wall_time || wall_time < idle_time))
			continue;

		cur_load = 100 * (wall_time - idle_time) / wall_time;
		j_cpu_state->max_load  = max(cur_load, j_cpu_state->prev_load);
		j_cpu_state->prev_load = cur_load;
		freq_avg = __cpufreq_driver_getavg(policy, j);
		if (freq_avg <= 0)
			freq_avg = policy->cur;

		load_freq = cur_load * freq_avg;
		if (load_freq > max_load_freq)
			max_load_freq = load_freq;
	}

	for_each_online_cpu(j) {
		struct cpu_state *j_cpu_state;
		j_cpu_state = &per_cpu(per_cpu_state, j);

		if (j == policy->cpu)
			continue;

		if (max_load_other_cpu < j_cpu_state->max_load)
			max_load_other_cpu = j_cpu_state->max_load;
		/*
		 * The other cpu could be running at higher frequency
		 * but may not have completed it's sampling_down_factor.
		 * For that case consider other cpu is loaded so that
		 * frequency imbalance does not occur.
		 */

		if ((j_cpu_state->cur_policy != NULL)
			&& (j_cpu_state->cur_policy->cur ==
					j_cpu_state->cur_policy->max)) {

			if (policy->cur >= tunables.optimal_freq)
				max_load_other_cpu =
				tunables.up_threshold_any_cpu_load;
		}
	}

	/* calculate the scaled load across CPU */
	load_at_max_freq = (cur_load * policy->cur)/policy->cpuinfo.max_freq;

	cpufreq_notify_utilization(policy, load_at_max_freq);
	/* Check for frequency increase */
	if (max_load_freq > tunables.up_threshold * policy->cur) {
		/* If switching to max speed, apply sampling_down_factor */
		if (policy->cur < policy->max)
			this_cpu_state->rate_mult =
				tunables.sampling_down_factor;
		freq_increase(policy, policy->max);
		return;
	}

	if (num_online_cpus() > 1) {

		if (max_load_other_cpu >
				tunables.up_threshold_any_cpu_load) {
			if (policy->cur < tunables.sync_freq)
				freq_increase(policy,
						tunables.sync_freq);
			return;
		}

		if (max_load_freq > tunables.up_threshold_multi_core *
								policy->cur) {
			if (policy->cur < tunables.optimal_freq)
				freq_increase(policy,
						tunables.optimal_freq);
			return;
		}
	}

	/* Check for frequency decrease */
	/* if we cannot reduce the frequency anymore, break out early */
	if (policy->cur == policy->min)
		return;

	/*
	 * The optimal frequency is the frequency that is the lowest that
	 * can support the current CPU usage without triggering the up
	 * policy. To be safe, we focus 10 points under the threshold.
	 */
	if (max_load_freq <
	    (tunables.up_threshold - tunables.down_differential) *
	     policy->cur) {
		unsigned int freq_next;
		freq_next = max_load_freq /
				(tunables.up_threshold -
				 tunables.down_differential);

		/* No longer fully busy, reset rate_mult */
		this_cpu_state->rate_mult = 1;

		if (freq_next < policy->min)
			freq_next = policy->min;

		if (num_online_cpus() > 1) {
			if (max_load_other_cpu >
			(tunables.up_threshold_multi_core -
			tunables.down_differential) &&
			freq_next < tunables.sync_freq)
				freq_next = tunables.sync_freq;

			if (max_load_freq >
				 (tunables.up_threshold_multi_core -
				  tunables.down_differential_multi_core) *
				  policy->cur)
				freq_next = tunables.optimal_freq;

		}
		if (!tunables.powersave_bias) {
			__cpufreq_driver_target(policy, freq_next,
					CPUFREQ_RELATION_L);
		} else {
			int freq = powersave_bias_target(policy, freq_next,
					CPUFREQ_RELATION_L);
			__cpufreq_driver_target(policy, freq,
				CPUFREQ_RELATION_L);
		}
	}
}

static void background_work(struct work_struct *work)
{
	struct cpu_state *cpu_state =
		container_of(work, struct cpu_state, work.work);
	unsigned int cpu = cpu_state->cpu;
	int sample_type = cpu_state->sample_type;

	int delay;

	mutex_lock(&cpu_state->timer_mutex);

	/* Common NORMAL_SAMPLE setup */
	cpu_state->sample_type = DBS_NORMAL_SAMPLE;
	if (!tunables.powersave_bias ||
	    sample_type == DBS_NORMAL_SAMPLE) {
		check_cpu(cpu_state);
	} else {
		__cpufreq_driver_target(cpu_state->cur_policy,
			cpu_state->freq_lo, CPUFREQ_RELATION_H);
	}
	schedule_work_locked(false);
	mutex_unlock(&cpu_state->timer_mutex);
}

#endif

static void update_cpu_state(int cpu)
{
	struct cpu_state *cpu_state = &per_cpu(per_cpu_state, cpu);
	cputime64_t wall;

	history_update(&cpu_state->idle, get_cpu_idle_time(cpu, &wall));
	history_update(&cpu_state->wall, wall);
}

static void update_cpu_freq(int cpu)
{
}

static void consider_unplugging_cpu(void)
{
}

static void consider_plugging_cpu(void)
{
}

static void poll_and_update_freqs_locked(void)
{
	int cpu;

	for_each_cpu(cpu, &shared.active) {
		if (lock_policy_rwsem_write(cpu) < 0) {
			pr_info(SD " failed to lock policy for cpu%d\n", cpu);
			continue;
		}
		update_cpu_state(cpu);
		update_cpu_freq(cpu);
		unlock_policy_rwsem_write(cpu);
	}

	consider_unplugging_cpu();
	consider_plugging_cpu();
}

static void poll_and_update_freqs(struct work_struct *work)
{
	mutex_lock(&shared_mutex);
	poll_and_update_freqs_locked();
	mutex_unlock(&shared_mutex);
}

#if 0
	struct cpufreq_policy *policy;
	struct cpu_state *this_cpu_state;
	unsigned int cpu;

	dbs_work = container_of(work, struct dbs_work_struct, work);
	cpu = dbs_work->cpu;

	get_online_cpus();


	this_cpu_state = &per_cpu(per_cpu_state, cpu);
	policy = this_cpu_state->cur_policy;
	if (!policy) {
		/* CPU not using ondemand governor */
		goto bail_incorrect_governor;
	}

	if (policy->cur < policy->max) {
		/*
		 * Arch specific cpufreq driver may fail.
		 * Don't update governor frequency upon failure.
		 */
		if (__cpufreq_driver_target(policy, policy->max,
					CPUFREQ_RELATION_L) >= 0)
			policy->cur = policy->max;

		this_cpu_state->prev_cpu_idle = get_cpu_idle_time(cpu,
				&this_cpu_state->prev_cpu_wall);
	}

bail_incorrect_governor:

bail_acq_sema_failed:
	put_online_cpus();
	return;
}
#endif

static void init_sampling_rate(struct cpufreq_policy *policy)
{
	unsigned int latency;

	/* policy latency is in nS. Convert it to uS first */
	latency = policy->cpuinfo.transition_latency / 1000;
	if (latency == 0)
		latency = 1;

	/* Bring kernel and HW constraints together */
	min_sampling_rate = max(min_sampling_rate, MIN_LATENCY_MULTIPLIER * latency);
	tunables.sampling_rate = max(min_sampling_rate, latency * LATENCY_MULTIPLIER);
}

static int simpledemand_governor(struct cpufreq_policy *policy,
				   unsigned int event)
{
	unsigned int cpu = policy->cpu;

	switch (event) {
	case CPUFREQ_GOV_START:
		if ((!cpu_online(cpu)) || (!policy->cur))
			return -EINVAL;

		init_cpu(cpu, policy);

		mutex_lock(&shared_mutex);

		cpumask_set_cpu(cpu, &shared.active);
		if (shared.n_active++ == 0) {
			init_sampling_rate(policy);
			pr_info(SD " cpu%d starting, scheduling work every %dms\n", cpu, delay());
			INIT_DELAYED_WORK_DEFERRABLE(&shared.work, poll_and_update_freqs);
			schedule_work_locked();
		} else {
			pr_info(SD " cpu%d added to the mix.\n", cpu);
		}

		mutex_lock(&shared_mutex);

		break;

	case CPUFREQ_GOV_STOP:
		pr_info(SD " cpu%d stopping\n", cpu);

		mutex_lock(&shared_mutex);

		cpumask_set_cpu(cpu, &shared.active);
		if (--shared.n_active == 0) {
			cancel_work_locked();
		}

		mutex_lock(&shared_mutex);
		break;

	case CPUFREQ_GOV_LIMITS:
		// This will fix itself on the next work tick
		break;
	}
	return 0;
}

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_SIMPLEDEMAND
static
#endif
struct cpufreq_governor cpufreq_gov_simpledemand = {
       .name                   = SD,
       .governor               = simpledemand_governor,
       .max_transition_latency = TRANSITION_LATENCY_LIMIT,
       .owner                  = THIS_MODULE,
};

static int __init simpledemand_init(void)
{
	u64 idle_time;
	int cpu;

	cpu = get_cpu();
	idle_time = get_cpu_idle_time_us(cpu, NULL);
	put_cpu();

	if (idle_time != -1ULL) {
		/* Idle micro accounting is supported. Use finer thresholds */
		tunables.up_threshold = MICRO_FREQUENCY_UP_THRESHOLD;
		tunables.down_differential =
					MICRO_FREQUENCY_DOWN_DIFFERENTIAL;
		/*
		 * In nohz/micro accounting case we set the minimum frequency
		 * not depending on HZ, but fixed (very low). The deferred
		 * timer might skip some samples if idle/sleeping as needed.
		*/
		min_sampling_rate = MICRO_FREQUENCY_MIN_SAMPLE_RATE;
	} else {
		/* For correct statistics, we need 10 ticks for each measure */
		min_sampling_rate =
			MIN_SAMPLING_RATE_RATIO * jiffies_to_usecs(10);
	}

	shared.active = *to_cpumask(0);
	return cpufreq_register_governor(&cpufreq_gov_simpledemand);
}

static void __exit simpledemand_gov_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_simpledemand);
}


MODULE_AUTHOR("Christopher R. Palmer <crpalmer@gmail.com>");
MODULE_DESCRIPTION("'cpufreq_simpledemand' - A dynamic cpufreq governor for "
	"multi-core low latency frequency transition capable processors");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_SIMPLEDEMAND
fs_initcall(simpledemand_init);
#else
module_init(simpledemand_init);
#endif
module_exit(simpledemand_gov_exit);
