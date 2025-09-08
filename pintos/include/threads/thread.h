#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#ifdef VM
#include "vm/vm.h"
#endif


/* States in a thread's life cycle. */
enum thread_status {
	THREAD_RUNNING,     /* Running thread. */
	THREAD_READY,       /* Not running but ready to run. */
	THREAD_BLOCKED,     /* Waiting for an event to trigger. */
	THREAD_DYING        /* About to be destroyed. */
};

/* 스레드 식별자 타입입니다. 원하는 타입으로 재정의할 수 있습니다. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

/*
	커널 스레드 또는 사용자 프로세스입니다.
	각 스레드 구조체는 자체 4kB 페이지에 저장됩니다. 
	스레드 구조체 자체는 페이지의 맨 아래(오프셋 0)에 위치합니다. 
	페이지의 나머지 부분은 스레드의 커널 스택을 위해 예약되며, 이는 페이지 상단(오프셋 4kB)에서 아래쪽으로 증가합니다. 
	다음은 그림입니다:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * 
 	이것의 결과는 두 가지입니다:
	1. 첫째, struct thread가 너무 커지는 것을 허용해서는 안 됩니다. 
	그렇게 되면 커널 스택을 위한 충분한 공간이 없을 것입니다. 우리의 기본 struct thread는 몇 바이트 크기에 불과합니다. 
	아마도 1kB 미만으로 유지되어야 할 것입니다.
	2. 둘째, 커널 스택이 너무 크게 증가하는 것을 허용해서는 안 됩니다. 
	스택이 오버플로우되면 스레드 상태가 손상됩니다. 따라서 커널 함수는 비정적 지역 변수로 큰 구조체나 배열을 할당해서는 안 됩니다.
	대신 malloc()이나 palloc_get_page()를 사용한 동적 할당을 사용하세요.
	이러한 문제들 중 하나의 첫 번째 증상은 아마도 thread_current()에서의 assertion 실패일 것입니다. 
	이는 실행 중인 스레드의 struct thread의 magic 멤버가 THREAD_MAGIC으로 설정되어 있는지 확인합니다. 
	스택 오버플로우는 보통 이 값을 변경하여 assertion을 발생시킵니다. 
*/
/*  
	elem 멤버는 이중 목적을 가집니다. 실행 큐(thread.c)의 요소가 될 수도 있고, 
	세마포어 대기 리스트(synch.c)의 요소가 될 수도 있습니다. 
	이 두 가지 방식으로 사용될 수 있는 이유는 서로 배타적이기 때문입니다: 
	READY 상태의 스레드만 실행 큐에 있고, 
	BLOCKED 상태의 스레드만 세마포어 대기 리스트에 있습니다. 
*/
struct thread {
	/* Owned by thread.c. */
	tid_t tid;                          /* Thread identifier. */
	enum thread_status status;          /* Thread state. */
	char name[16];                      /* Name (for debugging purposes). */
	int priority;                       /* 스케줄링 우선순위. */
	int64_t wake_time;				/* 깨어날 시간 */

	/* Shared between thread.c and synch.c. */
	struct list_elem elem;              /* ready queue나 다른 리스트 연결용. */
	struct list_elem sleeping_list;
#ifdef USERPROG
	/* Owned by userprog/process.c. */
	uint64_t *pml4;                     /* Page map level 4 */
#endif
#ifdef VM
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf;               /* Information for switching */
	unsigned magic;                     /* Detects stack overflow. */
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void do_iret (struct intr_frame *tf);

#endif /* threads/thread.h */
