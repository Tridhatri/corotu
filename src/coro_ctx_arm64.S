/*
 * coro_ctx_arm64.S — ARM64 context switch
 *
 * Why assembly?  makecontext() / swapcontext() are deprecated on macOS and
 * non-functional on ARM64 (Apple Silicon).  Every production coroutine
 * library (libco, boost.context, libaco) implements this in assembly.
 *
 * What we save:  only callee-saved registers.  The C ABI guarantees the
 * caller already saved any caller-saved registers (x0–x18) it cares about,
 * so we never need to touch them.
 *
 * coro_ctx_t layout (must match include/coro_ctx.h):
 *   [x0 +  0]  x19    [x0 +  8]  x20
 *   [x0 + 16]  x21    [x0 + 24]  x22
 *   [x0 + 32]  x23    [x0 + 40]  x24
 *   [x0 + 48]  x25    [x0 + 56]  x26
 *   [x0 + 64]  x27    [x0 + 72]  x28
 *   [x0 + 80]  fp     [x0 + 88]  lr
 *   [x0 + 96]  sp
 */

.text

/* ── coro_ctx_swap(coro_ctx_t *from, coro_ctx_t *to) ──────────────────────
 *
 *  x0 = from   x1 = to
 *
 *  Save current callee-saved registers + sp into *from.
 *  Load callee-saved registers + sp from *to.
 *  `ret` jumps to the lr we just loaded — which is either:
 *    • the return address of coro_ctx_swap in the *to context (resume), or
 *    • the trampoline function address (first run of a new coroutine).
 */
.global _coro_ctx_swap
.p2align 2
_coro_ctx_swap:
    /* save into *from */
    stp  x19, x20, [x0,  #0]
    stp  x21, x22, [x0, #16]
    stp  x23, x24, [x0, #32]
    stp  x25, x26, [x0, #48]
    stp  x27, x28, [x0, #64]
    stp  x29, x30, [x0, #80]   /* fp, lr */
    mov  x2,  sp
    str  x2,       [x0, #96]   /* sp */

    /* load from *to */
    ldp  x19, x20, [x1,  #0]
    ldp  x21, x22, [x1, #16]
    ldp  x23, x24, [x1, #32]
    ldp  x25, x26, [x1, #48]
    ldp  x27, x28, [x1, #64]
    ldp  x29, x30, [x1, #80]   /* fp, lr */
    ldr  x2,       [x1, #96]   /* sp */
    mov  sp,  x2

    ret                         /* jump to restored lr */
