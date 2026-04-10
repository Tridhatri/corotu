#pragma once

#include <stdint.h>
#include <stddef.h>
#include <ucontext.h>

#define CORO_DEFAULT_STACK_SIZE (64 * 1024)  /* 64KB per coroutine */
#define CORO_GUARD_PAGE_SIZE    4096         /* one page below stack, PROT_NONE */

/*
 * Coroutine states:
 *
 *   READY   — created and waiting to be resumed
 *   RUNNING — currently executing on a thread
 *   BLOCKED — waiting on a channel / sleep (will be re-queued later)
 *   DONE    — fn() returned, resources can be freed
 */
typedef enum {
    CORO_READY   = 0,
    CORO_RUNNING = 1,
    CORO_BLOCKED = 2,
    CORO_DONE    = 3,
} coro_state_t;

typedef struct coroutine {
    ucontext_t        ctx;           /* saved register state + stack pointer  */
    void             *stack;         /* base of usable stack (after guard page) */
    size_t            stack_size;    /* usable stack size in bytes             */

    void            (*fn)(void *);   /* entry function                        */
    void             *arg;           /* argument passed to fn                 */

    int               priority;      /* 0 = highest urgency, 7 = background   */
    uint64_t          deadline_ns;   /* absolute CLOCK_MONOTONIC ns, 0 = none */
    int               cpu_affinity;  /* pin to core N, -1 = any               */

    coro_state_t      state;

    struct coroutine *next;          /* intrusive linked list node (for queues) */
} coroutine_t;


/* --- lifecycle ------------------------------------------------------------ */

/*
 * Allocate a coroutine. Does NOT start running it.
 * priority:    0 (urgent) .. 7 (background)
 * deadline_ns: absolute nanosecond timestamp from clock_now_ns(), 0 = no deadline
 */
coroutine_t *coro_create(void (*fn)(void *), void *arg,
                          int priority, uint64_t deadline_ns);

/*
 * Switch into coroutine c from the scheduler context.
 * c must be READY or BLOCKED.
 */
void coro_resume(coroutine_t *c);

/*
 * Called from inside a running coroutine — voluntarily give up the CPU.
 * State becomes READY. Execution returns to whoever called coro_resume().
 */
void coro_yield(void);

/*
 * Called automatically by the trampoline when fn() returns.
 * Marks state DONE and returns to scheduler context.
 * You can also call it explicitly to exit early.
 */
void coro_exit(void);

/*
 * Free stack memory and the coroutine struct.
 * Only call on a DONE coroutine.
 */
void coro_destroy(coroutine_t *c);


/* --- introspection -------------------------------------------------------- */

/* Returns the coroutine currently running on this thread, or NULL. */
coroutine_t *coro_current(void);

/* Current time in nanoseconds (CLOCK_MONOTONIC). Useful for setting deadlines. */
uint64_t clock_now_ns(void);

/* Convenience: N milliseconds from now, as an absolute deadline_ns. */
static inline uint64_t deadline_in_ms(uint64_t ms) {
    extern uint64_t clock_now_ns(void);
    return clock_now_ns() + ms * 1000000ULL;
}
