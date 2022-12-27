/*
 * Core Exynos Mobile Scheduler
 *
 * Copyright (C) 2018 Samsung Electronics Co., Ltd
 * Park Bumgyu <bumgyu.park@samsung.com>
 */

#include <linux/ems.h>
#include <linux/freezer.h>

#define CREATE_TRACE_POINTS
#include <trace/events/ems.h>

#include "ems.h"
#include "../sched.h"
#include "../tune.h"

/*
 * When a task is dequeued, its estimated utilization should not be update if
 * its util_avg has not been updated at least once.
 * This flag is used to synchronize util_avg updates with util_est updates.
 * We map this information into the LSB bit of the utilization saved at
 * dequeue time (i.e. util_est.dequeued).
 */
#define UTIL_AVG_UNCHANGED 0x1

static
inline unsigned long _task_util_est(struct task_struct *p)
{
	struct util_est ue = READ_ONCE(p->se.avg.util_est);

	return max(ue.ewma, ue.enqueued);
}

unsigned long cpu_util_without(int cpu, struct task_struct *p)
{
	struct cfs_rq *cfs_rq;
	unsigned int util;

	/* Task has no contribution or is new */
	if (cpu != task_cpu(p) || !READ_ONCE(p->se.avg.last_update_time))
		return cpu_util(cpu);

	cfs_rq = &cpu_rq(cpu)->cfs;
	util = READ_ONCE(cfs_rq->avg.util_avg);

	/* Discount task's blocked util from CPU's util */
	util -= min_t(unsigned int, util, task_util_est(p));

	/*
	 * Covered cases:
	 *
	 * a) if *p is the only task sleeping on this CPU, then:
	 *      cpu_util (== task_util) > util_est (== 0)
	 *    and thus we return:
	 *      cpu_util_wake = (cpu_util - task_util) = 0
	 *
	 * b) if other tasks are SLEEPING on this CPU, which is now exiting
	 *    IDLE, then:
	 *      cpu_util >= task_util
	 *      cpu_util > util_est (== 0)
	 *    and thus we discount *p's blocked utilization to return:
	 *      cpu_util_wake = (cpu_util - task_util) >= 0
	 *
	 * c) if other tasks are RUNNABLE on that CPU and
	 *      util_est > cpu_util
	 *    then we use util_est since it returns a more restrictive
	 *    estimation of the spare capacity on that CPU, by just
	 *    considering the expected utilization of tasks already
	 *    runnable on that CPU.
	 *
	 * Cases a) and b) are covered by the above code, while case c) is
	 * covered by the following code when estimated utilization is
	 * enabled.
	 */
	if (sched_feat(UTIL_EST)) {
		unsigned int estimated =
			READ_ONCE(cfs_rq->avg.util_est.enqueued);

		/*
		 * Despite the following checks we still have a small window
		 * for a possible race, when an execl's select_task_rq_fair()
		 * races with LB's detach_task():
		 *
		 *   detach_task()
		 *     p->on_rq = TASK_ON_RQ_MIGRATING;
		 *     ---------------------------------- A
		 *     deactivate_task()                   \
		 *       dequeue_task()                     + RaceTime
		 *         util_est_dequeue()              /
		 *     ---------------------------------- B
		 *
		 * The additional check on "current == p" it's required to
		 * properly fix the execl regression and it helps in further
		 * reducing the chances for the above race.
		 */
		if (unlikely(task_on_rq_queued(p) || current == p)) {
			estimated -= min_t(unsigned int, estimated,
					   (_task_util_est(p) | UTIL_AVG_UNCHANGED));
		}
		util = max(util, estimated);
	}

	/*
	 * Utilization (estimated) can exceed the CPU capacity, thus let's
	 * clamp to the maximum CPU capacity to ensure consistency with
	 * the cpu_util call.
	 */
	return min_t(unsigned long, util, capacity_orig_of(cpu));
}

static inline int
check_cpu_capacity(struct rq *rq, struct sched_domain *sd)
{
	return ((rq->cpu_capacity * sd->imbalance_pct) <
				(rq->cpu_capacity_orig * 100));
}

#define lb_sd_parent(sd) \
	(sd->parent && sd->parent->groups != sd->parent->groups->next)

