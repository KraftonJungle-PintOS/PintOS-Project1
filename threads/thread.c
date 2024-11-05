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

/* struct thread의 `magic` 멤버를 위한 랜덤 값.
   스택 오버플로우를 감지하는 데 사용됩니다.
   자세한 내용은 thread.h 파일 상단의 큰 주석을 참조하세요. */
#define THREAD_MAGIC 0xcd6abf4b

/* 기본 스레드의 랜덤 값입니다.
   이 값은 수정하지 말아야 합니다. */
#define THREAD_BASIC 0xd42df210

/* THREAD_READY 상태의 프로세스 목록입니다.
   실행 준비는 되었지만 실제로 실행 중이지 않은 프로세스들이 포함됩니다. */
static struct list ready_list;

/* 유휴(Idle) 스레드입니다. */
static struct thread *idle_thread;

/* 초기 스레드, init.c의 main()을 실행하는 스레드입니다. */
static struct thread *initial_thread;

/* allocate_tid() 함수에서 사용되는 락입니다. */
static struct lock tid_lock;

/* 스레드 파괴 요청 목록입니다. */
static struct list destruction_req;

/* 통계 정보입니다. */
static long long idle_ticks;    /* 유휴 상태에서 사용된 타이머 틱 수 */
static long long kernel_ticks;  /* 커널 스레드에서 사용된 타이머 틱 수 */
static long long user_ticks;    /* 사용자 프로그램에서 사용된 타이머 틱 수 */

/* 스케줄링 설정입니다. */
#define TIME_SLICE 4            /* 각 스레드에 할당된 타이머 틱 수 */
static unsigned thread_ticks;   /* 마지막 양보 이후 경과한 타이머 틱 수 */

/* false(기본값)이면 라운드 로빈 스케줄러를 사용합니다.
   true이면 다단계 피드백 큐 스케줄러를 사용합니다.
   커널 커맨드 라인 옵션 "-o mlfqs"로 제어됩니다. */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

bool thread_compare_priority (struct list_elem *l, struct list_elem *s, void *aux UNUSED);

/* T가 유효한 스레드를 가리키는 것처럼 보이면 true를 반환합니다. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* 각 스레드는 독립적인 페이지에 위치 (스택은 다른 스레드와 공유되지 않고 고유한 페이지에 배정)
	자신만의 페이지를 가짐
	pg_round_down으로 스택 포인터가 페이지 내 어디를 가리키든 페이지의 시작 주소 얻을 수 있음
	각 스레드가 독립적인 페이지에 배정되기 때문에 스레드마다 스택 포인터가 가르키는 주소가 다르더라도
	각 스레드의 페이지 시작 주소를 안전하게 찾을 수 있음
	CPU의 스택 포인터 `rsp`를 읽고 이를 페이지의 시작 위치 반환
	`struct thread`는 항상 페이지의 시작에 위치
   현재 스레드를 찾는 데 사용할 수 있음. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// thread_start를 위한 전역 디스크립터 테이블입니다.
// gdt는 thread_init 이후 설정되므로 임시 gdt를 먼저 설정합니다.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* 현재 실행 중인 코드를 스레드로 변환하여 스레딩 시스템을 초기화합니다.
   일반적으로는 불가능하지만 loader.S가 스택의 하단을
   페이지 경계에 놓아 예외적으로 가능합니다.
   이 함수 호출 후, thread_create()로 스레드를 만들기 전에
   페이지 할당기를 초기화해야 합니다.
   이 함수가 완료될 때까지 thread_current()를 호출하는 것은 안전하지 않습니다. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* 커널용 임시 gdt를 재로드합니다.
	   이 gdt는 사용자 컨텍스트를 포함하지 않습니다.
	   커널은 사용자 컨텍스트가 포함된 gdt를 gdt_init에서 재구성합니다. */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* 전역 스레드 컨텍스트 초기화 */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&destruction_req);

	/* 실행 중인 스레드에 대한 스레드 구조를 설정합니다. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* 인터럽트를 활성화하여 선점형 스레드 스케줄링을 시작합니다.
   또한 유휴 스레드를 생성합니다. */
void
thread_start (void) {
	/* 유휴 스레드 생성 */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* 선점형 스레드 스케줄링 시작 */
	intr_enable ();

	/* 유휴 스레드가 idle_thread를 초기화할 때까지 대기 */
	sema_down (&idle_started);
}

