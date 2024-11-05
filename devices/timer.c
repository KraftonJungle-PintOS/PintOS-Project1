#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include <stdlib.h>

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

static int64_t ticks;
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops(unsigned loops);
static void busy_wait(int64_t loops);
static void real_time_sleep(int64_t num, int32_t denom);

static bool less_wake_tick(const struct list_elem *a, const struct list_elem *b, void *aux);

/* 잠든 스레드들을 관리하는 리스트 */
static struct list sleep_list;
static struct list awake_list;

/* 잠든 스레드를 관리하기 위한 구조체 */
struct sleep_thread {
    int64_t wake_tick;          /* 스레드가 깨어나야 할 시점 (ticks) */
    struct thread *sleeping_thread; /* 잠든 스레드 */
    struct list_elem elem;      /* 리스트 요소 */
    int priority;               /* 스레드의 우선 순위 추가 */
};

/* wake_tick 기준 정렬을 위한 비교 함수 */
bool less_wake_tick(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
    struct sleep_thread *st_a = list_entry(a, struct sleep_thread, elem);
    struct sleep_thread *st_b = list_entry(b, struct sleep_thread, elem);

    if (st_a->wake_tick == st_b->wake_tick)
        return st_a->priority > st_b->priority;  // 동일 시간일 경우 우선 순위 높은 순으로
    return st_a->wake_tick < st_b->wake_tick;
}

/* sleep_list에 스레드를 추가하는 함수 */
static void add_sleeping_thread(struct thread *t, int64_t wake_tick) {
    struct sleep_thread *st = malloc(sizeof(struct sleep_thread));
    ASSERT(st != NULL);

    st->wake_tick = wake_tick;
    st->sleeping_thread = t;
    st->priority = thread_get_priority();

    enum intr_level old_level = intr_disable();
    list_insert_ordered(&sleep_list, &st->elem, less_wake_tick, NULL);
    intr_set_level(old_level);
}

static void wake_sleeping_threads(void) {
    while (!list_empty(&sleep_list)) {
        struct list_elem *e = list_front(&sleep_list);
        struct sleep_thread *st = list_entry(e, struct sleep_thread, elem);

        if (st->wake_tick > ticks)  // 아직 깰 시점이 되지 않음
            break;

        list_pop_front(&sleep_list);  
        thread_unblock(st->sleeping_thread);  // 스레드를 준비 상태로 전환

        // awake_list에 추가하여 메모리 해제를 나중에 수행
        list_push_back(&awake_list, &st->elem);
    }
}

/* Timer interrupt handler. */
static void timer_interrupt(struct intr_frame *args UNUSED) {
    ticks++;
    wake_sleeping_threads();  // 깨울 스레드가 있는지 확인
    thread_tick();
}

/* 주기적으로 호출하여 awake_list의 메모리를 해제하는 함수 */
void cleanup_awake_list(void) {
    enum intr_level old_level = intr_disable();  // 안전한 동기화를 위해 인터럽트 비활성화
    while (!list_empty(&awake_list)) {
        struct list_elem *e = list_pop_front(&awake_list);
        struct sleep_thread *st = list_entry(e, struct sleep_thread, elem);
        free(st);  // 메모리 해제
    }
    intr_set_level(old_level);  // 인터럽트 복원
}

/* Suspends execution for approximately TICKS timer ticks. */
void timer_sleep(int64_t ticks) {
    if (ticks <= 0) return;

    int64_t wake_tick = timer_ticks() + ticks;

    enum intr_level old_level = intr_disable();
    add_sleeping_thread(thread_current(), wake_tick);  // 슬립 리스트에 추가
    thread_block();  // 스레드를 블록 상태로 전환
    intr_set_level(old_level);
}

/* Sets up the 8254 Programmable Interval Timer (PIT) to
   interrupt PIT_FREQ times per second, and registers the
   corresponding interrupt. */
void timer_init(void) {
    uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

    outb(0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
    outb(0x40, count & 0xff);
    outb(0x40, count >> 8);

    intr_register_ext(0x20, timer_interrupt, "8254 Timer");
    list_init(&sleep_list);    
    list_init(&awake_list); 
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void timer_calibrate(void) {
    unsigned high_bit, test_bit;

    ASSERT(intr_get_level() == INTR_ON);
    printf("Calibrating timer...  ");

    /* Approximate loops_per_tick as the largest power-of-two
       still less than one timer tick. */
    loops_per_tick = 1u << 10;
    while (!too_many_loops(loops_per_tick << 1)) {
        loops_per_tick <<= 1;
        ASSERT(loops_per_tick != 0);
    }

    /* Refine the next 8 bits of loops_per_tick. */
    high_bit = loops_per_tick;
    for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
        if (!too_many_loops(high_bit | test_bit))
            loops_per_tick |= test_bit;

    printf("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t timer_ticks(void) {
    enum intr_level old_level = intr_disable();
    int64_t t = ticks;
    intr_set_level(old_level);
    barrier();
    return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t timer_elapsed(int64_t then) {
    return timer_ticks() - then;
}

/* Suspends execution for approximately TICKS timer ticks. */
// void timer_sleep(int64_t ticks) {
//     int64_t start = timer_ticks();

//     ASSERT(intr_get_level() == INTR_ON);
//     while (timer_elapsed(start) < ticks)
//         thread_yield();
// }

/* Suspends execution for approximately MS milliseconds. */
void timer_msleep(int64_t ms) {
    real_time_sleep(ms, 1000);
}

/* Suspends execution for approximately US microseconds. */
void timer_usleep(int64_t us) {
    real_time_sleep(us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. */
void timer_nsleep(int64_t ns) {
    real_time_sleep(ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void timer_print_stats(void) {
    printf("Timer: %"PRId64" ticks\n", timer_ticks());
}

/* Returns true if LOOPS iterations waits for more than one timer tick, otherwise false. */
static bool too_many_loops(unsigned loops) {
    /* Wait for a timer tick. */
    int64_t start = ticks;
    while (ticks == start)
        barrier();

    /* Run LOOPS loops. */
    start = ticks;
    busy_wait(loops);

    /* If the tick count changed, we iterated too long. */
    barrier();
    return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing brief delays. */
static void NO_INLINE busy_wait(int64_t loops) {
    while (loops-- > 0)
        barrier();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void real_time_sleep(int64_t num, int32_t denom) {
    /* Convert NUM/DENOM seconds into timer ticks, rounding down.
       (NUM / DENOM) s
       ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
       1 s / TIMER_FREQ ticks
    */
    int64_t ticks = num * TIMER_FREQ / denom;

    ASSERT(intr_get_level() == INTR_ON);
    if (ticks > 0) {
        /* We're waiting for at least one full timer tick.  Use
           timer_sleep() because it will yield the CPU to other processes. */
        timer_sleep(ticks);
    } else {
        /* Otherwise, use a busy-wait loop for more accurate
           sub-tick timing.  We scale the numerator and denominator
           down by 1000 to avoid the possibility of overflow. */
        ASSERT(denom % 1000 == 0);
        busy_wait(loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
    }
}
