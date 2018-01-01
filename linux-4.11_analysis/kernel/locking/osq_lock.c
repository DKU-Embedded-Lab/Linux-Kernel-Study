#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/osq_lock.h>

/*
 * An MCS like lock especially tailored for optimistic spinning for sleeping
 * lock implementations (mutex, rwsem, etc).
 *
 * Using a single mcs node per CPU is safe because sleeping locks should not be
 * called from interrupt context and we have preemption disabled while
 * spinning.
 */
static DEFINE_PER_CPU_SHARED_ALIGNED(struct optimistic_spin_node, osq_node);

/*
 * We use the value 0 to represent "no CPU", thus the encoded value
 * will be the CPU number incremented by 1.
 */
static inline int encode_cpu(int cpu_nr)
{
	return cpu_nr + 1;
}

static inline int node_cpu(struct optimistic_spin_node *node)
{
	return node->cpu - 1;
}

static inline struct optimistic_spin_node *decode_cpu(int encoded_cpu_val)
{
	int cpu_nr = encoded_cpu_val - 1;
    // 원래 CPU 번호
	return per_cpu_ptr(&osq_node, cpu_nr); 
    // cpu_nr 번 core 의 osq_node 라는 optimistic_spin_node 형의 
    // per-CPU variable 주소 가져옴
}

/*
 * Get a stable @node->next pointer, either for unlock() or unqueue() purposes.
 * Can return NULL in case we were the last queued and we updated @lock instead.
 */
static inline struct optimistic_spin_node *
osq_wait_next(struct optimistic_spin_queue *lock,
	      struct optimistic_spin_node *node,
	      struct optimistic_spin_node *prev)
{
	struct optimistic_spin_node *next = NULL;
	int curr = encode_cpu(smp_processor_id());
	int old;

	/*
	 * If there is a prev node in queue, then the 'old' value will be
	 * the prev node's CPU #, else it's set to OSQ_UNLOCKED_VAL since if
	 * we're currently last in queue, then the queue will then become empty.
	 */
	old = prev ? prev->cpu : OSQ_UNLOCKED_VAL;
    // CPU2---CPU5---CPU6  =>  CPU2---CPU5            
    // 이 과정 중에 CPU2 의 per-CPU variable 이 사라진 경우
    // 
    // lock->tail 재설정
	for (;;) { 
        // 현재 cpu 가 osq 의 꼬리라면 tail 을 prev 의 cpu 번호로 바꾸고 break
		if (atomic_read(&lock->tail) == curr &&
		    atomic_cmpxchg_acquire(&lock->tail, curr, old) == curr) {
			/*
			 * We were the last queued, we moved @lock back. @prev
			 * will now observe @lock and will complete its
			 * unlock()/unqueue().
			 */
			break;
		}

		/*
		 * We must xchg() the @node->next value, because if we were to
		 * leave it in, a concurrent unlock()/unqueue() from
		 * @node->next might complete Step-A and think its @prev is
		 * still valid.
		 *
		 * If the concurrent unlock()/unqueue() wins the race, we'll
		 * wait for either @lock to point to us, through its Step-B, or
		 * wait for a new @node->next from its Step-C.
		 */
		if (node->next) {
			next = xchg(&node->next, NULL);
			if (next)
				break;
		}

		cpu_relax();
	}

	return next;
}

