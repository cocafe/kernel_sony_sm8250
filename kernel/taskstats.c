/*
 * taskstats.c - Export per-task statistics to userland
 *
 * Copyright (C) Shailabh Nagar, IBM Corp. 2006
 *           (C) Balbir Singh,   IBM Corp. 2006
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/taskstats_kern.h>
#include <linux/tsacct_kern.h>
#include <linux/delayacct.h>
#include <linux/cpumask.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/vmalloc.h>
#include <linux/cgroupstats.h>
#include <linux/sysstats.h>
#include <linux/cgroup.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/pid_namespace.h>
#include <net/genetlink.h>
#include <linux/atomic.h>
#include <linux/sched/cputime.h>
#include <linux/oom.h>

/*
 * Maximum length of a cpumask that can be specified in
 * the TASKSTATS_CMD_ATTR_REGISTER/DEREGISTER_CPUMASK attribute
 */
#define TASKSTATS_CPUMASK_MAXLEN	(100+6*NR_CPUS)

static DEFINE_PER_CPU(__u32, taskstats_seqnum);
static int family_registered;
struct kmem_cache *taskstats_cache;

static struct genl_family family;

static const struct nla_policy taskstats_cmd_get_policy[TASKSTATS_CMD_ATTR_MAX+1] = {
	[TASKSTATS_CMD_ATTR_PID]  = { .type = NLA_U32 },
	[TASKSTATS_CMD_ATTR_TGID] = { .type = NLA_U32 },
	[TASKSTATS_CMD_ATTR_REGISTER_CPUMASK] = { .type = NLA_STRING },
	[TASKSTATS_CMD_ATTR_DEREGISTER_CPUMASK] = { .type = NLA_STRING },
	[TASKSTATS_CMD_ATTR_FOREACH] = { .type = NLA_U32 },};

/*
 * We have to use TASKSTATS_CMD_ATTR_MAX here, it is the maxattr in the family.
 * Make sure they are always aligned.
 */
static const struct nla_policy cgroupstats_cmd_get_policy[TASKSTATS_CMD_ATTR_MAX+1] = {
	[CGROUPSTATS_CMD_ATTR_FD] = { .type = NLA_U32 },
};

static const struct nla_policy
		sysstats_cmd_get_policy[TASKSTATS_CMD_ATTR_MAX+1] = {
	[SYSSTATS_CMD_ATTR_SYSMEM_STATS] = { .type = NLA_U32 },
};

struct listener {
	struct list_head list;
	pid_t pid;
	char valid;
};

struct listener_list {
	struct rw_semaphore sem;
	struct list_head list;
};
static DEFINE_PER_CPU(struct listener_list, listener_array);

struct tgid_iter {
	unsigned int tgid;
	struct task_struct *task;
};

enum actions {
	REGISTER,
	DEREGISTER,
	CPU_DONT_CARE
};

