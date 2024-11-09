#ifndef DEVICES_TIMER_H
#define DEVICES_TIMER_H

#include <round.h>
#include <stdint.h>

/* Number of timer interrupts per second. */
#define TIMER_FREQ 100

void timer_init (void);
void timer_calibrate (void);

int64_t timer_ticks (void);
int64_t timer_elapsed (int64_t);

void timer_sleep (int64_t ticks);
void timer_msleep (int64_t milliseconds);
void timer_usleep (int64_t microseconds);
void timer_nsleep (int64_t nanoseconds);

void timer_print_stats (void);

/* sleep_thread 구조체 정의 */
struct sleep_thread {
    struct thread *t;  // 대기 중인 스레드
    int64_t wake_up_time;  // 스레드가 깨어날 시점 (tick 단위)
    struct list_elem elem;  // 리스트에 포함될 요소
};


#endif /* devices/timer.h */