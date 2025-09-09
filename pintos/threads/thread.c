#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);
bool priority_less(const struct list_elem *a, const struct list_elem *b, void *aux);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* 실행 중인 스레드를 반환합니다. CPU의 스택 포인터 'rsp'를 읽고, 그것을 페이지의 시작 부분으로 내림차순 반올림합니다. 
'struct thread'는 항상 페이지의 시작 부분에 있고 스택 포인터는 중간 어딘가에 있으므로, 이것으로 현재 스레드를 찾아냅니다 */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* Init the globla thread context */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&destruction_req);

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) {
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	intr_enable ();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* 
주어진 이름 NAME과 초기 PRIORITY를 가진 새로운 커널 스레드를 생성합니다. 
이 스레드는 AUX를 인자로 전달받아 FUNCTION을 실행하고, ready queue에 추가됩니다. 
새 스레드의 thread identifier를 반환하거나, 생성에 실패하면 TID_ERROR를 반환합니다.

thread_start()가 호출된 경우, 새 스레드는 thread_create()가 반환되기 전에 스케줄될 수 있습니다. 
thread_create()가 반환되기 전에 종료될 수도 있습니다. 
반대로, 원래 스레드는 새 스레드가 스케줄되기 전에 임의의 시간 동안 실행될 수 있습니다. 
순서를 보장해야 한다면 세마포어나 다른 형태의 동기화를 사용하세요.

제공된 코드는 새 스레드의 'priority' 멤버를 PRIORITY로 설정하지만, 실제 우선순위 스케줄링은 구현되지 않았습니다. 
우선순위 스케줄링은 Problem 1-3의 목표입니다. 
*/

tid_t
thread_create (const char *name, int priority,
	thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* Allocate thread. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. */
	thread_unblock (t);

	return tid;
}

/* 
현재 스레드를 잠들게 합니다. thread_unblock()에 의해 깨워질 때까지 다시 스케줄되지 않습니다.
이 함수는 인터럽트가 꺼진 상태에서 호출되어야 합니다. 
일반적으로 synch.h에 있는 동기화 기본 요소 중 하나를 사용하는 것이 더 좋은 방법입니다.
*/
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	//현재 실행중인 스래드의 상태값을 대기상태로바꾼다.
	thread_current ()->status = THREAD_BLOCKED;
	//
	schedule ();
}