static int prepare_reply(struct genl_info *info, u8 cmd, struct sk_buff **skbp,
				size_t size)
{
	struct sk_buff *skb;
	void *reply;

	/*
	 * If new attributes are added, please revisit this allocation
	 */
	skb = genlmsg_new(size, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	if (!info) {
		int seq = this_cpu_inc_return(taskstats_seqnum) - 1;

		reply = genlmsg_put(skb, 0, seq, &family, 0, cmd);
	} else
		reply = genlmsg_put_reply(skb, info, &family, 0, cmd);
	if (reply == NULL) {
		nlmsg_free(skb);
		return -EINVAL;
	}

	*skbp = skb;
	return 0;
}

/*
 * Send taskstats data in @skb to listener with nl_pid @pid
 */
static int send_reply(struct sk_buff *skb, struct genl_info *info)
{
	struct genlmsghdr *genlhdr = nlmsg_data(nlmsg_hdr(skb));
	void *reply = genlmsg_data(genlhdr);

	genlmsg_end(skb, reply);

	return genlmsg_reply(skb, info);
}

/*
 * Send taskstats data in @skb to listeners registered for @cpu's exit data
 */
static void send_cpu_listeners(struct sk_buff *skb,
					struct listener_list *listeners)
{
	struct genlmsghdr *genlhdr = nlmsg_data(nlmsg_hdr(skb));
	struct listener *s, *tmp;
	struct sk_buff *skb_next, *skb_cur = skb;
	void *reply = genlmsg_data(genlhdr);
	int rc, delcount = 0;

	genlmsg_end(skb, reply);

	rc = 0;
	down_read(&listeners->sem);
	list_for_each_entry(s, &listeners->list, list) {
		skb_next = NULL;
		if (!list_is_last(&s->list, &listeners->list)) {
			skb_next = skb_clone(skb_cur, GFP_KERNEL);
			if (!skb_next)
				break;
		}
		rc = genlmsg_unicast(&init_net, skb_cur, s->pid);
		if (rc == -ECONNREFUSED) {
			s->valid = 0;
			delcount++;
		}
		skb_cur = skb_next;
	}
	up_read(&listeners->sem);

	if (skb_cur)
		nlmsg_free(skb_cur);

	if (!delcount)
		return;

	/* Delete invalidated entries */
	down_write(&listeners->sem);
	list_for_each_entry_safe(s, tmp, &listeners->list, list) {
		if (!s->valid) {
			list_del(&s->list);
			kfree(s);
		}
	}
	up_write(&listeners->sem);
}

static void fill_stats(struct user_namespace *user_ns,
		       struct pid_namespace *pid_ns,
		       struct task_struct *tsk, struct taskstats *stats)
{
	memset(stats, 0, sizeof(*stats));
	/*
	 * Each accounting subsystem adds calls to its functions to
	 * fill in relevant parts of struct taskstsats as follows
	 *
	 *	per-task-foo(stats, tsk);
	 */

	delayacct_add_tsk(stats, tsk);

	/* fill in basic acct fields */
	stats->version = TASKSTATS_VERSION;
	stats->nvcsw = tsk->nvcsw;
	stats->nivcsw = tsk->nivcsw;
	bacct_add_tsk(user_ns, pid_ns, stats, tsk);

	/* fill in extended acct fields */
	xacct_add_tsk(stats, tsk);
}

static int fill_stats_for_pid(pid_t pid, struct taskstats *stats)
{
	struct task_struct *tsk;

	tsk = find_get_task_by_vpid(pid);
	if (!tsk)
		return -ESRCH;
	fill_stats(current_user_ns(), task_active_pid_ns(current), tsk, stats);
	put_task_struct(tsk);
	return 0;
}

static int fill_stats_for_tgid(pid_t tgid, struct taskstats *stats)
{
	struct task_struct *tsk, *first;
	unsigned long flags;
	int rc = -ESRCH;
	u64 delta, utime, stime;
	u64 start_time;

	/*
	 * Add additional stats from live tasks except zombie thread group
	 * leaders who are already counted with the dead tasks
	 */
	rcu_read_lock();
	first = find_task_by_vpid(tgid);

	if (!first || !lock_task_sighand(first, &flags))
		goto out;

	if (first->signal->stats)
		memcpy(stats, first->signal->stats, sizeof(*stats));
	else
		memset(stats, 0, sizeof(*stats));

	tsk = first;
	start_time = ktime_get_ns();
	do {
		if (tsk->exit_state)
			continue;
		/*
		 * Accounting subsystem can call its functions here to
		 * fill in relevant parts of struct taskstsats as follows
		 *
		 *	per-task-foo(stats, tsk);
		 */
		delayacct_add_tsk(stats, tsk);

		/* calculate task elapsed time in nsec */
		delta = start_time - tsk->start_time;
		/* Convert to micro seconds */
		do_div(delta, NSEC_PER_USEC);
		stats->ac_etime += delta;

		task_cputime(tsk, &utime, &stime);
		stats->ac_utime += div_u64(utime, NSEC_PER_USEC);
		stats->ac_stime += div_u64(stime, NSEC_PER_USEC);

		stats->nvcsw += tsk->nvcsw;
		stats->nivcsw += tsk->nivcsw;
	} while_each_thread(first, tsk);

	unlock_task_sighand(first, &flags);
	rc = 0;
out:
	rcu_read_unlock();

	stats->version = TASKSTATS_VERSION;
	/*
	 * Accounting subsystems can also add calls here to modify
	 * fields of taskstats.
	 */
	return rc;
}

static void fill_tgid_exit(struct task_struct *tsk)
{
	unsigned long flags;

	spin_lock_irqsave(&tsk->sighand->siglock, flags);
	if (!tsk->signal->stats)
		goto ret;

	/*
	 * Each accounting subsystem calls its functions here to
	 * accumalate its per-task stats for tsk, into the per-tgid structure
	 *
	 *	per-task-foo(tsk->signal->stats, tsk);
	 */
	delayacct_add_tsk(tsk->signal->stats, tsk);
ret:
	spin_unlock_irqrestore(&tsk->sighand->siglock, flags);
	return;
}

static int add_del_listener(pid_t pid, const struct cpumask *mask, int isadd)
{
	struct listener_list *listeners;
	struct listener *s, *tmp, *s2;
	unsigned int cpu;
	int ret = 0;

	if (!cpumask_subset(mask, cpu_possible_mask))
		return -EINVAL;

	if (current_user_ns() != &init_user_ns)
		return -EINVAL;

	if (task_active_pid_ns(current) != &init_pid_ns)
		return -EINVAL;

	if (isadd == REGISTER) {
		for_each_cpu(cpu, mask) {
			s = kmalloc_node(sizeof(struct listener),
					GFP_KERNEL, cpu_to_node(cpu));
			if (!s) {
				ret = -ENOMEM;
				goto cleanup;
			}
			s->pid = pid;
			s->valid = 1;

			listeners = &per_cpu(listener_array, cpu);
			down_write(&listeners->sem);
			list_for_each_entry(s2, &listeners->list, list) {
				if (s2->pid == pid && s2->valid)
					goto exists;
			}
			list_add(&s->list, &listeners->list);
			s = NULL;
exists:
			up_write(&listeners->sem);
			kfree(s); /* nop if NULL */
		}
		return 0;
	}

	/* Deregister or cleanup */
cleanup:
	for_each_cpu(cpu, mask) {
		listeners = &per_cpu(listener_array, cpu);
		down_write(&listeners->sem);
		list_for_each_entry_safe(s, tmp, &listeners->list, list) {
			if (s->pid == pid) {
				list_del(&s->list);
				kfree(s);
				break;
			}
		}
		up_write(&listeners->sem);
	}
	return ret;
}

static int parse(struct nlattr *na, struct cpumask *mask)
{
	char *data;
	int len;
	int ret;

	if (na == NULL)
		return 1;
	len = nla_len(na);
	if (len > TASKSTATS_CPUMASK_MAXLEN)
		return -E2BIG;
	if (len < 1)
		return -EINVAL;
	data = kmalloc(len, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	nla_strlcpy(data, na, len);
	ret = cpulist_parse(data, mask);
	kfree(data);
	return ret;
}

static struct taskstats *mk_reply(struct sk_buff *skb, int type, u32 pid)
{
	struct nlattr *na, *ret;
	int aggr;

	aggr = (type == TASKSTATS_TYPE_PID)
			? TASKSTATS_TYPE_AGGR_PID
			: TASKSTATS_TYPE_AGGR_TGID;

	na = nla_nest_start(skb, aggr);
	if (!na)
		goto err;

	if (nla_put(skb, type, sizeof(pid), &pid) < 0) {
		nla_nest_cancel(skb, na);
		goto err;
	}
	ret = nla_reserve_64bit(skb, TASKSTATS_TYPE_STATS,
				sizeof(struct taskstats), TASKSTATS_TYPE_NULL);
	if (!ret) {
		nla_nest_cancel(skb, na);
		goto err;
	}
	nla_nest_end(skb, na);

	return nla_data(ret);
err:
	return NULL;
}

#define K(x) ((x) << (PAGE_SHIFT - 10))
#ifndef CONFIG_NUMA
static void sysstats_fill_zoneinfo(struct sys_memstats *stats)
{
	pg_data_t *pgdat;
	struct zone *zone;
	struct zone *node_zones;
	unsigned long zspages = 0;

	pgdat = NODE_DATA(0);
	node_zones = pgdat->node_zones;

	for (zone = node_zones; zone - node_zones < MAX_NR_ZONES; ++zone) {
		if (!populated_zone(zone))
			continue;

		zspages += zone_page_state(zone, NR_ZSPAGES);
		if (!strcmp(zone->name, "DMA")) {
			stats->dma_nr_free_pages =
				K(zone_page_state(zone, NR_FREE_PAGES));
			stats->dma_nr_active_anon =
				K(zone_page_state(zone, NR_ZONE_ACTIVE_ANON));
			stats->dma_nr_inactive_anon =
				K(zone_page_state(zone, NR_ZONE_INACTIVE_ANON));
			stats->dma_nr_active_file =
				K(zone_page_state(zone, NR_ZONE_ACTIVE_FILE));
			stats->dma_nr_inactive_file =
				K(zone_page_state(zone, NR_ZONE_INACTIVE_FILE));
		} else if (!strcmp(zone->name, "Normal")) {
			stats->normal_nr_free_pages =
				K(zone_page_state(zone, NR_FREE_PAGES));
			stats->normal_nr_active_anon =
				K(zone_page_state(zone, NR_ZONE_ACTIVE_ANON));
			stats->normal_nr_inactive_anon =
				K(zone_page_state(zone, NR_ZONE_INACTIVE_ANON));
			stats->normal_nr_active_file =
				K(zone_page_state(zone, NR_ZONE_ACTIVE_FILE));
			stats->normal_nr_inactive_file =
				K(zone_page_state(zone, NR_ZONE_INACTIVE_FILE));
		} else if (!strcmp(zone->name, "HighMem")) {
			stats->highmem_nr_free_pages =
				K(zone_page_state(zone, NR_FREE_PAGES));
			stats->highmem_nr_active_anon =
				K(zone_page_state(zone, NR_ZONE_ACTIVE_ANON));
			stats->highmem_nr_inactive_anon =
				K(zone_page_state(zone, NR_ZONE_INACTIVE_ANON));
			stats->highmem_nr_active_file =
				K(zone_page_state(zone, NR_ZONE_ACTIVE_FILE));
			stats->highmem_nr_inactive_file =
				K(zone_page_state(zone, NR_ZONE_INACTIVE_FILE));
		} else if (!strcmp(zone->name, "Movable")) {
			stats->movable_nr_free_pages =
				K(zone_page_state(zone, NR_FREE_PAGES));
			stats->movable_nr_active_anon =
				K(zone_page_state(zone, NR_ZONE_ACTIVE_ANON));
			stats->movable_nr_inactive_anon =
				K(zone_page_state(zone, NR_ZONE_INACTIVE_ANON));
			stats->movable_nr_active_file =
				K(zone_page_state(zone, NR_ZONE_ACTIVE_FILE));
			stats->movable_nr_inactive_file =
				K(zone_page_state(zone, NR_ZONE_INACTIVE_FILE));
		}
	}
	stats->zram_compressed = K(zspages);
}
#elif
static void sysstats_fill_zoneinfo(struct sys_memstats *stats)
{
}
#endif

static void sysstats_build(struct sys_memstats *stats)
{
	struct sysinfo i;

	si_meminfo(&i);
	si_swapinfo(&i);

	stats->version = SYSSTATS_VERSION;
	stats->memtotal = K(i.totalram);
	stats->reclaimable =
		K(global_node_page_state(NR_KERNEL_MISC_RECLAIMABLE));
	stats->swap_used = K(i.totalswap - i.freeswap);
	stats->swap_total = K(i.totalswap);
	stats->vmalloc_total = K(vmalloc_nr_pages());
	stats->unreclaimable =
		K(global_node_page_state(NR_UNRECLAIMABLE_PAGES));
	stats->buffer = K(i.bufferram);
	stats->swapcache = K(total_swapcache_pages());
	stats->slab_reclaimable =
		K(global_node_page_state(NR_SLAB_RECLAIMABLE));
	stats->slab_unreclaimable =
		K(global_node_page_state(NR_SLAB_UNRECLAIMABLE));
	stats->free_cma = K(global_zone_page_state(NR_FREE_CMA_PAGES));
	stats->file_mapped = K(global_node_page_state(NR_FILE_MAPPED));
	stats->kernelstack = global_zone_page_state(NR_KERNEL_STACK_KB);
	stats->pagetable = K(global_zone_page_state(NR_PAGETABLE));
	stats->shmem = K(i.sharedram);
	sysstats_fill_zoneinfo(stats);
}
#undef K

static int sysstats_user_cmd(struct sk_buff *skb, struct genl_info *info)
{
	int rc = 0;
	struct sk_buff *rep_skb;
	struct sys_memstats *stats;
	struct nlattr *na;
	size_t size;

	size = nla_total_size(sizeof(struct sys_memstats));

	rc = prepare_reply(info, SYSSTATS_CMD_NEW, &rep_skb,
				size);
	if (rc < 0)
		goto err;

	na = nla_reserve(rep_skb, SYSSTATS_TYPE_SYSMEM_STATS,
				sizeof(struct sys_memstats));
	if (na == NULL) {
		nlmsg_free(rep_skb);
		rc = -EMSGSIZE;
		goto err;
	}

	stats = nla_data(na);
	memset(stats, 0, sizeof(*stats));

	sysstats_build(stats);

	rc = send_reply(rep_skb, info);

err:
	return rc;
}

static int cgroupstats_user_cmd(struct sk_buff *skb, struct genl_info *info)
{
	int rc = 0;
	struct sk_buff *rep_skb;
	struct cgroupstats *stats;
	struct nlattr *na;
	size_t size;
	u32 fd;
	struct fd f;

	na = info->attrs[CGROUPSTATS_CMD_ATTR_FD];
	if (!na)
		return -EINVAL;

	fd = nla_get_u32(info->attrs[CGROUPSTATS_CMD_ATTR_FD]);
	f = fdget(fd);
	if (!f.file)
		return 0;

	size = nla_total_size(sizeof(struct cgroupstats));

	rc = prepare_reply(info, CGROUPSTATS_CMD_NEW, &rep_skb,
				size);
	if (rc < 0)
		goto err;

	na = nla_reserve(rep_skb, CGROUPSTATS_TYPE_CGROUP_STATS,
				sizeof(struct cgroupstats));
	if (na == NULL) {
		nlmsg_free(rep_skb);
		rc = -EMSGSIZE;
		goto err;
	}

	stats = nla_data(na);
	memset(stats, 0, sizeof(*stats));

	rc = cgroupstats_build(stats, f.file->f_path.dentry);
	if (rc < 0) {
		nlmsg_free(rep_skb);
		goto err;
	}

	rc = send_reply(rep_skb, info);

err:
	fdput(f);
	return rc;
}

static int cmd_attr_register_cpumask(struct genl_info *info)
{
	cpumask_var_t mask;
	int rc;

	if (!alloc_cpumask_var(&mask, GFP_KERNEL))
		return -ENOMEM;
	rc = parse(info->attrs[TASKSTATS_CMD_ATTR_REGISTER_CPUMASK], mask);
	if (rc < 0)
		goto out;
	rc = add_del_listener(info->snd_portid, mask, REGISTER);
out:
	free_cpumask_var(mask);
	return rc;
}

static int cmd_attr_deregister_cpumask(struct genl_info *info)
{
	cpumask_var_t mask;
	int rc;

	if (!alloc_cpumask_var(&mask, GFP_KERNEL))
		return -ENOMEM;
	rc = parse(info->attrs[TASKSTATS_CMD_ATTR_DEREGISTER_CPUMASK], mask);
	if (rc < 0)
		goto out;
	rc = add_del_listener(info->snd_portid, mask, DEREGISTER);
out:
	free_cpumask_var(mask);
	return rc;
}

static size_t taskstats_packet_size(void)
{
	size_t size;

	size = nla_total_size(sizeof(u32)) +
		nla_total_size_64bit(sizeof(struct taskstats)) +
		nla_total_size(0);

	return size;
}

static int taskstats2_cmd_attr_pid(struct genl_info *info)
{
	struct taskstats2 *stats;
	struct sk_buff *rep_skb;
	struct nlattr *ret;
	struct task_struct *tsk;
	struct task_struct *p;
	size_t size;
	u32 pid;
	int rc;
	u64 utime, stime;
	const struct cred *tcred;
#ifdef CONFIG_CPUSETS
	struct cgroup_subsys_state *css;
#endif
	unsigned long flags;
	struct signal_struct *sig;

	size = nla_total_size_64bit(sizeof(struct taskstats2));

	rc = prepare_reply(info, TASKSTATS_CMD_NEW, &rep_skb, size);
	if (rc < 0)
		return rc;

	rc = -EINVAL;
	pid = nla_get_u32(info->attrs[TASKSTATS_CMD_ATTR_PID]);

	ret = nla_reserve_64bit(rep_skb, TASKSTATS_TYPE_STATS,
				sizeof(struct taskstats2), TASKSTATS_TYPE_NULL);
	if (!ret)
		goto err;

	stats = nla_data(ret);

	rcu_read_lock();
	tsk = find_task_by_vpid(pid);
	if (tsk)
		get_task_struct(tsk);
	rcu_read_unlock();
	if (!tsk) {
		rc = -ESRCH;
		goto err;
	}
	memset(stats, 0, sizeof(*stats));
	stats->version = TASKSTATS2_VERSION;
	stats->pid = task_pid_nr_ns(tsk, task_active_pid_ns(current));
	p = find_lock_task_mm(tsk);
	if (p) {
#define K(x) ((x) << (PAGE_SHIFT - 10))
		stats->anon_rss = K(get_mm_counter(p->mm, MM_ANONPAGES));
		stats->file_rss = K(get_mm_counter(p->mm, MM_FILEPAGES));
		stats->shmem_rss = K(get_mm_counter(p->mm, MM_SHMEMPAGES));
		stats->swap_rss = K(get_mm_counter(p->mm, MM_SWAPENTS));
		stats->unreclaimable =
				K(get_mm_counter(p->mm, MM_UNRECLAIMABLE));
#undef K
		task_unlock(p);
	}

	/* version 2 fields begin here */
	task_cputime(tsk, &utime, &stime);
	stats->utime = div_u64(utime, NSEC_PER_USEC);
	stats->stime = div_u64(stime, NSEC_PER_USEC);

	if (lock_task_sighand(tsk, &flags)) {
		sig = tsk->signal;
		stats->cutime = sig->cutime;
		stats->cstime = sig->cstime;
		unlock_task_sighand(tsk, &flags);
	}

	rcu_read_lock();
	tcred = __task_cred(tsk);
	stats->uid = from_kuid_munged(current_user_ns(), tcred->uid);
	stats->ppid = pid_alive(tsk) ?
		task_tgid_nr_ns(rcu_dereference(tsk->real_parent),
			task_active_pid_ns(current)) : 0;
	rcu_read_unlock();

	strlcpy(stats->name, tsk->comm, sizeof(stats->name));

#ifdef CONFIG_CPUSETS
	css = task_get_css(tsk, cpuset_cgrp_id);
	cgroup_path_ns(css->cgroup, stats->state, sizeof(stats->state),
				current->nsproxy->cgroup_ns);
	css_put(css);
	/* version 2 fields end here */
#endif

	put_task_struct(tsk);

	return send_reply(rep_skb, info);
err:
	nlmsg_free(rep_skb);
	return rc;
}

static int cmd_attr_pid(struct genl_info *info)
{
	struct taskstats *stats;
	struct sk_buff *rep_skb;
	size_t size;
	u32 pid;
	int rc;

	size = taskstats_packet_size();

	rc = prepare_reply(info, TASKSTATS_CMD_NEW, &rep_skb, size);
	if (rc < 0)
		return rc;

	rc = -EINVAL;
	pid = nla_get_u32(info->attrs[TASKSTATS_CMD_ATTR_PID]);
	stats = mk_reply(rep_skb, TASKSTATS_TYPE_PID, pid);
	if (!stats)
		goto err;

	rc = fill_stats_for_pid(pid, stats);
	if (rc < 0)
		goto err;
	return send_reply(rep_skb, info);
err:
	nlmsg_free(rep_skb);
	return rc;
}

static int cmd_attr_tgid(struct genl_info *info)
{
	struct taskstats *stats;
	struct sk_buff *rep_skb;
	size_t size;
	u32 tgid;
	int rc;

	size = taskstats_packet_size();

	rc = prepare_reply(info, TASKSTATS_CMD_NEW, &rep_skb, size);
	if (rc < 0)
		return rc;

	rc = -EINVAL;
	tgid = nla_get_u32(info->attrs[TASKSTATS_CMD_ATTR_TGID]);
	stats = mk_reply(rep_skb, TASKSTATS_TYPE_TGID, tgid);
	if (!stats)
		goto err;

	rc = fill_stats_for_tgid(tgid, stats);
	if (rc < 0)
		goto err;
	return send_reply(rep_skb, info);
err:
	nlmsg_free(rep_skb);
	return rc;
}

static struct tgid_iter next_tgid(struct pid_namespace *ns,
					struct tgid_iter iter)
{
	struct pid *pid;

	if (iter.task)
		put_task_struct(iter.task);
	rcu_read_lock();
retry:
	iter.task = NULL;
	pid = find_ge_pid(iter.tgid, ns);
	if (pid) {
		iter.tgid = pid_nr_ns(pid, ns);
		iter.task = pid_task(pid, PIDTYPE_PID);
		if (!iter.task || !has_group_leader_pid(iter.task)) {
			iter.tgid += 1;
			goto retry;
		}
		get_task_struct(iter.task);
	}
	rcu_read_unlock();
	return iter;
}

static int taskstats2_foreach(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct pid_namespace *ns = task_active_pid_ns(current);
	struct tgid_iter iter;
	void *reply;
	struct nlattr *attr;
	struct nlattr *nla;
	struct taskstats2 *stats;
	struct task_struct *p;
	short oom_score;
	short oom_score_min;
	short oom_score_max;
	u32 buf;

	nla = nla_find(nlmsg_attrdata(cb->nlh, GENL_HDRLEN),
			nlmsg_attrlen(cb->nlh, GENL_HDRLEN),
			TASKSTATS_TYPE_FOREACH);

	if (!nla)
		goto out;

	buf  = nla_get_u32(nla);
	oom_score_min = (short) (buf & 0xFFFF);
	oom_score_max = (short) ((buf >> 16) & 0xFFFF);

	iter.tgid = cb->args[0];
	iter.task = NULL;
	for (iter = next_tgid(ns, iter); iter.task;
			iter.tgid += 1, iter = next_tgid(ns, iter)) {

		if (iter.task->flags & PF_KTHREAD)
			continue;

		oom_score = iter.task->signal->oom_score_adj;
		if ((oom_score < oom_score_min)
			|| (oom_score > oom_score_max))
			continue;

		reply = genlmsg_put(skb, NETLINK_CB(cb->skb).portid,
			cb->nlh->nlmsg_seq, &family, 0, TASKSTATS2_CMD_GET);
		if (reply == NULL) {
			put_task_struct(iter.task);
			break;
		}
		attr = nla_reserve(skb, TASKSTATS_TYPE_FOREACH,
				sizeof(struct taskstats2));
		if (!attr) {
			put_task_struct(iter.task);
			genlmsg_cancel(skb, reply);
			break;
		}
		stats = nla_data(attr);
		memset(stats, 0, sizeof(struct taskstats2));
		stats->version = TASKSTATS2_VERSION;
		rcu_read_lock();
		stats->pid = task_pid_nr_ns(iter.task,
						task_active_pid_ns(current));
		stats->oom_score = iter.task->signal->oom_score_adj;
		rcu_read_unlock();
		p = find_lock_task_mm(iter.task);
		if (p) {
#define K(x) ((x) << (PAGE_SHIFT - 10))
			stats->anon_rss =
				K(get_mm_counter(p->mm, MM_ANONPAGES));
			stats->file_rss =
				K(get_mm_counter(p->mm, MM_FILEPAGES));
			stats->shmem_rss =
				K(get_mm_counter(p->mm, MM_SHMEMPAGES));
			stats->swap_rss =
				K(get_mm_counter(p->mm, MM_SWAPENTS));
			task_unlock(p);
#undef K
		}
		genlmsg_end(skb, reply);
	}

	cb->args[0] = iter.tgid;
out:
	return skb->len;
}

static int taskstats2_user_cmd(struct sk_buff *skb, struct genl_info *info)
{
	if (info->attrs[TASKSTATS_CMD_ATTR_PID])
		return taskstats2_cmd_attr_pid(info);
	else
		return -EINVAL;
}

static int taskstats_user_cmd(struct sk_buff *skb, struct genl_info *info)
{
	if (info->attrs[TASKSTATS_CMD_ATTR_REGISTER_CPUMASK])
		return cmd_attr_register_cpumask(info);
	else if (info->attrs[TASKSTATS_CMD_ATTR_DEREGISTER_CPUMASK])
		return cmd_attr_deregister_cpumask(info);
	else if (info->attrs[TASKSTATS_CMD_ATTR_PID])
		return cmd_attr_pid(info);
	else if (info->attrs[TASKSTATS_CMD_ATTR_TGID])
		return cmd_attr_tgid(info);
	else
		return -EINVAL;
}

static struct taskstats *taskstats_tgid_alloc(struct task_struct *tsk)
{
	struct signal_struct *sig = tsk->signal;
	struct taskstats *stats_new, *stats;

	/* Pairs with smp_store_release() below. */
	stats = smp_load_acquire(&sig->stats);
	if (stats || thread_group_empty(tsk))
		return stats;

	/* No problem if kmem_cache_zalloc() fails */
	stats_new = kmem_cache_zalloc(taskstats_cache, GFP_KERNEL);

	spin_lock_irq(&tsk->sighand->siglock);
	stats = sig->stats;
	if (!stats) {
		/*
		 * Pairs with smp_store_release() above and order the
		 * kmem_cache_zalloc().
		 */
		smp_store_release(&sig->stats, stats_new);
		stats = stats_new;
		stats_new = NULL;
	}
	spin_unlock_irq(&tsk->sighand->siglock);

	if (stats_new)
		kmem_cache_free(taskstats_cache, stats_new);

	return stats;
}

/* Send pid data out on exit */
void taskstats_exit(struct task_struct *tsk, int group_dead)
{
	int rc;
	struct listener_list *listeners;
	struct taskstats *stats;
	struct sk_buff *rep_skb;
	size_t size;
	int is_thread_group;

	if (!family_registered)
		return;

	/*
	 * Size includes space for nested attributes
	 */
	size = taskstats_packet_size();

	is_thread_group = !!taskstats_tgid_alloc(tsk);
	if (is_thread_group) {
		/* PID + STATS + TGID + STATS */
		size = 2 * size;
		/* fill the tsk->signal->stats structure */
		fill_tgid_exit(tsk);
	}

	listeners = raw_cpu_ptr(&listener_array);
	if (list_empty(&listeners->list))
		return;

	rc = prepare_reply(NULL, TASKSTATS_CMD_NEW, &rep_skb, size);
	if (rc < 0)
		return;

	stats = mk_reply(rep_skb, TASKSTATS_TYPE_PID,
			 task_pid_nr_ns(tsk, &init_pid_ns));
	if (!stats)
		goto err;

	fill_stats(&init_user_ns, &init_pid_ns, tsk, stats);

	/*
	 * Doesn't matter if tsk is the leader or the last group member leaving
	 */
	if (!is_thread_group || !group_dead)
		goto send;

	stats = mk_reply(rep_skb, TASKSTATS_TYPE_TGID,
			 task_tgid_nr_ns(tsk, &init_pid_ns));
	if (!stats)
		goto err;

	memcpy(stats, tsk->signal->stats, sizeof(*stats));

send:
	send_cpu_listeners(rep_skb, listeners);
	return;
err:
	nlmsg_free(rep_skb);
}

static const struct genl_ops taskstats_ops[] = {
	{
		.cmd		= TASKSTATS_CMD_GET,
		.doit		= taskstats_user_cmd,
		.policy		= taskstats_cmd_get_policy,
		.flags		= GENL_ADMIN_PERM,
	},
	{
		.cmd		= TASKSTATS2_CMD_GET,
		.doit		= taskstats2_user_cmd,
		.dumpit		= taskstats2_foreach,
		.policy		= taskstats_cmd_get_policy,
	},
	{
		.cmd		= CGROUPSTATS_CMD_GET,
		.doit		= cgroupstats_user_cmd,
		.policy		= cgroupstats_cmd_get_policy,
	},
	{
		.cmd		= SYSSTATS_CMD_GET,
		.doit		= sysstats_user_cmd,
		.policy		= sysstats_cmd_get_policy,
	},
};

static struct genl_family family __ro_after_init = {
	.name		= TASKSTATS_GENL_NAME,
	.version	= TASKSTATS_GENL_VERSION,
	.maxattr	= TASKSTATS_CMD_ATTR_MAX,
	.module		= THIS_MODULE,
	.ops		= taskstats_ops,
	.n_ops		= ARRAY_SIZE(taskstats_ops),
};

/* Needed early in initialization */
void __init taskstats_init_early(void)
{
	unsigned int i;

	taskstats_cache = KMEM_CACHE(taskstats, SLAB_PANIC);
	for_each_possible_cpu(i) {
		INIT_LIST_HEAD(&(per_cpu(listener_array, i).list));
		init_rwsem(&(per_cpu(listener_array, i).sem));
	}
}

static int __init taskstats_init(void)
{
	int rc;

	rc = genl_register_family(&family);
	if (rc)
		return rc;

	family_registered = 1;
	pr_info("registered taskstats version %d\n", TASKSTATS_GENL_VERSION);
	return 0;
}

/*
 * late initcall ensures initialization of statistics collection
 * mechanisms precedes initialization of the taskstats interface
 */
late_initcall(taskstats_init);