bool osq_lock(struct optimistic_spin_queue *lock)
{
	struct optimistic_spin_node *node = this_cpu_ptr(&osq_node); 
    // 현재 processor 의 optimistic_spin_node per-CPU 변수를 가져옴
	struct optimistic_spin_node *prev, *next;
	int curr = encode_cpu(smp_processor_id());
    // osq lock 의 cpu 번호 계산법에 따라 기존 cpu 번호 +1 값을 설정
	int old;

	node->locked = 0;
    // lock 을 잡으려 기다릴 것이므로 0 으로
	node->next = NULL;
	node->cpu = curr;
    // 현재 속한 CPU 번호 초기화

	/*
	 * We need both ACQUIRE (pairs with corresponding RELEASE in
	 * unlock() uncontended, or fastpath) and RELEASE (to publish
	 * the node fields we just initialised) semantics when updating
	 * the lock tail.
	 */
	old = atomic_xchg(&lock->tail, curr);
    // osq wait queue 의 마지막에 추가될 것이므로 osq lock queue 의 tail 에 
    // 현재 curr 값을 대입하고, 기존의 마지막 node 가 속한 CPU 번호를 알아온다.
    //
    // -- optimistic_spin_queue ----
    // |   tail           5->3     |
    // |               (old->curr) |
    // -----------------------------  
    //                              
    //                                                                 prev                                   node
    //              -- optimistic_spin_node -- <-| |------> -- optimistic_spin_node -- <-| |------> -- optimistic_spin_node --
    //              |  prev             NULL |   --|--------|  prev                  |   --|--------|  prev                  | 
    //              |  next                  |------        |  next                  |------        |  next                  |
    //              |  locked           1    |              |  locked           0    |              |  locked          0     |
    //              |  cpu              2    |              |  cpu              5    |              |  cpu           curr 6  |
    //              --------------------------              --------------------------              --------------------------
    // CPU0 ...  CPU1          CPU 2    ...     CPU3  CPU4      ...   CPU5                  ...               CPU6

    if (old == OSQ_UNLOCKED_VAL)
		return true;
    // optimistic_spin_node 가 없다면 즉 osq 에 처음 enqueue 되는 거라면 
    // tail 값이 QSQ_UNLOCKED_VAL 이며, 바로 lock 획득 가능 
    // midpath 에서 lock 잡기 성공!!

	prev = decode_cpu(old);
	node->prev = prev;
	WRITE_ONCE(prev->next, node);
    // prev 와 node 간 연결

	/*
	 * Normally @prev is untouchable after the above store; because at that
	 * moment unlock can proceed and wipe the node element from stack.
	 *
	 * However, since our nodes are static per-cpu storage, we're
	 * guaranteed their existence -- this allows us to apply
	 * cmpxchg in an attempt to undo our queueing.
	 */

	while (!READ_ONCE(node->locked)) { 
        // 이제 계속 자신의 local per-CPU variable 을 확인하며 spinning 하여 
        // lock 을 얻을 수 있는지 확인 
		/*
		 * If we need to reschedule bail... so we can block.
		 * Use vcpu_is_preempted() to avoid waiting for a preempted
		 * lock holder:
		 */
		if (need_resched() || vcpu_is_preempted(node_cpu(node->prev)))
			goto unqueue;
        // 현재 thread 가 선점되어야 하는경우 즉 현재 spinning 하는 thread 
        // 보다 높은 우선순위의 task 가 요청을 하는 경우 midpath 를 포기하고 
        // 빠져나가 slowpath 로 lock 대기
		cpu_relax();
        // NOP(No Operation) 수행
	}
	return true;
    // 위의 loop 를 빠져나온 것은 node->locked 가 1 로 변경 즉 
    // lock acquire 성공한 것이므로 midpath 에서 lock 잡기 성공!!

unqueue:
	/*
	 * Step - A  -- stabilize @prev
	 *
	 * Undo our @prev->next assignment; this will make @prev's
	 * unlock()/unqueue() wait for a next pointer since @lock points to us
	 * (or later).
	 */

    // 
    // midpath 에서 spinning 하며 lock 잡으려 하다가 현재 thread 보다
    // 우선순위 높은 놈에 의해 선점 요청이 들어온 경우 midpath 를 포기 
    // 해야 하므로 queue 에서 삭제하고 slowpath 로 전환작업 수행
	for (;;) {
		if (prev->next == node &&
		    cmpxchg(&prev->next, node, NULL) == node)
			break; 
        // CPU2---CPU5---CPU6  =>  CPU2---CPU5            

		/*
		 * We can only fail the cmpxchg() racing against an unlock(),
		 * in which case we should observe @node->locked becomming
		 * true.
		 */ 
        // 위에 osq 재설정 과정이 성공하지 못한다면 아직 osq 에 node 를 
        // 추가하려는 구성 중에 수행된 것일 수 있으므로
		if (smp_load_acquire(&node->locked))
			return true;

		cpu_relax();
        // NOP(No Operation) 수행
		/*
		 * Or we race against a concurrent unqueue()'s step-B, in which
		 * case its step-C will write us a new @node->prev pointer.
		 */
		prev = READ_ONCE(node->prev); 
        // 순서 보장이 되었으니 다시 osq 재구성을 위해 prev 를 다시 
        // 읽어들임
	}
    // 
    // CPU 5 에서 선점 요청이 들어온 상태라면
    // -- optimistic_spin_queue ----
    // |   tail           5->3     |
    // |               (old->curr) |
    // -----------------------------  
    //                              
    //                                                                node
    //              -- optimistic_spin_node -- <-| |------> -- optimistic_spin_node --
    //              |  prev             NULL |   --|--------|  prev                  | 
    //              |  next                  |------        |  next                  |
    //              |  locked           1    |              |  locked          0     |
    //              |  cpu              2    |              |  cpu           curr 6  |
    //              --------------------------              --------------------------
    // CPU0 ...  CPU1          CPU 2    ...  CPU3..CPU4..CPU5  ...     CPU6
    //



	/*
	 * Step - B -- stabilize @next
	 *
	 * Similar to unlock(), wait for @node->next or move @lock from @node
	 * back to @prev.
	 */

	next = osq_wait_next(lock, node, prev);
    // lock->tail 재설정 및 node->next를 가져옴
    //  e.g. CPU6
	if (!next)
		return false;
    // NULL 인 경우 내가 tail 이였으므로 더이상 작업 없이 종료 
    // 즉 slowpath 로 넘어갈 준비 끝!!
	/*
	 * Step - C -- unlink
	 *
	 * @prev is stable because its still waiting for a new @prev->next
	 * pointer, @next is stable because our @node->next pointer is NULL and
	 * it will wait in Step-A.
	 */

	WRITE_ONCE(next->prev, prev);
	WRITE_ONCE(prev->next, next);
    // CPU 2 와 CPU6 서로 연결
	return false;
}

void osq_unlock(struct optimistic_spin_queue *lock)
{
	struct optimistic_spin_node *node, *next;
	int curr = encode_cpu(smp_processor_id());

	/*
	 * Fast path for the uncontended case.
	 */
	if (likely(atomic_cmpxchg_release(&lock->tail, curr,
					  OSQ_UNLOCKED_VAL) == curr))
		return; 
    // 현재 per-CPU variable 이 마지막이라면 OSQ_UNLOCKED_VAL 설정하여 
    // osq 를 아얘 비우고 끝

	/*
	 * Second most likely case.
	 */
	node = this_cpu_ptr(&osq_node);
    // 현재 cpu 의 optimistic_spin_node per-CPU variable 주소 가져옴
	next = xchg(&node->next, NULL); 
    // 삭제될 현재 node->next 에 NULL 적고 next 주소 가져옴
	if (next) {
        // next 가 있으면 next 의 locked 상태를 1 로 변경하여 
        // 너차례라고 알림
		WRITE_ONCE(next->locked, 1);
		return; 
        // osq 연결 끝고 다음에도 알렸으므로 끝!!
	}

	next = osq_wait_next(lock, node, NULL);
	if (next)
		WRITE_ONCE(next->locked, 1);
}
