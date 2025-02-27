/*
 *     MPTCP implementation - MPTCP-subflow-management
 *
 *     Initial Design & Implementation:
 *     Sébastien Barré <sebastien.barre@uclouvain.be>
 *
 *     Current Maintainer & Author:
 *     Christoph Paasch <christoph.paasch@uclouvain.be>
 *
 *     Additional authors:
 *     Jaakko Korkeaniemi <jaakko.korkeaniemi@aalto.fi>
 *     Gregory Detal <gregory.detal@uclouvain.be>
 *     Fabien Duchêne <fabien.duchene@uclouvain.be>
 *     Andreas Seelinger <Andreas.Seelinger@rwth-aachen.de>
 *     Lavkesh Lahngir <lavkesh51@gmail.com>
 *     Andreas Ripke <ripke@neclab.eu>
 *     Vlad Dogaru <vlad.dogaru@intel.com>
 *     Octavian Purdila <octavian.purdila@intel.com>
 *     John Ronan <jronan@tssg.org>
 *     Catalin Nicutar <catalin.nicutar@gmail.com>
 *     Brandon Heller <brandonh@stanford.edu>
 *
 *
 *     This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#include <linux/module.h>
#include <net/mptcp.h>

static DEFINE_SPINLOCK(mptcp_pm_list_lock);
static LIST_HEAD(mptcp_pm_list);

static int mptcp_default_id(const struct sock *meta_sk, sa_family_t family,
			    union inet_addr *addr, bool *low_prio)
{
	return 0;
}

struct mptcp_pm_ops mptcp_pm_default = {
	.get_local_id = mptcp_default_id, /* We do not care */
	.name = "default",
	.owner = THIS_MODULE,
};

static struct mptcp_pm_ops *mptcp_pm_find(const char *name)
{
	struct mptcp_pm_ops *e;

	list_for_each_entry_rcu(e, &mptcp_pm_list, list) {
		if (strcmp(e->name, name) == 0)
			return e;
	}

	return NULL;
}

int mptcp_register_path_manager(struct mptcp_pm_ops *pm)
{
	int ret = 0;

	if (!pm->get_local_id)
		return -EINVAL;

	spin_lock(&mptcp_pm_list_lock);
	if (mptcp_pm_find(pm->name)) {
		pr_notice("%s already registered\n", pm->name);
		ret = -EEXIST;
	} else {
		list_add_tail_rcu(&pm->list, &mptcp_pm_list);
		pr_info("%s registered\n", pm->name);
	}
	spin_unlock(&mptcp_pm_list_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mptcp_register_path_manager);

void mptcp_unregister_path_manager(struct mptcp_pm_ops *pm)
{
	spin_lock(&mptcp_pm_list_lock);
	list_del_rcu(&pm->list);
	spin_unlock(&mptcp_pm_list_lock);

	/* Wait for outstanding readers to complete before the
	 * module gets removed entirely.
	 *
	 * A try_module_get() should fail by now as our module is
	 * in "going" state since no refs are held anymore and
	 * module_exit() handler being called.
	 */
	synchronize_rcu();
}
EXPORT_SYMBOL_GPL(mptcp_unregister_path_manager);

void mptcp_get_default_path_manager(char *name)
{
	struct mptcp_pm_ops *pm;

	BUG_ON(list_empty(&mptcp_pm_list));

	rcu_read_lock();
	pm = list_entry(mptcp_pm_list.next, struct mptcp_pm_ops, list);
	strncpy(name, pm->name, MPTCP_PM_NAME_MAX);
	rcu_read_unlock();
}

int mptcp_set_default_path_manager(const char *name)
{
	struct mptcp_pm_ops *pm;
	int ret = -ENOENT;

	spin_lock(&mptcp_pm_list_lock);
	pm = mptcp_pm_find(name);
#ifdef CONFIG_MODULES
	if (!pm && capable(CAP_NET_ADMIN)) {
		spin_unlock(&mptcp_pm_list_lock);

		request_module("mptcp_%s", name);
		spin_lock(&mptcp_pm_list_lock);
		pm = mptcp_pm_find(name);
	}
#endif

	if (pm) {
		list_move(&pm->list, &mptcp_pm_list);
		ret = 0;
	} else {
		pr_info("%s is not available\n", name);
	}
	spin_unlock(&mptcp_pm_list_lock);

	return ret;
}

static struct mptcp_pm_ops *__mptcp_pm_find_autoload(const char *name)
{
	struct mptcp_pm_ops *pm = mptcp_pm_find(name);
#ifdef CONFIG_MODULES
	if (!pm && capable(CAP_NET_ADMIN)) {
		rcu_read_unlock();
		request_module("mptcp_%s", name);
		rcu_read_lock();
		pm = mptcp_pm_find(name);
	}
#endif
	return pm;
}

void mptcp_init_path_manager(struct mptcp_cb *mpcb)
{
	struct mptcp_pm_ops *pm;
	struct sock *meta_sk = mpcb->meta_sk;
	struct tcp_sock *meta_tp = tcp_sk(meta_sk);

	rcu_read_lock();
	/* if path manager was set using socket option */
	if (meta_tp->mptcp_pm_setsockopt) {
		pm = __mptcp_pm_find_autoload(meta_tp->mptcp_pm_name);
		if (pm && try_module_get(pm->owner)) {
			mpcb->pm_ops = pm;
			goto out;
		}
	}

	list_for_each_entry_rcu(pm, &mptcp_pm_list, list) {
		if (try_module_get(pm->owner)) {
			mpcb->pm_ops = pm;
			break;
		}
	}
out:
	rcu_read_unlock();
}

/* Change path manager for socket */
int mptcp_set_path_manager(struct sock *sk, const char *name)
{
	struct mptcp_pm_ops *pm;
	int err = 0;

	rcu_read_lock();
	pm = __mptcp_pm_find_autoload(name);

	if (!pm) {
		err = -ENOENT;
	} else if (!ns_capable(sock_net(sk)->user_ns, CAP_NET_ADMIN)) {
		err = -EPERM;
	} else {
		strcpy(tcp_sk(sk)->mptcp_pm_name, name);
		tcp_sk(sk)->mptcp_pm_setsockopt = 1;
	}
	rcu_read_unlock();

	return err;
}

/* Manage refcounts on socket close. */
void mptcp_cleanup_path_manager(struct mptcp_cb *mpcb)
{
	module_put(mpcb->pm_ops->owner);
}

/* Fallback to the default path-manager. */
void mptcp_fallback_default(struct mptcp_cb *mpcb)
{
	struct mptcp_pm_ops *pm;

	mptcp_cleanup_path_manager(mpcb);
	pm = mptcp_pm_find("default");

	/* Cannot fail - it's the default module */
	try_module_get(pm->owner);
	mpcb->pm_ops = pm;
}
EXPORT_SYMBOL_GPL(mptcp_fallback_default);

/* Set default value from kernel configuration at bootup */
static int __init mptcp_path_manager_default(void)
{
	return mptcp_set_default_path_manager(CONFIG_DEFAULT_MPTCP_PM);
}
late_initcall(mptcp_path_manager_default);
