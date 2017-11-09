#ifndef _LINUX_PID_H
#define _LINUX_PID_H

#include <linux/rculist.h>

enum pid_type
{
	PIDTYPE_PID,
	PIDTYPE_PGID,
	PIDTYPE_SID,
	PIDTYPE_MAX
};

/*
 * What is struct pid?
 *
 * A struct pid is the kernel's internal notion of a process identifier.
 * It refers to individual tasks, process groups, and sessions.  While
 * there are processes attached to it the struct pid lives in a hash
 * table, so it and then the processes that it refers to can be found
 * quickly from the numeric pid value.  The attached processes may be
 * quickly accessed by following pointers from struct pid.
 *
 * Storing pid_t values in the kernel and referring to them later has a
 * problem.  The process originally with that pid may have exited and the
 * pid allocator wrapped, and another process could have come along
 * and been assigned that pid.
 *
 * Referring to user space processes by holding a reference to struct
 * task_struct has a problem.  When the user space process exits
 * the now useless task_struct is still kept.  A task_struct plus a
 * stack consumes around 10K of low kernel memory.  More precisely
 * this is THREAD_SIZE + sizeof(struct task_struct).  By comparison
 * a struct pid is about 64 bytes.
 *
 * Holding a reference to struct pid solves both of these problems.
 * It is small so holding a reference does not consume a lot of
 * resources, and since a new struct pid is allocated when the numeric pid
 * value is reused (when pids wrap around) we don't mistakenly refer to new
 * processes.
 */


/*
 * struct upid is used to get the id of the struct pid, as it is
 * seen in particular namespace. Later the struct pid is found with
 * find_pid_ns() using the int nr and struct pid_namespace *ns.
 */

//specific name space 에서만 visible 한 구조 이며 hash table 으로 관리됨
struct upid {
	/* Try to keep pid_chain in the same cacheline as nr for find_vpid */
	int nr;
    // pid, tgid, pgid, sid 등등...각 namespace 별 local id 값
	struct pid_namespace *ns;
    // 해당 pid 가 속한 namespace 
	struct hlist_node pid_chain;
    // pid hash table 에서 같은 index 에 속한 upid 들을 연결
};

// kernel-internal representation of pid 
// 모든 해당 pid 값을 공유하는 task 들은 struct pid 를 공유
// (e.g. process group 내의 process 들은 process group id 에 해당하는 pid 가 같으므로
// process group 내의 모든 proces 들이task_struct.pids[PIDTYPE_PGID].pid 가 같은 struct pid 를 가리키며
// process group leader 의 경우 task_struct.pis[PIDTYPE_PID].pid 도 해당 struct pid 를 가리킨다.)
//
// 이런 공유 구조는 pid, pgid, sid 에만 해당. tgid 는 해당되지 않음 tgid 의 경우 단순히 task_struct.group_leader 가 
// 가리키는 task_struct 의 pid 값만 가져오면 됨.(task_struct 에 해당 task_struct 를 가리키는 변수가 바로 있음)
struct pid
{
	atomic_t count;
    // reference counter 
	unsigned int level;
	/* lists of tasks that use this pid */
    // 하나의 process 가 각각의 namsespace 에 다른 local id 값으로 존재 가능
    // level 은 process 가 local id 로  보여질 수 있는 namespace 의 수를 의미 
    // (struct pid 를 사용중인 pid_namespace hierarchy 가 지금 얼만큼 level 까지 존재하는가)
	struct hlist_head tasks[PIDTYPE_MAX]; 
    // 이 struct pid 값을 공유하는 task_struct 들을 type 별로 list 로 관리됨
    // 아래 그림의 예시의 경우에 
    // pid 가 100 인 struct pid 는
    //
    //  tasks[PIDTYPE_PID] <-> task A's task_struct.pids[PIDTYPE_PID].node 와 연결
    //  tasks[PIDTYPE_PGID] <-> task A.s task_struct.pids[PIDTYPE_PGID].node <-> task B's task_struct.pids[PIDTYPE_PGID].node 와 연결
    //
    // 이거 통해서 struct pid 를 통해 task_struct 를 결과적으로 찾을 수 있음.
    //
	struct rcu_head rcu;
	struct upid numbers[1];
    // struct pid 가 속한 각 namespace 에게 보여질 id 값.
    // 지금 배열 크기가 1로 설정. 즉 procss 가 global namespace 에 속해있는 경우엔 struct pid 가 이렇게만 
    // 사용되지만, struct pid 가 속한 namespace 가 추가될 경우, struct pid 내에 numbers 가 마지막에 오기 때문에
    // 
    // namespace 가 여러개 사용 된다면 이 배열의  첫번째가 global id 임.
    //
};

