/*
 * drivers/cpufreq/cpufreq_flash.c
 *
 * Copyright (C) 2010 Google, Inc.
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
 * Author: Mike Chan (mike@android.com)
 *
 */

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/tick.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/sched_loadab.h>
#include <linux/kernel_stat.h>
#include <linux/display_state.h>
#include <asm/cputime.h>

#define CREATE_TRACE_POINTS
#include <trace/events/cpufreq_flash.h>

struct cpufreq_flash_policyinfo {
	struct timer_list policy_timer;
	struct timer_list policy_sched_loadack_timer;
	spinlock_t load_lock; /* protects load tracking stat */
	u64 last_evaluated_jiffy;
	struct cpufreq_policy *policy;
	struct cpufreq_frequency_table *freq_table;
	spinlock_t target_freq_lock; /*protects target freq */
	unsigned int target_freq;
	unsigned int floor_freq;
	unsigned int min_freq;
	u64 floor_validate_time;
	u64 hispeed_validate_time;
	u64 max_freq_hyst_start_time;
	struct rw_semaphore enable_sem;
	bool reject_notification;
	int governor_enabled;
	struct cpufreq_flash_tunables *cached_tunables;
	struct sched_load;
};

/* Protected by per-policy load_lock */
struct cpufreq_flash_cpuinfo {
	u64 time_in_idle;
	u64 time_in_idle_timestamp;
	u64 cputime_speedadj;
	u64 cputime_speedadj_timestamp;
	unsigned int loadadjfreq;
};

static DEFINE_PER_CPU(struct cpufreq_flash_policyinfo *, polinfo);
static DEFINE_PER_CPU(struct cpufreq_flash_cpuinfo, cpuinfo);

/* realtime thread handles frequency scaling */
static struct task_struct *speedchange_task;
static cpumask_t speedchange_cpumask;
static spinlock_t speedchange_cpumask_lock;
static struct mutex gov_lock;

static int set_window_count;
static int migration_register_count;
static struct mutex sched_lock;

/* Target load.  Lower values result in higher CPU speeds. */
#define DEFAULT_TARGET_LOAD 90
static unsigned int default_target_loads[] = {DEFAULT_TARGET_LOAD};

#define DEFAULT_TIMER_RATE (20 * USEC_PER_MSEC)
#define SCREEN_OFF_TIMER_RATE ((unsigned long)(50 * USEC_PER_MSEC))
#define DEFAULT_ABOVE_HISPEED_DELAY DEFAULT_TIMER_RATE
static unsigned int default_above_hispeed_delay[] = {
	DEFAULT_ABOVE_HISPEED_DELAY };

struct cpufreq_flash_tunables {
	int usage_count;
	/* Hi speed to bump to from lo speed when load burst (default max) */
	unsigned int hispeed_freq;
	/* Go to hi speed when CPU load at or above this value. */
#define DEFAULT_GO_HISPEED_LOAD 99
	unsigned long go_hispeed_load;
	/* Target load. Lower values result in higher CPU speeds. */
	spinlock_t target_loads_lock;
	unsigned int *target_loads;
	int ntarget_loads;
	/*
	 * The minimum amount of time to spend at a frequency before we can ramp
	 * down.
	 */
#define DEFAULT_MIN_SAMPLE_TIME (80 * USEC_PER_MSEC)
	unsigned long min_sample_time;
	/*
	 * The sample rate of the timer used to increase frequency
	 */
	unsigned long timer_rate;
	unsigned long prev_timer_rate;
	/*
	 * Wait this long before raising speed above hispeed, by default a
	 * single timer interval.
	 */
	spinlock_t above_hispeed_delay_lock;
	unsigned int *above_hispeed_delay;
	int nabove_hispeed_delay;
	/*
	 * Max additional time to wait in idle, beyond timer_rate, at speeds
	 * above minimum before wakeup to reduce speed, or -1 if unnecessary.
	 */
#define DEFAULT_TIMER_sched_loadACK (4 * DEFAULT_TIMER_RATE)
	int timer_sched_loadack_val;
	bool io_is_busy;

	/* scheduler input related flags */
	bool use_sched_load;
	bool use_migration_notif;

	/*
	 * Whether to align timer windows across all CPUs. When
	 * use_sched_load is true, this flag is ignored and windows
	 * will always be aligned.
	 */
	bool align_windows;

	/*
	 * Stay at max freq for at least max_freq_hysteresis before dropping
	 * frequency.
	 */
	unsigned int max_freq_hysteresis;

	/* Use agressive frequency step calculation, above a given load threshold */
	bool fastlane;
	unsigned int fastlane_threshold;

	/* Ignore hispeed_freq and above_hispeed_delay for notification */
	bool ignore_hispeed_on_notif;

	/* Ignore min_sample_time for notification */
	bool fast_ramp_down;

	/* Improves frequency selection for more energy */
	bool powersave_bias;

	/* Maximum frequency while the screen is off */
#define DEFAULT_SCREEN_OFF_MAX 1248000
	unsigned long screen_off_max;
};

/* For cases where we have single governor instance for system */
static struct cpufreq_flash_tunables *common_tunables;
static struct cpufreq_flash_tunables *cached_common_tunables;

static struct attribute_group *get_sysfs_attr(void);

/* Round to starting jiffy of next evaluation window */
static u64 round_to_nw_start(u64 jif,
			     struct cpufreq_flash_tunables *tunables)
{
	unsigned long step = usecs_to_jiffies(tunables->timer_rate);
	u64 ret;

	if (tunables->use_sched_load || tunables->align_windows) {
		do_div(jif, step);
		ret = (jif + 1) * step;
	} else {
		ret = jiffies + usecs_to_jiffies(tunables->timer_rate);
	}

	return ret;
}

static inline int set_window_helper(
			struct cpufreq_flash_tunables *tunables)
{
	return sched_set_window(round_to_nw_start(get_jiffies_64(), tunables),
			 usecs_to_jiffies(tunables->timer_rate));
}

static void cpufreq_flash_timer_resched(unsigned long cpu,
					      bool sched_loadack_only)
{
	struct cpufreq_flash_policyinfo *ppol = per_cpu(polinfo, cpu);
	struct cpufreq_flash_cpuinfo *pcpu;
	struct cpufreq_flash_tunables *tunables =
		ppol->policy->governor_data;
	u64 expires;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&ppol->load_lock, flags);
	expires = round_to_nw_start(ppol->last_evaluated_jiffy, tunables);
	if (!sched_loadack_only) {
		for_each_cpu(i, ppol->policy->cpus) {
			pcpu = &per_cpu(cpuinfo, i);
			pcpu->time_in_idle = get_cpu_idle_time(i,
						&pcpu->time_in_idle_timestamp,
						tunables->io_is_busy);
			pcpu->cputime_speedadj = 0;
			pcpu->cputime_speedadj_timestamp =
						pcpu->time_in_idle_timestamp;
		}
		del_timer(&ppol->policy_timer);
		ppol->policy_timer.expires = expires;
		add_timer(&ppol->policy_timer);
	}

	if (tunables->timer_sched_loadack_val >= 0 &&
	    ppol->target_freq > ppol->policy->min) {
		expires += usecs_to_jiffies(tunables->timer_sched_loadack_val);
		del_timer(&ppol->policy_sched_loadack_timer);
		ppol->policy_sched_loadack_timer.expires = expires;
		add_timer(&ppol->policy_sched_loadack_timer);
	}

	spin_unlock_irqrestore(&ppol->load_lock, flags);
}