int exynos_need_active_balance(enum cpu_idle_type idle, struct sched_domain *sd,
					int src_cpu, int dst_cpu)
{
	unsigned int src_imb_pct = lb_sd_parent(sd) ? sd->imbalance_pct : 1;
	unsigned int dst_imb_pct = lb_sd_parent(sd) ? 100 : 1;
	unsigned long src_cap = capacity_of(src_cpu);
	unsigned long dst_cap = capacity_of(dst_cpu);
	int level = sd->level;

	/* dst_cpu is idle */
	if ((idle != CPU_NOT_IDLE) &&
	    (cpu_rq(src_cpu)->cfs.h_nr_running == 1)) {
		if ((check_cpu_capacity(cpu_rq(src_cpu), sd)) &&
		    (src_cap * sd->imbalance_pct < dst_cap * 100)) {
			return 1;
		}

		/* This domain is top and dst_cpu is bigger than src_cpu*/
		if (!lb_sd_parent(sd) && src_cap < dst_cap)
			if (lbt_overutilized(src_cpu, level) || global_boosted())
				return 1;
	}

	if ((src_cap * src_imb_pct < dst_cap * dst_imb_pct) &&
			cpu_rq(src_cpu)->cfs.h_nr_running == 1 &&
			lbt_overutilized(src_cpu, level) &&
			!lbt_overutilized(dst_cpu, level)) {
		return 1;
	}

	return unlikely(sd->nr_balance_failed > sd->cache_nice_tries + 2);
}

extern int wake_cap(struct task_struct *p, int cpu, int prev_cpu);
bool is_cpu_preemptible(struct task_struct *p, int prev_cpu, int cpu, int sync)
{
	struct rq *rq = cpu_rq(cpu);
#ifdef CONFIG_SCHED_TUNE
	struct task_struct *curr = READ_ONCE(rq->curr);

	/* allow sync wakeups for boosted tasks */
	if (sync && schedtune_task_boost(p) > 0)
		return true;

	if (is_slowest_cpu(cpu) || !curr)
		goto skip_ux;

	/* Allow preemption if not top-app */
	if (!schedtune_task_top_app(curr))
		goto skip_ux;

	/* Always avoid preempting the app in front of user */
	if (p != curr && schedtune_task_on_top(curr))
		return false;

	/* Check if 'curr' is a high-cap top-app task */
	if (schedtune_prefer_high_cap(curr) > 0)
		return false;

skip_ux:
#endif

	if (sync && (rq->nr_running != 1 || wake_cap(p, cpu, prev_cpu)))
		return false;

	return true;
}

