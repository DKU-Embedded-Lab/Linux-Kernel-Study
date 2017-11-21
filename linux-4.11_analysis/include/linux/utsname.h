#ifndef _LINUX_UTSNAME_H
#define _LINUX_UTSNAME_H


#include <linux/sched.h>
#include <linux/kref.h>
#include <linux/nsproxy.h>
#include <linux/ns_common.h>
#include <linux/err.h>
#include <uapi/linux/utsname.h>

enum uts_proc {
	UTS_PROC_OSTYPE,
	UTS_PROC_OSRELEASE,
	UTS_PROC_VERSION,
	UTS_PROC_HOSTNAME,
	UTS_PROC_DOMAINNAME,
};

struct user_namespace;
extern struct user_namespace init_user_ns;
// running kernel 의 version, name, architecture type 등 관련
struct uts_namespace {
	struct kref kref;
    // kernel 에서 uts_namespace 의 사용 횟수(reference counting)
	struct new_utsname name;
    // uts 관련 정보로 새로 초기화한 값 e.g. release, version ...
	struct user_namespace *user_ns;
    // 기존(v2.6.39)에 nsproxy 에 있던 user_namespace 와 많이 다름.
    // 보안과 관련된 식별자(uid,gid,key) 속성들을 독립적으로 구성
	struct ucounts *ucounts;
	struct ns_common ns;
};
extern struct uts_namespace init_uts_ns;

#ifdef CONFIG_UTS_NS
static inline void get_uts_ns(struct uts_namespace *ns)
{
	kref_get(&ns->kref);
}

extern struct uts_namespace *copy_utsname(unsigned long flags,
	struct user_namespace *user_ns, struct uts_namespace *old_ns);
extern void free_uts_ns(struct kref *kref);

static inline void put_uts_ns(struct uts_namespace *ns)
{
	kref_put(&ns->kref, free_uts_ns);
}
#else
static inline void get_uts_ns(struct uts_namespace *ns)
{
}

static inline void put_uts_ns(struct uts_namespace *ns)
{
}

static inline struct uts_namespace *copy_utsname(unsigned long flags,
	struct user_namespace *user_ns, struct uts_namespace *old_ns)
{
	if (flags & CLONE_NEWUTS)
		return ERR_PTR(-EINVAL);

	return old_ns;
}
#endif

#ifdef CONFIG_PROC_SYSCTL
extern void uts_proc_notify(enum uts_proc proc);
#else
static inline void uts_proc_notify(enum uts_proc proc)
{
}
#endif

static inline struct new_utsname *utsname(void)
{
	return &current->nsproxy->uts_ns->name;
}

static inline struct new_utsname *init_utsname(void)
{
	return &init_uts_ns.name;
}

extern struct rw_semaphore uts_sem;

#endif /* _LINUX_UTSNAME_H */