/* The caller shall take enable_sem write semaphore to avoid any timer race.
 * The policy_timer and policy_sched_loadack_timer must be deactivated when calling
 * this function.
 */
static void cpufreq_flash_timer_start(
	struct cpufreq_flash_tunables *tunables, int cpu)
{
	struct cpufreq_flash_policyinfo *ppol = per_cpu(polinfo, cpu);
	struct cpufreq_flash_cpuinfo *pcpu;
	u64 expires = round_to_nw_start(ppol->last_evaluated_jiffy, tunables);
	unsigned long flags;
	int i;

	spin_lock_irqsave(&ppol->load_lock, flags);
	ppol->policy_timer.expires = expires;
	add_timer(&ppol->policy_timer);
	if (tunables->timer_sched_loadack_val >= 0 &&
	    ppol->target_freq > ppol->policy->min) {
		expires += usecs_to_jiffies(tunables->timer_sched_loadack_val);
		ppol->policy_sched_loadack_timer.expires = expires;
		add_timer(&ppol->policy_sched_loadack_timer);
	}

	for_each_cpu(i, ppol->policy->cpus) {
		pcpu = &per_cpu(cpuinfo, i);
		pcpu->time_in_idle =
			get_cpu_idle_time(i, &pcpu->time_in_idle_timestamp,
					  tunables->io_is_busy);
		pcpu->cputime_speedadj = 0;
		pcpu->cputime_speedadj_timestamp = pcpu->time_in_idle_timestamp;
	}
	spin_unlock_irqrestore(&ppol->load_lock, flags);
}

static unsigned int freq_to_above_hispeed_delay(
	struct cpufreq_flash_tunables *tunables,
	unsigned int freq)
{
	int i;
	unsigned int ret;
	unsigned long flags;

	spin_lock_irqsave(&tunables->above_hispeed_delay_lock, flags);

	for (i = 0; i < tunables->nabove_hispeed_delay - 1 &&
			freq >= tunables->above_hispeed_delay[i+1]; i += 2)
		;

	ret = tunables->above_hispeed_delay[i];
	spin_unlock_irqrestore(&tunables->above_hispeed_delay_lock, flags);
	return ret;
}

static unsigned int freq_to_targetload(
	struct cpufreq_flash_tunables *tunables, unsigned int freq)
{
	int i;
	unsigned int ret;
	unsigned long flags;

	spin_lock_irqsave(&tunables->target_loads_lock, flags);

	for (i = 0; i < tunables->ntarget_loads - 1 &&
		    freq >= tunables->target_loads[i+1]; i += 2)
		;

	ret = tunables->target_loads[i];
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);
	return ret;
}

/*
 * If increasing frequencies never map to a lower target load then
 * choose_freq() will find the minimum frequency that does not exceed its
 * target load given the current load.
 */
static unsigned int choose_freq(struct cpufreq_flash_policyinfo *pcpu,
		unsigned int loadadjfreq)
{
	unsigned int freq = pcpu->policy->cur;
	unsigned int prevfreq, freqmin, freqmax;
	unsigned int tl;
	int index;

	freqmin = 0;
	freqmax = UINT_MAX;

	do {
		prevfreq = freq;
		tl = freq_to_targetload(pcpu->policy->governor_data, freq);

		/*
		 * Find the lowest frequency where the computed load is less
		 * than or equal to the target load.
		 */

		if (cpufreq_frequency_table_target(
			    pcpu->policy, pcpu->freq_table, loadadjfreq / tl,
			    CPUFREQ_RELATION_L, &index))
			break;
		freq = pcpu->freq_table[index].frequency;

		if (freq > prevfreq) {
			/* The previous frequency is too low. */
			freqmin = prevfreq;

			if (freq >= freqmax) {
				/*
				 * Find the highest frequency that is less
				 * than freqmax.
				 */
				if (cpufreq_frequency_table_target(
					    pcpu->policy, pcpu->freq_table,
					    freqmax - 1, CPUFREQ_RELATION_H,
					    &index))
					break;
				freq = pcpu->freq_table[index].frequency;

				if (freq == freqmin) {
					/*
					 * The first frequency below freqmax
					 * has already been found to be too
					 * low.  freqmax is the lowest speed
					 * we found that is fast enough.
					 */
					freq = freqmax;
					break;
				}
			}
		} else if (freq < prevfreq) {
			/* The previous frequency is high enough. */
			freqmax = prevfreq;

			if (freq <= freqmin) {
				/*
				 * Find the lowest frequency that is higher
				 * than freqmin.
				 */
				if (cpufreq_frequency_table_target(
					    pcpu->policy, pcpu->freq_table,
					    freqmin + 1, CPUFREQ_RELATION_L,
					    &index))
					break;
				freq = pcpu->freq_table[index].frequency;

				/*
				 * If freqmax is the first frequency above
				 * freqmin then we have already found that
				 * this speed is fast enough.
				 */
				if (freq == freqmax)
					break;
			}
		}

		/* If same frequency chosen as previous then done. */
	} while (freq != prevfreq);

	return freq;
}


static unsigned int fastlane_freq(struct cpufreq_flash_policyinfo *pcpu,
		unsigned int cpu_load)
{
	unsigned int freq;

	freq = pcpu->policy->min + cpu_load * (pcpu->policy->max - pcpu->policy->min) / 100;

	return freq;
}

static u64 update_load(int cpu)
{
	struct cpufreq_flash_policyinfo *ppol = per_cpu(polinfo, cpu);
	struct cpufreq_flash_cpuinfo *pcpu = &per_cpu(cpuinfo, cpu);
	struct cpufreq_flash_tunables *tunables =
		ppol->policy->governor_data;
	u64 now;
	u64 now_idle;
	u64 delta_idle;
	u64 delta_time;
	u64 active_time;

	now_idle = get_cpu_idle_time(cpu, &now, tunables->io_is_busy);
	delta_idle = (now_idle - pcpu->time_in_idle);
	delta_time = (now - pcpu->time_in_idle_timestamp);

	if (delta_time <= delta_idle)
		active_time = 0;
	else
		active_time = delta_time - delta_idle;

	pcpu->cputime_speedadj += active_time * ppol->policy->cur;

	pcpu->time_in_idle = now_idle;
	pcpu->time_in_idle_timestamp = now;
	return now;
}