static int select_proper_cpu(struct eco_env *eenv)
{
	int cpu;
	unsigned long best_active_util = ULONG_MAX;
	unsigned long best_idle_util = ULONG_MAX;
	unsigned long target_capacity = ULONG_MAX;
	int best_idle_cstate = INT_MAX;

	int best_active_cpu = -1;
	int best_idle_cpu = -1;
	int best_cpu = -1;

	bool prefer_idle = (eenv->prefer_idle > 0);

	for_each_cpu(cpu, cpu_active_mask) {
		int i;

		/* visit each coregroup only once */
		if (cpu != cpumask_first(cpu_coregroup_mask(cpu)))
			continue;

		/* skip if task cannot be assigned to coregroup */
		if (!cpumask_intersects(tsk_cpus_allowed(eenv->p), cpu_coregroup_mask(cpu)))
			continue;

		for_each_cpu_and(i, tsk_cpus_allowed(eenv->p), cpu_coregroup_mask(cpu)) {
			unsigned long cpu_capacity = get_cpu_max_capacity(cpu);
			unsigned long wake_util, new_util;

			/*
			 * Skip processing placement further if we are visiting
			 * cpus with lower capacity than start cpu
			 */
			if (!pm_freezing && (cpu_capacity < eenv->start_cpu_cap))
				continue;

			wake_util = cpu_util_without(i, eenv->p);
			new_util = wake_util + eenv->task_util;
			new_util = max(new_util, eenv->min_util);

			/* skip over-capacity cpu */
			if (new_util > capacity_orig_of(i))
				continue;

			if (idle_cpu(i)) {
				int idle_idx = idle_get_state_idx(cpu_rq(i));

				/* find shallowest idle state cpu */
				if (cpu_capacity == target_capacity &&
				    idle_idx > best_idle_cstate)
					continue;

				/* if same cstate, select lower util */
				if (idle_idx == best_idle_cstate &&
				    cpu_capacity == target_capacity &&
				    (best_idle_cpu == eenv->prev_cpu ||
				    (i != eenv->prev_cpu &&
				    new_util >= best_idle_util)))
					continue;

				/* Keep track of best idle CPU */
				target_capacity = cpu_capacity;
				best_idle_cstate = idle_idx;
				best_idle_util = new_util;
				best_idle_cpu = i;
				continue;
			}

			/*
			 * Best target) lowest utilization among lowest-cap cpu
			 *
			 * If the sequence reaches this function, the wakeup task
			 * does not require performance and the prev cpu is over-
			 * utilized, so it should do load balancing without
			 * considering energy side. Therefore, it selects cpu
			 * with smallest cpapacity or highest spare capacity
			 * and the least utilization among cpus that fits the task.
			 */
			if (new_util > best_active_util)
				continue;

			target_capacity = cpu_capacity;
			best_active_util = new_util;
			best_active_cpu = i;
		}

		/*
		 * if it fails to find the best cpu in this coregroup, visit next
		 * coregroup.
		 */
		if (cpu_selected(best_active_cpu))
			best_cpu = best_active_cpu;

		if (cpu_selected(best_idle_cpu)) {
			if (prefer_idle || !cpu_selected(best_cpu) ||
				(!is_slowest_cpu(best_active_cpu) && !is_cpu_preemptible(eenv->p, -1, best_active_cpu, 0))) {
				best_cpu = best_idle_cpu;
			}
		}

		if (cpu_selected(best_cpu))
			break;
	}

	trace_ems_select_proper_cpu(eenv->p, best_cpu,
		best_cpu == best_idle_cpu ? best_idle_util : best_active_util);

	/*
	 * if it fails to find the vest cpu, choosing any cpu is meaningless.
	 * Return prev cpu.
	 */
	return cpu_selected(best_cpu) ? best_cpu : eenv->prev_cpu;
}

static
int start_cpu(struct task_struct *p, unsigned long task_util, int prefer_perf) {
	struct cpumask active_fast_mask;
	int start_cpu = cpumask_first_and(cpu_slowest_mask(), cpu_active_mask);

	// Avoid recommending fast CPUs during idle as these are inactive
	if (pm_freezing)
		return start_cpu;

	/* Get all active fast CPUs */
	cpumask_and(&active_fast_mask, cpu_fastest_mask(), cpu_active_mask);

	/* Start with fast CPU if available, task is allowed to be placed, and matches criteria */
	if (!cpumask_empty(&active_fast_mask) && cpumask_intersects(tsk_cpus_allowed(p), &active_fast_mask)) {
		/* Return fast CPU if task is prefer_perf or global boosting */
		if (prefer_perf || global_boosted())
			return cpumask_first(&active_fast_mask);

		/* 
		 * Check if task can be placed on big cluster and
		 * fits slowest CPU. Return fast if overutil.
		 */
		if ((task_util * 100 >= get_cpu_max_capacity(start_cpu) * 61))
			return cpumask_first(&active_fast_mask);
	}

	/* If task does not match criteria, return slowest CPU */
	return start_cpu;
}

