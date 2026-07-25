/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/lockable.h"
#include "system/cpu-timers.h"
#include "exec/icount.h"
#include "system/replay.h"
#include "system/cpus.h"
#include "hw/core/cpu.h"
#include "hw/misc/gnw_env.h"

#ifdef CONFIG_POSIX
#include <pthread.h>
#endif

#ifdef CONFIG_PPOLL
#include <poll.h>
#endif

#ifdef CONFIG_PRCTL_PR_SET_TIMERSLACK
#include <sys/prctl.h>
#endif

/***********************************************************/
/* timers */

typedef struct QEMUClock {
    /* We rely on BQL to protect the timerlists */
    QLIST_HEAD(, QEMUTimerList) timerlists;

    QEMUClockType type;
    bool enabled;
} QEMUClock;

QEMUTimerListGroup main_loop_tlg;
static QEMUClock qemu_clocks[QEMU_CLOCK_MAX];

/* A QEMUTimerList is a list of timers attached to a clock. More
 * than one QEMUTimerList can be attached to each clock, for instance
 * used by different AioContexts / threads. Each clock also has
 * a list of the QEMUTimerLists associated with it, in order that
 * reenabling the clock can call all the notifiers.
 */

struct QEMUTimerList {
    QEMUClock *clock;
    QemuMutex active_timers_lock;
    QEMUTimer *active_timers;
    QLIST_ENTRY(QEMUTimerList) list;
    QEMUTimerListNotifyCB *notify_cb;
    void *notify_opaque;

    /* lightweight method to mark the end of timerlist's running */
    QemuEvent timers_done_ev;
};

/**
 * qemu_clock_ptr:
 * @type: type of clock
 *
 * Translate a clock type into a pointer to QEMUClock object.
 *
 * Returns: a pointer to the QEMUClock object
 */
static inline QEMUClock *qemu_clock_ptr(QEMUClockType type)
{
    return &qemu_clocks[type];
}

static bool timer_expired_ns(const QEMUTimer *timer_head, int64_t current_time)
{
    return timer_head && (timer_head->expire_time <= current_time);
}

QEMUTimerList *timerlist_new(QEMUClockType type,
                             QEMUTimerListNotifyCB *cb,
                             void *opaque)
{
    QEMUTimerList *timer_list;
    QEMUClock *clock = qemu_clock_ptr(type);

    timer_list = g_new0(QEMUTimerList, 1);
    qemu_event_init(&timer_list->timers_done_ev, true);
    timer_list->clock = clock;
    timer_list->notify_cb = cb;
    timer_list->notify_opaque = opaque;
    qemu_mutex_init(&timer_list->active_timers_lock);
    QLIST_INSERT_HEAD(&clock->timerlists, timer_list, list);
    return timer_list;
}

void timerlist_free(QEMUTimerList *timer_list)
{
    assert(!timerlist_has_timers(timer_list));
    if (timer_list->clock) {
        QLIST_REMOVE(timer_list, list);
    }
    qemu_mutex_destroy(&timer_list->active_timers_lock);
    g_free(timer_list);
}

static void qemu_clock_init(QEMUClockType type, QEMUTimerListNotifyCB *notify_cb)
{
    QEMUClock *clock = qemu_clock_ptr(type);

    /* Assert that the clock of type TYPE has not been initialized yet. */
    assert(main_loop_tlg.tl[type] == NULL);

    clock->type = type;
    clock->enabled = (type == QEMU_CLOCK_VIRTUAL ? false : true);
    QLIST_INIT(&clock->timerlists);
    main_loop_tlg.tl[type] = timerlist_new(type, notify_cb, NULL);
}

bool qemu_clock_use_for_deadline(QEMUClockType type)
{
    return !(icount_enabled() && (type == QEMU_CLOCK_VIRTUAL));
}

void qemu_clock_notify(QEMUClockType type)
{
    QEMUTimerList *timer_list;
    QEMUClock *clock = qemu_clock_ptr(type);
    QLIST_FOREACH(timer_list, &clock->timerlists, list) {
        timerlist_notify(timer_list);
    }
}

