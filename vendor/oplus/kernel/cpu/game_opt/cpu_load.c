// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 Oplus. All rights reserved.
 */

#include <linux/types.h>
#include <linux/cpu.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include <linux/percpu-defs.h>
#include <linux/spinlock.h>
#include <linux/cpufreq.h>
#include <linux/cpumask.h>
#include <linux/cpuidle.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <trace/hooks/cpuidle.h>
#include <trace/hooks/cpufreq.h>

#include "game_ctrl.h"

#define KHZ_PER_MHZ 1000

struct time_in_state {
	spinlock_t lock;
	u64 last_read;
	u64 last_update;
	unsigned int *freq_table; /* mHz */
	u64 *time;
	unsigned int time_array_size;
	unsigned int time_byte_size;
	unsigned int max_freq; /* mHz */
	unsigned int max_freq_state;
	unsigned int max_idle_state;
	/* 0=active 1=idle */
	unsigned int cur_idle_idx;
	/* 0~freq_num-1 */
	unsigned int cur_freq_idx;
};

static DEFINE_PER_CPU(struct time_in_state, stats_info);

static bool initialized = false;

/* called with icpu->lock held */
static void inline update_cur_state(struct time_in_state *icpu, u64 now)
{
	u64 delta_time;
	unsigned int pos;

	delta_time = now - icpu->last_update;
	pos = icpu->cur_idle_idx * icpu->max_freq_state + icpu->cur_freq_idx;
	/* sanity check, almid: 5120983 */
	if (pos >= icpu->time_array_size)
		pos = icpu->time_array_size - 1;
	icpu->time[pos] += delta_time;
	icpu->last_update = now;
}

/* called with icpu->lock held */
static void inline reset_cur_state_after_read(struct time_in_state *icpu, u64 now)
{
	memset(icpu->time, 0, icpu->time_byte_size);
	icpu->last_read = icpu->last_update = now;
}

static int cpufreq_table_get_index(struct time_in_state *stats, unsigned int freq)
{
	int index;
	freq /= KHZ_PER_MHZ;
	for (index = 0; index < stats->max_freq_state; index++)
		if (stats->freq_table[index] == freq)
			return index;
	return -1;
}

static void get_cpu_load(int cpu, int *util_pct, int *busy_pct)
{
	unsigned int i;
	u64 now, delta_time, delta_idle = 0, timeadjfreq = 0;
	struct time_in_state *icpu = per_cpu_ptr(&stats_info, cpu);
	unsigned long flags;

	if (!initialized)
		return;

	now = ktime_to_us(ktime_get());

	spin_lock_irqsave(&icpu->lock, flags);

	update_cur_state(icpu, now);

	delta_time = now - icpu->last_read;
	for (i = 0; i < icpu->max_freq_state; i++)
		timeadjfreq += icpu->freq_table[i] * icpu->time[i];
	for (i = 0; i < icpu->max_freq_state; i++)
		delta_idle += icpu->time[icpu->max_freq_state + i];

	if (timeadjfreq <= 0)
		*util_pct = 0;
	else
		*util_pct = div64_u64(100 * timeadjfreq, delta_time * icpu->max_freq);

	if (delta_time <= delta_idle)
		*busy_pct = 0;
	else
		*busy_pct = div64_u64(100 * (delta_time - delta_idle), delta_time);

	reset_cur_state_after_read(icpu, now);

	spin_unlock_irqrestore(&icpu->lock, flags);
}

static void time_in_state_update_idle(int cpu, unsigned int new_idle_index)
{
	struct time_in_state *icpu = NULL;
	unsigned long flags;

	if (!initialized || cpu < 0 || cpu >= nr_cpu_ids)
		return;

	icpu = per_cpu_ptr(&stats_info, cpu);
	if (new_idle_index >= icpu->max_idle_state)
		return;

	spin_lock_irqsave(&icpu->lock, flags);
	update_cur_state(icpu, ktime_to_us(ktime_get()));
	icpu->cur_idle_idx = new_idle_index;
	spin_unlock_irqrestore(&icpu->lock, flags);
}

static void time_in_state_update_freq(struct cpumask *cpus,
			       unsigned int new_freq)
{
	int cpu;
	unsigned int new_freq_index;
	struct time_in_state *icpu = NULL;
	u64 now;
	unsigned long flags;

	if (!initialized || cpus == NULL || cpumask_empty(cpus))
		return;

	icpu = per_cpu_ptr(&stats_info, cpumask_first(cpus));
	new_freq_index = cpufreq_table_get_index(icpu, new_freq);
	if (new_freq_index < 0 || new_freq_index >= icpu->max_freq_state)
		return;

	now = ktime_to_us(ktime_get());
	for_each_cpu(cpu, cpus) {
		icpu = per_cpu_ptr(&stats_info, cpu);
		spin_lock_irqsave(&icpu->lock, flags);
		update_cur_state(icpu, now);
		icpu->cur_freq_idx = new_freq_index;
		spin_unlock_irqrestore(&icpu->lock, flags);
	}
}

static inline bool need_stat_cpu_load(void)
{
	return (atomic_read(&have_valid_game_pid) > 0) || (atomic_read(&have_valid_render_pid) > 0);
}