/* 
차단된 스레드 T를 실행 준비 상태로 전환합니다. T가 차단 상태가 아니라면 오류입니다. 
(실행 중인 스레드를 준비 상태로 만들려면 thread_yield()를 사용하세요.)
이 함수는 실행 중인 스레드를 선점하지 않습니다. 이는 중요할 수 있습니다: 
호출자가 스스로 인터럽트를 비활성화한 경우, 원자적으로 스레드를 차단 해제하고 
다른 데이터를 업데이트할 수 있을 것으로 기대할 수 있습니다. 
*/
void thread_unblock (struct thread *t) {
    enum intr_level old_level;
    
    ASSERT (is_thread (t));
    
    old_level = intr_disable ();
    ASSERT (t->status == THREAD_BLOCKED);
    
    list_insert_ordered(&ready_list, &t->elem, priority_less, NULL);
    
    t->status = THREAD_READY;
    intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* 실행 중인 스레드를 반환합니다. 
이는 running_thread()에 몇 가지 정상성 검사를 추가한 것입니다. 
자세한 내용은 thread.h 상단의 큰 주석을 참조하세요. */
// 현재 실행중인 스레드의 정보에 접근한다.
struct thread* thread_current (void) {
	struct thread *t = running_thread ();

	/* T가 정말로 스레드인지 확인하세요. 이 두 assertion 중 하나라도 실행되면, 
	당신의 스레드가 스택을 오버플로우했을 수 있습니다. 각 스레드는 4kB 미만의 스택을 가지므로,
	몇 개의 큰 자동 배열이나 적당한 재귀 호출도 스택 오버플로우를 일으킬 수 있습니다. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* 실행 중인 스레드의 tid를 반환합니다. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* 현재 스레드를 스케줄에서 제외하고 파괴합니다. 호출자에게 절대 반환되지 않습니다. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* CPU를 양보합니다. 현재 스레드는 잠들지 않으며 스케줄러의 재량에 따라 즉시 다시 스케줄될 수 있습니다. */
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
		list_insert_ordered(&ready_list, &curr->elem, priority_less, NULL);
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

/* 현재 스레드의 우선순위를 NEW_PRIORITY로 설정합니다. */
void
thread_set_priority (int new_priority) {
	thread_current ()->priority = new_priority;
}

/* 현재 스레드의 우선순위를 반환합니다. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: Your implementation goes here */
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* 
유휴 스레드입니다. 다른 스레드가 실행 준비가 되지 않았을 때 실행됩니다.
유휴 스레드는 처음에 thread_start()에 의해 준비 리스트에 놓입니다. 처음에 한 번 스케줄되며, 
그 시점에서 idle_thread를 초기화하고, 전달받은 세마포어를 "up"하여 thread_start()가 계속 진행할 수 있게 한 다음,햣
즉시 차단됩니다. 그 후에 유휴 스레드는 준비 리스트에 나타나지 않습니다. 
준비 리스트가 비어있을 때 특별한 경우로 next_thread_to_run()에 의해 반환됩니다. 
*/
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* 다른 누군가 실행되도록 합니다. */
		intr_disable ();
		thread_block ();

		/* 
		인터럽트를 다시 활성화하고 다음 인터럽트를 기다립니다. 
		'sti' 명령어는 다음 명령어가 완료될 때까지 인터럽트를 비활성화하므로, 
		이 두 명령어는 원자적으로 실행됩니다. 이 원자성은 중요합니다. 
		그렇지 않으면 인터럽트를 다시 활성화하는 것과 다음 인터럽트가 발생하기를 기다리는 사이에 인터럽트가 
		처리되어 최대 한 클록 틱만큼의 시간을 낭비할 수 있습니다.
		[IA32-v2a] "HLT", [IA32-v2b] "STI", 그리고 [IA32-v3a] 7.11.1 "HLT Instruction"을 참조하세요. 
		*/
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* The scheduler runs with interrupts off. */
	function (aux);       /* Execute the thread function. */
	thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;
}

// 우선순위 비교
bool priority_less(const struct list_elem *a, const struct list_elem *b, void *aux) {

	struct thread *ta = list_entry(a, struct thread, elem);
    struct thread *tb = list_entry(b, struct thread, elem);
    
	ASSERT(is_thread(ta));
    ASSERT(is_thread(tb));

    // 높은 우선순위가 앞에 오도록 (내림차순)
    return ta->priority > tb->priority;
}
/* 
스케줄될 다음 스레드를 선택하고 반환합니다. 실행 큐가 비어있지 않다면 실행 큐에서 스레드를 반환해야 합니다. 
(실행 중인 스레드가 계속 실행될 수 있다면, 그것은 실행 큐에 있을 것입니다.) 실행 큐가 비어있다면 idle_thread를 반환합니다 
*/
// static struct thread *
// next_thread_to_run (void) {

// 	if (list_empty (&ready_list))
// 		return idle_thread;
// 	else
// 		return list_entry (list_pop_front (&ready_list), struct thread, elem);
// }
static struct thread *
next_thread_to_run (void) {
    if (list_empty (&ready_list))
        return idle_thread;
    else {
        struct list_elem *e = list_pop_front(&ready_list);
        struct thread *t = list_entry (e, struct thread, elem);
        
        // 디버깅: 유효성 검사
        if (!is_thread(t)) {
            PANIC("Invalid thread in ready_list");
        }
        
        return t;
    }
}

/* Use iretq to launch the thread */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch (struct thread *th) {

	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile (
			/* Store registers that will be used. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	if (curr != next) {
		/* 
		전환된 스레드가 종료 중이라면, 그것의 struct thread를 파괴합니다. 
		thread_exit()가 자기 자신의 발밑을 빼지 않도록 이것은 늦게 일어나야 합니다. 
		페이지가 현재 스택에 의해 사용되고 있기 때문에 여기서는 페이지 해제 요청을 대기열에 넣기만 합니다. 
		실제 파괴 로직은 schedule()의 시작 부분에서 호출될 것입니다..
		 */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch (next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}
