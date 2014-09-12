/*
 * Copyright (C) 2009 Philippe Gerum <rpm@xenomai.org>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#include <linux/types.h>
#include "internal.h"
#include "thread.h"
#include "sched.h"
#include "clock.h"
#include <trace/events/cobalt-posix.h>

struct xnsched_class *
cobalt_sched_policy_param(union xnsched_policy_param *param,
			  int u_policy, const struct sched_param_ex *param_ex,
			  xnticks_t *tslice_r)
{
	struct xnsched_class *sched_class;
	int prio, policy;
	xnticks_t tslice;

	prio = param_ex->sched_priority;
	tslice = XN_INFINITE;
	policy = u_policy;

	/*
	 * NOTE: The user-defined policy may be different than ours,
	 * e.g. SCHED_FIFO,prio=-7 from userland would be interpreted
	 * as SCHED_WEAK,prio=7 in kernel space.
	 */
	if (prio < 0) {
		prio = -prio;
		policy = SCHED_WEAK;
	}
	sched_class = &xnsched_class_rt;
	param->rt.prio = prio;

	switch (policy) {
	case SCHED_NORMAL:
		if (prio)
			return NULL;
		/*
		 * When the weak scheduling class is compiled in,
		 * SCHED_WEAK and SCHED_NORMAL threads are scheduled
		 * by xnsched_class_weak, at their respective priority
		 * levels. Otherwise, SCHED_NORMAL is scheduled by
		 * xnsched_class_rt at priority level #0.
		 */
	case SCHED_WEAK:
#ifdef CONFIG_XENO_OPT_SCHED_WEAK
		if (prio < XNSCHED_WEAK_MIN_PRIO ||
		    prio > XNSCHED_WEAK_MAX_PRIO)
			return NULL;
		param->weak.prio = prio;
		sched_class = &xnsched_class_weak;
#else
		if (prio)
			return NULL;
#endif
		break;
	case SCHED_RR:
		/* if unspecified, use current one. */
		tslice = ts2ns(&param_ex->sched_rr_quantum);
		if (tslice == XN_INFINITE && tslice_r)
			tslice = *tslice_r;
		/* falldown wanted */
	case SCHED_FIFO:
		if (prio < XNSCHED_FIFO_MIN_PRIO ||
		    prio > XNSCHED_FIFO_MAX_PRIO)
			return NULL;
		break;
	case SCHED_COBALT:
		if (prio < XNSCHED_CORE_MIN_PRIO ||
		    prio > XNSCHED_CORE_MAX_PRIO)
			return NULL;
		break;
#ifdef CONFIG_XENO_OPT_SCHED_SPORADIC
	case SCHED_SPORADIC:
		param->pss.normal_prio = param_ex->sched_priority;
		param->pss.low_prio = param_ex->sched_ss_low_priority;
		param->pss.current_prio = param->pss.normal_prio;
		param->pss.init_budget = ts2ns(&param_ex->sched_ss_init_budget);
		param->pss.repl_period = ts2ns(&param_ex->sched_ss_repl_period);
		param->pss.max_repl = param_ex->sched_ss_max_repl;
		sched_class = &xnsched_class_sporadic;
		break;
#endif
#ifdef CONFIG_XENO_OPT_SCHED_TP
	case SCHED_TP:
		param->tp.prio = param_ex->sched_priority;
		param->tp.ptid = param_ex->sched_tp_partition;
		sched_class = &xnsched_class_tp;
		break;
#endif
#ifdef CONFIG_XENO_OPT_SCHED_QUOTA
	case SCHED_QUOTA:
		param->quota.prio = param_ex->sched_priority;
		param->quota.tgid = param_ex->sched_quota_group;
		sched_class = &xnsched_class_quota;
		break;
#endif
	default:
		return NULL;
	}

	if (tslice_r)
		*tslice_r = tslice;

	return sched_class;
}

COBALT_SYSCALL(sched_minprio, current, int, (int policy))
{
	int ret;

	switch (policy) {
	case SCHED_FIFO:
	case SCHED_RR:
	case SCHED_SPORADIC:
	case SCHED_TP:
	case SCHED_QUOTA:
		ret = XNSCHED_FIFO_MIN_PRIO;
		break;
	case SCHED_COBALT:
		ret = XNSCHED_CORE_MIN_PRIO;
		break;
	case SCHED_NORMAL:
	case SCHED_WEAK:
		ret = 0;
		break;
	default:
		ret = -EINVAL;
	}

	trace_cobalt_sched_min_prio(policy, ret);

	return ret;
}

