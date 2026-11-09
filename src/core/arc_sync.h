/* arc_sync：解包并行写盘的最小线程/同步原语（DESIGN-parallel-unpack）。
 * Win32 走 SRWLOCK + CONDITION_VARIABLE + _beginthreadex（CRT 线程安全），
 * POSIX 走 pthread；无第三方依赖。仅 archive.c 消费，头文件即实现。 */
#ifndef OMNIC_ARC_SYNC_H
#define OMNIC_ARC_SYNC_H

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif

#ifdef _WIN32

typedef SRWLOCK arc_mutex_t;
typedef CONDITION_VARIABLE arc_cond_t;
typedef HANDLE arc_thread_t;
typedef unsigned (__stdcall *arc_thread_fn_t)(void *);
#define ARC_THREAD_FN static unsigned __stdcall
#define ARC_THREAD_RET_OK return 0

static void arc_mutex_init(arc_mutex_t *m) { InitializeSRWLock(m); }
static void arc_mutex_lock(arc_mutex_t *m) { AcquireSRWLockExclusive(m); }
static void arc_mutex_unlock(arc_mutex_t *m) { ReleaseSRWLockExclusive(m); }
static void arc_cond_init(arc_cond_t *c) { InitializeConditionVariable(c); }
static void arc_cond_wait(arc_cond_t *c, arc_mutex_t *m) {
    SleepConditionVariableSRW(c, m, INFINITE, 0);
}
static void arc_cond_signal(arc_cond_t *c) { WakeConditionVariable(c); }
static void arc_cond_broadcast(arc_cond_t *c) { WakeAllConditionVariable(c); }
static int arc_thread_create(arc_thread_t *t, arc_thread_fn_t fn, void *arg) {
    *t = (HANDLE)_beginthreadex(NULL, 0, fn, arg, 0, NULL);
    return (*t != NULL) ? 0 : -1;
}
static void arc_thread_join(arc_thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

#else /* POSIX */

typedef pthread_mutex_t arc_mutex_t;
typedef pthread_cond_t arc_cond_t;
typedef pthread_t arc_thread_t;
typedef void *(*arc_thread_fn_t)(void *);
#define ARC_THREAD_FN static void *
#define ARC_THREAD_RET_OK return NULL

static void arc_mutex_init(arc_mutex_t *m) { pthread_mutex_init(m, NULL); }
static void arc_mutex_lock(arc_mutex_t *m) { pthread_mutex_lock(m); }
static void arc_mutex_unlock(arc_mutex_t *m) { pthread_mutex_unlock(m); }
static void arc_cond_init(arc_cond_t *c) { pthread_cond_init(c, NULL); }
static void arc_cond_wait(arc_cond_t *c, arc_mutex_t *m) {
    pthread_cond_wait(c, m);
}
static void arc_cond_signal(arc_cond_t *c) { pthread_cond_signal(c); }
static void arc_cond_broadcast(arc_cond_t *c) { pthread_cond_broadcast(c); }
static int arc_thread_create(arc_thread_t *t, arc_thread_fn_t fn, void *arg) {
    return (pthread_create(t, NULL, fn, arg) == 0) ? 0 : -1;
}
static void arc_thread_join(arc_thread_t t) { pthread_join(t, NULL); }

#endif

#endif /* OMNIC_ARC_SYNC_H */