/* Disabling the clock will wait for related timerlists to stop
 * executing qemu_run_timers.  Thus, this functions should not
 * be used from the callback of a timer that is based on @clock.
 * Doing so would cause a deadlock.
 *
 * Caller should hold BQL.
 */
void qemu_clock_enable(QEMUClockType type, bool enabled)
{
    QEMUClock *clock = qemu_clock_ptr(type);
    QEMUTimerList *tl;
    bool old = clock->enabled;
    clock->enabled = enabled;
    if (enabled && !old) {
        qemu_clock_notify(type);
    } else if (!enabled && old) {
        QLIST_FOREACH(tl, &clock->timerlists, list) {
            qemu_event_wait(&tl->timers_done_ev);
        }
    }
}

bool timerlist_has_timers(QEMUTimerList *timer_list)
{
    return !!qatomic_read(&timer_list->active_timers);
}

bool qemu_clock_has_timers(QEMUClockType type)
{
    return timerlist_has_timers(
        main_loop_tlg.tl[type]);
}

bool timerlist_expired(QEMUTimerList *timer_list)
{
    int64_t expire_time = 0;

    if (!qatomic_read(&timer_list->active_timers)) {
        return false;
    }

    WITH_QEMU_LOCK_GUARD(&timer_list->active_timers_lock) {
        if (!timer_list->active_timers) {
            return false;
        }
        expire_time = timer_list->active_timers->expire_time;
    }

    return expire_time <= qemu_clock_get_ns(timer_list->clock->type);
}

bool qemu_clock_expired(QEMUClockType type)
{
    return timerlist_expired(
        main_loop_tlg.tl[type]);
}

/*
 * As above, but return -1 for no deadline, and do not cap to 2^32
 * as we know the result is always positive.
 */

int64_t timerlist_deadline_ns(QEMUTimerList *timer_list)
{
    int64_t delta;
    int64_t expire_time = 0;

    if (!qatomic_read(&timer_list->active_timers)) {
        return -1;
    }

    if (!timer_list->clock->enabled) {
        return -1;
    }

    /* The active timers list may be modified before the caller uses our return
     * value but ->notify_cb() is called when the deadline changes.  Therefore
     * the caller should notice the change and there is no race condition.
     */
    WITH_QEMU_LOCK_GUARD(&timer_list->active_timers_lock) {
        if (!timer_list->active_timers) {
            return -1;
        }
        expire_time = timer_list->active_timers->expire_time;
    }

    delta = expire_time - qemu_clock_get_ns(timer_list->clock->type);

    if (delta <= 0) {
        return 0;
    }

    return delta;
}

/* Calculate the soonest deadline across all timerlists attached
 * to the clock. This is used for the icount timeout so we
 * ignore whether or not the clock should be used in deadline
 * calculations.
 */
int64_t qemu_clock_deadline_ns_all(QEMUClockType type, int attr_mask)
{
    int64_t deadline = -1;
    int64_t delta;
    int64_t expire_time;
    QEMUTimer *ts;
    QEMUTimerList *timer_list;
    QEMUClock *clock = qemu_clock_ptr(type);

    if (!clock->enabled) {
        return -1;
    }

    QLIST_FOREACH(timer_list, &clock->timerlists, list) {
        if (!qatomic_read(&timer_list->active_timers)) {
            continue;
        }
        qemu_mutex_lock(&timer_list->active_timers_lock);
        ts = timer_list->active_timers;
        /* Skip all external timers */
        while (ts && (ts->attributes & ~attr_mask)) {
            ts = ts->next;
        }
        if (!ts) {
            qemu_mutex_unlock(&timer_list->active_timers_lock);
            continue;
        }
        expire_time = ts->expire_time;
        qemu_mutex_unlock(&timer_list->active_timers_lock);

        delta = expire_time - qemu_clock_get_ns(type);
        if (delta <= 0) {
            delta = 0;
        }
        deadline = qemu_soonest_timeout(deadline, delta);
    }
    return deadline;
}