int exynos_wakeup_balance(struct task_struct *p, int prev_cpu, int sd_flag, int sync)
{
	int target_cpu = -1;
	char state[30] = "fail";

	unsigned long sched_task_util = task_util_est(p);
	int sched_prefer_perf = schedtune_prefer_perf(p);
	int sched_start_cpu = start_cpu(p, sched_task_util, sched_prefer_perf);

	struct eco_env eenv = {
		.p = p,
		.task_util = sched_task_util,
		.min_util = boosted_task_util(p),

		.boost = schedtune_task_boost(p),
		.task_on_top = schedtune_task_on_top(p),
		.prefer_idle = schedtune_prefer_idle(p),
		.prefer_perf = sched_prefer_perf,
		.prefer_high_cap = schedtune_prefer_high_cap(p),

		.start_cpu = sched_start_cpu,
		.start_cpu_cap = get_cpu_max_capacity(sched_start_cpu),

		.prev_cpu = prev_cpu,
	};

	/* 
	 * Priority 1: fast prev_cpu path
	 *
	 * Do not migrate task if prev_cpu is shallow idle, and has same capacity
	 * as start_cpu. This is the highest priority to avoid scheduling from the
	 * slow path if not needed.
	 *
	 */
	if (cpu_active(prev_cpu) && idle_cpu(prev_cpu) && cpumask_test_cpu(prev_cpu, tsk_cpus_allowed(p))) {
		if ((eenv.start_cpu_cap == get_cpu_max_capacity(prev_cpu)) && !lbt_util_overutilized(prev_cpu)) {
			if (idle_get_state_idx(cpu_rq(prev_cpu)) <= 1) {
				target_cpu = prev_cpu;
				strcpy(state, "fast path");
				goto out;
			}
		}
	}

	/*
	 * Priority 2 : service task
	 *
	 * Service selection is a function that operates on cgroup basis managed by
	 * schedtune. When perfer-high-cap is set to 1, the tasks in the group are
	 * placed onto big cluster cpu.
	 *
	 * It has a high priority because it is a function that is turned on
	 * temporarily in scenario requiring reactivity(touch, app laucning).
	 */
	target_cpu = select_service_cpu(&eenv);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "service");
		goto out;
	}

	/*
	 * Priority 3 : ontime task
	 *
	 * If task which has more utilization than threshold wakes up, the task is
	 * classified as "ontime task" and assigned to performance cpu. Conversely,
	 * if heavy task that has been classified as ontime task sleeps for a long
	 * time and utilization becomes small, it is excluded from ontime task and
	 * is no longer guaranteed to operate on performance cpu.
	 *
	 * Ontime task is very sensitive to performance because it is usually the
	 * main task of application. Therefore, it has the highest priority.
	 */
	target_cpu = ontime_task_wakeup(p, sync);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "ontime migration");
		goto out;
	}

	/*
	 * Priority 4 : prefer-perf
	 *
	 * Prefer-perf is a function that operates on cgroup basis managed by
	 * schedtune. When perfer-perf is set to 1, the tasks in the group are
	 * preferentially assigned to the performance cpu.
	 */
	target_cpu = prefer_perf_cpu(&eenv);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "prefer-perf");
		goto out;
	}

	/*
	 * Priority 5 : global boosting
	 *
	 * Global boost is a function that preferentially assigns all tasks in the
	 * system to the performance cpu. Unlike prefer-perf, which targets only
	 * group tasks, global boost targets all tasks. So, it maximizes performance
	 * cpu utilization.
	 *
	 * Typically, prefer-perf operates on groups that contains UX related tasks,
	 * such as "top-app" or "foreground", so that major tasks are likely to be
	 * assigned to performance cpu. On the other hand, global boost assigns
	 * all tasks to performance cpu, which is not as effective as perfer-perf.
	 * For this reason, global boost has a lower priority than prefer-perf.
	 */
	target_cpu = global_boosting(&eenv);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "global boosting");
		goto out;
	}

	/*
	 * Priority 6 : prefer-idle
	 *
	 * Prefer-idle is a function that operates on cgroup basis managed by
	 * schedtune. When perfer-idle is set to 1, the tasks in the group are
	 * preferentially assigned to the idle cpu.
	 *
	 * Prefer-idle has a smaller performance impact than the above. Therefore
	 * it has a relatively low priority.
	 */
	target_cpu = prefer_idle_cpu(&eenv);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "prefer-idle");
		goto out;
	}

	/*
	 * Priority 7 : energy cpu
	 *
	 * A scheduling scheme based on cpu energy, find the least power consumption
	 * cpu with energy table when assigning task.
	 */
	target_cpu = select_energy_cpu(&eenv, sd_flag, sync);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "energy cpu");
		goto out;
	}

	/*
	 * Priority 8 : proper cpu
	 *
	 * If the task failed to find a cpu to assign from the above conditions,
	 * it means that assigning task to any cpu does not have performance and
	 * power benefit. In this case, select cpu for balancing cpu utilization.
	 */
	target_cpu = select_proper_cpu(&eenv);
	if (cpu_selected(target_cpu))
		strcpy(state, "proper cpu");

out:
	trace_ems_wakeup_balance(p, target_cpu, state);
	return target_cpu;
}

struct kobject *ems_kobj;

static int __init init_sysfs(void)
{
	ems_kobj = kobject_create_and_add("ems", kernel_kobj);
	if (!ems_kobj)
		return -ENOMEM;

	return 0;
}
core_initcall(init_sysfs);
