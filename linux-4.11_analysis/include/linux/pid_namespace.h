#ifndef _LINUX_PID_NS_H
#define _LINUX_PID_NS_H

#include <linux/sched.h>
#include <linux/bug.h>
#include <linux/mm.h>
#include <linux/workqueue.h>
#include <linux/threads.h>
#include <linux/nsproxy.h>
#include <linux/kref.h>
#include <linux/ns_common.h>

struct pidmap {
       atomic_t nr_free;
       void *page;
};

#define BITS_PER_PAGE		(PAGE_SIZE * 8)
#define BITS_PER_PAGE_MASK	(BITS_PER_PAGE-1)
#define PIDMAP_ENTRIES		((PID_MAX_LIMIT+BITS_PER_PAGE-1)/BITS_PER_PAGE)

struct fs_pin;

enum { /* definitions for pid_namespace's hide_pid field */
	HIDEPID_OFF	  = 0,
	HIDEPID_NO_ACCESS = 1,
	HIDEPID_INVISIBLE = 2,
};

//           
//           parent namespace A
//            -------------------------------------------------
//            |                                               | level 0
//            |  pid : 1   2   3   4   5   6   7   8   9...   | 
//            |                    ^   ^   ^   ^   ^   ^      |
//            ---------------------|---|---|---|---|---|-------
//              --------------------   |   |   |   |   |
//              |   --------------------   |   |   |   |
//              |   |   --------------------   |   |   -----
//  child       |   |   |          child       |   -----   |
//  namespace B |   |   |         namesapce  C -----   |   |
//     ---------|---|---|----             ---------|---|---|----
//     |        |   |   |   |             |        |   |   |   | level 1
//     |  pid : 1   2   3   |             |  pid : 1   2   3   | parent : A
//     |                    |             |                    |
//     ----------------------             ----------------------
//
//

struct pid_namespace {
	struct kref kref;
	struct pidmap pidmap[PIDMAP_ENTRIES];
    // pid 의 할당 상태가 관리되는 bitmap 
	struct rcu_head rcu;
	int last_pid;
	unsigned int nr_hashed;
	struct task_struct *child_reaper;
    // linux kernel 에서 pid 1 process 인 init proces 는 orphan task 들로부터 자원을 반납받기 위해 
    // wait4(특정 process 가 종료 될 때 까지 기다림) 를 호출한다. name space 에서는 각 namespace 별
    // init variant 가 이를 대신 수행하는데 이 task_struct 를 가리키는 것이 child_reaper 이다.
    //
	struct kmem_cache *pid_cachep;
	unsigned int level;
    // namespace 계층 구조에서 현재 namespace 의 level 을 의미 
	struct pid_namespace *parent;
    // 부모 namespace 로의 포인터
#ifdef CONFIG_PROC_FS
	struct vfsmount *proc_mnt;
	struct dentry *proc_self;
	struct dentry *proc_thread_self;
#endif
#ifdef CONFIG_BSD_PROCESS_ACCT
	struct fs_pin *bacct;
#endif
	struct user_namespace *user_ns;
	struct ucounts *ucounts;
	struct work_struct proc_work;
	kgid_t pid_gid;
	int hide_pid;
	int reboot;	/* group exit code if this pidns was rebooted */
	struct ns_common ns;
};

extern struct pid_namespace init_pid_ns;

#define PIDNS_HASH_ADDING (1U << 31)

#ifdef CONFIG_PID_NS
static inline struct pid_namespace *get_pid_ns(struct pid_namespace *ns)
{
	if (ns != &init_pid_ns)
		kref_get(&ns->kref);
	return ns;
}

extern struct pid_namespace *copy_pid_ns(unsigned long flags,
	struct user_namespace *user_ns, struct pid_namespace *ns);
extern void zap_pid_ns_processes(struct pid_namespace *pid_ns);
extern int reboot_pid_ns(struct pid_namespace *pid_ns, int cmd);
extern void put_pid_ns(struct pid_namespace *ns);

#else /* !CONFIG_PID_NS */
#include <linux/err.h>

static inline struct pid_namespace *get_pid_ns(struct pid_namespace *ns)
{
	return ns;
}

static inline struct pid_namespace *copy_pid_ns(unsigned long flags,
	struct user_namespace *user_ns, struct pid_namespace *ns)
{
	if (flags & CLONE_NEWPID)
		ns = ERR_PTR(-EINVAL);
	return ns;
}

static inline void put_pid_ns(struct pid_namespace *ns)
{
}

static inline void zap_pid_ns_processes(struct pid_namespace *ns)
{
	BUG();
}

static inline int reboot_pid_ns(struct pid_namespace *pid_ns, int cmd)
{
	return 0;
}
#endif /* CONFIG_PID_NS */

extern struct pid_namespace *task_active_pid_ns(struct task_struct *tsk);
void pidhash_init(void);
void pidmap_init(void);

#endif /* _LINUX_PID_NS_H */