static void __cpufreq_flash_timer(unsigned long data, bool is_notif)
{
	u64 now;
	unsigned int delta_time;
	u64 cputime_speedadj;
	int cpu_load;
	struct cpufreq_flash_policyinfo *ppol = per_cpu(polinfo, data);
	struct cpufreq_flash_tunables *tunables =
		ppol->policy->governor_data;
	struct cpufreq_flash_cpuinfo *pcpu;
	unsigned int new_freq;
	unsigned int loadadjfreq = 0, tmploadadjfreq;
	unsigned int index;
	unsigned long flags;
	unsigned long max_cpu;
	int i, fcpu;
        struct sched_load *sched_load;
	struct cpufreq_govinfo govinfo;
	unsigned int this_hispeed_freq;
	bool display_on = is_display_on();

	if (!down_read_trylock(&ppol->enable_sem))
		return;
	if (!ppol->governor_enabled)
		goto exit;
	if (ppol->policy->min == ppol->policy->max)
		goto rearm;

	fcpu = cpumask_first(ppol->policy->related_cpus);
	now = ktime_to_us(ktime_get());
	spin_lock_irqsave(&ppol->load_lock, flags);
	ppol->last_evaluated_jiffy = get_jiffies_64();

	if (display_on
		&& tunables->timer_rate != tunables->prev_timer_rate)
		tunables->timer_rate = tunables->prev_timer_rate;
	else if (!display_on
		&& tunables->timer_rate != SCREEN_OFF_TIMER_RATE) {
		tunables->prev_timer_rate = tunables->timer_rate;
		tunables->timer_rate
			= max(tunables->timer_rate,
				SCREEN_OFF_TIMER_RATE);
	}

	if (tunables->use_sched_load)
		sched_get_cpus_busy(ppol->sched_load, ppol->policy->related_cpus);
	max_cpu = cpumask_first(ppol->policy->cpus);
	for_each_cpu(i, ppol->policy->cpus) {
		pcpu = &per_cpu(cpuinfo, i);
                sched_load = &ppol->sched_load[i - fcpu];
		if (tunables->use_sched_load) {
			cputime_speedadj = (u64)sched_load->prev_load *
					   ppol->policy->cpuinfo.max_freq;
			do_div(cputime_speedadj, tunables->timer_rate);
		} else {
			now = update_load(i);
			delta_time = (unsigned int)
				(now - pcpu->cputime_speedadj_timestamp);
			if (WARN_ON_ONCE(!delta_time))
				continue;
			cputime_speedadj = pcpu->cputime_speedadj;
			do_div(cputime_speedadj, delta_time);
		}
		tmploadadjfreq = (unsigned int)cputime_speedadj * 100;
		pcpu->loadadjfreq = tmploadadjfreq;
		trace_cpufreq_flash_cpuload(i, tmploadadjfreq /
						  ppol->target_freq);

		if (tmploadadjfreq > loadadjfreq) {
			loadadjfreq = tmploadadjfreq;
			max_cpu = i;
		}
	}
	spin_unlock_irqrestore(&ppol->load_lock, flags);

	/*
	 * Send govinfo notification.
	 * Govinfo notification could potentially wake up another thread
	 * managed by its clients. Thread wakeups might trigger a load
	 * change callback that executes this function again. Therefore
	 * no spinlock could be held when sending the notification.
	 */
	for_each_cpu(i, ppol->policy->cpus) {
		pcpu = &per_cpu(cpuinfo, i);
		govinfo.cpu = i;
		govinfo.load = pcpu->loadadjfreq / ppol->policy->max;
		govinfo.sampling_rate_us = tunables->timer_rate;
		atomic_notifier_call_chain(&cpufreq_govinfo_notifier_list,
					   CPUFREQ_LOAD_CHANGE, &govinfo);
	}

	spin_lock_irqsave(&ppol->target_freq_lock, flags);
	cpu_load = loadadjfreq / ppol->target_freq;
	this_hispeed_freq = max(tunables->hispeed_freq, ppol->policy->min);
	this_hispeed_freq = min(this_hispeed_freq, ppol->policy->max);

	if (tunables->ignore_hispeed_on_notif && is_notif) {
		new_freq = choose_freq(ppol, loadadjfreq);
	} else if (cpu_load >= tunables->go_hispeed_load) {
		if (ppol->target_freq < this_hispeed_freq) {
			new_freq = this_hispeed_freq;
		} else {
			if (tunables->fastlane && cpu_load > tunables->fastlane_threshold)
				new_freq = fastlane_freq(ppol, cpu_load);
			else
				new_freq = choose_freq(ppol, loadadjfreq);

			if (new_freq < this_hispeed_freq)
				new_freq = this_hispeed_freq;
		}
	} else {
		if (tunables->fastlane && cpu_load > tunables->fastlane_threshold)
			new_freq = fastlane_freq(ppol, cpu_load);
		else
			new_freq = choose_freq(ppol, loadadjfreq);

		if (new_freq > tunables->hispeed_freq &&
				ppol->target_freq < tunables->hispeed_freq)
			new_freq = tunables->hispeed_freq;
	}

	if ((!tunables->ignore_hispeed_on_notif || !is_notif) &&
	    ppol->target_freq >= this_hispeed_freq &&
	    new_freq > ppol->target_freq &&
	    now - ppol->hispeed_validate_time <
	    freq_to_above_hispeed_delay(tunables, ppol->target_freq)) {
		trace_cpufreq_flash_notyet(
			max_cpu, cpu_load, ppol->target_freq,
			ppol->policy->cur, new_freq);
		spin_unlock_irqrestore(&ppol->target_freq_lock, flags);
		goto rearm;
	}

	ppol->hispeed_validate_time = now;

	if (cpufreq_frequency_table_target(ppol->policy, ppol->freq_table,
					   new_freq, CPUFREQ_RELATION_L,
					   &index)) {
		spin_unlock_irqrestore(&ppol->target_freq_lock, flags);
		goto rearm;
	}

	new_freq = ppol->freq_table[index].frequency;

	if ((!tunables->fast_ramp_down || !is_notif) &&
	    new_freq < ppol->target_freq &&
	    now - ppol->max_freq_hyst_start_time <
	    tunables->max_freq_hysteresis) {
		trace_cpufreq_flash_notyet(max_cpu, cpu_load,
			ppol->target_freq, ppol->policy->cur, new_freq);
		spin_unlock_irqrestore(&ppol->target_freq_lock, flags);
		goto rearm;
	}

	/*
	 * Do not scale below floor_freq unless we have been at or above the
	 * floor frequency for the minimum sample time since last validated.
	 */
	if ((!tunables->fast_ramp_down || !is_notif) &&
	    new_freq < ppol->floor_freq) {
		if (now - ppol->floor_validate_time <
				tunables->min_sample_time) {
			trace_cpufreq_flash_notyet(
				max_cpu, cpu_load, ppol->target_freq,
				ppol->policy->cur, new_freq);
			spin_unlock_irqrestore(&ppol->target_freq_lock, flags);
			goto rearm;
		}
	}

	/*
	 * Update the timestamp for checking whether speed has been held at
	 * or above the selected frequency for a minimum of min_sample_time,
	 * if not boosted to this_hispeed_freq.  If boosted to this_hispeed_freq
	 * then we allow the speed to drop as soon as the boostpulse duration
	 * expires (or the indefinite boost is turned off).
	 */

	if (new_freq > this_hispeed_freq) {
		ppol->floor_freq = new_freq;
		ppol->floor_validate_time = now;
	}

	if (new_freq == ppol->policy->max)
		ppol->max_freq_hyst_start_time = now;

	if (ppol->target_freq == new_freq) {
		trace_cpufreq_flash_already(
			max_cpu, cpu_load, ppol->target_freq,
			ppol->policy->cur, new_freq);
		spin_unlock_irqrestore(&ppol->target_freq_lock, flags);
		goto rearm;
	}

	trace_cpufreq_flash_target(max_cpu, cpu_load, ppol->target_freq,
					 ppol->policy->cur, new_freq);

	ppol->target_freq = new_freq;
	spin_unlock_irqrestore(&ppol->target_freq_lock, flags);
	spin_lock_irqsave(&speedchange_cpumask_lock, flags);
	cpumask_set_cpu(max_cpu, &speedchange_cpumask);
	spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);
	wake_up_process_no_notif(speedchange_task);