COBALT_SYSCALL(sched_maxprio, current, int, (int policy))
{
	int ret;

	switch (policy) {
	case SCHED_FIFO:
	case SCHED_RR:
	case SCHED_SPORADIC:
	case SCHED_TP:
	case SCHED_QUOTA:
		ret = XNSCHED_FIFO_MAX_PRIO;
		break;
	case SCHED_COBALT:
		ret = XNSCHED_CORE_MAX_PRIO;
		break;
	case SCHED_NORMAL:
		ret = 0;
		break;
	case SCHED_WEAK:
#ifdef CONFIG_XENO_OPT_SCHED_WEAK
		ret = XNSCHED_FIFO_MAX_PRIO;
#else
		ret = 0;
#endif
		break;
	default:
		ret = -EINVAL;
	}

	trace_cobalt_sched_max_prio(policy, ret);

	return ret;
}

COBALT_SYSCALL(sched_yield, primary, int, (void))
{
	struct cobalt_thread *curr = cobalt_current_thread();
	int ret = 0;

	trace_cobalt_pthread_yield(0);

	/* Maybe some extension wants to handle this. */
  	if (cobalt_call_extension(sched_yield, &curr->extref, ret) && ret)
		return ret > 0 ? 0 : ret;

	xnthread_resume(&curr->threadbase, 0);
	if (xnsched_run())
		return 0;

	/*
	 * If the round-robin move did not beget any context switch to
	 * a thread running in primary mode, then wait for the next
	 * linux context switch to happen.
	 *
	 * Rationale: it is most probably unexpected that
	 * sched_yield() does not cause any context switch, since this
	 * service is commonly used for implementing a poor man's
	 * cooperative scheduling. By waiting for a context switch to
	 * happen in the regular kernel, we guarantee that the CPU has
	 * been relinquished for a while.
	 *
	 * Typically, this behavior allows a thread running in primary
	 * mode to effectively yield the CPU to a thread of
	 * same/higher priority stuck in secondary mode.
	 *
	 * NOTE: calling cobalt_yield() with no timeout
	 * (i.e. XN_INFINITE) is probably never a good idea. This
	 * means that a SCHED_FIFO non-rt thread stuck in a tight loop
	 * would prevent the caller from waking up, since no
	 * linux-originated schedule event would happen for unblocking
	 * it on the current CPU. For this reason, we pass the
	 * arbitrary TICK_NSEC value to limit the wait time to a
	 * reasonable amount.
	 */
	return cobalt_yield(TICK_NSEC, TICK_NSEC);
}

#ifdef CONFIG_XENO_OPT_SCHED_TP

static inline
int set_tp_config(int cpu, union sched_config *config, size_t len)
{
	xnticks_t offset, duration, next_offset;
	struct xnsched_tp_schedule *gps, *ogps;
	struct xnsched_tp_window *w;
	struct sched_tp_window *p;
	struct xnsched *sched;
	spl_t s;
	int n;

	if (len < sizeof(config->tp))
		return -EINVAL;

	if (config->tp.nr_windows == 0) {
		gps = NULL;
		goto set_schedule;
	}

	gps = xnmalloc(sizeof(*gps) + config->tp.nr_windows * sizeof(*w));
	if (gps == NULL)
		goto fail;

	for (n = 0, p = config->tp.windows, w = gps->pwins, next_offset = 0;
	     n < config->tp.nr_windows; n++, p++, w++) {
		/*
		 * Time windows must be strictly contiguous. Holes may
		 * be defined using windows assigned to the pseudo
		 * partition #-1.
		 */
		offset = ts2ns(&p->offset);
		if (offset != next_offset)
			goto cleanup_and_fail;

		duration = ts2ns(&p->duration);
		if (duration <= 0)
			goto cleanup_and_fail;

		if (p->ptid < -1 ||
		    p->ptid >= CONFIG_XENO_OPT_SCHED_TP_NRPART)
			goto cleanup_and_fail;

		w->w_offset = next_offset;
		w->w_part = p->ptid;
		next_offset += duration;
	}

	atomic_set(&gps->refcount, 1);
	gps->pwin_nr = n;
	gps->tf_duration = next_offset;
set_schedule:
	sched = xnsched_struct(cpu);
	xnlock_get_irqsave(&nklock, s);
	ogps = xnsched_tp_set_schedule(sched, gps);
	xnsched_tp_start_schedule(sched);
	xnlock_put_irqrestore(&nklock, s);

	if (ogps)
		xnsched_tp_put_schedule(ogps);

	return 0;

cleanup_and_fail:
	xnfree(gps);
fail:
	return -EINVAL;
}

