/*
 * SchedTune add-on features
 *
 * Copyright (C) 2018 Samsung Electronics Co., Ltd
 * Park Bumgyu <bumgyu.park@samsung.com>
 */

#include <linux/sched.h>
#include <linux/kobject.h>
#include <linux/ems.h>
#include <trace/events/ems.h>

#include "ems.h"
#include "../sched.h"
#include "../tune.h"

/**********************************************************************
 *                            Prefer Perf                             *
 **********************************************************************/
/*
 * If the prefger_perf of the group to which the task belongs is set, the task
 * is assigned to the performance cpu preferentially.
 */
inline
int prefer_perf_cpu(struct eco_env *eenv)
{
	if (eenv->prefer_perf <= 0)
		return -1;

	return select_perf_cpu(eenv);
}

/**********************************************************************
 *                            Prefer Idle                             *
 **********************************************************************/
static bool mark_lowest_idle_util_cpu(int cpu, unsigned long new_util, unsigned long capacity_orig,
			int *lowest_idle_cpu, unsigned long *lowest_idle_util, int *lowest_idle_cstate)
{
	int idle_idx;

	if (!idle_cpu(cpu))
		return false;

	idle_idx = idle_get_state_idx(cpu_rq(cpu));

	/* find shallowest idle state cpu */
	if (idle_idx > *lowest_idle_cstate)
		return false;

	/* if same cstate, select lower util */
	if (idle_idx == *lowest_idle_cstate &&
		new_util >= *lowest_idle_util)
		return false;

	*lowest_idle_util = new_util;
	*lowest_idle_cstate = idle_idx;
	*lowest_idle_cpu = cpu;

	return true;
}

static bool mark_highest_spare_cpu(int cpu, unsigned long new_util, unsigned long capacity_orig,
			unsigned long *target_capacity, int *highest_spare_cpu, unsigned long *highest_spare_util, bool boosted)
{
	unsigned long spare_util;

	if (boosted &&
		capacity_orig < *target_capacity)
		return true;

	if (!boosted &&
		capacity_orig > *target_capacity)
		return true;

	if (capacity_curr_of(cpu) < new_util)
		return false;
	
	spare_util = capacity_orig - new_util;

	if (spare_util <= *highest_spare_util)
		return false;

	*highest_spare_util = spare_util;
	*highest_spare_cpu = cpu;
	*target_capacity = capacity_orig;
	
	return true;
}

static bool mark_lowest_util_cpu(int cpu, unsigned long wake_util, unsigned long new_util, unsigned long new_util_cuml,
			int *lowest_util_cpu, unsigned long *lowest_wake_util, unsigned long *lowest_util, unsigned long *lowest_util_cuml)
{
	if (wake_util > *lowest_wake_util)
		return false;

	if (new_util > *lowest_util)
		return false;

	/*
	 * If utilization is the same between CPUs,
	 * break the ties with cumulative demand,
	 * also prefer lower order cpu.
	 */
	if (new_util == *lowest_util &&
		new_util_cuml >= *lowest_util_cuml)
		return false;

	*lowest_util = new_util;
	*lowest_wake_util = wake_util;
	*lowest_util_cuml = new_util_cuml;
	*lowest_util_cpu = cpu;

	return true;
}

static int select_idle_cpu(struct eco_env *eenv)
{
	unsigned long lowest_idle_util = ULONG_MAX;
	unsigned long highest_spare_util = 0;
	unsigned long lowest_wake_util = ULONG_MAX;
	unsigned long lowest_util_cuml = ULONG_MAX;
	unsigned long lowest_util = ULONG_MAX;
	unsigned long target_capacity = ULONG_MAX;
	int lowest_idle_cstate = INT_MAX;
	int lowest_idle_cpu = -1;
	int highest_spare_cpu = -1;
	int lowest_util_cpu = -1;
	int target_cpu = -1;
	int cpu;
	int i;
	bool boosted = (eenv->boost > 0);
	char state[30] = "prev_cpu";

	if (boosted)
		target_capacity = 0;

	for_each_cpu(cpu, cpu_active_mask) {
		if (cpu != cpumask_first(cpu_coregroup_mask(cpu)))
			continue;

		for_each_cpu_and(i, tsk_cpus_allowed(eenv->p), cpu_coregroup_mask(cpu)) {
			unsigned long capacity_orig = capacity_orig_of(i);
			unsigned long new_util, wake_util, new_util_cuml;

			wake_util = cpu_util_without(i, eenv->p);
			new_util = wake_util + eenv->task_util;
			new_util = max(new_util, eenv->min_util);

			if (lbt_util_bring_overutilize(cpu, new_util))
				continue;
			
			new_util_cuml = cpu_util(cpu) + eenv->min_util;
			if (task_in_cum_window_demand(cpu_rq(cpu), eenv->p))
				new_util_cuml -= eenv->task_util;

			trace_ems_prefer_idle(eenv->p, eenv->prev_cpu, i, capacity_orig, eenv->task_util,
							new_util, idle_cpu(i));

			/* Priority #1 : idle cpu with lowest util */
			if (mark_lowest_idle_util_cpu(i, new_util, capacity_orig,
				&lowest_idle_cpu, &lowest_idle_util, &lowest_idle_cstate))
				continue;

			/* Priority #2 : active cpu with highest spare */
			if (mark_highest_spare_cpu(i, new_util, capacity_orig,
				&target_capacity, &highest_spare_cpu, &highest_spare_util, boosted))
				continue;

			/* Priority #3 : active cpu with lowest util */
			mark_lowest_util_cpu(i, wake_util, new_util, new_util_cuml,
				&lowest_util_cpu, &lowest_wake_util, &lowest_util, &lowest_util_cuml);
		}

		if (cpu_selected(lowest_idle_cpu)) {
			strcpy(state, "lowest_idle_util");
			target_cpu = lowest_idle_cpu;
			continue;
		}

		if (cpu_selected(highest_spare_cpu)) {
			strcpy(state, "highest_spare_util");
			target_cpu = highest_spare_cpu;
			continue;
		}

		if (cpu_selected(lowest_util_cpu)) {
			strcpy(state, "lowest_util");
			target_cpu = lowest_util_cpu;
		}
	}

	/* Return the previous CPU if CPU is not overutilized */
	if (!cpu_selected(target_cpu) && !lbt_util_overutilized(eenv->prev_cpu))
		target_cpu = eenv->prev_cpu;

	trace_ems_select_idle_cpu(eenv->p, target_cpu, state);

	return target_cpu;
}

inline
int prefer_idle_cpu(struct eco_env *eenv)
{
	if (eenv->prefer_idle <= 0)
		return -1;

	return select_idle_cpu(eenv);
}