void timerlist_notify(QEMUTimerList *timer_list)
{
    if (timer_list->notify_cb) {
        timer_list->notify_cb(timer_list->notify_opaque, timer_list->clock->type);
    } else {
        qemu_notify_event();
    }
}

/* Transition function to convert a nanosecond timeout to ms
 * This is used where a system does not support ppoll
 */
int qemu_timeout_ns_to_ms(int64_t ns)
{
    int64_t ms;
    if (ns < 0) {
        return -1;
    }

    if (!ns) {
        return 0;
    }

    /* Always round up, because it's better to wait too long than to wait too
     * little and effectively busy-wait
     */
    ms = DIV_ROUND_UP(ns, SCALE_MS);

    /* To avoid overflow problems, limit this to 2^31, i.e. approx 25 days */
    return MIN(ms, INT32_MAX);
}


#if defined(__APPLE__) && !defined(CONFIG_PPOLL)
/*
 * Sub-millisecond wait for hosts that have no ppoll().
 *
 * g_poll()'s timeout is whole milliseconds and qemu_timeout_ns_to_ms()
 * deliberately rounds UP, so on a ppoll-less host every timer deadline
 * closer than 1ms overshoots. That measurably starves the main loop:
 * 2095 wakeups/s on Linux vs 1223/s on a ppoll-less host for the same
 * workload, and the guest then renders roughly two thirds of its frames
 * (Celeste: 30.0 fps on Linux, 27.1 on macOS).
 *
 * macOS does have pselect(), whose struct timespec timeout is honoured
 * at nanosecond resolution, so use it to get the true bounded wait that
 * ppoll() would have given us. Only the timeout mechanism differs; the
 * fd set, the revents written back and the return value all keep
 * g_poll()'s exact semantics.
 *
 * Returns the number of ready fds, 0 on timeout, -1 with errno set on
 * error, or -2 if the request cannot be expressed as a select() call
 * (an fd at or above FD_SETSIZE) -- in which case the caller must fall
 * back to g_poll() rather than silently dropping an fd.
 */
static int qemu_pselect_ns(GPollFD *fds, guint nfds, int64_t timeout)
{
    fd_set rfds, wfds, xfds;
    struct timespec ts;
    int nsel = 0;
    int ret, count;
    guint i;

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&xfds);

    for (i = 0; i < nfds; i++) {
        int fd = fds[i].fd;

        fds[i].revents = 0;
        if (fd < 0) {
            continue;
        }
        if (fd >= FD_SETSIZE) {
            return -2;
        }
        if (fds[i].events & (G_IO_IN | G_IO_HUP | G_IO_ERR)) {
            FD_SET(fd, &rfds);
        }
        if (fds[i].events & G_IO_OUT) {
            FD_SET(fd, &wfds);
        }
        if (fds[i].events & G_IO_PRI) {
            FD_SET(fd, &xfds);
        }
        if (fd >= nsel) {
            nsel = fd + 1;
        }
    }

    ts.tv_sec = timeout / 1000000000LL;
    ts.tv_nsec = timeout % 1000000000LL;

    ret = pselect(nsel, &rfds, &wfds, &xfds, &ts, NULL);
    if (ret <= 0) {
        /* 0 == timed out, -1 == error with errno already set */
        return ret;
    }

    /*
     * pselect() counts an fd once per set it is ready in; g_poll() counts
     * each fd at most once, so recount rather than forwarding pselect()'s
     * return value.
     */
    count = 0;
    for (i = 0; i < nfds; i++) {
        int fd = fds[i].fd;

        if (fd < 0) {
            continue;
        }
        if (FD_ISSET(fd, &rfds)) {
            /*
             * select() folds EOF and error conditions into "readable",
             * so report plain G_IO_IN and let the caller's read discover
             * which it was -- never synthesise G_IO_HUP/G_IO_ERR, which
             * callers treat as "tear this channel down".
             */
            fds[i].revents |= G_IO_IN;
        }
        if (FD_ISSET(fd, &wfds)) {
            fds[i].revents |= G_IO_OUT;
        }
        if (FD_ISSET(fd, &xfds)) {
            fds[i].revents |= G_IO_PRI;
        }
        if (fds[i].revents) {
            count++;
        }
    }
    return count;
}
#endif /* __APPLE__ && !CONFIG_PPOLL */