/* 타이머 인터럽트 핸들러가 타이머 틱마다 호출됩니다.
   외부 인터럽트 컨텍스트에서 실행됩니다. */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* 통계 업데이트 */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* 선점 적용 */
	if (++thread_ticks >= TIME_SLICE)
	// intr_yield_on_return 함수는 직접적으로 CPU 자원은 다른 스레드에게 할당 X
	// 다음 시점에서 스케중링이 이뤄지도록 요청 (현재 인터럽트 핸들러 종료 후 양보될 것 표시)
		intr_yield_on_return ();
}

/* 스레드 통계 출력 */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* 주어진 초기 우선 순위로 NAME이라는 이름의 새 커널 스레드를 생성합니다.
   AUX를 인수로 전달하여 FUNCTION을 실행하며,
   이를 준비 큐에 추가합니다.
   새 스레드의 식별자를 반환하거나 실패 시 TID_ERROR를 반환합니다.
   새 스레드가 스케줄링될 때까지 동기화가 필요할 수 있습니다. */
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* 스레드 할당 */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* 스레드 초기화 */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	/* 스케줄링 시 kernel_thread 호출
	   주의: rdi는 첫 번째 인수, rsi는 두 번째 인수입니다. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* 실행 큐에 추가 */
	thread_unblock (t);
	thread_test_preemption ();

	return tid;
}

/* 현재 스레드를 수면 상태로 전환합니다.
   thread_unblock()에 의해 깨어나기 전까지 스케줄링되지 않습니다.
   이 함수는 인터럽트가 꺼진 상태에서 호출되어야 합니다.
   일반적으로 synch.h의 동기화 원시를 사용하는 것이 좋습니다. */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* 차단된 스레드 T를 실행 준비 상태로 전환합니다.
   T가 차단되지 않았다면 오류입니다.
   현재 실행 중인 스레드를 선점하지 않습니다. */
void thread_unblock (struct thread *t) {
    enum intr_level old_level;

    ASSERT (is_thread (t)); // t가 유효한 스레드인지 확인

    old_level = intr_disable (); // 인터럽트 비활성화
    ASSERT (t->status == THREAD_BLOCKED); // 스레드 상태가 BLOCKED인지 확인

  	list_insert_ordered (&ready_list, &t->elem, thread_compare_priority, 0);
    t->status = THREAD_READY; // 스레드 상태를 READY로 변경

    intr_set_level (old_level); // 이전 인터럽트 레벨 복원
}

bool 
thread_compare_priority (struct list_elem *l, struct list_elem *s, void *aux UNUSED)
{
    return list_entry (l, struct thread, elem)->priority
         > list_entry (s, struct thread, elem)->priority;
}


/* 실행 중인 스레드의 이름을 반환합니다. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* 실행 중인 스레드를 반환합니다.
   이는 running_thread()와 몇 가지 검사를 추가한 것입니다.
   자세한 내용은 thread.h 상단의 주석 참조 */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* T가 진짜 스레드인지 확인.
	   이 어서션이 실패하면 스택 오버플로우가 발생했을 수 있습니다.
	   각 스레드의 스택은 4 kB 미만이므로,
	   큰 자동 배열 또는 중간 재귀가 스택 오버플로우를 유발할 수 있습니다. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* 실행 중인 스레드의 tid를 반환합니다. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* 현재 스레드를 디스케줄링하고 파괴합니다.
   호출자에게 절대 반환하지 않습니다. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* 상태를 죽어가는 상태로 설정하고 다른 프로세스를 스케줄링합니다.
	   schedule_tail()에서 파괴됩니다. */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* CPU를 양보합니다.
   현재 스레드는 잠들지 않고 스케줄러의 판단에 따라 즉시 다시 스케줄링될 수 있습니다. */
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
	    list_insert_ordered (&ready_list, &curr->elem, thread_compare_priority, 0);
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}


//**************************************************************************
// 추가된 내용
void 
thread_test_preemption (void)
{
    if (!list_empty (&ready_list) && 
    thread_current ()->priority < 
    list_entry (list_front (&ready_list), struct thread, elem)->priority)
        thread_yield ();
}

//**************************************************************************

