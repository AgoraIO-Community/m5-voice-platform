/* Host regression: compile with the shim and -pthread, then run. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

extern int __wrap_aosl_hal_cond_timedwait(void *, void *, int);
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static bool signalled;

static long long monotonic_ms(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void *signal_later(void *unused)
{
    (void)unused;
    usleep(8000);
    pthread_mutex_lock(&mutex);
    signalled = true;
    pthread_cond_signal(&condition);
    pthread_mutex_unlock(&mutex);
    return NULL;
}

int main(void)
{
    pthread_mutex_lock(&mutex);
    struct timespec old_deadline;
    clock_gettime(CLOCK_MONOTONIC, &old_deadline);
    old_deadline.tv_sec += 1;
    long long began = monotonic_ms();
    assert(pthread_cond_timedwait(&condition, &mutex, &old_deadline) == ETIMEDOUT);
    long long broken_elapsed = monotonic_ms() - began;
    assert(broken_elapsed < 100);

    began = monotonic_ms();
    assert(__wrap_aosl_hal_cond_timedwait(&condition, &mutex, 30) == ETIMEDOUT);
    long long fixed_elapsed = monotonic_ms() - began;
    assert(fixed_elapsed >= 25 && fixed_elapsed < 500);
    assert(pthread_mutex_trylock(&mutex) == EBUSY);

    pthread_t signal_thread;
    assert(pthread_create(&signal_thread, NULL, signal_later, NULL) == 0);
    began = monotonic_ms();
    while (!signalled) {
        assert(__wrap_aosl_hal_cond_timedwait(&condition, &mutex, 1000) == 0);
    }
    long long signal_elapsed = monotonic_ms() - began;
    assert(signal_elapsed < 500);
    assert(pthread_mutex_trylock(&mutex) == EBUSY);
    pthread_mutex_unlock(&mutex);
    pthread_join(signal_thread, NULL);

    assert(__wrap_aosl_hal_cond_timedwait(NULL, &mutex, 10) == -1);
    assert(__wrap_aosl_hal_cond_timedwait(&condition, &mutex, 0) == ETIMEDOUT);
    printf("PASS: old deadline %lld ms; fixed timeout %lld ms; signal %lld ms; mutex restored\n",
           broken_elapsed, fixed_elapsed, signal_elapsed);
    return 0;
}
