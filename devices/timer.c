#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* 8254 타이머 칩에 대한 하드웨어 세부사항은 [8254]를 참조하세요. */

#if TIMER_FREQ < 19
#error 8254 타이머는 TIMER_FREQ >= 19 필요
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 권장
#endif

static struct list sleep_list;  // 스레드를 재울 대기 리스트

/* OS가 부팅된 이후 지난 타이머 틱(tick)의 수를 저장하는 변수 */
static int64_t ticks;

/* 타이머 틱당 반복 횟수 (짧은 지연을 구현하기 위해 사용됨).
   timer_calibrate() 함수에 의해 초기화됩니다. */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;  // 타이머 인터럽트 핸들러 함수 포인터
static bool too_many_loops (unsigned loops);  // 반복 횟수가 너무 많은지 확인하는 함수
static void busy_wait (int64_t loops);  // 짧은 지연을 위한 반복 대기 함수
static void real_time_sleep (int64_t num, int32_t denom);  // 실수 기반의 대기 함수

/* wake_up_tick 기준으로 정렬하기 위한 비교 함수 */
bool cmp_wake_up_ticks(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
    struct thread *t_a = list_entry(a, struct thread, elem);
    struct thread *t_b = list_entry(b, struct thread, elem);
    
    // 먼저 wake_up_tick 기준으로 정렬
    if (t_a->wake_up_tick != t_b->wake_up_tick) {
        return t_a->wake_up_tick < t_b->wake_up_tick;
    }
    // wake_up_tick이 같으면 우선순위 기준으로 정렬
    return t_a->priority > t_b->priority;
}

/* 
   8254 프로그래머블 인터벌 타이머(PIT)를 설정하여 
   매 초마다 PIT_FREQ 횟수로 인터럽트를 발생시키고,
   타이머 인터럽트를 등록합니다.
*/
void timer_init (void) {
    /* 8254의 입력 주파수를 TIMER_FREQ로 나눈 값을 반올림하여 설정 */
    uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

    outb (0x43, 0x34);    /* CW: counter 0, LSB 먼저, 모드 2, 이진수 형식 */
    outb (0x40, count & 0xff);  // LSB 전송
    outb (0x40, count >> 8);    // MSB 전송

    // 인터럽트 벡터 0x20에 타이머 인터럽트 핸들러 등록
    intr_register_ext (0x20, timer_interrupt, "8254 Timer");
    list_init(&sleep_list);  // sleep_list 초기화
}

