"Coroutines are just threads with function pointers"
Almost. But there's one important distinction.

A thread is managed by the OS. The OS decides when it runs, switches it, schedules it. You don't control that. The OS can interrupt a thread at any instruction.

A coroutine is managed by you. It only switches when it explicitly calls coro_yield(). The OS doesn't know coroutines exist — it only sees one thread (for now). All the switching happens entirely in userspace.

So:

Thread	Coroutine
Who creates it	OS	You (coro_create)
Who schedules it	OS kernel	Your scheduler
When does it switch	Anytime (preemptive)	Only on coro_yield() (cooperative)
Cost	Heavy (~8MB stack, kernel involved)	Cheap (64KB stack, no kernel)
The function pointer part — yes, exactly right. void (*fn)(void *) is the actual work the coroutine does. Same idea as pthread_create.

"ucontext_t is a general struct for storing and retrieving context between CPU registers and userspace"
Exactly right. It is the bridge. CPU → uc_mcontext on save. uc_mcontext → CPU on restore.

"The stack is the scratchpad — just memory, right?"
Yes. The stack is just a region of RAM. A flat array of bytes. The CPU treats it as a scratchpad for:

Local variables
Function arguments
Return addresses
Temporarily saved register values between calls
Nothing special about the RAM itself. What makes it a "stack" is purely the convention of how the CPU uses it — SP moves down on allocation, up on deallocation, in a strict last-in-first-out order.

"Stack is the actual size of what?"
The stack_t struct stores:


typedef struct {
    void  *ss_sp;    // start address of the stack region
    size_t ss_size;  // total size in bytes
    int    ss_flags; // usually 0
} stack_t;
ss_size is the total byte count of the allocated memory region. In our case 64KB = 65536 bytes. That's how much scratchpad the coroutine gets. If it overflows that — function calls too deep, too many local variables — it hits the guard page and crashes.

"uc_mcontext is the CPU register snapshot at a particular point in time"
Exactly right. It is a photograph. The moment swapcontext is called, the live CPU registers get written into uc_mcontext. That photograph then sits in RAM doing nothing until that context is restored.

"uc_stack is metadata — stores start address and size"
Exactly right. And to be precise about when it matters:


makecontext()  →  reads uc_stack.ss_sp and ss_size
                  computes top of stack
                  writes that address into uc_mcontext.sp
                  
After makecontext() runs:
  uc_stack is never read again
  uc_mcontext.sp is what actually tracks the stack pointer
uc_stack is used exactly once, during setup. After that it's just dead metadata sitting in the struct.

"uc_link — we set it NULL and handle exit ourselves via coro_exit()"
Right. Let me fill this in more completely.

uc_link is what happens at the hardware level when a context's function returns via a plain ret instruction. No swapcontext, no explicit jump — just a normal function return falling off the end.

When uc_link is non-NULL:


fn() returns
  ↓
CPU automatically calls setcontext(uc_link)
  ↓
execution continues in uc_link's context
When uc_link is NULL and fn() returns:


fn() returns
  ↓
CPU has no valid address to jump to
  ↓
undefined behavior — usually a crash
We set it to NULL because we never let fn() return directly. Our trampoline catches the return first:


static void trampoline(unsigned int hi, unsigned int lo) {
    coroutine_t *c = ...;
    c->fn(c->arg);   // fn() returns HERE, back into trampoline
    coro_exit();     // we intercept it before the CPU ever sees a bare return
}
coro_exit() then does:


void coro_exit(void) {
    t_current->state = CORO_DONE;   // mark it done FIRST
    setcontext(&t_sched_ctx);        // then jump to scheduler
}
If we used uc_link instead, we'd jump to the next context but never get to set state = CORO_DONE. The scheduler would think the coroutine is still running. That's why we intercept it ourselves.

"The CPU and RAM intersect through the stack pointer register"
Yes. And let me complete this picture fully because it's fundamental.

The CPU has two registers that act as bridges between the chip and RAM:

SP — Stack Pointer
Points to the current top of the stack in RAM. Every function call, local variable, pushed register — SP moves. The CPU uses SP automatically for all of this. You rarely touch SP directly.

PC — Program Counter
Points to the next instruction to execute in RAM. After each instruction, PC increments automatically. On a function call, PC jumps to the function's address. On return, PC jumps back to the saved return address.


Physical CPU                    RAM
┌────────────────┐              ┌──────────────────────┐
│ PC = 0x1000a4 ─┼─────────────►│ instruction at 1000a4│
│ SP = 0x7fff20 ─┼──────┐      │ ...                  │
│ x0 = 42        │      │      ├──────────────────────┤
│ x1 = 7         │      └─────►│ stack top at 7fff20  │
└────────────────┘             │ local var a = 10     │
                               │ saved return addr    │
                               │ ...                  │
                               └──────────────────────┘
PC says "I'm executing this instruction right now."
SP says "my stack currently reaches this far into RAM."

Everything else in uc_mcontext — x0 through x28, flags — lives entirely on the chip during execution. They only touch RAM when explicitly stored (like str x0, [sp] in ARM assembly, which writes x0 onto the stack).

"Each process has a stack — explain that"
Every process gets its own virtual address space from the OS. Inside that address space, the OS carves out several regions automatically at startup:


Process virtual address space (64-bit)

high address  0xFFFFFFFFFFFFFFFF
              ┌───────────────────┐
              │   kernel space    │  ← OS lives here, process can't touch
              ├───────────────────┤  0x00007FFFFFFFFFFF
              │   stack           │  ← grows downward from here
              │   (main thread)   │    ~8MB default on macOS/Linux
              │                   │    local vars, function calls
              │   ↓ grows down    │
              ├───────────────────┤
              │   ...free space...│
              ├───────────────────┤
              │   heap            │  ← malloc lives here, grows upward
              │   ↑ grows up      │
              ├───────────────────┤
              │   BSS segment     │  ← uninitialized globals (zeroed)
              ├───────────────────┤
              │   data segment    │  ← initialized globals
              ├───────────────────┤
              │   text segment    │  ← your compiled code (read-only)
              └───────────────────┘
low address   0x0000000000000000
The main thread's stack is that top region — automatically created by the OS when the process starts. 8MB on most systems. SP starts near the top and moves downward as main calls functions.

Now here is the key thing for our project: that default stack belongs to the main thread only. When we create coroutines, we mmap new separate stack regions for each one. Each coroutine gets its own private stack, completely separate from main's stack and from each other's stacks.


Our process address space with coroutines:

              ┌───────────────────┐
              │  main's stack 8MB │  ← OS provided, main thread uses this
              ├───────────────────┤
              │  ...              │
              ├───────────────────┤
              │  coroutine A stack│  ← we mmap'd this (64KB + guard)
              ├───────────────────┤
              │  coroutine B stack│  ← we mmap'd this (64KB + guard)
              ├───────────────────┤
              │  coroutine C stack│  ← we mmap'd this (64KB + guard)
              ├───────────────────┤
              │  heap             │  ← coroutine_t structs malloc'd here
              ├───────────────────┤
              │  text (code)      │
              └───────────────────┘
When we swapcontext into coroutine A, SP gets loaded from A->ctx.uc_mcontext.sp — which points into coroutine A's mmap'd region. The CPU is now using A's stack. When we swap back to the scheduler, SP gets loaded from t_sched_ctx.uc_mcontext.sp — which points back into main's stack. The stacks never mix.