/* 현재 스레드의 우선 순위를 NEW_PRIORITY로 설정합니다. */
void
thread_set_priority (int new_priority) {
	thread_current ()->priority = new_priority;
	thread_test_preemption ();
}

/* 현재 스레드의 우선 순위를 반환합니다. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* 현재 스레드의 nice 값을 NICE로 설정합니다. */
void
thread_set_nice (int nice UNUSED) {
	/* 구현이 필요함 */
}

/* 현재 스레드의 nice 값을 반환합니다. */
int
thread_get_nice (void) {
	/* 구현이 필요함 */
	return 0;
}

/* 시스템 평균 부하의 100배를 반환합니다. */
int
thread_get_load_avg (void) {
	/* 구현이 필요함 */
	return 0;
}

/* 현재 스레드의 최근 CPU 사용량의 100배를 반환합니다. */
int
thread_get_recent_cpu (void) {
	/* 구현이 필요함 */
	return 0;
}

/* 유휴 스레드.
   실행할 다른 스레드가 없을 때 실행됩니다.
   thread_start()에서 초기 실행 큐에 등록됩니다.
   초기화 후 block 상태로 대기합니다. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* 다른 스레드에게 CPU 양보 */
		intr_disable ();
		thread_block ();

		/* 인터럽트를 다시 활성화하고 다음 인터럽트를 대기합니다.
		   `sti` 명령은 다음 명령이 끝날 때까지 인터럽트를 비활성화하므로,
		   이 두 명령은 원자적으로 실행됩니다.
		   원자성은 중요합니다. 그렇지 않으면 인터럽트가 발생하여
		   최대 하나의 클럭 틱이 낭비될 수 있습니다. */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* 커널 스레드의 기초로 사용되는 함수입니다. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* 스케줄러는 인터럽트가 꺼진 상태에서 실행됩니다. */
	function (aux);       /* 스레드 함수를 실행합니다. */
	thread_exit ();       /* function()이 반환되면 스레드를 종료합니다. */
}

/* 차단된 스레드 T를 초기화합니다.
   이름은 NAME으로 지정되고 우선 순위는 PRIORITY로 설정됩니다. */
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

/* 스케줄링할 다음 스레드를 선택하여 반환합니다.
   실행 큐가 비어 있지 않다면 큐에서 스레드를 반환해야 합니다.
   (실행 중인 스레드가 계속 실행될 수 있다면 실행 큐에 있어야 합니다.)
   실행 큐가 비어 있다면 idle_thread를 반환합니다. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* iretq를 사용하여 스레드를 실행합니다 */
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