/* 
   짧은 지연을 구현하기 위해 loops_per_tick 값을 보정하는 함수.
   loops_per_tick은 한 틱당 반복 횟수를 나타냅니다.
*/
void timer_calibrate (void) {
    unsigned high_bit, test_bit;

    ASSERT (intr_get_level () == INTR_ON);  // 인터럽트가 활성화되어 있어야 함
    printf ("Calibrating timer...  ");

    /* 타이머 틱 내에서 가능한 가장 큰 2의 제곱수를 loops_per_tick으로 초기화 */
    loops_per_tick = 1u << 10;
    while (!too_many_loops (loops_per_tick << 1)) {
        loops_per_tick <<= 1;
        ASSERT (loops_per_tick != 0);
    }

    /* loops_per_tick의 다음 8비트를 세밀하게 보정하여 더 정확하게 설정 */
    high_bit = loops_per_tick;
    for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
        if (!too_many_loops (high_bit | test_bit))
            loops_per_tick |= test_bit;

    printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* OS가 부팅된 이후 전체 타이머 틱 수를 반환 */
int64_t timer_ticks (void) {
    enum intr_level old_level = intr_disable ();  // 인터럽트 비활성화
    int64_t t = ticks;  // 현재 틱 수 저장
    intr_set_level (old_level);  // 이전 인터럽트 상태 복구
    barrier ();  // 최적화 방지용 메모리 장벽
    return t;
}

/* 주어진 시간(then) 이후 경과된 타이머 틱 수를 반환 */
/* start 시점부터 경과한 틱 수를 반환*/
int64_t timer_elapsed (int64_t then) {
    return timer_ticks () - then;
}

/* 
   현재 실행 중인 스레드를 약 TICKS 틱 동안 대기시킴.
   여기서는 busy-waiting 방식을 사용하여 CPU를 점유한 상태로 대기합니다.
*/
// void timer_sleep (int64_t ticks) {
//     int64_t start = timer_ticks ();  // 시작 시점의 타이머 틱 수를 저장

//     ASSERT (intr_get_level () == INTR_ON);  // 인터럽트가 활성화되어 있어야 함
//     while (timer_elapsed (start) < ticks)  // 지정된 틱 수가 지날 때까지 반복
//         thread_yield ();  // 다른 스레드에게 CPU를 양보 (busy-waiting 방식)
// }

//  ***************************추가된 코드 내용***************************
void timer_sleep(int64_t ticks) {
    int64_t start = timer_ticks();

    ASSERT(intr_get_level() == INTR_ON);
    if (ticks <= 0) return;

    struct thread *current_thread = thread_current();
    enum intr_level old_level = intr_disable();  // 인터럽트 비활성화로 안전하게 조작

    current_thread->wake_up_tick = start + ticks;  // 깨어나야 할 시간 설정
    list_insert_ordered(&sleep_list, &current_thread->elem, cmp_wake_up_ticks, NULL);  // 대기 리스트에 삽입

    thread_block();  // 현재 스레드를 재움
    intr_set_level(old_level);  // 이전 인터럽트 레벨로 복구
}


/* 타이머 인터럽트 핸들러 */
void timer_interrupt(struct intr_frame *args UNUSED) {
    ticks++;  // 타이머 틱 증가
    thread_tick();  // 스레드 틱 함수 호출

    /* 깨어나야 할 스레드들을 검사하고 깨움 */
    while (!list_empty(&sleep_list)) {
        struct thread *t = list_entry(list_front(&sleep_list), struct thread, elem);

        // 깨어날 시간이 아직 이르다면, 반복 종료
        if (t->wake_up_tick > ticks) break;

        // 깨어날 시간이 된 스레드가 있으면 리스트에서 제거하고 깨움
        list_pop_front(&sleep_list);
        thread_unblock(t);  // 스레드를 깨움
    }
}

//  *********************************************************************

/* 약 MS 밀리초 동안 실행 중단 */
void timer_msleep (int64_t ms) {
    real_time_sleep (ms, 1000);  // 1초에 1000밀리초
}

/* 약 US 마이크로초 동안 실행 중단 */
void timer_usleep (int64_t us) {
    real_time_sleep (us, 1000 * 1000);  // 1초에 1000000 마이크로초
}

/* 약 NS 나노초 동안 실행 중단 */
void timer_nsleep (int64_t ns) {
    real_time_sleep (ns, 1000 * 1000 * 1000);  // 1초에 1000000000 나노초
}

/* 현재까지 발생한 타이머 틱 수를 출력 */
void timer_print_stats (void) {
    printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* LOOPS 횟수만큼 반복하는 데 한 타이머 틱 이상 걸리는지 확인 */
static bool too_many_loops (unsigned loops) {
    int64_t start = ticks;  // 시작 틱 수 저장
    while (ticks == start)
        barrier ();  // 최적화 방지용 메모리 장벽

    /* LOOPS 횟수만큼 반복 */
    start = ticks;
    busy_wait (loops);

    /* 반복 중 틱 수가 변경되었으면 반복이 너무 긴 것 */
    barrier ();
    return start != ticks;
}

/* 
   짧은 지연을 구현하기 위해 LOOPS 횟수만큼 반복
   NO_INLINE 키워드로 인해 인라인되지 않아 정확한 타이밍 유지
*/
static void NO_INLINE busy_wait (int64_t loops) {
    while (loops-- > 0)
        barrier ();  // 최적화 방지용 메모리 장벽
}

/* 
   약 NUM/DENOM 초 동안 대기.
   주어진 시간을 타이머 틱 단위로 변환하여 대기하거나,
   경우에 따라 busy-waiting 방식을 사용
*/
static void real_time_sleep (int64_t num, int32_t denom) {
    /* NUM/DENOM 초를 타이머 틱으로 변환 */
    int64_t ticks = num * TIMER_FREQ / denom;

    ASSERT (intr_get_level () == INTR_ON);  // 인터럽트가 활성화되어 있어야 함
    if (ticks > 0) {
        /* 타이머 틱 단위로 대기할 수 있는 경우 timer_sleep() 사용 */
        timer_sleep (ticks);
    } else {
        /* 짧은 지연을 위해 busy-waiting 사용.
           오버플로 방지를 위해 분모와 분자를 1000으로 축소 */
        ASSERT (denom % 1000 == 0);
        busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
    }
}