rearm:
	if (!timer_pending(&ppol->policy_timer))
		cpufreq_flash_timer_resched(data, false);

exit:
	up_read(&ppol->enable_sem);
	return;
}

static void cpufreq_flash_timer(unsigned long data)
{
	__cpufreq_flash_timer(data, false);
}

static int cpufreq_flash_speedchange_task(void *data)
{
	unsigned int cpu;
	cpumask_t tmp_mask;
	unsigned long flags;
	struct cpufreq_flash_policyinfo *ppol;
	struct cpufreq_flash_tunables *tunables;
	bool display_on = is_display_on();

	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);
		spin_lock_irqsave(&speedchange_cpumask_lock, flags);

		if (cpumask_empty(&speedchange_cpumask)) {
			spin_unlock_irqrestore(&speedchange_cpumask_lock,
					       flags);
			schedule();

			if (kthread_should_stop())
				break;

			spin_lock_irqsave(&speedchange_cpumask_lock, flags);
		}

		set_current_state(TASK_RUNNING);
		tmp_mask = speedchange_cpumask;
		cpumask_clear(&speedchange_cpumask);
		spin_unlock_irqrestore(&speedchange_cpumask_lock, flags);

		for_each_cpu(cpu, &tmp_mask) {
			ppol = per_cpu(polinfo, cpu);
			tunables = ppol->policy->governor_data;
			if (!down_read_trylock(&ppol->enable_sem))
				continue;
			if (!ppol->governor_enabled) {
				up_read(&ppol->enable_sem);
				continue;
			}

			if (unlikely(!display_on)) {
			    if (ppol->target_freq > tunables->screen_off_max)
				ppol->target_freq = tunables->screen_off_max;
			}

			if (ppol->target_freq != ppol->policy->cur) {
			    if (tunables->powersave_bias || !display_on)
				    __cpufreq_driver_target(ppol->policy,
							    ppol->target_freq,
							    CPUFREQ_RELATION_C);
			    else
				    __cpufreq_driver_target(ppol->policy,
							    ppol->target_freq,
							    CPUFREQ_RELATION_H);
			}
			trace_cpufreq_flash_setspeed(cpu,
						     ppol->target_freq,
						     ppol->policy->cur);
			up_read(&ppol->enable_sem);
		}
	}

	return 0;
}

static int load_change_callback(struct notifier_block *nb, unsigned long val,
				void *data)
{
	unsigned long cpu = (unsigned long) data;
	struct cpufreq_flash_policyinfo *ppol = per_cpu(polinfo, cpu);
	struct cpufreq_flash_tunables *tunables;

	if (speedchange_task == current)
		return 0;
	if (!ppol || ppol->reject_notification)
		return 0;

	if (!down_read_trylock(&ppol->enable_sem))
		return 0;
	if (!ppol->governor_enabled) {
		up_read(&ppol->enable_sem);
		return 0;
	}
	tunables = ppol->policy->governor_data;
	if (!tunables->use_sched_load || !tunables->use_migration_notif) {
		up_read(&ppol->enable_sem);
		return 0;
	}

	trace_cpufreq_flash_load_change(cpu);
	del_timer(&ppol->policy_timer);
	del_timer(&ppol->policy_sched_loadack_timer);
	__cpufreq_flash_timer(cpu, true);

	up_read(&ppol->enable_sem);
	return 0;
}

static struct notifier_block load_notifier_block = {
	.notifier_call = load_change_callback,
};

static int cpufreq_flash_notifier(
	struct notifier_block *nb, unsigned long val, void *data)
{
	struct cpufreq_freqs *freq = data;
	struct cpufreq_flash_policyinfo *ppol;
	int cpu;
	unsigned long flags;

	if (val == CPUFREQ_PRECHANGE) {
		ppol = per_cpu(polinfo, freq->cpu);
		if (!ppol)
			return 0;
		if (!down_read_trylock(&ppol->enable_sem))
			return 0;
		if (!ppol->governor_enabled) {
			up_read(&ppol->enable_sem);
			return 0;
		}

		if (cpumask_first(ppol->policy->cpus) != freq->cpu) {
			up_read(&ppol->enable_sem);
			return 0;
		}
		spin_lock_irqsave(&ppol->load_lock, flags);
		for_each_cpu(cpu, ppol->policy->cpus)
			update_load(cpu);
		spin_unlock_irqrestore(&ppol->load_lock, flags);

		up_read(&ppol->enable_sem);
	}
	return 0;
}

static struct notifier_block cpufreq_notifier_block = {
	.notifier_call = cpufreq_flash_notifier,
};

static unsigned int *get_tokenized_data(const char *buf, int *num_tokens)
{
	const char *cp;
	int i;
	int ntokens = 1;
	unsigned int *tokenized_data;
	int err = -EINVAL;

	cp = buf;
	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	if (!(ntokens & 0x1))
		goto err;

	tokenized_data = kmalloc(ntokens * sizeof(unsigned int), GFP_KERNEL);
	if (!tokenized_data) {
		err = -ENOMEM;
		goto err;
	}

	cp = buf;
	i = 0;
	while (i < ntokens) {
		if (sscanf(cp, "%u", &tokenized_data[i++]) != 1)
			goto err_kfree;

		cp = strpbrk(cp, " :");
		if (!cp)
			break;
		cp++;
	}

	if (i != ntokens)
		goto err_kfree;

	*num_tokens = ntokens;
	return tokenized_data;

err_kfree:
	kfree(tokenized_data);
err:
	return ERR_PTR(err);
}

static ssize_t show_target_loads(
	struct cpufreq_flash_tunables *tunables,
	char *buf)
{
	int i;
	ssize_t ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&tunables->target_loads_lock, flags);

	for (i = 0; i < tunables->ntarget_loads; i++)
		ret += sprintf(buf + ret, "%u%s", tunables->target_loads[i],
			       i & 0x1 ? ":" : " ");

	sprintf(buf + ret - 1, "\n");
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);
	return ret;
}