/* thread_launch 함수 스위칭 로직 어셈블리어 형태로 변환
     ; 현재 사용 중인 레지스터 값을 스택에 저장하여 보존
            push rax                 ; rax (일반적으로 함수 반환값을 저장하는 레지스터), 스택에 임시로 저장
            push rbx                 ; rbx (베이스 레지스터로 사용되는 레지스터), 스택에 임시로 저장
            push rcx                 ; rcx (루프 카운터나 임시 데이터 저장에 사용되는 레지스터), 스택에 임시로 저장

    ; 현재 스레드의 tf 주소를 rax에 로드, 전환할 스레드의 tf 주소를 rcx에 로드
            mov rax, [tf_cur]        ; 현재 스레드의 tf (intr_frame) 구조체 주소를 rax에 로드
            mov rcx, [tf]            ; 전환할 스레드의 tf (intr_frame) 구조체 주소를 rcx에 로드

    ; 현재 스레드의 레지스터 값을 tf_cur에 순서대로 저장

			(r12, r13, r14, r15: 임시 데이터 저장에 사용)
            mov [rax + 0], r15       ; r15 레지스터 값을 현재 스레드의 intr_frame 구조체의 첫 번째 위치에 저장
            mov [rax + 8], r14       ; r14 레지스터 값을 현재 스레드의 intr_frame 구조체의 두 번째 위치에 저장
            mov [rax + 16], r13      ; r13 레지스터 값을 현재 스레드의 intr_frame 구조체의 세 번째 위치에 저장
            mov [rax + 24], r12      ; r12 레지스터 값을 현재 스레드의 intr_frame 구조체의 네 번째 위치에 저장

			(r8, r9, r10, r11: 주로 함수 호출 시 추가 매개변수 전달에 사용)
            mov [rax + 32], r11      ; r11 레지스터 값을 현재 스레드의 intr_frame 구조체의 다섯 번째 위치에 저장
            mov [rax + 40], r10      ; r10 레지스터 값을 현재 스레드의 intr_frame 구조체의 여섯 번째 위치에 저장
            mov [rax + 48], r9       ; r9 레지스터 값을 현재 스레드의 intr_frame 구조체의 일곱 번째 위치에 저장
            mov [rax + 56], r8       ; r8 레지스터 값을 현재 스레드의 intr_frame 구조체의 여덟 번째 위치에 저장

			(rsi, rdi: 함수 호출 시 첫 번째와 두 번째 매개변수 전달)
            mov [rax + 64], rsi      ; rsi 레지스터 값을 현재 스레드의 intr_frame 구조체에 저장
            mov [rax + 72], rdi      ; rdi 레지스터 값을 현재 스레드의 intr_frame 구조체에 저장

			(rbp: 베이스 포인터, 함수 호출 시 스택 프레임을 관리)
            mov [rax + 80], rbp      ; rbp 레지스터 값을 현재 스레드의 intr_frame 구조체에 저장

			(rdx: 함수 호출 시 세 번째 매개변수 전달)
            mov [rax + 88], rdx      ; rdx 레지스터 값을 현재 스레드의 intr_frame 구조체에 저장


    ; 스택에서 rcx, rbx, rax를 복구하여 현재 스레드의 intr_frame에 저장

			(rbx: 베이스 레지스터. 일반적으로 임시 저장 및 베이스 포인터로 사용)

            pop rbx                  ; 스택에서 rcx 값을 복구하여 rbx에 저장 (임시로 저장)
            mov [rax + 96], rbx      ; 복구한 rcx 값을 현재 스레드의 intr_frame 구조체에 저장

            pop rbx                  ; 스택에서 rbx 값을 복구하여 rbx에 저장 (베이스 레지스터 복구)
            mov [rax + 104], rbx     ; rbx 값을 현재 스레드의 intr_frame 구조체에 저장

            pop rbx                  ; 스택에서 rax 값을 복구하여 rbx에 저장 (함수 반환값 복구)
            mov [rax + 112], rbx     ; rax 값을 현재 스레드의 intr_frame 구조체에 저장

    ; tf_cur의 다음 위치로 이동
            add rax, 120             ; 레지스터 값이 모두 저장된 후 tf_cur의 다음 위치로 rax를 이동

    ; 세그먼트 레지스터를 intr_frame에 저장 (해당 부분은 2바이트만으로 충분)
            mov es, [rax]            ; es 세그먼트 값을 현재 스레드의 intr_frame 구조체에 저장
            mov ds, [rax + 8]        ; ds 세그먼트 값을 현재 스레드의 intr_frame 구조체에 저장

    ; tf_cur에서 다음 위치로 이동하여 레지스터 저장 위치를 건너뜀 (32바이트는 구조체의 데이터 정렬과 필드 간 구분 명확히 하기 위해 선택된 간격)
            add rax, 32              

    ; 현재 실행 위치를 설정하기 위한 준비
    __next:
            call __next              ; 현재 RIP 위치를 스택에 저장하고 __next 레이블로 이동
            pop rbx                  ; 스택에서 RIP 값을 복구하여 rbx에 저장
            add rbx, (out_iret - __next) ; RIP에 필요한 오프셋을 더함

    ; 복구한 RIP와 세그먼트 값을 현재 스레드의 intr_frame에 저장
            mov [rax], rbx           ; 복구한 RIP 값을 현재 스레드의 intr_frame 구조체에 저장
            mov cs, [rax + 8]        ; 현재 CS (코드 세그먼트) 값을 현재 스레드의 intr_frame 구조체에 저장

    ; 플래그 레지스터와 스택 포인터 값을 현재 스레드의 intr_frame에 저장
            pushfq                   ; 플래그 레지스터를 스택에 저장
            pop rbx                  ; 스택에서 플래그 값을 rbx로 복구
            mov [rax + 16], rbx      ; 플래그 값을 현재 스레드의 intr_frame 구조체에 저장

            mov rsp, [rax + 24]      ; 현재 스택 포인터를 현재 스레드의 intr_frame 구조체에 저장
            mov ss, [rax + 32]       ; 현재 스택 세그먼트를 현재 스레드의 intr_frame 구조체에 저장

    ; 전환할 스레드의 tf 주소를 rdi에 전달하고 do_iret 호출
            mov rdi, rcx             ; 전환할 스레드의 intr_frame 구조체 주소를 rdi에 전달 (do_iret 호출 시 인자로 전달)
            call do_iret             ; do_iret을 호출하여 다음 스레드로 전환
    out_iret:


*/