#if defined(_WIN32) && !defined(CONFIG_PPOLL)
/*
 * Sub-millisecond wait for Windows, which has no ppoll().
 *
 * Same disease as macOS (see qemu_pselect_ns above): g_poll()'s timeout is
 * whole milliseconds and qemu_timeout_ns_to_ms() rounds UP, so every timer
 * deadline closer than 1ms overshoots, the main loop's wakeup rate collapses
 * (2095/s on Linux vs 1223/s on Windows for the same workload) and the guest
 * drops frames -- measured 20.0 fps on Windows vs 30.0 on Linux on the same
 * physical CPU running Celeste.
 *
 * The macOS cure does NOT port. Winsock select() only understands sockets,
 * whereas the fds this function is handed on win32 are Windows HANDLEs
 * (glib's win32 g_poll is built on MsgWaitForMultipleObjectsEx). So instead
 * of replacing the wait, we express the timeout AS a member of the wait set:
 * a high-resolution waitable timer (100ns granularity) armed to the exact
 * deadline, appended to the caller's fds, and then g_poll() with an infinite
 * timeout. The timer firing *is* the timeout. glib still does the actual
 * wait, so all the win32 handle/message semantics stay exactly as they were;
 * only the rounding disappears. No spinning, no extra wakeups.
 *
 * The timer handle is created once per thread and reused -- creating one per
 * call would cost a kernel object per main-loop iteration.
 * CREATE_WAITABLE_TIMER_HIGH_RESOLUTION needs Windows 10 1803+; if it is
 * refused we probe once, cache the answer and fall back to a plain waitable
 * timer (millisecond-ish, i.e. no worse than today), and if even that fails
 * we return -2 so the caller uses the old rounded g_poll() path.
 *
 * Returns g_poll() semantics: ready fd count, 0 on timeout, -1 with errno
 * set on error; or -2 if this mechanism is unavailable for this call.
 */
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static __thread HANDLE gnw_wait_timer;
static __thread bool gnw_wait_timer_failed;

static HANDLE qemu_get_wait_timer(void)
{
    if (gnw_wait_timer || gnw_wait_timer_failed) {
        return gnw_wait_timer;
    }
    /* Auto-reset: the wait consumes the signal, and SetWaitableTimer()
     * re-arms (and cancels any previous arming) on every call anyway. */
    gnw_wait_timer = CreateWaitableTimerExW(NULL, NULL,
                                            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                            TIMER_ALL_ACCESS);
    if (!gnw_wait_timer) {
        gnw_wait_timer = CreateWaitableTimerExW(NULL, NULL, 0,
                                                TIMER_ALL_ACCESS);
    }
    if (!gnw_wait_timer) {
        gnw_wait_timer_failed = true;
    }
    return gnw_wait_timer;
}

static int qemu_timed_wait_ns(GPollFD *fds, guint nfds, int64_t timeout)
{
    GPollFD local[MAXIMUM_WAIT_OBJECTS + 1];
    LARGE_INTEGER due;
    HANDLE timer;
    int ret, count;
    guint i;

    /*
     * We add one handle to the wait set; if that would exceed what a single
     * MsgWaitForMultipleObjectsEx() can hold, don't risk glib's overflow
     * handling -- just take the old rounded path for that (rare) iteration.
     */
    if (nfds + 1 > MAXIMUM_WAIT_OBJECTS) {
        return -2;
    }

    timer = qemu_get_wait_timer();
    if (!timer) {
        return -2;
    }

    /* Negative == relative, in 100ns units. Round up to a whole unit so a
     * sub-100ns timeout can never be armed as "fire immediately, forever". */
    due.QuadPart = -DIV_ROUND_UP(timeout, 100);
    if (!SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
        return -2;
    }

    for (i = 0; i < nfds; i++) {
        local[i] = fds[i];
        local[i].revents = 0;
    }
    local[nfds].fd = (gintptr)timer;
    local[nfds].events = G_IO_IN;
    local[nfds].revents = 0;

    ret = g_poll(local, nfds + 1, -1);

    CancelWaitableTimer(timer);

    if (ret < 0) {
        return ret;
    }

    /* Copy back only the caller's fds, and recount: the timer is our own
     * private member of the wait set and must never be reported as ready. */
    count = 0;
    for (i = 0; i < nfds; i++) {
        fds[i].revents = local[i].revents;
        if (fds[i].revents) {
            count++;
        }
    }
    return count;
}
#endif /* _WIN32 && !CONFIG_PPOLL */