static ssize_t store_target_loads(
	struct cpufreq_flash_tunables *tunables,
	const char *buf, size_t count)
{
	int ntokens;
	unsigned int *new_target_loads = NULL;
	unsigned long flags;

	new_target_loads = get_tokenized_data(buf, &ntokens);
	if (IS_ERR(new_target_loads))
		return PTR_RET(new_target_loads);

	spin_lock_irqsave(&tunables->target_loads_lock, flags);
	if (tunables->target_loads != default_target_loads)
		kfree(tunables->target_loads);
	tunables->target_loads = new_target_loads;
	tunables->ntarget_loads = ntokens;
	spin_unlock_irqrestore(&tunables->target_loads_lock, flags);
	return count;
}

static ssize_t show_above_hispeed_delay(
	struct cpufreq_flash_tunables *tunables, char *buf)
{
	int i;
	ssize_t ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&tunables->above_hispeed_delay_lock, flags);

	for (i = 0; i < tunables->nabove_hispeed_delay; i++)
		ret += sprintf(buf + ret, "%u%s",
			       tunables->above_hispeed_delay[i],
			       i & 0x1 ? ":" : " ");

	sprintf(buf + ret - 1, "\n");
	spin_unlock_irqrestore(&tunables->above_hispeed_delay_lock, flags);
	return ret;
}

static ssize_t store_above_hispeed_delay(
	struct cpufreq_flash_tunables *tunables,
	const char *buf, size_t count)
{
	int ntokens, i;
	unsigned int *new_above_hispeed_delay = NULL;
	unsigned long flags;

	new_above_hispeed_delay = get_tokenized_data(buf, &ntokens);
	if (IS_ERR(new_above_hispeed_delay))
		return PTR_RET(new_above_hispeed_delay);

	/* Make sure frequencies are in ascending order. */
	for (i = 3; i < ntokens; i += 2) {
		if (new_above_hispeed_delay[i] <=
		    new_above_hispeed_delay[i - 2]) {
			kfree(new_above_hispeed_delay);
			return -EINVAL;
		}
	}

	spin_lock_irqsave(&tunables->above_hispeed_delay_lock, flags);
	if (tunables->above_hispeed_delay != default_above_hispeed_delay)
		kfree(tunables->above_hispeed_delay);
	tunables->above_hispeed_delay = new_above_hispeed_delay;
	tunables->nabove_hispeed_delay = ntokens;
	spin_unlock_irqrestore(&tunables->above_hispeed_delay_lock, flags);
	return count;

}

static ssize_t show_hispeed_freq(struct cpufreq_flash_tunables *tunables,
		char *buf)
{
	return sprintf(buf, "%u\n", tunables->hispeed_freq);
}

static ssize_t store_hispeed_freq(struct cpufreq_flash_tunables *tunables,
		const char *buf, size_t count)
{
	int ret;
	long unsigned int val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	tunables->hispeed_freq = val;
	return count;
}

#define show_store_one(file_name)					\
static ssize_t show_##file_name(					\
	struct cpufreq_flash_tunables *tunables, char *buf)	\
{									\
	return snprintf(buf, PAGE_SIZE, "%u\n", tunables->file_name);	\
}									\
static ssize_t store_##file_name(					\
		struct cpufreq_flash_tunables *tunables,		\
		const char *buf, size_t count)				\
{									\
	int ret;							\
	long unsigned int val;						\
									\
	ret = kstrtoul(buf, 0, &val);				\
	if (ret < 0)							\
		return ret;						\
	tunables->file_name = val;					\
	return count;							\
}
show_store_one(max_freq_hysteresis);
show_store_one(align_windows);
show_store_one(ignore_hispeed_on_notif);
show_store_one(fast_ramp_down);

static ssize_t show_go_hispeed_load(struct cpufreq_flash_tunables
		*tunables, char *buf)
{
	return sprintf(buf, "%lu\n", tunables->go_hispeed_load);
}

static ssize_t store_go_hispeed_load(struct cpufreq_flash_tunables
		*tunables, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	tunables->go_hispeed_load = val;
	return count;
}

static ssize_t show_min_sample_time(struct cpufreq_flash_tunables
		*tunables, char *buf)
{
	return sprintf(buf, "%lu\n", tunables->min_sample_time);
}

static ssize_t store_min_sample_time(struct cpufreq_flash_tunables
		*tunables, const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	tunables->min_sample_time = val;
	return count;
}

static ssize_t show_timer_rate(struct cpufreq_flash_tunables *tunables,
		char *buf)
{
	return sprintf(buf, "%lu\n", tunables->timer_rate);
}

static ssize_t store_timer_rate(struct cpufreq_flash_tunables *tunables,
		const char *buf, size_t count)
{
	int ret;
	unsigned long val, val_round;
	struct cpufreq_flash_tunables *t;
	int cpu;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	val_round = jiffies_to_usecs(usecs_to_jiffies(val));
	if (val != val_round)
		pr_warn("timer_rate not aligned to jiffy. Rounded up to %lu\n",
			val_round);
	tunables->timer_rate = val_round;
	tunables->prev_timer_rate = val_round;

	if (!tunables->use_sched_load)
		return count;

	for_each_possible_cpu(cpu) {
		if (!per_cpu(polinfo, cpu))
			continue;
		t = per_cpu(polinfo, cpu)->cached_tunables;
		if (t && t->use_sched_load) {
			t->timer_rate = val_round;
			t->prev_timer_rate = val_round;
		}
	}
	set_window_helper(tunables);

	return count;
}

static ssize_t show_timer_sched_loadack(struct cpufreq_flash_tunables *tunables,
		char *buf)
{
	return sprintf(buf, "%d\n", tunables->timer_sched_loadack_val);
}

static ssize_t store_timer_sched_loadack(struct cpufreq_flash_tunables *tunables,
		const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtol(buf, 10, &val);
	if (ret < 0)
		return ret;

	tunables->timer_sched_loadack_val = val;
	return count;
}

static ssize_t show_io_is_busy(struct cpufreq_flash_tunables *tunables,
		char *buf)
{
	return sprintf(buf, "%u\n", tunables->io_is_busy);
}

static ssize_t store_io_is_busy(struct cpufreq_flash_tunables *tunables,
		const char *buf, size_t count)
{
	int ret;
	unsigned long val;
	struct cpufreq_flash_tunables *t;
	int cpu;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	tunables->io_is_busy = val;

	if (!tunables->use_sched_load)
		return count;

	for_each_possible_cpu(cpu) {
		if (!per_cpu(polinfo, cpu))
			continue;
		t = per_cpu(polinfo, cpu)->cached_tunables;
		if (t && t->use_sched_load)
			t->io_is_busy = val;
	}
	sched_set_io_is_busy(val);

	return count;
}

static int cpufreq_flash_enable_sched_input(
			struct cpufreq_flash_tunables *tunables)
{
	int rc = 0, j;
	struct cpufreq_flash_tunables *t;

	mutex_lock(&sched_lock);