static inline
ssize_t get_tp_config(int cpu, void __user *u_config, size_t len,
		      union sched_config *(*fetch_config)
		      (int policy, const void __user *u_config,
		       size_t *len),
		      ssize_t (*put_config)(int policy, void __user *u_config,
					    size_t u_len,
					    const union sched_config *config,
					    size_t len))
{
	struct xnsched_tp_window *pw, *w;
	struct xnsched_tp_schedule *gps;
	struct sched_tp_window *pp, *p;
	union sched_config *config;
	struct xnsched *sched;
	ssize_t ret, elen;
	spl_t s;
	int n;

	xnlock_get_irqsave(&nklock, s);

	sched = xnsched_struct(cpu);
	gps = xnsched_tp_get_schedule(sched);
	if (gps == NULL) {
		xnlock_put_irqrestore(&nklock, s);
		return 0;
	}

	xnlock_put_irqrestore(&nklock, s);

	elen = sched_tp_confsz(gps->pwin_nr);
	config = xnmalloc(elen);
	if (config == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	config->tp.nr_windows = gps->pwin_nr;
	for (n = 0, pp = p = config->tp.windows, pw = w = gps->pwins;
	     n < gps->pwin_nr; pp = p, p++, pw = w, w++, n++) {
		ns2ts(&p->offset, w->w_offset);
		ns2ts(&pp->duration, w->w_offset - pw->w_offset);
		p->ptid = w->w_part;
	}
	ns2ts(&pp->duration, gps->tf_duration - pw->w_offset);
	ret = put_config(SCHED_TP, u_config, len, config, elen);
	xnfree(config);
out:
	xnsched_tp_put_schedule(gps);

	return ret;
}

#else /* !CONFIG_XENO_OPT_SCHED_TP */

static inline int
set_tp_config(int cpu, union sched_config *config, size_t len)
{
	return -EINVAL;
}

static inline ssize_t
get_tp_config(int cpu, union sched_config __user *u_config, size_t len,
	      union sched_config *(*fetch_config)
	      (int policy, const void __user *u_config,
	       size_t *len),
	      ssize_t (*put_config)(int policy, void __user *u_config,
				    size_t u_len,
				    const union sched_config *config,
				    size_t len))
{
	return -EINVAL;
}

#endif /* !CONFIG_XENO_OPT_SCHED_TP */

#ifdef CONFIG_XENO_OPT_SCHED_QUOTA

static inline
int set_quota_config(int cpu, union sched_config *config, size_t len)
{
	struct __sched_config_quota *p = &config->quota;
	struct __sched_quota_info *iq = &p->info;
	struct cobalt_sched_group *group;
	struct xnsched_quota_group *tg;
	struct xnsched *sched;
	int ret, quota_sum;
	spl_t s;

	if (len < sizeof(*p))
		return -EINVAL;

	switch (p->op) {
	case sched_quota_add:
		group = xnmalloc(sizeof(*group));
		if (group == NULL)
			return -ENOMEM;
		tg = &group->quota;
		group->pshared = p->add.pshared != 0;
		group->kq = cobalt_kqueues(group->pshared);
		xnlock_get_irqsave(&nklock, s);
		sched = xnsched_struct(cpu);
		ret = xnsched_quota_create_group(tg, sched, &quota_sum);
		if (ret) {
			xnlock_put_irqrestore(&nklock, s);
			xnfree(group);
			return ret;
		}
		list_add(&group->next, &group->kq->schedq);
		xnlock_put_irqrestore(&nklock, s);
		break;
	case sched_quota_remove:
	case sched_quota_force_remove:
		xnlock_get_irqsave(&nklock, s);
		sched = xnsched_struct(cpu);
		tg = xnsched_quota_find_group(sched, p->remove.tgid);
		if (tg == NULL)
			goto bad_tgid;
		group = container_of(tg, struct cobalt_sched_group, quota);
		if (group->kq != cobalt_kqueues(group->pshared))
			goto bad_tgid;
		ret = xnsched_quota_destroy_group(tg,
						  p->op == sched_quota_force_remove,
						  &quota_sum);
		if (ret) {
			xnlock_put_irqrestore(&nklock, s);
			return ret;
		}
		list_del(&group->next);
		xnlock_put_irqrestore(&nklock, s);
		xnfree(group);
		break;
	case sched_quota_set:
		xnlock_get_irqsave(&nklock, s);
		sched = xnsched_struct(cpu);
		tg = xnsched_quota_find_group(sched, p->set.tgid);
		if (tg == NULL)
			goto bad_tgid;
		group = container_of(tg, struct cobalt_sched_group, quota);
		if (group->kq != cobalt_kqueues(group->pshared))
			goto bad_tgid;
		xnsched_quota_set_limit(tg, p->set.quota, p->set.quota_peak,
					&quota_sum);
		xnlock_put_irqrestore(&nklock, s);
		break;
	default:
		return -EINVAL;
	}

	iq->tgid = tg->tgid;
	iq->quota = tg->quota_percent;
	iq->quota_peak = tg->quota_peak_percent;
	iq->quota_sum = quota_sum;

	return 0;
bad_tgid:
	xnlock_put_irqrestore(&nklock, s);

	return -ESRCH;
}

static inline
ssize_t get_quota_config(int cpu, void __user *u_config, size_t len,
			 union sched_config *(*fetch_config)
			 (int policy, const void __user *u_config,
			  size_t *len),
			 ssize_t (*put_config)(int policy, void __user *u_config,
					       size_t u_len,
					       const union sched_config *config,
					       size_t len))
{
	struct cobalt_sched_group *group;
	struct xnsched_quota_group *tg;
	union sched_config *config;
	struct xnsched *sched;
	ssize_t ret;
	spl_t s;

	config = fetch_config(SCHED_QUOTA, u_config, &len);
	if (IS_ERR(config))
		return PTR_ERR(config);

	xnlock_get_irqsave(&nklock, s);
	sched = xnsched_struct(cpu);
	tg = xnsched_quota_find_group(sched, config->quota.get.tgid);
	if (tg == NULL)
		goto bad_tgid;

	group = container_of(tg, struct cobalt_sched_group, quota);
	if (group->kq != cobalt_kqueues(group->pshared))
		goto bad_tgid;

	config->quota.info.tgid = tg->tgid;
	config->quota.info.quota = tg->quota_percent;
	config->quota.info.quota_peak = tg->quota_peak_percent;
	config->quota.info.quota_sum = xnsched_quota_sum_all(sched);
	xnlock_put_irqrestore(&nklock, s);

	ret = put_config(SCHED_QUOTA, u_config, len, config, sizeof(*config));
	xnfree(config);

	return ret;
bad_tgid:
	xnlock_put_irqrestore(&nklock, s);
	xnfree(config);

	return -ESRCH;
}

#else /* !CONFIG_XENO_OPT_SCHED_QUOTA */

static inline
int set_quota_config(int cpu, union sched_config *config, size_t len)
{
	return -EINVAL;
}

static inline
ssize_t get_quota_config(int cpu, void __user *u_config,
			 size_t len,
			 union sched_config *(*fetch_config)
			 (int policy, const void __user *u_config,
			  size_t *len),
			 ssize_t (*put_config)(int policy, void __user *u_config,
					       size_t u_len,
					       const union sched_config *config,
					       size_t len))
{
	return -EINVAL;
}

#endif /* !CONFIG_XENO_OPT_SCHED_QUOTA */

static union sched_config *
sched_fetch_config(int policy, const void __user *u_config, size_t *len)
{
	union sched_config *buf;
	int ret;

	if (u_config == NULL)
		return ERR_PTR(-EFAULT);

	if (policy == SCHED_QUOTA && *len < sizeof(buf->quota))
		return ERR_PTR(-EINVAL);

	buf = xnmalloc(*len);
	if (buf == NULL)
		return ERR_PTR(-ENOMEM);

	ret = __xn_safe_copy_from_user(buf, u_config, *len);
	if (ret) {
		xnfree(buf);
		return ERR_PTR(ret);
	}

	return buf;
}

static int sched_ack_config(int policy, const union sched_config *config,
			    void __user *u_config)
{
	union sched_config __user *u_p = u_config;

	if (policy != SCHED_QUOTA)
		return 0;

	return u_p == NULL ? -EFAULT :
		__xn_safe_copy_to_user(&u_p->quota.info, &config->quota.info,
				       sizeof(u_p->quota.info));
}

static ssize_t sched_put_config(int policy,
				void __user *u_config, size_t u_len,
				const union sched_config *config, size_t len)
{
	union sched_config *u_p = u_config;

	if (u_config == NULL)
		return -EFAULT;

	if (policy == SCHED_QUOTA) {
		if (u_len < sizeof(config->quota))
			return -EINVAL;
		return __xn_safe_copy_to_user(&u_p->quota.info, &config->quota.info,
					      sizeof(u_p->quota.info)) ?:
			sizeof(u_p->quota.info);
	}

	return __xn_safe_copy_to_user(u_config, config, len) ?: len;
}

int __cobalt_sched_setconfig_np(int cpu, int policy,
				void __user *u_config,
				size_t len,
				union sched_config *(*fetch_config)
				(int policy, const void __user *u_config,
				 size_t *len),
				int (*ack_config)(int policy,
						  const union sched_config *config,
						  void __user *u_config))
{
	union sched_config *buf;
	int ret;

	trace_cobalt_sched_setconfig(cpu, policy, len);

	if (cpu < 0 || cpu >= NR_CPUS || !cpu_online(cpu))
		return -EINVAL;

	if (len == 0)
		return -EINVAL;

	buf = fetch_config(policy, u_config, &len);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	switch (policy)	{
	case SCHED_TP:
		ret = set_tp_config(cpu, buf, len);
		break;
	case SCHED_QUOTA:
		ret = set_quota_config(cpu, buf, len);
		break;
	default:
		ret = -EINVAL;
	}

	if (ret == 0)
		ret = ack_config(policy, buf, u_config);

	xnfree(buf);

	return ret;
}

COBALT_SYSCALL(sched_setconfig_np, current,
	       int, (int cpu, int policy,
		     union sched_config __user *u_config,
		     size_t len))
{
	return __cobalt_sched_setconfig_np(cpu, policy, u_config, len,
					   sched_fetch_config, sched_ack_config);
}

ssize_t __cobalt_sched_getconfig_np(int cpu, int policy,
				    void __user *u_config,
				    size_t len,
				    union sched_config *(*fetch_config)
				    (int policy, const void __user *u_config,
				     size_t *len),
				    ssize_t (*put_config)(int policy,
							  void __user *u_config,
							  size_t u_len,
							  const union sched_config *config,
							  size_t len))
{
	ssize_t ret;

	switch (policy)	{
	case SCHED_TP:
		ret = get_tp_config(cpu, u_config, len,
				    fetch_config, put_config);
		break;
	case SCHED_QUOTA:
		ret = get_quota_config(cpu, u_config, len,
				       fetch_config, put_config);
		break;
	default:
		ret = -EINVAL;
	}

	trace_cobalt_sched_get_config(cpu, policy, ret);

	return ret;
}

COBALT_SYSCALL(sched_getconfig_np, current,
	       ssize_t, (int cpu, int policy,
			 union sched_config __user *u_config,
			 size_t len))
{
	return __cobalt_sched_getconfig_np(cpu, policy, u_config, len,
					   sched_fetch_config, sched_put_config);
}

int __cobalt_sched_weightprio(int policy,
			      const struct sched_param_ex *param_ex)
{
	struct xnsched_class *sched_class;
	union xnsched_policy_param param;
	int prio;

	sched_class = cobalt_sched_policy_param(&param, policy,
						param_ex, NULL);
	if (sched_class == NULL)
		return -EINVAL;

	prio = param_ex->sched_priority;
	if (prio < 0)
		prio = -prio;

	return prio + sched_class->weight;
}

COBALT_SYSCALL(sched_weightprio, current,
	       int, (int policy,
		     const struct sched_param_ex __user *u_param))
{
	struct sched_param_ex param_ex;

	if (__xn_safe_copy_from_user(&param_ex, u_param, sizeof(param_ex)))
		return -EFAULT;

	return __cobalt_sched_weightprio(policy, &param_ex);
}

void cobalt_sched_cleanup(struct cobalt_kqueues *q)
{
	struct cobalt_sched_group *group;
#ifdef CONFIG_XENO_OPT_SCHED_QUOTA
	int quota_sum;
#endif
	spl_t s;

	xnlock_get_irqsave(&nklock, s);

	for (;;) {
		if (list_empty(&q->schedq))
			break;

		group = list_get_entry(&q->schedq, struct cobalt_sched_group, next);
#ifdef CONFIG_XENO_OPT_SCHED_QUOTA
		xnsched_quota_destroy_group(&group->quota, 1, &quota_sum);
#endif
		xnlock_put_irqrestore(&nklock, s);
		xnfree(group);
		xnlock_get_irqsave(&nklock, s);
	}

	xnlock_put_irqrestore(&nklock, s);
}

void cobalt_sched_pkg_init(void)
{
	INIT_LIST_HEAD(&cobalt_global_kqueues.schedq);
}

void cobalt_sched_pkg_cleanup(void)
{
	cobalt_sched_cleanup(&cobalt_global_kqueues);
}