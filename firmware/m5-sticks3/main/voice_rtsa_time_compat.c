/*
 * The bundled AOSL archive builds pthread condition deadlines with
 * clock_gettime(CLOCK_MONOTONIC), but creates the conditions with default
 * pthread_cond_init(..., NULL) attributes. ESP-IDF's pthread implementation
 * compares the absolute deadline with gettimeofday() (CLOCK_REALTIME).
 * After SNTP sets wall time, every monotonic deadline is already expired:
 * the SDK's high-priority polling task spins instead of blocking.
 *
 * Interpose only this AOSL HAL boundary with
 *   -Wl,--wrap=aosl_hal_cond_timedwait
 * Other SDK monotonic clocks and the system pthread implementation retain
 * their original behavior. The archive already uses 64-bit time_t; global
 * time-ABI wrappers would be both unnecessary and harmful.
 */
#include <errno.h>
#include <pthread.h>
#include <time.h>

int __wrap_aosl_hal_cond_timedwait(void *condition, void *mutex, int timeout_ms)
{
    /* These opaque AOSL handles point directly to pthread objects; confirmed
     * by the archive's aosl_hal_cond_create / aosl_hal_mutex_create. The HAL
     * returns pthread error codes directly, and -1 for a null handle. */
    if (condition == NULL || mutex == NULL) {
        return -1;
    }
    if (timeout_ms <= 0) {
        return ETIMEDOUT;
    }

    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return errno != 0 ? errno : EINVAL;
    }
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait((pthread_cond_t *)condition,
                                  (pthread_mutex_t *)mutex, &deadline);
}