	set_window_count++;
	if (set_window_count > 1) {
		for_each_possible_cpu(j) {
			if (!per_cpu(polinfo, j))
				continue;
			t = per_cpu(polinfo, j)->cached_tunables;
			if (t && t->use_sched_load) {
				tunables->timer_rate = t->timer_rate;
				tunables->io_is_busy = t->io_is_busy;
				break;
			}
		}
	} else {
		rc = set_window_helper(tunables);
		if (rc) {
			pr_err("%s: Failed to set sched window\n", __func__);
			set_window_count--;
			goto out;
		}
		sched_set_io_is_busy(tunables->io_is_busy);
	}

	if (!tunables->use_migration_notif)
		goto out;

	migration_register_count++;
	if (migration_register_count > 1)
		goto out;
	else
		atomic_notifier_chain_register(&load_alert_notifier_head,
						&load_notifier_block);
out:
	mutex_unlock(&sched_lock);
	return rc;
}

static int cpufreq_flash_disable_sched_input(
			struct cpufreq_flash_tunables *tunables)
{
	mutex_lock(&sched_lock);

	if (tunables->use_migration_notif) {
		migration_register_count--;
		if (migration_register_count < 1)
			atomic_notifier_chain_unregister(
					&load_alert_notifier_head,
					&load_notifier_block);
	}
	set_window_count--;

	mutex_unlock(&sched_lock);
	return 0;
}

static ssize_t show_use_sched_load(
		struct cpufreq_flash_tunables *tunables, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", tunables->use_sched_load);
}

static ssize_t store_use_sched_load(
			struct cpufreq_flash_tunables *tunables,
			const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	if (tunables->use_sched_load == (bool) val)
		return count;

	tunables->use_sched_load = val;

	if (val)
		ret = cpufreq_flash_enable_sched_input(tunables);
	else
		ret = cpufreq_flash_disable_sched_input(tunables);

	if (ret) {
		tunables->use_sched_load = !val;
		return ret;
	}

	return count;
}

static ssize_t show_use_migration_notif(
		struct cpufreq_flash_tunables *tunables, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n",
			tunables->use_migration_notif);
}

static ssize_t store_use_migration_notif(
			struct cpufreq_flash_tunables *tunables,
			const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	if (tunables->use_migration_notif == (bool) val)
		return count;
	tunables->use_migration_notif = val;

	if (!tunables->use_sched_load)
		return count;

	mutex_lock(&sched_lock);
	if (val) {
		migration_register_count++;
		if (migration_register_count == 1)
			atomic_notifier_chain_register(
					&load_alert_notifier_head,
					&load_notifier_block);
	} else {
		migration_register_count--;
		if (!migration_register_count)
			atomic_notifier_chain_unregister(
					&load_alert_notifier_head,
					&load_notifier_block);
	}
	mutex_unlock(&sched_lock);

	return count;
}

static ssize_t show_fastlane(
		struct cpufreq_flash_tunables *tunables, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", tunables->fastlane);
}

static ssize_t store_fastlane(
			struct cpufreq_flash_tunables *tunables,
			const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	tunables->fastlane = val;
	return count;
}

static ssize_t show_fastlane_threshold(
		struct cpufreq_flash_tunables *tunables, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", tunables->fastlane_threshold);
}

static ssize_t store_fastlane_threshold(
			struct cpufreq_flash_tunables *tunables,
			const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0 || ret > 100)
		return ret;
	tunables->fastlane_threshold = val;
	return count;
}

static ssize_t show_powersave_bias(struct cpufreq_flash_tunables *tunables,
		char *buf)
{
	return sprintf(buf, "%u\n", tunables->powersave_bias);
}

static ssize_t store_powersave_bias(struct cpufreq_flash_tunables *tunables,
		const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (ret < 0)
		return ret;
	tunables->powersave_bias = val;
	return count;
}

static ssize_t show_screen_off_maxfreq(
		struct cpufreq_flash_tunables *tunables,
                char *buf)
{
	return sprintf(buf, "%lu\n", tunables->screen_off_max);
}

static ssize_t store_screen_off_maxfreq(
		struct cpufreq_flash_tunables *tunables,
                const char *buf, size_t count)
{
	int ret;
	unsigned long val;

	ret = strict_strtoul(buf, 0, &val);
	if (ret < 0)
		return ret;

	if (val < 384000)
		tunables->screen_off_max = DEFAULT_SCREEN_OFF_MAX;
	else
		tunables->screen_off_max = val;

	return count;
}

/*
 * Create show/store routines
 * - sys: One governor instance for complete SYSTEM
 * - pol: One governor instance per struct cpufreq_policy
 */
#define show_gov_pol_sys(file_name)					\
static ssize_t show_##file_name##_gov_sys				\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return show_##file_name(common_tunables, buf);			\
}									\
									\
static ssize_t show_##file_name##_gov_pol				\
(struct cpufreq_policy *policy, char *buf)				\
{									\
	return show_##file_name(policy->governor_data, buf);		\
}

#define store_gov_pol_sys(file_name)					\
static ssize_t store_##file_name##_gov_sys				\
(struct kobject *kobj, struct attribute *attr, const char *buf,		\
	size_t count)							\
{									\
	return store_##file_name(common_tunables, buf, count);		\
}									\
									\
static ssize_t store_##file_name##_gov_pol				\
(struct cpufreq_policy *policy, const char *buf, size_t count)		\
{									\
	return store_##file_name(policy->governor_data, buf, count);	\
}

#define show_store_gov_pol_sys(file_name)				\
show_gov_pol_sys(file_name);						\
store_gov_pol_sys(file_name)

show_store_gov_pol_sys(target_loads);
show_store_gov_pol_sys(above_hispeed_delay);
show_store_gov_pol_sys(hispeed_freq);
show_store_gov_pol_sys(go_hispeed_load);
show_store_gov_pol_sys(min_sample_time);
show_store_gov_pol_sys(timer_rate);
show_store_gov_pol_sys(timer_sched_loadack);
show_store_gov_pol_sys(io_is_busy);
show_store_gov_pol_sys(use_sched_load);
show_store_gov_pol_sys(use_migration_notif);
show_store_gov_pol_sys(max_freq_hysteresis);
show_store_gov_pol_sys(align_windows);
show_store_gov_pol_sys(fastlane);
show_store_gov_pol_sys(fastlane_threshold);
show_store_gov_pol_sys(ignore_hispeed_on_notif);
show_store_gov_pol_sys(fast_ramp_down);
show_store_gov_pol_sys(powersave_bias);
show_store_gov_pol_sys(screen_off_maxfreq);