/* qemu implementation of g_poll which uses a nanosecond timeout but is
 * otherwise identical to g_poll
 */
int qemu_poll_ns(GPollFD *fds, guint nfds, int64_t timeout)
{
#ifndef CONFIG_PPOLL
    /*
     * DIAGNOSTIC (GNW_POLL_SPIN): hosts without ppoll -- macOS and
     * Windows -- fall through to the g_poll() path below, whose timeout
     * is whole milliseconds and is deliberately rounded UP. Every timer
     * deadline closer than 1ms therefore overshoots, which measurably
     * cuts the main loop's wakeup rate (2095/s on Linux vs 1223/s on
     * Windows for the same workload) and with it the guest's frame rate.
     * Spin on a zero-timeout poll instead, to measure how much of the
     * macOS/Windows deficit that rounding accounts for. Burns a core
     * while it spins -- diagnostic only, never a default.
     */
    if (timeout > 0 && timeout < SCALE_MS && gnw_env_enabled("GNW_POLL_SPIN")) {
        int64_t deadline = get_clock() + timeout;
        int ret;
        do {
            ret = g_poll(fds, nfds, 0);
        } while (ret == 0 && get_clock() < deadline);
        return ret;
    }
#endif
#if defined(__APPLE__) && !defined(CONFIG_PPOLL)
    /*
     * Default path on macOS: any positive timeout goes through pselect()
     * so it is honoured at nanosecond rather than rounded-up millisecond
     * resolution (see qemu_pselect_ns above). GNW_POLL_MS_ONLY=1 forces
     * the old g_poll() behaviour back, for A/B measurement.
     */
    if (timeout > 0 && !gnw_env_enabled("GNW_POLL_MS_ONLY")) {
        int ret = qemu_pselect_ns(fds, nfds, timeout);
        if (ret != -2) {
            return ret;
        }
        /* fd >= FD_SETSIZE: not expressible as select(), fall through */
    }
#endif
#if defined(_WIN32) && !defined(CONFIG_PPOLL)
    /*
     * Default path on Windows: any positive timeout is armed as a
     * high-resolution waitable timer inside the wait set instead of being
     * rounded up to a whole millisecond (see qemu_timed_wait_ns above).
     * GNW_POLL_MS_ONLY=1 forces the old g_poll() behaviour back, for A/B
     * measurement.
     */
    if (timeout > 0 && !gnw_env_enabled("GNW_POLL_MS_ONLY")) {
        int ret = qemu_timed_wait_ns(fds, nfds, timeout);
        if (ret != -2) {
            return ret;
        }
        /* mechanism unavailable for this call, fall through */
    }
#endif
#ifdef CONFIG_PPOLL
    if (timeout < 0) {
        return ppoll((struct pollfd *)fds, nfds, NULL, NULL);
    } else {
        struct timespec ts;
        int64_t tvsec = timeout / 1000000000LL;
        /* Avoid possibly overflowing and specifying a negative number of
         * seconds, which would turn a very long timeout into a busy-wait.
         */
        if (tvsec > (int64_t)INT32_MAX) {
            tvsec = INT32_MAX;
        }
        ts.tv_sec = tvsec;
        ts.tv_nsec = timeout % 1000000000LL;
        return ppoll((struct pollfd *)fds, nfds, &ts, NULL);
    }
#else
    return g_poll(fds, nfds, qemu_timeout_ns_to_ms(timeout));
#endif
}


