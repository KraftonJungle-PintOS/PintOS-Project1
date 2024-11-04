#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

void* thread_func1(void* arg) {
    for (int i = 0; i < 5; i++) {
        printf("Thread 1 is running\n");
        sleep(1); // 1초 대기하여 다른 스레드가 실행될 수 있도록 함
    }
    return NULL;
}

void* thread_func2(void* arg) {
    for (int i = 0; i < 5; i++) {
        printf("Thread 2 is running\n");
        sleep(1); // 1초 대기하여 다른 스레드가 실행될 수 있도록 함
    }
    return NULL;
}

int main() {
    pthread_t t1, t2;

    // 두 개의 스레드를 생성
    pthread_create(&t1, NULL, thread_func1, NULL);
    pthread_create(&t2, NULL, thread_func2, NULL);

    // 각 스레드가 종료될 때까지 기다림
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    printf("Both threads have finished.\n");
    return 0;
}