#define gov_sys_attr_rw(_name)						\
static struct global_attr _name##_gov_sys =				\
__ATTR(_name, 0664, show_##_name##_gov_sys, store_##_name##_gov_sys)

#define gov_pol_attr_rw(_name)						\
static struct freq_attr _name##_gov_pol =				\
__ATTR(_name, 0664, show_##_name##_gov_pol, store_##_name##_gov_pol)

#define gov_sys_pol_attr_rw(_name)					\
	gov_sys_attr_rw(_name);						\
	gov_pol_attr_rw(_name)

gov_sys_pol_attr_rw(target_loads);
gov_sys_pol_attr_rw(above_hispeed_delay);
gov_sys_pol_attr_rw(hispeed_freq);
gov_sys_pol_attr_rw(go_hispeed_load);
gov_sys_pol_attr_rw(min_sample_time);
gov_sys_pol_attr_rw(timer_rate);
gov_sys_pol_attr_rw(timer_sched_loadack);
gov_sys_pol_attr_rw(io_is_busy);
gov_sys_pol_attr_rw(use_sched_load);
gov_sys_pol_attr_rw(use_migration_notif);
gov_sys_pol_attr_rw(max_freq_hysteresis);
gov_sys_pol_attr_rw(align_windows);
gov_sys_pol_attr_rw(fastlane);
gov_sys_pol_attr_rw(fastlane_threshold);
gov_sys_pol_attr_rw(ignore_hispeed_on_notif);
gov_sys_pol_attr_rw(fast_ramp_down);
gov_sys_pol_attr_rw(powersave_bias);
gov_sys_pol_attr_rw(screen_off_maxfreq);

/* One Governor instance for entire system */
static struct attribute *flash_attributes_gov_sys[] = {
	&target_loads_gov_sys.attr,
	&above_hispeed_delay_gov_sys.attr,
	&hispeed_freq_gov_sys.attr,
	&go_hispeed_load_gov_sys.attr,
	&min_sample_time_gov_sys.attr,
	&timer_rate_gov_sys.attr,
	&timer_sched_loadack_gov_sys.attr,
	&io_is_busy_gov_sys.attr,
	&use_sched_load_gov_sys.attr,
	&use_migration_notif_gov_sys.attr,
	&max_freq_hysteresis_gov_sys.attr,
	&align_windows_gov_sys.attr,
	&fastlane_gov_sys.attr,
	&fastlane_threshold_gov_sys.attr,
	&ignore_hispeed_on_notif_gov_sys.attr,
	&fast_ramp_down_gov_sys.attr,
	&powersave_bias_gov_sys.attr,
	&screen_off_maxfreq_gov_sys.attr,
	NULL,
};

static struct attribute_group flash_attr_group_gov_sys = {
	.attrs = flash_attributes_gov_sys,
	.name = "flash",
};

/* Per policy governor instance */
static struct attribute *flash_attributes_gov_pol[] = {
	&target_loads_gov_pol.attr,
	&above_hispeed_delay_gov_pol.attr,
	&hispeed_freq_gov_pol.attr,
	&go_hispeed_load_gov_pol.attr,
	&min_sample_time_gov_pol.attr,
	&timer_rate_gov_pol.attr,
	&timer_sched_loadack_gov_pol.attr,
	&io_is_busy_gov_pol.attr,
	&use_sched_load_gov_pol.attr,
	&use_migration_notif_gov_pol.attr,
	&max_freq_hysteresis_gov_pol.attr,
	&align_windows_gov_pol.attr,
	&fastlane_gov_pol.attr,
	&fastlane_threshold_gov_pol.attr,
	&ignore_hispeed_on_notif_gov_pol.attr,
	&fast_ramp_down_gov_pol.attr,
	&powersave_bias_gov_pol.attr,
	&screen_off_maxfreq_gov_pol.attr,
	NULL,
};

static struct attribute_group flash_attr_group_gov_pol = {
	.attrs = flash_attributes_gov_pol,
	.name = "flash",
};

static struct attribute_group *get_sysfs_attr(void)
{
	if (have_governor_per_policy())
		return &flash_attr_group_gov_pol;
	else
		return &flash_attr_group_gov_sys;
}

static void cpufreq_flash_nop_timer(unsigned long data)
{
}

static struct cpufreq_flash_tunables *alloc_tunable(
					struct cpufreq_policy *policy)
{
	struct cpufreq_flash_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (!tunables)
		return ERR_PTR(-ENOMEM);

	tunables->above_hispeed_delay = default_above_hispeed_delay;
	tunables->nabove_hispeed_delay =
		ARRAY_SIZE(default_above_hispeed_delay);
	tunables->go_hispeed_load = DEFAULT_GO_HISPEED_LOAD;
	tunables->target_loads = default_target_loads;
	tunables->ntarget_loads = ARRAY_SIZE(default_target_loads);
	tunables->min_sample_time = DEFAULT_MIN_SAMPLE_TIME;
	tunables->timer_rate = DEFAULT_TIMER_RATE;
	tunables->prev_timer_rate = DEFAULT_TIMER_RATE;
	tunables->timer_sched_loadack_val = DEFAULT_TIMER_sched_loadACK;
	tunables->fastlane = false;
	tunables->fastlane_threshold = 50;
	tunables->screen_off_max = DEFAULT_SCREEN_OFF_MAX;

	spin_lock_init(&tunables->target_loads_lock);
	spin_lock_init(&tunables->above_hispeed_delay_lock);

	return tunables;
}

static struct cpufreq_flash_policyinfo *get_policyinfo(
					struct cpufreq_policy *policy)
{
	struct cpufreq_flash_policyinfo *ppol =
				per_cpu(polinfo, policy->cpu);
	int i;
	struct sched_load *sched_load;

	/* polinfo already allocated for policy, return */
	if (ppol)
		return ppol;

	ppol = kzalloc(sizeof(*ppol), GFP_KERNEL);
	if (!ppol)
		return ERR_PTR(-ENOMEM);

	sched_load = kcalloc(cpumask_weight(policy->related_cpus), sizeof(*sched_load),
		     GFP_KERNEL);
	if (!sched_load) {
		kfree(ppol);
		return ERR_PTR(-ENOMEM);
	}
	ppol->sched_load = sched_load;

	init_timer_deferrable(&ppol->policy_timer);
	ppol->policy_timer.function = cpufreq_flash_timer;
	init_timer(&ppol->policy_sched_loadack_timer);
	ppol->policy_sched_loadack_timer.function = cpufreq_flash_nop_timer;
	spin_lock_init(&ppol->load_lock);
	spin_lock_init(&ppol->target_freq_lock);
	init_rwsem(&ppol->enable_sem);

	for_each_cpu(i, policy->related_cpus)
		per_cpu(polinfo, i) = ppol;
	return ppol;
}

/* This function is not multithread-safe. */
static void free_policyinfo(int cpu)
{
	struct cpufreq_flash_policyinfo *ppol = per_cpu(polinfo, cpu);
	int j;

	if (!ppol)
		return;

	for_each_possible_cpu(j)
		if (per_cpu(polinfo, j) == ppol)
			per_cpu(polinfo, cpu) = NULL;
	kfree(ppol->cached_tunables);
	kfree(ppol->sched_load);
	kfree(ppol);
}

static struct cpufreq_flash_tunables *get_tunables(
				struct cpufreq_flash_policyinfo *ppol)
{
	if (have_governor_per_policy())
		return ppol->cached_tunables;
	else
		return cached_common_tunables;
}

static int cpufreq_governor_flash(struct cpufreq_policy *policy,
		unsigned int event)
{
	int rc;
	struct cpufreq_flash_policyinfo *ppol;
	struct cpufreq_frequency_table *freq_table;
	struct cpufreq_flash_tunables *tunables;
	unsigned long flags;
	unsigned int anyboost;

	if (have_governor_per_policy())
		tunables = policy->governor_data;
	else
		tunables = common_tunables;

	BUG_ON(!tunables && (event != CPUFREQ_GOV_POLICY_INIT));

	switch (event) {
	case CPUFREQ_GOV_POLICY_INIT:
		ppol = get_policyinfo(policy);
		if (IS_ERR(ppol))
			return PTR_ERR(ppol);

		if (have_governor_per_policy()) {
			WARN_ON(tunables);
		} else if (tunables) {
			tunables->usage_count++;
			policy->governor_data = tunables;
			return 0;
		}

		tunables = get_tunables(ppol);
		if (!tunables) {
			tunables = alloc_tunable(policy);
			if (IS_ERR(tunables))
				return PTR_ERR(tunables);
		}

		tunables->usage_count = 1;
		policy->governor_data = tunables;
		if (!have_governor_per_policy()) {
			WARN_ON(cpufreq_get_global_kobject());
			common_tunables = tunables;
		}

		rc = sysfs_create_group(get_governor_parent_kobj(policy),
				get_sysfs_attr());
		if (rc) {
			kfree(tunables);
			policy->governor_data = NULL;
			if (!have_governor_per_policy()) {
				common_tunables = NULL;
				cpufreq_put_global_kobject();
			}
			return rc;
		}

		if (!policy->governor->initialized)
			cpufreq_register_notifier(&cpufreq_notifier_block,
					CPUFREQ_TRANSITION_NOTIFIER);

		if (tunables->use_sched_load)
			cpufreq_flash_enable_sched_input(tunables);

		if (have_governor_per_policy())
			ppol->cached_tunables = tunables;
		else
			cached_common_tunables = tunables;

		break;

	case CPUFREQ_GOV_POLICY_EXIT:
		if (!--tunables->usage_count) {
			if (policy->governor->initialized == 1)
				cpufreq_unregister_notifier(&cpufreq_notifier_block,
						CPUFREQ_TRANSITION_NOTIFIER);

			sysfs_remove_group(get_governor_parent_kobj(policy),
					get_sysfs_attr());
			if (!have_governor_per_policy())
				cpufreq_put_global_kobject();
			common_tunables = NULL;
		}

		policy->governor_data = NULL;

		if (tunables->use_sched_load)
			cpufreq_flash_disable_sched_input(tunables);

		break;

	case CPUFREQ_GOV_START:
		mutex_lock(&gov_lock);

		freq_table = cpufreq_frequency_get_table(policy->cpu);

		ppol = per_cpu(polinfo, policy->cpu);
		ppol->policy = policy;
		ppol->target_freq = policy->cur;
		ppol->freq_table = freq_table;
		ppol->floor_freq = ppol->target_freq;
		ppol->floor_validate_time = ktime_to_us(ktime_get());
		ppol->hispeed_validate_time = ppol->floor_validate_time;
		ppol->min_freq = policy->min;
		ppol->reject_notification = true;
		down_write(&ppol->enable_sem);
		del_timer_sync(&ppol->policy_timer);
		del_timer_sync(&ppol->policy_sched_loadack_timer);
		ppol->policy_timer.data = policy->cpu;
		ppol->last_evaluated_jiffy = get_jiffies_64();
		cpufreq_flash_timer_start(tunables, policy->cpu);
		ppol->governor_enabled = 1;
		up_write(&ppol->enable_sem);
		ppol->reject_notification = false;

		mutex_unlock(&gov_lock);
		break;

	case CPUFREQ_GOV_STOP:
		mutex_lock(&gov_lock);

		ppol = per_cpu(polinfo, policy->cpu);
		ppol->reject_notification = true;
		down_write(&ppol->enable_sem);
		ppol->governor_enabled = 0;
		ppol->target_freq = 0;
		del_timer_sync(&ppol->policy_timer);
		del_timer_sync(&ppol->policy_sched_loadack_timer);
		up_write(&ppol->enable_sem);
		ppol->reject_notification = false;

		mutex_unlock(&gov_lock);
		break;

	case CPUFREQ_GOV_LIMITS:
		__cpufreq_driver_target(policy,
				policy->cur, CPUFREQ_RELATION_L);

		ppol = per_cpu(polinfo, policy->cpu);

		down_read(&ppol->enable_sem);
		if (ppol->governor_enabled) {
			spin_lock_irqsave(&ppol->target_freq_lock, flags);
			if (policy->max < ppol->target_freq) {
				ppol->target_freq = policy->max;
			} else if (policy->min > ppol->target_freq) {
				ppol->target_freq = policy->min;
				anyboost = 1;
			}
			spin_unlock_irqrestore(&ppol->target_freq_lock, flags);

			if (policy->min < ppol->min_freq)
				cpufreq_flash_timer_resched(policy->cpu,
								  true);
			ppol->min_freq = policy->min;
		}

		up_read(&ppol->enable_sem);

		if (anyboost) {
			u64 now = ktime_to_us(ktime_get());

			ppol->hispeed_validate_time = now;
			ppol->floor_freq = policy->min;
			ppol->floor_validate_time = now;
		}

		break;
	}
	return 0;
}

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_FLASH
static
#endif
struct cpufreq_governor cpufreq_gov_flash = {
	.name = "flash",
	.governor = cpufreq_governor_flash,
	.max_transition_latency = 10000000,
	.owner = THIS_MODULE,
};

static int __init cpufreq_flash_init(void)
{
	struct sched_param param = { .sched_priority = MAX_RT_PRIO-1 };

	spin_lock_init(&speedchange_cpumask_lock);
	mutex_init(&gov_lock);
	mutex_init(&sched_lock);
	speedchange_task =
		kthread_create(cpufreq_flash_speedchange_task, NULL,
			       "cfflash");
	if (IS_ERR(speedchange_task))
		return PTR_ERR(speedchange_task);

	sched_setscheduler_nocheck(speedchange_task, SCHED_FIFO, &param);
	get_task_struct(speedchange_task);

	/* NB: wake up so the thread does not look hung to the freezer */
	wake_up_process_no_notif(speedchange_task);

	return cpufreq_register_governor(&cpufreq_gov_flash);
}

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_FLASH
fs_initcall(cpufreq_flash_init);
#else
module_init(cpufreq_flash_init);
#endif

static void __exit cpufreq_flash_exit(void)
{
	int cpu;

	cpufreq_unregister_governor(&cpufreq_gov_flash);
	kthread_stop(speedchange_task);
	put_task_struct(speedchange_task);

	for_each_possible_cpu(cpu)
		free_policyinfo(cpu);
}

module_exit(cpufreq_flash_exit);

MODULE_AUTHOR("Mike Chan <mike@android.com>");
MODULE_DESCRIPTION("'cpufreq_flash' - A cpufreq governor for "
	"Latency sensitive workloads");
MODULE_LICENSE("GPL");