void timer_init_full(QEMUTimer *ts,
                     QEMUTimerListGroup *timer_list_group, QEMUClockType type,
                     int scale, int attributes,
                     QEMUTimerCB *cb, void *opaque)
{
    if (!timer_list_group) {
        timer_list_group = &main_loop_tlg;
    }
    ts->timer_list = timer_list_group->tl[type];
    ts->cb = cb;
    ts->opaque = opaque;
    ts->scale = scale;
    ts->attributes = attributes;
    ts->expire_time = -1;
}

void timer_deinit(QEMUTimer *ts)
{
    assert(ts->expire_time == -1);
    ts->timer_list = NULL;
}

static void timer_del_locked(QEMUTimerList *timer_list, QEMUTimer *ts)
{
    QEMUTimer **pt, *t;

    ts->expire_time = -1;
    pt = &timer_list->active_timers;
    for(;;) {
        t = *pt;
        if (!t)
            break;
        if (t == ts) {
            qatomic_set(pt, t->next);
            break;
        }
        pt = &t->next;
    }
}

static void timer_fire(CPUState *cpu, run_on_cpu_data data)
{
    QEMUTimer *t = data.host_ptr;

    t->cb(t->opaque);
}

static bool timer_mod_ns_locked(QEMUTimerList *timer_list,
                                QEMUTimer *ts, int64_t expire_time)
{
    QEMUTimer **pt, *t;

    /*
     * Normally during record-replay virtual clock timers and CPU work are
     * deterministically ordered. This is because the virtual clock can be
     * advanced only by instructions running on a CPU.
     *
     * A notable exception are timers that are armed already expired. Their
     * expiration is not constrained by instruction execution, and, therefore,
     * their ordering relative to CPU work is affected by what the
     * record-replay thread is doing when they are armed. This introduces
     * non-determinism.
     *
     * Convert such timers to CPU work in order to avoid it.
     */
    if (replay_mode != REPLAY_MODE_NONE &&
        timer_list->clock->type == QEMU_CLOCK_VIRTUAL &&
        !(ts->attributes & QEMU_TIMER_ATTR_EXTERNAL) &&
        expire_time <= qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) {
        async_run_on_cpu(first_cpu, timer_fire,
                         RUN_ON_CPU_HOST_PTR(ts));
        return false;
    }

    /* add the timer in the sorted list */
    pt = &timer_list->active_timers;
    for (;;) {
        t = *pt;
        if (!timer_expired_ns(t, expire_time)) {
            break;
        }
        pt = &t->next;
    }
    ts->expire_time = MAX(expire_time, 0);
    ts->next = *pt;
    qatomic_set(pt, ts);

    return pt == &timer_list->active_timers;
}

static void timerlist_rearm(QEMUTimerList *timer_list)
{
    timerlist_notify(timer_list);
}

/* stop a timer, but do not dealloc it */
void timer_del(QEMUTimer *ts)
{
    QEMUTimerList *timer_list = ts->timer_list;

    if (timer_list) {
        qemu_mutex_lock(&timer_list->active_timers_lock);
        timer_del_locked(timer_list, ts);
        qemu_mutex_unlock(&timer_list->active_timers_lock);
    }
}

/* modify the current timer so that it will be fired when current_time
   >= expire_time. The corresponding callback will be called. */
void timer_mod_ns(QEMUTimer *ts, int64_t expire_time)
{
    QEMUTimerList *timer_list = ts->timer_list;
    bool rearm;

    qemu_mutex_lock(&timer_list->active_timers_lock);
    timer_del_locked(timer_list, ts);
    rearm = timer_mod_ns_locked(timer_list, ts, expire_time);
    qemu_mutex_unlock(&timer_list->active_timers_lock);

    if (rearm) {
        timerlist_rearm(timer_list);
    }
}