static void android_vh_cpu_idle_enter(void *unused, int *state, struct cpuidle_device *dev)
{
	if (!need_stat_cpu_load())
		return;

	time_in_state_update_idle(dev->cpu, 1);
}

static void android_vh_cpu_idle_exit(void *unused, int state, struct cpuidle_device *dev)
{
	if (!need_stat_cpu_load())
		return;

	time_in_state_update_idle(dev->cpu, 0);
}

static void android_vh_cpufreq_fast_switch(void *data, struct cpufreq_policy *policy,
		unsigned int *target_freq, unsigned int old_target_freq)
{
	if (!need_stat_cpu_load())
		return;

	if (*target_freq != policy->cur)
		time_in_state_update_freq(policy->cpus, *target_freq);
}

static int time_in_state_init(void)
{
	int cpu, ret, freq_index;
	struct time_in_state *icpu = NULL;
	struct cpufreq_policy policy;
	unsigned int alloc_size, i, count;
	struct cpufreq_frequency_table *pos = NULL;

	for_each_present_cpu(cpu) {
		icpu = per_cpu_ptr(&stats_info, cpu);

		icpu->max_idle_state = 2; /* 0 active, 1 idle */
		icpu->cur_idle_idx = 0;

		ret = cpufreq_get_policy(&policy, cpu);
		if (ret != 0)
			goto err_out;

		icpu->max_freq = policy.cpuinfo.max_freq / KHZ_PER_MHZ;

		count = cpufreq_table_count_valid_entries(&policy);
		if (!count)
			goto err_out;

		icpu->max_freq_state = count;
		icpu->time_array_size = icpu->max_idle_state * icpu->max_freq_state;
		icpu->time_byte_size = icpu->time_array_size * sizeof(u64);
		alloc_size = icpu->max_freq_state * sizeof(unsigned int) + icpu->time_byte_size;
		icpu->freq_table = kzalloc(alloc_size, GFP_KERNEL);
		if (icpu->freq_table == NULL) {
			pr_err("time in stats alloc fail for cpu:%d\n", cpu);
			goto err_out;
		}
		icpu->time = (u64 *)(icpu->freq_table + icpu->max_freq_state);

		i = 0;
		cpufreq_for_each_valid_entry(pos, policy.freq_table)
			icpu->freq_table[i++] = pos->frequency / KHZ_PER_MHZ;

		freq_index = cpufreq_table_get_index(icpu, policy.cur);
		if (freq_index < 0)
			goto err_out;
		icpu->cur_freq_idx = freq_index;

		spin_lock_init(&icpu->lock);
		icpu->last_read = icpu->last_update = ktime_to_us(ktime_get());
	}

	register_trace_android_vh_cpu_idle_enter(android_vh_cpu_idle_enter, NULL);
	register_trace_android_vh_cpu_idle_exit(android_vh_cpu_idle_exit, NULL);
	register_trace_android_vh_cpufreq_fast_switch(android_vh_cpufreq_fast_switch, NULL);

	initialized = true;

err_out:
	if (!initialized) {
		for_each_present_cpu(cpu) {
			icpu = per_cpu_ptr(&stats_info, cpu);

			if (icpu->freq_table != NULL) {
				kfree(icpu->freq_table);
				icpu->freq_table = NULL;
			}
		}
	}

	return 0;
}

static int cpu_load_show(struct seq_file *m, void *v)
{
	int cpu;
	int util_pct = 0, busy_pct = 0;

	for_each_possible_cpu(cpu) {
		if (need_stat_cpu_load())
			get_cpu_load(cpu, &util_pct, &busy_pct);
		seq_printf(m, "CPU:%d busy_pct:%d util_pct:%d\n", cpu, busy_pct, util_pct);
	}

	return 0;
}

static int cpu_load_proc_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, cpu_load_show, inode);
}

static const struct proc_ops cpu_load_proc_ops = {
	.proc_open		= cpu_load_proc_open,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release	= single_release,
};

/* for tencent amt */
static char gamt_buffer[2048];
static DEFINE_MUTEX(gamt_mutex);

static int gamt_show(struct seq_file *m, void *v)
{
	mutex_lock(&gamt_mutex);
	seq_puts(m, gamt_buffer);
	mutex_unlock(&gamt_mutex);

	return 0;
}

static int gamt_proc_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, gamt_show, inode);
}

static ssize_t gamt_proc_write(struct file *file, const char __user *buf,
	size_t count, loff_t *ppos)
{
	int ret;

	mutex_lock(&gamt_mutex);
	ret = simple_write_to_buffer(gamt_buffer, sizeof(gamt_buffer) - 1, ppos, buf, count);
	mutex_unlock(&gamt_mutex);

	return ret;
}

static const struct proc_ops gamt_proc_ops = {
	.proc_open		= gamt_proc_open,
	.proc_write		= gamt_proc_write,
	.proc_read		= seq_read,
	.proc_lseek		= seq_lseek,
	.proc_release	= single_release,
};

int cpu_load_init(void)
{
	time_in_state_init();

	proc_create_data("cpu_load", 0444, game_opt_dir, &cpu_load_proc_ops, NULL);
	proc_create_data("gamt", 0664, game_opt_dir, &gamt_proc_ops, NULL);

	return 0;
}
