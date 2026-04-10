#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/mman.h>

/* macOS uses MAP_ANON; Linux exposes MAP_ANONYMOUS */
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#include "coroutine.h"

/* -------------------------------------------------------------------------
 * Thread-local state
 *
 * Each OS thread has:
 *   t_current  — which coroutine is running right now (NULL = scheduler)
 *   t_sched_ctx — the saved context of whoever called coro_resume()
 *                 we jump back here on yield/exit
 * ------------------------------------------------------------------------- */
static __thread coroutine_t *t_current   = NULL;
static __thread ucontext_t   t_sched_ctx;


/* -------------------------------------------------------------------------
 * Trampoline
 *
 * makecontext() only passes int-sized arguments. On 64-bit systems a pointer
 * is 8 bytes but int is 4 bytes, so we split the pointer into two uint32_t
 * halves (hi and lo) and reassemble inside the trampoline.
 * ------------------------------------------------------------------------- */
static void trampoline(unsigned int hi, unsigned int lo) {
    uintptr_t     ptr = ((uintptr_t)hi << 32) | (uintptr_t)lo;
    coroutine_t  *c   = (coroutine_t *)ptr;

    c->fn(c->arg);   /* run the actual coroutine function */
    coro_exit();     /* fn() returned — clean up          */
}


/* -------------------------------------------------------------------------
 * coro_create
 * ------------------------------------------------------------------------- */
coroutine_t *coro_create(void (*fn)(void *), void *arg,
                          int priority, uint64_t deadline_ns) {
    coroutine_t *c = calloc(1, sizeof(coroutine_t));
    if (!c) return NULL;

    c->fn           = fn;
    c->arg          = arg;
    c->priority     = priority;
    c->deadline_ns  = deadline_ns;
    c->cpu_affinity = -1;
    c->state        = CORO_READY;
    c->stack_size   = CORO_DEFAULT_STACK_SIZE;

    /*
     * Memory layout (low address → high address):
     *
     *   [ guard page | PROT_NONE ][ stack | PROT_READ|WRITE ]
     *   ^                         ^
     *   mem                       c->stack
     *
     * The stack grows downward on x86_64. If it overflows past c->stack
     * into the guard page, the OS raises SIGSEGV instead of silently
     * corrupting adjacent memory.
     */
    size_t total = CORO_GUARD_PAGE_SIZE + c->stack_size;
    void  *mem   = mmap(NULL, total,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        free(c);
        return NULL;
    }

    /* Make guard page inaccessible */
    if (mprotect(mem, CORO_GUARD_PAGE_SIZE, PROT_NONE) != 0) {
        munmap(mem, total);
        free(c);
        return NULL;
    }

    c->stack = (char *)mem + CORO_GUARD_PAGE_SIZE;

    /* Set up the ucontext */
    getcontext(&c->ctx);
    c->ctx.uc_stack.ss_sp   = c->stack;
    c->ctx.uc_stack.ss_size = c->stack_size;
    c->ctx.uc_link          = NULL; /* we handle exit ourselves in coro_exit() */

    /* Split pointer into two unsigned ints for makecontext */
    uintptr_t ptr = (uintptr_t)c;
    makecontext(&c->ctx, (void (*)())trampoline, 2,
                (unsigned int)(ptr >> 32),
                (unsigned int)(ptr & 0xFFFFFFFFU));

    return c;
}


/* -------------------------------------------------------------------------
 * coro_resume  — called from scheduler context to run coroutine c
 * ------------------------------------------------------------------------- */
void coro_resume(coroutine_t *c) {
    if (c->state != CORO_READY && c->state != CORO_BLOCKED) return;

    t_current = c;
    c->state  = CORO_RUNNING;

    /*
     * swapcontext saves our (scheduler) registers into t_sched_ctx,
     * then restores c->ctx and jumps into the coroutine.
     *
     * We return here when the coroutine calls coro_yield() or coro_exit().
     */
    swapcontext(&t_sched_ctx, &c->ctx);

    t_current = NULL;
}


/* -------------------------------------------------------------------------
 * coro_yield  — called from inside a coroutine
 * ------------------------------------------------------------------------- */
void coro_yield(void) {
    coroutine_t *c = t_current;
    if (!c) return;  /* called outside a coroutine — no-op */

    c->state = CORO_READY;

    /*
     * Save our registers into c->ctx, restore t_sched_ctx.
     * Execution in the coroutine resumes from here next time coro_resume(c)
     * is called.
     */
    swapcontext(&c->ctx, &t_sched_ctx);
}


/* -------------------------------------------------------------------------
 * coro_exit  — called when fn() returns (via trampoline) or explicitly
 * ------------------------------------------------------------------------- */
void coro_exit(void) {
    coroutine_t *c = t_current;
    if (c) c->state = CORO_DONE;

    /*
     * setcontext (not swapcontext) — we don't save anything because this
     * coroutine is finished. Just jump back to the scheduler.
     */
    setcontext(&t_sched_ctx);

    /* unreachable */
}


/* -------------------------------------------------------------------------
 * coro_destroy  — free resources of a DONE coroutine
 * ------------------------------------------------------------------------- */
void coro_destroy(coroutine_t *c) {
    if (!c) return;

    /* stack base = c->stack - CORO_GUARD_PAGE_SIZE */
    void  *mem   = (char *)c->stack - CORO_GUARD_PAGE_SIZE;
    size_t total = CORO_GUARD_PAGE_SIZE + c->stack_size;
    munmap(mem, total);
    free(c);
}


/* -------------------------------------------------------------------------
 * Introspection helpers
 * ------------------------------------------------------------------------- */
coroutine_t *coro_current(void) {
    return t_current;
}

uint64_t clock_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