//      자신의 task_struct, session leader 의 task_struct, process 
//      group_leader 의 task_struct 에 해당하는
//      task_struct->pid_link->node 와 연결됨
// 
//                 그룹 속한 프로세스                                                 그룹 리더 프로세스 
//                      pid 101                                                 pid 100
//                      pid {                     ----------------------------->pid {   
//                          ...                   |                             ...
//                          hlist_head tasks      |                             hlist_head tasks [PIDTYPE_MAX]
//                          [PIDTYPE_MAX]         |                             [PIDTYPE_MAX]
//    --------------------------0 PIDTYPE_PID,    |        -------------------------0 PIDTYPE_PID, 
//    |                         1 PIDTYPE_PGID,---|-----   |  ----------------------1 PIDTYPE_PGID, 
//    |                         2 PIDTYPE_SID-----|--- |   |  |                     2 PIDTYPE_SID --------------------- 
//    |                 } <---------------------  |  | |   |  |                 } <------------------------           |
//    |                                        |  |  | |   |  |                                            |          |
//    |                                        |  |  | |   |  |                                            |          |
//    |   task B                               |  |  | |   |  |   task A                                   |          |
//    |   task_struct{                         |  |  | |   |  |   task_struct{                             |          |
//    |       ...                              |  |  | |   |  |      ...                                   |          |
//    |       pids[PIDTYPE_MAX]                |  |  | |   |  |      pids[PIDTYPE_MAX]                     |          |
//    |       ------------------- PIDTYPE_PID  |  |  | |   |  |      ------------------- PIDTYPE_PID       |          |
//    |       | 자신 pid        |              |  |  | |   |  |      | 자신 pid        |                   |          |
//    |       | pid_link{       |              |  |  | |   |  |      | pid_link{       |                   |          |
//    |       |   pid-----------|---------------  |  | |   |  |      |   pid-----------|-------------------|          |
//    --------|-->node          |                 |  | |   ---|------|-->node          |                   |          |
//            | }               |                 |  | |      |      | }               |                   |          |
//            ------------------- PIDTYPE_PGID    |  | |      |      ------------------- PIDTYPE_PGID      |          |
//            | 속한 p 그룹리더 |                 |  | |      |      | 속한 p 그룹리더 |                   |          |
//            | pid_link{       |                 |  | |      |      | pid_link{       |                   |          |
//            |   pid-----------|-----------------   | |      |      |   pid-----------|--------------------          |
//            |   node          |                    | --------------|-->node          |                              |
//            | }               |                    |               | }               |                              |
//            ------------------- PIDTYPE_SID        |               ------------------- PIDTYPE_SID                  |
//            | 속한 s 그룹 리더|                    |               | 속한 s 그룹 리더|                              |
//            | pid_link{       |                    |               | pid_link{       |                              |
//            |  pid            |                    |               |  pid            |                              |
//            |  node           |                    |               |  node           |                              |
//            | }               |                    |               | }               |                              |
//            -------------------                    |               -------------------                              |
//            ...                                    |               ...                                              |
//        }                                          |           }                                                    |
//                                                   |                                                                |
//                                                   ---> 세션 리더 프로ㅔ스의 task_struct->pids[PIDTYPE_SID].node <---


extern struct pid init_struct_pid;

struct pid_link
{
	struct hlist_node node; 
    // pis.tasks 를 통해 type 별 task_struct 가 연결되는데 pids.tasks 에 연결되는 hlist_node 가 이거임
	struct pid *pid;
};

static inline struct pid *get_pid(struct pid *pid)
{
	if (pid)
		atomic_inc(&pid->count);
	return pid;
}

extern void put_pid(struct pid *pid);
extern struct task_struct *pid_task(struct pid *pid, enum pid_type);
extern struct task_struct *get_pid_task(struct pid *pid, enum pid_type);

extern struct pid *get_task_pid(struct task_struct *task, enum pid_type type);