static void
thread_launch (struct thread *th) {
    uint64_t tf_cur = (uint64_t) &running_thread ()->tf;  // 현재 스레드의 intr_frame 주소를 tf_cur에 저장
    uint64_t tf = (uint64_t) &th->tf;  // 전환할 스레드 th의 intr_frame 주소를 tf에 저장
    ASSERT (intr_get_level () == INTR_OFF);  // 인터럽트가 비활성화된 상태인지 확인

    /* 주 스위칭 로직.
       intr_frame에 전체 실행 컨텍스트를 복원한 후 do_iret를 호출하여 다음 스레드로 전환합니다.
       주의: 스위칭이 완료될 때까지 스택을 사용하지 말아야 합니다. */
    __asm __volatile (
            /* 사용할 레지스터 저장. */
            "push %%rax\n"  // rax 레지스터 값을 스택에 저장
            "push %%rbx\n"  // rbx 레지스터 값을 스택에 저장
            "push %%rcx\n"  // rcx 레지스터 값을 스택에 저장
            /* 한 번만 입력 가져오기 */
			// "g"(tf_cur), "g" (tf) : "memory"에 의해 tf_cur과 tf 정해짐
            "movq %0, %%rax\n"  // tf_cur 값을 rax에 로드 (현재 스레드의 tf 주소)
            "movq %1, %%rcx\n"  // tf 값을 rcx에 로드 (전환할 스레드의 tf 주소)
            /* 현재 스레드의 레지스터 상태를 tf_cur에 저장 */
            "movq %%r15, 0(%%rax)\n"   // r15 레지스터 값을 tf_cur의 첫 번째 위치에 저장
            "movq %%r14, 8(%%rax)\n"   // r14 레지스터 값을 tf_cur의 두 번째 위치에 저장
            "movq %%r13, 16(%%rax)\n"  // r13 레지스터 값을 tf_cur의 세 번째 위치에 저장
            "movq %%r12, 24(%%rax)\n"  // r12 레지스터 값을 tf_cur의 네 번째 위치에 저장
            "movq %%r11, 32(%%rax)\n"  // r11 레지스터 값을 tf_cur의 다섯 번째 위치에 저장
            "movq %%r10, 40(%%rax)\n"  // r10 레지스터 값을 tf_cur의 여섯 번째 위치에 저장
            "movq %%r9, 48(%%rax)\n"   // r9 레지스터 값을 tf_cur의 일곱 번째 위치에 저장
            "movq %%r8, 56(%%rax)\n"   // r8 레지스터 값을 tf_cur의 여덟 번째 위치에 저장
            "movq %%rsi, 64(%%rax)\n"  // rsi 레지스터 값을 tf_cur에 저장
            "movq %%rdi, 72(%%rax)\n"  // rdi 레지스터 값을 tf_cur에 저장
            "movq %%rbp, 80(%%rax)\n"  // rbp 레지스터 값을 tf_cur에 저장
            "movq %%rdx, 88(%%rax)\n"  // rdx 레지스터 값을 tf_cur에 저장
            "pop %%rbx\n"              // 스택에서 저장해둔 rcx 값을 rbx로 복구
            "movq %%rbx, 96(%%rax)\n"  // rcx 값을 tf_cur에 저장
            "pop %%rbx\n"              // 스택에서 저장해둔 rbx 값을 rbx로 복구
            "movq %%rbx, 104(%%rax)\n" // rbx 값을 tf_cur에 저장
            "pop %%rbx\n"              // 스택에서 저장해둔 rax 값을 rbx로 복구
            "movq %%rbx, 112(%%rax)\n" // rax 값을 tf_cur에 저장
            "addq $120, %%rax\n"       // tf_cur의 다음 위치로 rax 값을 이동 (레지스터 저장 후 다음 공간)
            "movw %%es, (%%rax)\n"     // 현재 es 세그먼트 레지스터를 tf_cur에 저장 , w는 2바이트 크기 데이터 이동 (q는 8바이트 이동)
            "movw %%ds, 8(%%rax)\n"    // 현재 ds 세그먼트 레지스터를 tf_cur에 저장
            "addq $32, %%rax\n"        // tf_cur의 다음 위치로 이동

            /* 다음 실행 위치 설정 */
            "call __next\n"            // 현재 rip(명령어 포인터)를 스택에 저장한 후 __next로 이동
            "__next:\n"
            "pop %%rbx\n"              // 스택에서 rip 값을 가져와 rbx에 저장
            "addq $(out_iret - __next), %%rbx\n"  // out_iret까지의 오프셋을 rbx에 더함
            "movq %%rbx, 0(%%rax)\n"   // rip 값을 tf_cur에 저장 (다음 실행 위치)
            "movw %%cs, 8(%%rax)\n"    // 현재 cs 세그먼트를 tf_cur에 저장 (코드 세그먼트)
            "pushfq\n"                 // 플래그 레지스터를 스택에 저장
            "popq %%rbx\n"             // 스택에서 플래그 값을 rbx로 복구
            "mov %%rbx, 16(%%rax)\n"   // 플래그 값을 tf_cur에 저장 (eflags)
            "mov %%rsp, 24(%%rax)\n"   // 현재 스택 포인터 값을 tf_cur에 저장 (rsp)
            "movw %%ss, 32(%%rax)\n"   // 현재 스택 세그먼트 값을 tf_cur에 저장 (ss)
            
            /* 전환할 스레드로의 문맥 복원 준비 */
            "mov %%rcx, %%rdi\n"       // 전환할 스레드의 tf 주소를 rdi에 전달 (do_iret의 인자)
            "call do_iret\n"           // do_iret 호출로 전환할 스레드의 실행 상태를 복원하고 실행 시작

            "out_iret:\n"              // do_iret 이후 복귀 지점
            : : "g"(tf_cur), "g" (tf) : "memory"
    );
}