/* modify the current timer so that it will be fired when current_time
   >= expire_time or the current deadline, whichever comes earlier.
   The corresponding callback will be called. */
void timer_mod_anticipate_ns(QEMUTimer *ts, int64_t expire_time)
{
    QEMUTimerList *timer_list = ts->timer_list;
    bool rearm = false;

    WITH_QEMU_LOCK_GUARD(&timer_list->active_timers_lock) {
        if (ts->expire_time == -1 || ts->expire_time > expire_time) {
            if (ts->expire_time != -1) {
                timer_del_locked(timer_list, ts);
            }
            rearm = timer_mod_ns_locked(timer_list, ts, expire_time);
        } else {
            rearm = false;
        }
    }
    if (rearm) {
        timerlist_rearm(timer_list);
    }
}

void timer_mod(QEMUTimer *ts, int64_t expire_time)
{
    timer_mod_ns(ts, expire_time * ts->scale);
}

void timer_mod_anticipate(QEMUTimer *ts, int64_t expire_time)
{
    timer_mod_anticipate_ns(ts, expire_time * ts->scale);
}

bool timer_pending(const QEMUTimer *ts)
{
    return ts->expire_time >= 0;
}

bool timer_expired(const QEMUTimer *timer_head, int64_t current_time)
{
    return timer_expired_ns(timer_head, current_time * timer_head->scale);
}

bool timerlist_run_timers(QEMUTimerList *timer_list)
{
    QEMUTimer *ts;
    int64_t current_time;
    bool progress = false;
    QEMUTimerCB *cb;
    void *opaque;

    if (!qatomic_read(&timer_list->active_timers)) {
        return false;
    }

    qemu_event_reset(&timer_list->timers_done_ev);
    if (!timer_list->clock->enabled) {
        goto out;
    }

    switch (timer_list->clock->type) {
    case QEMU_CLOCK_REALTIME:
        break;
    default:
    case QEMU_CLOCK_VIRTUAL:
        break;
    case QEMU_CLOCK_HOST:
        if (!replay_checkpoint(CHECKPOINT_CLOCK_HOST)) {
            goto out;
        }
        break;
    case QEMU_CLOCK_VIRTUAL_RT:
        if (!replay_checkpoint(CHECKPOINT_CLOCK_VIRTUAL_RT)) {
            goto out;
        }
        break;
    }

    /*
     * Extract expired timers from active timers list and process them.
     *
     * In rr mode we need "filtered" checkpointing for virtual clock.  The
     * checkpoint must be recorded/replayed before processing any non-EXTERNAL timer,
     * and that must only be done once since the clock value stays the same. Because
     * non-EXTERNAL timers may appear in the timers list while it being processed,
     * the checkpoint can be issued at a time until no timers are left and we are
     * done".
     */
    current_time = qemu_clock_get_ns(timer_list->clock->type);
    qemu_mutex_lock(&timer_list->active_timers_lock);
    while ((ts = timer_list->active_timers)) {
        if (!timer_expired_ns(ts, current_time)) {
            /* No expired timers left.  The checkpoint can be skipped
             * if no timers fired or they were all external.
             */
            break;
        }
        /* Checkpoint for virtual clock is redundant in cases where
         * it's being triggered with only non-EXTERNAL timers, because
         * these timers don't change guest state directly.
         */
        if (replay_mode != REPLAY_MODE_NONE
            && timer_list->clock->type == QEMU_CLOCK_VIRTUAL
            && !(ts->attributes & QEMU_TIMER_ATTR_EXTERNAL)
            && !replay_checkpoint(CHECKPOINT_CLOCK_VIRTUAL)) {
            qemu_mutex_unlock(&timer_list->active_timers_lock);
            goto out;
        }

        /* remove timer from the list before calling the callback */
        timer_list->active_timers = ts->next;
        ts->next = NULL;
        ts->expire_time = -1;
        cb = ts->cb;
        opaque = ts->opaque;

        /* run the callback (the timer list can be modified) */
        qemu_mutex_unlock(&timer_list->active_timers_lock);
        cb(opaque);
        qemu_mutex_lock(&timer_list->active_timers_lock);

        progress = true;
    }
    qemu_mutex_unlock(&timer_list->active_timers_lock);

out:
    qemu_event_set(&timer_list->timers_done_ev);
    return progress;
}