/*
 * these helpers must be called with the tasklist_lock write-held.
 */
extern void attach_pid(struct task_struct *task, enum pid_type);
extern void detach_pid(struct task_struct *task, enum pid_type);
extern void change_pid(struct task_struct *task, enum pid_type,
			struct pid *pid);
extern void transfer_pid(struct task_struct *old, struct task_struct *new,
			 enum pid_type);

struct pid_namespace;
extern struct pid_namespace init_pid_ns;

/*
 * look up a PID in the hash table. Must be called with the tasklist_lock
 * or rcu_read_lock() held.
 *
 * find_pid_ns() finds the pid in the namespace specified
 * find_vpid() finds the pid by its virtual id, i.e. in the current namespace
 *
 * see also find_task_by_vpid() set in include/linux/sched.h
 */
extern struct pid *find_pid_ns(int nr, struct pid_namespace *ns);
extern struct pid *find_vpid(int nr);

/*
 * Lookup a PID in the hash table, and return with it's count elevated.
 */
extern struct pid *find_get_pid(int nr);
extern struct pid *find_ge_pid(int nr, struct pid_namespace *);
int next_pidmap(struct pid_namespace *pid_ns, unsigned int last);

extern struct pid *alloc_pid(struct pid_namespace *ns);
extern void free_pid(struct pid *pid);
extern void disable_pid_allocation(struct pid_namespace *ns);

/*
 * ns_of_pid() returns the pid namespace in which the specified pid was
 * allocated.
 *
 * NOTE:
 * 	ns_of_pid() is expected to be called for a process (task) that has
 * 	an attached 'struct pid' (see attach_pid(), detach_pid()) i.e @pid
 * 	is expected to be non-NULL. If @pid is NULL, caller should handle
 * 	the resulting NULL pid-ns.
 */
static inline struct pid_namespace *ns_of_pid(struct pid *pid)
{
	struct pid_namespace *ns = NULL;
	if (pid)
		ns = pid->numbers[pid->level].ns;
	return ns;
}

/*
 * is_child_reaper returns true if the pid is the init process
 * of the current namespace. As this one could be checked before
 * pid_ns->child_reaper is assigned in copy_process, we check
 * with the pid number.
 */
static inline bool is_child_reaper(struct pid *pid)
{
	return pid->numbers[pid->level].nr == 1;
}

/*
 * the helpers to get the pid's id seen from different namespaces
 *
 * pid_nr()    : global id, i.e. the id seen from the init namespace;
 * pid_vnr()   : virtual id, i.e. the id seen from the pid namespace of
 *               current.
 * pid_nr_ns() : id seen from the ns specified.
 *
 * see also task_xid_nr() etc in include/linux/sched.h
 */


//
// struct pid 에 해당하는 여러개의 namespace 가 있을 수 있음 즉 각각 namespace 마다 
// 다른 local pid 를 가지는 것임 
// 이 함수는 그 struct pid 에서 pid 를 처음 할당받아 진짜 init process 에 의해 보여지는 
// global id 값을 가져옴 
// (global id 값은 numbers 배열 즉 upid 배열의 첫번째 element 임)
//
static inline pid_t pid_nr(struct pid *pid)
{
	pid_t nr = 0;
	if (pid)
		nr = pid->numbers[0].nr;
	return nr;
}

pid_t pid_nr_ns(struct pid *pid, struct pid_namespace *ns);
pid_t pid_vnr(struct pid *pid);

#define do_each_pid_task(pid, type, task)				\
	do {								\
		if ((pid) != NULL)					\
			hlist_for_each_entry_rcu((task),		\
				&(pid)->tasks[type], pids[type].node) {

			/*
			 * Both old and new leaders may be attached to
			 * the same pid in the middle of de_thread().
			 */
#define while_each_pid_task(pid, type, task)				\
				if (type == PIDTYPE_PID)		\
					break;				\
			}						\
	} while (0)

#define do_each_pid_thread(pid, type, task)				\
	do_each_pid_task(pid, type, task) {				\
		struct task_struct *tg___ = task;			\
		for_each_thread(tg___, task) {

#define while_each_pid_thread(pid, type, task)				\
		}							\
		task = tg___;						\
	} while_each_pid_task(pid, type, task)
#endif /* _LINUX_PID_H */