/* 새로운 프로세스를 스케줄링합니다.
   이 함수가 실행될 때는 인터럽트가 꺼져 있어야 합니다.
   이 함수는 현재 스레드의 상태를 status로 변경한 후 실행할 다른 스레드를 찾아 스위칭합니다.
   schedule() 함수 내에서는 printf() 호출이 안전하지 않습니다. */
static void do_schedule(int status) {
    ASSERT (intr_get_level () == INTR_OFF);  // 인터럽트가 비활성화 상태인지 확인
    ASSERT (thread_current()->status == THREAD_RUNNING);  // 현재 스레드가 실행 중인지 확인

    while (!list_empty(&destruction_req)) {  // 파괴 요청 리스트가 비어있지 않다면 반복
        struct thread *victim =
            list_entry(list_pop_front(&destruction_req), struct thread, elem);
        palloc_free_page(victim);  // 파괴된 스레드의 메모리 페이지를 해제
    }

    thread_current()->status = status;  // 현재 스레드의 상태를 인자로 전달된 상태로 설정
    schedule();  // 스케줄러를 호출하여 다음 스레드로 전환
}


static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));

	/* 현재 스레드를 실행 중으로 표시 */
	next->status = THREAD_RUNNING;

	/* 새로운 타임 슬라이스 시작 */
	thread_ticks = 0;

#ifdef USERPROG
	/* 새 주소 공간 활성화 */
	process_activate (next);
#endif

	if (curr != next) {
		/* 새로운 스레드(next)로 전환할 때 처리해야 할 몇 가지 중요한 작업을 수행
			스레드가 스스로를 종료할 때 직접 메모리를 해제하는 데 따른 위험
		   다음 스케줄링 과정에서 안전하게 메모리를 해제할 수 있도록 처리
		   실제 파괴 로직은 schedule()의 시작에서 호출됨 */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* 스레드 전환 전에 현재 실행 상태 정보를 저장 
		CPU 문맥 전환 작업으로 스겔드 중단된 시점의 상태 저장 
		다음에 실행한 next 문맥을 복원하여 실행 이어감 */
		thread_launch (next);
	}
}

/* 새 스레드에 사용할 tid를 반환합니다 */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);  // tid 할당 시 동시 접근 방지
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}
