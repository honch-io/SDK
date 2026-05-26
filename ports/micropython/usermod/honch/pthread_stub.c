#if !defined(__APPLE__) && !defined(__linux__) && !defined(__unix__)

#include "compat/pthread.h"

int clock_gettime(clockid_t clock_id, struct timespec *tp)
{
    (void)clock_id;
    if (tp != 0) {
        tp->tv_sec = time(0);
        tp->tv_nsec = 0;
    }
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr)
{
    (void)attr;
    if (mutex != 0) {
        *mutex = 0;
    }
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    if (mutex != 0) {
        *mutex = 1;
    }
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    if (mutex != 0) {
        *mutex = 0;
    }
    return 0;
}

int pthread_cond_init(pthread_cond_t *cond, const void *attr)
{
    (void)attr;
    if (cond != 0) {
        *cond = 0;
    }
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
    (void)cond;
    return 0;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
    if (cond != 0) {
        *cond = 1;
    }
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    (void)cond;
    (void)mutex;
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *deadline)
{
    (void)cond;
    (void)mutex;
    (void)deadline;
    return 0;
}

int pthread_create(pthread_t *thread, const void *attr, void *(*start)(void *), void *arg)
{
    (void)thread;
    (void)attr;
    (void)start;
    (void)arg;
    return -1;
}

int pthread_join(pthread_t thread, void **retval)
{
    (void)thread;
    (void)retval;
    return 0;
}

#endif
