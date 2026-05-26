#ifndef HONCH_MICROPYTHON_PTHREAD_H
#define HONCH_MICROPYTHON_PTHREAD_H

#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
#include_next <pthread.h>
#else

#include <time.h>

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

typedef int pthread_mutex_t;
typedef int pthread_cond_t;
typedef int pthread_t;

int clock_gettime(clockid_t clock_id, struct timespec *tp);
int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);
int pthread_cond_init(pthread_cond_t *cond, const void *attr);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *deadline);
int pthread_create(pthread_t *thread, const void *attr, void *(*start)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);

#endif

#endif