bool qemu_clock_run_timers(QEMUClockType type)
{
    return timerlist_run_timers(main_loop_tlg.tl[type]);
}

void timerlistgroup_init(QEMUTimerListGroup *tlg,
                         QEMUTimerListNotifyCB *cb, void *opaque)
{
    QEMUClockType type;
    for (type = 0; type < QEMU_CLOCK_MAX; type++) {
        tlg->tl[type] = timerlist_new(type, cb, opaque);
    }
}

void timerlistgroup_deinit(QEMUTimerListGroup *tlg)
{
    QEMUClockType type;
    for (type = 0; type < QEMU_CLOCK_MAX; type++) {
        timerlist_free(tlg->tl[type]);
    }
}

bool timerlistgroup_run_timers(QEMUTimerListGroup *tlg)
{
    QEMUClockType type;
    bool progress = false;
    for (type = 0; type < QEMU_CLOCK_MAX; type++) {
        progress |= timerlist_run_timers(tlg->tl[type]);
    }
    return progress;
}

int64_t timerlistgroup_deadline_ns(QEMUTimerListGroup *tlg)
{
    int64_t deadline = -1;
    QEMUClockType type;
    for (type = 0; type < QEMU_CLOCK_MAX; type++) {
        if (qemu_clock_use_for_deadline(type)) {
            deadline = qemu_soonest_timeout(deadline,
                                            timerlist_deadline_ns(tlg->tl[type]));
        }
    }
    return deadline;
}

int64_t qemu_clock_get_ns(QEMUClockType type)
{
    switch (type) {
    case QEMU_CLOCK_REALTIME:
        return get_clock();
    default:
    case QEMU_CLOCK_VIRTUAL:
        return cpus_get_virtual_clock();
    case QEMU_CLOCK_HOST:
        return REPLAY_CLOCK(REPLAY_CLOCK_HOST, get_clock_realtime());
    case QEMU_CLOCK_VIRTUAL_RT:
        return REPLAY_CLOCK(REPLAY_CLOCK_VIRTUAL_RT, cpu_get_clock());
    }
}

static void qemu_virtual_clock_set_ns(int64_t time)
{
    return cpus_set_virtual_clock(time);
}

void qemu_init_clocks(QEMUTimerListNotifyCB *notify_cb)
{
    QEMUClockType type;
    for (type = 0; type < QEMU_CLOCK_MAX; type++) {
        qemu_clock_init(type, notify_cb);
    }

#ifdef CONFIG_PRCTL_PR_SET_TIMERSLACK
    prctl(PR_SET_TIMERSLACK, 1, 0, 0, 0);
#endif
}

uint64_t timer_expire_time_ns(const QEMUTimer *ts)
{
    return timer_pending(ts) ? ts->expire_time : -1;
}

bool qemu_clock_run_all_timers(void)
{
    bool progress = false;
    QEMUClockType type;

    for (type = 0; type < QEMU_CLOCK_MAX; type++) {
        if (qemu_clock_use_for_deadline(type)) {
            progress |= qemu_clock_run_timers(type);
        }
    }

    return progress;
}

int64_t qemu_clock_advance_virtual_time(int64_t dest)
{
    int64_t clock = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    AioContext *aio_context;
    aio_context = qemu_get_aio_context();
    while (clock < dest) {
        int64_t deadline = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL,
                                                      QEMU_TIMER_ATTR_ALL);
        int64_t warp = qemu_soonest_timeout(dest - clock, deadline);

        qemu_virtual_clock_set_ns(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + warp);

        qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL);
        timerlist_run_timers(aio_context->tlg.tl[QEMU_CLOCK_VIRTUAL]);
        clock = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    qemu_clock_notify(QEMU_CLOCK_VIRTUAL);

    return clock;
}
