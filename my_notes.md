One line summary

Why mmap instead of malloc.
malloc packs many allocations into shared pages — you can't mprotect part of a shared page without killing your neighbors. mmap gives you your own dedicated pages — you can mprotect freely because nobody else lives the

mmap for large files, larger memory chunks.
Ensures page allocation unlike malloc.
mmap'ed things can be controlled fully by the user, 
using mprotect. And it only belongs to this process directly. Directly in the virtual address space, not HEAP like malloc. HEAP slices are allocated by malloc and it gives you a slice, instead of a page-aligned dedicated memory chunk that you cna do whatever you want.
Hence.
Now, You can do fread instead of mmap.
But it is the thing that mmap offers.
Direct control to the virtual address space.


## What fread does

FILE *f = fopen("data.bin", "rb");
char buf[1024];
fread(buf, 1, 1024, f);  // copies 1024 bytes from file → buf
The flow:


disk → kernel buffer → your buffer (buf)




## What mmap with a file does

int fd = open("data.bin", O_RDONLY);
void *mem = mmap(NULL, 1024, PROT_READ, MAP_PRIVATE, fd, 0);
The flow:


disk → kernel buffer → that's it, you access it directly

The OS maps the file's contents directly into your process's virtual address space. No second copy. You just read mem[0], mem[1] etc. like a normal array. The kernel fetches pages from disk on demand as you access them.


char first_byte = ((char *)mem)[0];  // OS loads this page from disk if not cached


Your process thinks:         What's actually there:
address 0x1000  ──────────►  RAM page 47
address 0x2000  ──────────►  RAM page 12
address 0x3000  ──────────►  file "data.bin" page 0 on disk
address 0x4000  ──────────►  nothing yet (will fault and load on access)
mmap with a file inserts entries into this translation table — "when my process accesses address 0x3000, go fetch that page from data.bin."


Dumbed down: virtual memory is like a fake address book. Every process has its own address book where "123 Main St" might mean completely different physical locations for different processes. mmap adds an entry to that address book that points to a file instead of RAM.

So when does mmap with a file actually beat fread
Situation	Winner
Small file, read once	fread — simpler
Large file, only read parts of it	mmap — only loads pages you touch
Large file, random access	mmap — just index like an array, OS handles fetching
Multiple processes reading the same file	mmap — they share the same physical pages, one copy in RAM
Parsing a 2GB binary file	mmap — no need to allocate 2GB buffer upfront
Database files (SQLite, PostgreSQL)	mmap — databases do this heavily



## ucontext_t

typdef struct ucontext {
    unsigned int uc_onstack; 
    sigset_t uc_sigmask;
    stack_t uc_stack;
    struct ucontext *uc_link;
    mcontext_t uc_mcontext;

} ucontext_t; 

### uc_onstack 

Are we running on an alternative signal stack.



When a signal fires like SIGSEGV, SIGINT, the OS needs a stack to run the signal handler on.
You can eitehr user the current stack or set up a seperate dedicaed stack for signals via sigaltstack().


uv_onstack is a flag -1 means the context was running on the alternate signal stack when it was saved.
0 means on the normal stack.
we never touch this. It gets set by the OS automatically when stignals are involved.
For our corotuiens it is always 0.

### uc_sigmask - which signals are blocked.

Every thread has a signal mask - a bitmask of which signals are currently blocked. i.e OS will not deleiver them to this thread right now.
SIGINT, SIGTERM, SIGSEGV etc are each one bit.

uc_sigmask saves that bitmask as part of the context.When you swapcontext from A to B, the signal mask from A is saved to A->uc)sgmask, and B's signal mask is restroed from B->uc_sigmask.

This means two coroutines could theroritically have dfiferent signal masks
coorutine A has SIGINT blocked, corotutine B does not.
Each time you swap, the mask swaps with it.

### uc_stack - the stack this context uses.

stack_t uc_stack

stack_t is itself a struct;

typedef struct {
    void* ss_sp; // pointer to stack memory
    int ss_flags; // flags (usually 0)
    size_t ss_size; // size of stack in bytes.
} stack_t;


This is the field we actually care about and set explicitly in coro_create:


c->ctx.uc_stack.ss_sp   = c->stack;       // our mmap'd memory
c->ctx.uc_stack.ss_size = c->stack_size;  // 64KB
makecontext reads these fields to know where the stack is when it sets up the new context. When the coroutine starts executing, the CPU's stack pointer (SP) will be set to point at the top of this region.

Important nuance: uc_stack describes where the stack is, not the current stack pointer position. The actual SP lives inside uc_mcontext. uc_stack is metadata — makecontext uses it during setup, but after that uc_mcontext is what matters during execution.

Dumbed down: tells makecontext "here is the whiteboard I want this context to use." After setup, the actual pointer into the whiteboard lives in uc_mcontext.



### uc_link — what to run when this context finishes

struct ucontext *uc_link;
When a context's function returns naturally (not via swapcontext, just a plain return), the CPU needs to go somewhere. uc_link is that somewhere.

If uc_link is non-NULL, the OS automatically switches to that context when the current one's function returns.

If uc_link is NULL and the function returns, the behavior is undefined — typically a crash.

We set ours to NULL:


c->ctx.uc_link = NULL;
Because we never let coroutine functions return naturally without going through our trampoline first:


static void trampoline(unsigned int hi, unsigned int lo) {
    coroutine_t *c = ...;
    c->fn(c->arg);   // run the coroutine function
    coro_exit();     // WE handle the exit — don't need uc_link
}
coro_exit() calls setcontext(&t_sched_ctx) — we jump back to the scheduler ourselves. uc_link would do the same thing but with less control. We don't use it because we need to set c->state = CORO_DONE before jumping away.


### All together - how they interact in our code
All together — how they interact in our code

// coro_create:
getcontext(&c->ctx);              // snapshots current CPU state as baseline
                                   // fills uc_sigmask, uc_onstack automatically
c->ctx.uc_stack.ss_sp   = ...;    // tell makecontext which stack to use
c->ctx.uc_stack.ss_size = ...;
c->ctx.uc_link          = NULL;   // we handle exit in trampoline
makecontext(&c->ctx, trampoline, 2, hi, lo);
// makecontext modifies uc_mcontext:
//   sets pc = address of trampoline
//   sets sp = top of our mmap'd stack
//   sets x0,x1 (ARM64) = hi, lo arguments

// coro_resume:
swapcontext(&t_sched_ctx, &c->ctx);
// saves all CPU registers → t_sched_ctx.uc_mcontext
// loads all CPU registers ← c->ctx.uc_mcontext
// CPU is now running trampoline() on coroutine's stack

// coro_yield:
swapcontext(&c->ctx, &t_sched_ctx);
// saves all CPU registers → c->ctx.uc_mcontext (pc points here)
// loads all CPU registers ← t_sched_ctx.uc_mcontext
// CPU is back in coro_resume, right after its swapcontext ca





## STACK and CONTEXT


Good question. These two things get conflated a lot. Let me separate them completely.

Stack and context are two completely different things
They serve different purposes. One is storage for your program's work. The other is a snapshot of the CPU's state at a moment in time.

The stack — a region of memory
The stack is just a chunk of RAM. It's where your program stores:

Local variables
Function arguments
Return addresses (where to go when a function returns)
Saved registers between function calls

void foo(int x) {
    int a = 10;      // lives on the stack
    int b = x + a;   // b lives on the stack
    bar(b);          // b gets pushed onto stack as argument
}                    // a, b popped off stack when foo returns
Every time you call a function, a stack frame is pushed. Every time a function returns, its frame is popped. The stack pointer (SP) tracks the top.


high address
┌─────────────────┐
│   main's frame  │  ← main's locals, args
├─────────────────┤
│   foo's frame   │  ← a=10, b=15, saved return address
├─────────────────┤
│   bar's frame   │  ← bar's locals
├─────────────────┤  ← SP points here (top of stack, grows downward)
│   (free space)  │
└─────────────────┘
low address
The stack itself has no idea what code is running. It's just memory. A dumb array of bytes. It doesn't know about pc or what instruction is executing. It only stores values.

The context — a snapshot of the CPU
The CPU has a fixed set of registers — small, ultra-fast storage slots built into the chip itself. Not RAM. Not the stack. The chip itself.


CPU registers (NOT in RAM — physically inside the chip):

  PC  = 0x1000a4   ← which instruction executes next
  SP  = 0x7fff20   ← where the top of the stack is right now
  x0  = 42         ← first argument / return value
  x1  = 100        ← second argument
  x2  = 0          ← third argument
  x28 = ...        ← saved value
  FP  = 0x7fff50   ← base of current stack frame
  LR  = 0x100088   ← where to return when current function exits
  ... 30+ more
uc_mcontext is a photograph of all these registers taken at one instant. When you call swapcontext, the CPU's live register values get written into uc_mcontext. When you restore a context, those values get loaded back into the CPU's actual registers.

The context has no memory of its own. It only remembers register values.

How they relate — SP is the bridge
The one register that connects context and stack is SP — the stack pointer.

SP is a CPU register (lives in uc_mcontext). Its value is an address that points into the stack (lives in RAM).


uc_mcontext                  RAM (the stack)
┌─────────────────────┐      ┌──────────────────┐
│ pc = 0x1000a4       │      │                  │
│ sp = 0x7fff20  ──────────► │ ← top of stack   │
│ fp = 0x7fff50       │      │   local vars here│
│ x0 = 42             │      │   saved regs     │
│ x1 = 100            │      │   return addrs   │
│ ...                 │      │                  │
└─────────────────────┘      └──────────────────┘
   (CPU snapshot)               (memory region)
The context says "SP is 0x7fff20." That address points into the stack region. That's the only connection between the two.

So what does uc_stack do then?
uc_stack is not the current stack pointer. It is metadata used only by makecontext during setup.

When you call makecontext, it needs to know: "where is this new context's stack, and how big is it?" It reads uc_stack.ss_sp and uc_stack.ss_size to find out, then sets uc_mcontext.sp to the top of that region.

After makecontext runs, uc_stack is never read again. All actual stack tracking happens through uc_mcontext.sp.


makecontext reads uc_stack:
  ss_sp   = 0x200000   (base of our mmap'd region)
  ss_size = 65536      (64KB)

makecontext computes top of stack:
  top = 0x200000 + 65536 = 0x210000

makecontext writes into uc_mcontext:
  sp = 0x210000   ← now uc_mcontext knows where the stack is
  pc = address of trampoline
From this point on, uc_stack is irrelevant. uc_mcontext.sp is what the CPU actually uses.

Concrete analogy
Think of a musician mid-performance:

Concept	Analogy
Stack	The sheet music stand — holds everything they're working with
Context (uc_mcontext)	A photo of the musician — which bar they're on, which hand position, which note they just played
SP in uc_mcontext	The finger pointing to the current position in the sheet music
uc_stack	The label on the stand saying "this stand belongs to musician A, it holds 200 pages" — only needed when setting up, not during performance
Saving a context = taking a photo of the musician mid-performance.
Restoring a context = the musician looks at the photo and resumes from that exact position.
The sheet music (stack) stays on the stand — the photo just remembers where in the music they were.

Summary
Stack	Context (uc_mcontext)
What it is	A region of RAM	A snapshot of CPU registers
Where it lives	In memory (our mmap region)	In the ucontext_t struct (also memory, but just register values)
What it stores	Local variables, return addresses, saved values	PC, SP, general purpose registers
Who manages it	CPU pushes/pops automatically as functions call/return	swapcontext saves/restores it explicitly
Connection	SP register points into it	SP register IS one of the values saved in it
uc_stack role	Describes where the stack region is	Not involved — uc_stack is only for makecontext setup




## uccontext_t and registers and computation
Good question. These two things get conflated a lot. Let me separate them completely.

Stack and context are two completely different things
They serve different purposes. One is storage for your program's work. The other is a snapshot of the CPU's state at a moment in time.

The stack — a region of memory
The stack is just a chunk of RAM. It's where your program stores:

Local variables
Function arguments
Return addresses (where to go when a function returns)
Saved registers between function calls

void foo(int x) {
    int a = 10;      // lives on the stack
    int b = x + a;   // b lives on the stack
    bar(b);          // b gets pushed onto stack as argument
}                    // a, b popped off stack when foo returns
Every time you call a function, a stack frame is pushed. Every time a function returns, its frame is popped. The stack pointer (SP) tracks the top.


high address
┌─────────────────┐
│   main's frame  │  ← main's locals, args
├─────────────────┤
│   foo's frame   │  ← a=10, b=15, saved return address
├─────────────────┤
│   bar's frame   │  ← bar's locals
├─────────────────┤  ← SP points here (top of stack, grows downward)
│   (free space)  │
└─────────────────┘
low address
The stack itself has no idea what code is running. It's just memory. A dumb array of bytes. It doesn't know about pc or what instruction is executing. It only stores values.

The context — a snapshot of the CPU
The CPU has a fixed set of registers — small, ultra-fast storage slots built into the chip itself. Not RAM. Not the stack. The chip itself.


CPU registers (NOT in RAM — physically inside the chip):

  PC  = 0x1000a4   ← which instruction executes next
  SP  = 0x7fff20   ← where the top of the stack is right now
  x0  = 42         ← first argument / return value
  x1  = 100        ← second argument
  x2  = 0          ← third argument
  x28 = ...        ← saved value
  FP  = 0x7fff50   ← base of current stack frame
  LR  = 0x100088   ← where to return when current function exits
  ... 30+ more
uc_mcontext is a photograph of all these registers taken at one instant. When you call swapcontext, the CPU's live register values get written into uc_mcontext. When you restore a context, those values get loaded back into the CPU's actual registers.

The context has no memory of its own. It only remembers register values.

How they relate — SP is the bridge
The one register that connects context and stack is SP — the stack pointer.

SP is a CPU register (lives in uc_mcontext). Its value is an address that points into the stack (lives in RAM).


uc_mcontext                  RAM (the stack)
┌─────────────────────┐      ┌──────────────────┐
│ pc = 0x1000a4       │      │                  │
│ sp = 0x7fff20  ──────────► │ ← top of stack   │
│ fp = 0x7fff50       │      │   local vars here│
│ x0 = 42             │      │   saved regs     │
│ x1 = 100            │      │   return addrs   │
│ ...                 │      │                  │
└─────────────────────┘      └──────────────────┘
   (CPU snapshot)               (memory region)
The context says "SP is 0x7fff20." That address points into the stack region. That's the only connection between the two.

So what does uc_stack do then?
uc_stack is not the current stack pointer. It is metadata used only by makecontext during setup.

When you call makecontext, it needs to know: "where is this new context's stack, and how big is it?" It reads uc_stack.ss_sp and uc_stack.ss_size to find out, then sets uc_mcontext.sp to the top of that region.

After makecontext runs, uc_stack is never read again. All actual stack tracking happens through uc_mcontext.sp.


makecontext reads uc_stack:
  ss_sp   = 0x200000   (base of our mmap'd region)
  ss_size = 65536      (64KB)

makecontext computes top of stack:
  top = 0x200000 + 65536 = 0x210000

makecontext writes into uc_mcontext:
  sp = 0x210000   ← now uc_mcontext knows where the stack is
  pc = address of trampoline
From this point on, uc_stack is irrelevant. uc_mcontext.sp is what the CPU actually uses.

Concrete analogy
Think of a musician mid-performance:

Concept	Analogy
Stack	The sheet music stand — holds everything they're working with
Context (uc_mcontext)	A photo of the musician — which bar they're on, which hand position, which note they just played
SP in uc_mcontext	The finger pointing to the current position in the sheet music
uc_stack	The label on the stand saying "this stand belongs to musician A, it holds 200 pages" — only needed when setting up, not during performance
Saving a context = taking a photo of the musician mid-performance.
Restoring a context = the musician looks at the photo and resumes from that exact position.
The sheet music (stack) stays on the stand — the photo just remembers where in the music they were.

Summary
Stack	Context (uc_mcontext)
What it is	A region of RAM	A snapshot of CPU registers
Where it lives	In memory (our mmap region)	In the ucontext_t struct (also memory, but just register values)
What it stores	Local variables, return addresses, saved values	PC, SP, general purpose registers
Who manages it	CPU pushes/pops automatically as functions call/return	swapcontext saves/restores it explicitly
Connection	SP register points into it	SP register IS one of the values saved in it
uc_stack role	Describes where the stack region is	Not involved — uc_stack is only for makecontext setup
So all computation actually happens in the CPU snapshot uc_mccontext?
No — that's the key thing to get straight.

uc_mcontext is the snapshot. The CPU is the thing that computes.
uc_mcontext is just a struct in RAM. It's inert. It doesn't compute anything. It's a saved photograph — dead data sitting in memory.

All computation happens in the actual physical CPU registers.

The CPU registers are the real thing
There is one set of physical registers on the chip. One pc. One sp. One x0. They're not in RAM — they're transistors on the die.


Physical CPU chip
┌─────────────────────────────┐
│  pc  = 0x1000a4  (live)     │  ← actual execution happening here
│  sp  = 0x7fff20  (live)     │
│  x0  = 42        (live)     │
│  x1  = 7         (live)     │
│  ...                        │
└─────────────────────────────┘
         ↑
    THIS is where computation happens.
    Only one program runs here at a time.
uc_mcontext is a copy of those values, sitting somewhere in RAM, doing nothing:


RAM somewhere
┌─────────────────────────────┐
│  uc_mcontext:               │  ← dead snapshot, not executing
│    pc  = 0x2000b8           │
│    sp  = 0x6fff10           │
│    x0  = 99                 │
│    ...                      │
└─────────────────────────────┘
What swapcontext actually does physically

swapcontext(&A, &B);
Step 1 — dump live CPU registers → write into A.uc_mcontext in RAM


CPU registers ──────────────────► A.uc_mcontext (in RAM)
pc=0x1000a4, sp=0x7fff20 ...      pc=0x1000a4, sp=0x7fff20 ...
Step 2 — load from B.uc_mcontext in RAM → overwrite live CPU registers


B.uc_mcontext (in RAM) ─────────► CPU registers
pc=0x2000b8, sp=0x6fff10 ...      pc=0x2000b8, sp=0x6fff10 ...
Step 3 — CPU's pc now points somewhere new. Next instruction fetch goes there. Execution continues from B's last saved position.

The CPU never stopped. It just got new values loaded into its registers. It has no idea it was "somewhere else" a moment ago.

So the full picture

         LIVE                           SAVED (inert)
┌─────────────────────┐      ┌──────────────────────────┐
│   Physical CPU      │      │  RAM                     │
│                     │      │                          │
│  pc = 0x1000a4  ◄───┼──────┼── coroutine A's ctx      │
│  sp = 0x7fff20      │      │  coroutine B's ctx       │
│  x0 = 42            │      │  coroutine C's ctx       │
│  ...                │      │  scheduler ctx           │
│                     │      │                          │
│  (only ONE of       │      │  (all sitting in RAM,    │
│   these runs at     │      │   doing nothing,         │
│   a time)           │      │   waiting their turn)    │
└─────────────────────┘      └──────────────────────────┘
At any moment, exactly one context is LIVE in the CPU. All the others are frozen snapshots in RAM. swapcontext freezes the current one and thaws another.

One line answer
uc_mcontext is where a context waits. The physical CPU registers are where computation happens. swapcontext moves values between the two.


## Stack pointer and frame pointer


 The Frame Pointer (FP) and Stack Pointer (SP) are both registers that manage the call stack, but they serve different purposes:

  Stack Pointer (SP)
  - Points to the top of the stack (the current end, where the next push occurs)
  - Changes dynamically as the function executes (pushes/pops data)
  - Tracks where new stack data will be written
  - In ARM64: SP (x31) | In x86-64: RSP

  Frame Pointer (FP)
  - Points to the base of the current function's stack frame
  - Remains constant throughout the function's execution
  - Provides a stable reference point for accessing local variables and parameters
  - Set up in the function prologue (push rbp; mov rbp, rsp)
  - Restored in the function epilogue
  - In ARM64: FP (x29) | In x86-64: RBP

  Visual example:
  Higher memory addresses
  |                    |
  |  Previous FP       |  <- Old FP value (saved)
  |  Return address    |
  |  Local variables   |  <- FP points here (base of current frame)
  |  More locals       |
  |  Temp data         |  <- SP points here (top/current position)
  |                    |
  Lower memory addresses

  Why use FP?
  - Without a frame pointer, finding local variables requires calculating from SP, which is complex since SP changes constantly
  - FP provides a fixed reference point: local variables are at fixed offsets from FP (e.g., [FP - 8], [FP - 16])
  - Some systems omit FP (frame pointer omission/FPO) for one extra register, but lose debugging/unwinding ability

### part 2 explanation


 Good question! Let me clarify with a concrete example showing nested function calls.

  Imagine: main() calls foo() which calls bar()

  When bar() is executing:

  Memory Layout (Higher addresses at top)
  ┌─────────────────────┐
  │   main's locals     │
  │   ...               │
  ├─────────────────────┤
  │   main's FP value   │  <- "Previous FP" (saved when foo() was called)
  │   return to main    │  <- Return address for foo()
  ├─────────────────────┤
  │   foo's locals      │
  │   ...               │
  ├─────────────────────┤
  │   foo's FP value    │  <- "Previous FP" (saved when bar() was called)
  │   return to foo     │  <- Return address for bar()
  ├─────────────────────┤
  │   bar's local var 1 │
  │   bar's local var 2 │
  ├─────────────────────┤
      FP points here    │  <- Current FP (bar's frame base)
  ├─────────────────────┤
  │   SP points here    │  <- Current SP (top of stack)
  │   (free space)      │
  └─────────────────────┘
  Lower addresses

  Key points:

  - "Previous FP" = the saved FP of the calling function (foo in this case). It's stored on the stack so when bar() returns, we can restore foo's FP.
  - "FP points here" = the base of the current function's frame (bar). It's a fixed reference point for bar's variables.

  Function prologue (when bar() starts):
  push rbp        ; Save foo's FP ("Previous FP")
  mov rbp, rsp    ; Set new FP to current stack position

  Function epilogue (when bar() returns):
  mov rsp, rbp    ; Restore SP
  pop rbp         ; Restore foo's FP from the stack
  ret             ; Jump back to foo

  So the "Previous FP" is the saved pointer of the parent function, creating a linked chain that allows unwinding the entire call stack.

---



⏺ Yes, exactly right! You've got it. Let me confirm:

  Each coroutine's context structure contains:

  1. General-purpose registers (x19-x28 on ARM64)
    - Values the coroutine was using
  2. Frame Pointer (FP/x29)
    - Saves the base address of the function currently executing in that coroutine
    - Allows access to that function's local variables at fixed offsets
  3. Stack Pointer (SP/x31)
    - Saves the current top of that coroutine's stack
    - The actual execution position in the stack
  4. Link Register (LR/x30)
    - Where to resume execution when the coroutine is switched back in

  And your observation about SP vs FP is spot-on:

  Higher addresses (older data)
  ┌──────────────┐
  │              │
  │ FP = 0x2000  │  <- Function's base (frame pointer)
  │              │
  │ local var 1  │
  │ local var 2  │
  │ local var 3  │
  │ SP = 0x1000  │  <- Current top (stack pointer)
  │              │
  └──────────────┘
  Lower addresses (newer data)

  Since stack grows downward (toward lower addresses) on ARM64/x86-64:
  - SP < FP (SP is at a lower address)
  - As the function allocates locals, SP moves to even lower addresses
  - FP stays constant for that function

  So your understanding is completely correct. The context/control block saves everything needed to restore a coroutine to exactly where it left off:
  its registers, where its stack is, and which function it was in.


---


❯ what is the link register?

⏺ The Link Register (LR) stores the return address — where to jump back to after a function returns.

  How it works:

  When you call a function, the CPU automatically saves the address of the next instruction in the link register. When the function finishes, it jumps
  back to that address.

  Example:

  main():
    Instruction 1: call foo()         ; LR = address of Instruction 2
    Instruction 2: print("returned")  ; Resume here after foo() returns

  foo():
    Instruction A: ...
    Instruction B: ret                ; Jump back to address in LR

  Detailed flow:

  1. CPU executes "call foo()"
     → LR is set to address of next instruction (Instruction 2)
     → PC jumps to foo()

  2. foo() executes its code

  3. foo() executes "ret"
     → PC = LR (jumps to Instruction 2)
     → Execution resumes in main()

  In your coroutine context:

  typedef struct {
      uint64_t x19, x20, x21, x22, x23, x24;
      uint64_t x25, x26, x27, x28;
      uint64_t fp;   /* x29 */
      uint64_t lr;   /* x30 — ret in coro_ctx_swap jumps here */
      uint64_t sp;
  } coro_ctx_t;

  When you save the coroutine's LR, you're saving:
  - "When this coroutine resumes, it should continue from this instruction address"

  Without LR, the coroutine wouldn't know where to resume execution when switched back in.

  Register names:
  - ARM64: x30 or LR
  - x86-64: rax (return value) + stack-stored return address (no dedicated LR register)



---

⏺ Yes, exactly! You've nailed the hierarchy. Let me affirm it clearly:

  Three levels of stack management:

  1. LR (Link Register) - INTER-FUNCTION level
    - Handles transitions between functions (function calls and returns)
    - "When I call another function, where do I come back to?"
    - One LR for the entire call, stores the return address
  2. FP (Frame Pointer) - INTRA-FUNCTION level
    - Manages data within a single function
    - "Where are my local variables and parameters in this function?"
    - Stays constant while that function is executing
    - One FP per active function
  3. SP (Stack Pointer) - INSTRUCTION level
    - Changes with every instruction that uses the stack
    - Tracks the current top as the function executes
    - push, pop, sub sp, add sp all change SP
    - One SP for the entire coroutine, constantly moving

  Visual summary:

  CALL foo()           ← LR involved (inter-function)
    |
    └─> Inside foo():
         FP constant   ← FP used (intra-function)
         SP changes    ← SP involved (instruction-level)
         Every instruction may adjust SP
         But FP remains the same
    |
  RETURN from foo()    ← LR involved (jump back)

  So for coroutines, saving all three is essential:
  - LR = where to resume in the call chain
  - FP = which function's scope we're in
  - SP = where in the stack execution we left off

  Excellent mental model!


---





⏺ What "caller-saved" actually means — from scratch

  Let's start with the problem the ABI is solving.

  ---
  The problem

  The CPU has a fixed set of registers. Every piece of code — your
  function, the function you call, the function that calls you — shares the
   same physical registers. There's only one x0, one x5, one x12.

  So when you call a function, a conflict arises:

  your code:
      x0 = 42          // you put something important here
      bl  some_function // you call a function
      // some_function used x0 internally and set it to 999
      // now x0 = 999, your 42 is gone
      use x0           // WRONG VALUE

  Someone has to save and restore registers around calls. The question is:
  who?

  ---
  The ABI's answer — split responsibility

  The ABI (the agreement that all compiled code follows) divides registers
  into two groups and assigns responsibility:

  Caller-saved (x0–x18):
  The caller is responsible. If you call a function and want your value in
  x5 to survive, YOU save x5 before the call and restore it after. The
  function you're calling is free to use x5 however it wants — it owes you
  nothing.

  Callee-saved (x19–x28, fp, lr, sp):
  The callee is responsible. If a function uses x19, it must save x19 at
  the start and restore it before returning. The caller can put something
  in x19, call any function, and trust that x19 will be unchanged when the
  function returns.

  ---
  Why this split makes sense

  Think about it from a compiler's perspective. When it compiles
  some_function, it doesn't know who calls it or what values the caller has
   in registers. So it can't know which registers to preserve for the
  caller.

  Instead, the ABI says: caller-saved registers are a free-for-all scratch
  space for whatever function is currently running. If you want a value to
  survive a call, save it yourself first.

  Callee-saved registers are a guarantee — any function can use them and
  trust they won't be clobbered by functions it calls.

  ---
  Concrete example

  void your_coroutine(void *arg) {
      int a = 10;   // compiler puts this in x19 (callee-saved)
      int b = 20;   // compiler puts this in x0  (caller-saved)

      coro_yield();

      // after yield, a must still be 10, b must still be 20
      printf("%d %d\n", a, b);
  }

  The compiler sees coro_yield() is a function call. It knows:
  - x19 (which holds a) is callee-saved — coro_yield must preserve it
  - x0 (which holds b) is caller-saved — coro_yield might trash it

  So the compiler generates something like this:

  your_coroutine:
      mov  x19, #10        // a = 10  (callee-saved, safe across calls)
      mov  x0,  #20        // b = 20  (caller-saved, NOT safe)

      str  x0, [sp, #-8]!  // compiler pushes b onto the stack BEFORE the
  call
                           // because it knows coro_yield might trash x0

      bl   coro_yield      // call yield — x0 might get destroyed inside

      ldr  x0, [sp], #8    // compiler pops b back off the stack AFTER the
  call
                           // restoring b = 20 into x0

      // now x19 = 10 (survived because coro_yield preserved it)
      // and x0  = 20 (survived because WE saved/restored it around the
  call)

  Notice: a survived automatically because coro_yield follows the ABI and
  preserves x19. b survived because the compiler saved it on the stack
  before the call. Neither value was lost.

  ---
  Why this matters for our context switch

  When coro_yield calls coro_ctx_swap, the same thing happens one level
  deeper. The compiler, when it compiled coro_yield, already saved any
  caller-saved registers it cared about before calling coro_ctx_swap.

  So by the time coro_ctx_swap runs — the actual assembly that does the
  switch — the only registers that matter are the callee-saved ones.
  Everything else is already on the stack.

  That's why coro_ctx_t only saves x19–x28, fp, lr, sp. Not because x0–x18
  don't matter, but because the compiler already took care of them. They're
   sitting on the coroutine's stack, and the stack is preserved because sp
  is in coro_ctx_t.

  ---
  The full chain visualized

  coroutine is running:
    x0  = some value   ← caller-saved, compiler manages
    x19 = some value   ← callee-saved, coro_ctx_swap manages

  coroutine calls coro_yield():
    compiler: "x0 might be trashed, save it"
    → pushes x0 onto coroutine's stack
    → stack now has x0's value sitting in it

  coro_yield calls coro_ctx_swap():
    our asm: saves x19-x28, fp, lr, sp into coro_ctx_t
             sp in coro_ctx_t now points to the stack
             that stack still has x0's value in it
    → switches to scheduler

  later, coroutine resumes via coro_ctx_swap():
    our asm: restores x19-x28, fp, lr, sp from coro_ctx_t
             sp now points back to coroutine's stack
             x0's value is still sitting there

  coro_yield returns:
    compiler: "I saved x0 before, restore it now"
    → pops x0 back off the stack into the register
    → x0 has its original value again

  coroutine continues:
    x0  = original value ✓  (came back via stack)
    x19 = original value ✓  (came back via coro_ctx_t)

  *** 
  ### REALLY IMPORTANT 
  sp is the key. It's the pointer into the stack. Saving sp in coro_ctx_t
  means the entire stack — including all the compiler-pushed caller-saved
  values — is implicitly preserved too.
  ***

---


---

### This is it 

⏺ SP is the key — explained fully

  Let's build this up from scratch with a concrete scenario.

  ---
  The setup

  You have a coroutine running task_a. Inside it, the compiler placed
  some values in registers:

  CPU registers (live, right now):
    x0  = 42      ← caller-saved (some value the coroutine is using)
    x5  = 99      ← caller-saved
    x19 = 10      ← callee-saved (a local variable)
    sp  = 0x7000  ← points to top of coroutine's stack

  The coroutine calls coro_yield().

  ---
  What happens before coro_ctx_swap even runs

  The compiler compiled coro_yield() as a function call. Before
  generating the bl coro_yield instruction, the compiler looked at which
  registers it was using and asked: "which of these are caller-saved?"

  It found x0 = 42 and x5 = 99. It knows coro_yield is allowed to trash
  those. So it emits code to push them onto the stack before the call:

  ; compiler-generated code inside task_a, before bl coro_yield:
  stp  x0, x5, [sp, #-16]!    ; push x0 and x5 onto the stack
                                ; sp is now 0x6FF0

  bl   coro_yield              ; call yield — x0 and x5 might get
  destroyed

  Stack right now, before coro_ctx_swap runs:

  high address
  ┌─────────────────┐
  │  ... other data │ ← older stack frames
  ├─────────────────┤
  │  x5 = 99        │ ← pushed by compiler at 0x6FF8
  │  x0 = 42        │ ← pushed by compiler at 0x6FF0
  ├─────────────────┤ ← sp = 0x6FF0  (top of stack right now)
  │  (free space)   │
  └─────────────────┘
  low address

  The values 42 and 99 are now inside the stack. They live at memory
  addresses 0x6FF0 and 0x6FF8. They're not in registers anymore — they're
   just bytes sitting in RAM.

  ---
  Now coro_ctx_swap runs

  Our assembly saves:

  coro_ctx_t saves:
    x19 = 10       ← callee-saved register
    sp  = 0x6FF0   ← current top of stack
    lr  = <return address into coro_yield>
    ... other callee-saved regs

  That's it. x0 and x5 are not in coro_ctx_t. They don't need to be.
  They're already in the stack at 0x6FF0 and 0x6FF8.

  The context switch loads the scheduler's registers. The coroutine is
  now frozen.

  ---
  Why the values are still safe

  The coroutine's stack is just a region of RAM. Nobody else touches it.
  It just sits there.

  When the scheduler eventually calls coro_resume(a) again, coro_ctx_swap
   loads a->ctx:

  restored from coro_ctx_t:
    x19 = 10       ← callee-saved, back in the register
    sp  = 0x6FF0   ← stack pointer restored
    lr  = <address inside coro_yield>
    ret            ← CPU jumps back into coro_yield

  Now sp is pointing at 0x6FF0 again — the exact same place it was when
  we left.

  coro_yield returns. Back in task_a, the compiler now generates the
  restore:

  ; compiler-generated code inside task_a, after bl coro_yield returns:
  ldp  x0, x5, [sp], #16    ; pop x0 and x5 back off the stack
                              ; x0 = 42, x5 = 99 again
                              ; sp is back to 0x7000

  x0 = 42 and x5 = 99 are back. They survived the entire context switch
  without ever being in coro_ctx_t.

  ---
  The chain visualized

  BEFORE yield:
    CPU: x0=42, x5=99, x19=10, sp=0x7000

  task_a calls coro_yield():
    compiler pushes x0, x5 onto stack
    CPU: x0=42, x5=99, x19=10, sp=0x6FF0
    stack[0x6FF0] = 42
    stack[0x6FF8] = 99

  coro_ctx_swap saves:
    coro_ctx_t: { x19=10, sp=0x6FF0, lr=... }
    x0 and x5 are NOT here — they're in the stack

    ~~~ coroutine frozen, other things run ~~~

    stack[0x6FF0] = 42   ← still there, nobody touched it
    stack[0x6FF8] = 99   ← still there

  coro_ctx_swap restores (on resume):
    CPU: x19=10, sp=0x6FF0, lr=...
    ret → back into coro_yield

  coro_yield returns to task_a:
    compiler pops x0, x5 off the stack
    CPU: x0=42, x5=99  ← back
    sp=0x7000           ← back

  ---
  The one-sentence version

  sp is a pointer. When you save sp, you preserve everything the pointer
  is pointing to — the entire stack, including every value the compiler
  pushed onto it. The values don't need to be in coro_ctx_t because
  they're already stored in memory, and sp is the address that finds
  them.

  coro_ctx_t saves what's in registers. The stack saves what the compiler
   pushed from registers. sp connects the two. Restore sp, and you get
  both back.



---





 here in the assembly function we are not provding any arguments to it
  right? I mean from and to?

⏺ Great question! This is about the calling convention — how arguments get
  passed to functions.

  ---
  Arguments ARE being passed — via registers

  When coro_yield calls coro_ctx_swap(&c->ctx, &t_sched_ctx), the C code looks
  like this:

  coro_ctx_swap(&c->ctx, &t_sched_ctx);

  The compiler sees a function call with two arguments. Following the ARM64 ABI
  (Application Binary Interface), the compiler automatically puts those
  arguments into specific registers:

  - First argument (&c->ctx) → goes into register x0
  - Second argument (&t_sched_ctx) → goes into register x1

  By the time the assembly function _coro_ctx_swap starts executing, those
  pointers are already sitting in x0 and x1. The assembly doesn't need to ask
  for them — they're just there, handed over by the caller.

  ---
  Why it looks like there are no arguments

  In C, you write:
  coro_ctx_swap(&c->ctx, &t_sched_ctx);

  In assembly, that becomes:
  mov  x0, <address of c->ctx>      ; compiler generates this
  mov  x1, <address of t_sched_ctx>  ; compiler generates this
  bl   _coro_ctx_swap               ; branch and link (call the function)

  The bl instruction doesn't have argument syntax. The arguments are already in
  the registers. The assembly function just assumes they're there and uses them
  immediately.

  ---
  Inside the assembly function

  When _coro_ctx_swap starts:

  _coro_ctx_swap:
      stp  x19, x20, [x0, #0]    ; save to *x0 (first arg)
      ...
      ldp  x19, x20, [x1, #0]    ; load from *x1 (second arg)

  x0 and x1 are not declared or created. They're just used as if they hold the
  arguments — because they do. The caller put them there before calling.

  ---
  The ABI contract

  This is the calling convention:
  - Caller's responsibility: put arguments in x0, x1, x2, ... before calling
  - Callee's responsibility: find arguments already in x0, x1, x2, ... when the
  function starts

  It's a handshake. The C compiler handles the caller side (putting arguments in
   registers). The assembly function just assumes they're there and uses them
  directly.

  ---
  Another way to see it

  Think of it like walking into a room:

  C code:
  coro_ctx_swap(&c->ctx, &t_sched_ctx);
  Translation: "I'm calling this function with two pointers."

  What actually happens:
  The compiler says: "I'll put those two pointers in x0 and x1, then jump to the
   function."

  Assembly function:
  _coro_ctx_swap:
      stp x19, x20, [x0, #0]
  Says: "Oh, my first argument is in x0. Let me use it."

  No explicit parameter list. The registers ARE the parameter list. That's how
  ARM64 works.
  

---



### IMPORTANT BASICS


❯ when we mean by coroutine, it is a thread or a
  process or what

⏺ Coroutine ≠ Thread ≠ Process

  They're three different things. Let me compare them
  clearly.

  ---
  Process

  ┌─────────────────────────────────────────┐
  │         Process (task in taskbar)       │
  │  ┌───────────────────────────────────┐  │
  │  │ Virtual Address Space (64-bit)    │  │
  │  │  ┌─────────────┐                  │  │
  │  │  │  code       │                  │  │
  │  │  ├─────────────┤                  │  │
  │  │  │  heap       │                  │  │
  │  │  ├─────────────┤                  │  │
  │  │  │  stack      │                  │  │
  │  │  │  (one only) │                  │  │
  │  │  └─────────────┘                  │  │
  │  └───────────────────────────────────┘  │
  │                                          │
  │  Managed by: OS kernel                   │
  │  Created by: fork(), execve()            │
  │  Context switch cost: expensive          │
  │  Memory footprint: ~10 MB                │
  └─────────────────────────────────────────┘

  - Owned by the OS kernel — only the OS can
  create/destroy it
  - Isolated virtual address space — each process has
  its own memory view, completely separate from others
  - Heavy weight — context switching between processes
  is slow (must flush caches, update page tables, etc.)
  - One execution flow (at least in the single-threaded
   case)

  ---
  Thread

  ┌────────────────────────────────────────────────────
  ──┐
  │                 Process
    │
  │  ┌────────────────────────────────────────────────┐
    │
  │  │    Shared Virtual Address Space                │
    │
  │  │  ┌────────────────────────────────────────┐    │
    │
  │  │  │         heap (shared by all threads)   │    │
    │
  │  │  │         code (shared by all threads)   │    │
    │
  │  │  └────────────────────────────────────────┘    │
    │
  │  │
  │  │
  │  │  ┌──────────────┐  ┌──────────────┐            │
    │
  │  │  │ Thread 1     │  │ Thread 2     │            │
    │
  │  │  │ stack        │  │ stack        │            │
    │
  │  │  │ registers    │  │ registers    │            │
    │
  │  │  │ (separate)   │  │ (separate)   │            │
    │
  │  │  └──────────────┘  └──────────────┘            │
    │
  │  └────────────────────────────────────────────────┘
    │
  │
     │
  │  Managed by: OS kernel
    │
  │  Created by: pthread_create(), std::thread
    │
  │  Context switch cost: moderately expensive
    │
  │  Memory footprint: ~1-8 MB per thread
    │
  └────────────────────────────────────────────────────
  ──┘

  - Also owned by the OS kernel — the OS schedules
  which thread runs when
  - Share the same virtual address space — multiple
  threads in one process can see/access the same memory
  - Lighter than processes — context switching is
  cheaper than processes, but still involves OS
  - Multiple execution flows in parallel — truly
  concurrent (on multicore) or time-sliced (on single
  core)
  - Synchronization needed — mutex, semaphore,
  condition variables to protect shared memory

  ---
  Coroutine (what we're building)

  ┌────────────────────────────────────────────────────
  ──┐
  │                 Process
    │
  │  ┌────────────────────────────────────────────────┐
    │
  │  │    Shared Virtual Address Space                │
    │
  │  │  ┌────────────────────────────────────────┐    │
    │
  │  │  │         heap (shared)                  │    │
    │
  │  │  │         code (shared)                  │    │
    │
  │  │  └────────────────────────────────────────┘    │
    │
  │  │
  │  │
  │  │  ┌──────────────┐  ┌──────────────┐            │
    │
  │  │  │ Coroutine 1  │  │ Coroutine 2  │  (all on  │
   │
  │  │  │ stack        │  │ stack        │   one OS  │
   │
  │  │  │ registers    │  │ registers    │   thread) │
   │
  │  │  │ (saved)      │  │ (saved)      │            │
    │
  │  │  └──────────────┘  └──────────────┘            │
    │
  │  │
  │  │
  │  │  (User code manages switching)                 │
    │
  │  └────────────────────────────────────────────────┘
    │
  │
     │
  │  Managed by: YOUR CODE (library)
     │
  │  Created by: coro_create()
     │
  │  Context switch cost: very cheap (~100 CPU cycles)
     │
  │  Memory footprint: 64 KB stack + struct
  (configurable)│
  └────────────────────────────────────────────────────
  ──┘

  - Owned by your application code — not the OS. Your
  library (coro_create, coro_resume) manages them
  - Share the same virtual address space — like
  threads, but all run on the same OS thread (usually)
  - Very lightweight — context switch is just
  saving/restoring registers. No OS involvement. No
  cache flushing.
  - Cooperative multitasking — coroutines voluntarily
  yield control (unlike threads which are preempted by
  the OS)
  - No synchronization overhead — since only one runs
  at a time on a given OS thread, no mutex needed

  ---
  Side-by-side comparison

  Aspect: Who creates it?
  Process: OS kernel
  Thread: OS kernel
  Coroutine: Your code
  ────────────────────────────────────────
  Aspect: Who schedules it?
  Process: OS kernel
  Thread: OS kernel
  Coroutine: You (or a scheduler library)
  ────────────────────────────────────────
  Aspect: Memory isolation
  Process: Complete
  Thread: Shared heap
  Coroutine: Shared heap
  ────────────────────────────────────────
  Aspect: Stack
  Process: One per process
  Thread: One per thread
  Coroutine: One per coroutine, mmap'd
  ────────────────────────────────────────
  Aspect: Context switch cost
  Process: ~1000s of cycles
  Thread: ~100s of cycles
  Coroutine: ~10-50 cycles
  ────────────────────────────────────────
  Aspect: Concurrency type
  Process: Preemptive
  Thread: Preemptive
  Coroutine: Cooperative (you call yield)
  ────────────────────────────────────────
  Aspect: Synchronization
  Process: Heavy (messages, pipes)
  Thread: Complex (mutex, semaphore)
  Coroutine: Simple (no locks needed usually)
  ────────────────────────────────────────
  Aspect: Number you can have
  Process: Few (system limit)
  Thread: Hundreds to thousands
  Coroutine: Millions (memory permitting)

  ---
  The key difference: who's in control?

  Process/Thread: The OS kernel decides when to switch.
   Your code just runs until the OS yanks the CPU away
  (preemption).

  Coroutine: Your code decides when to switch.
  Explicitly call coro_yield() to give up the CPU.

  // Thread: you DON'T control the switch
  void thread_func() {
      printf("step 1\n");
      // ← OS might yank the CPU here and run another
  thread
      printf("step 2\n");
  }

  // Coroutine: you DO control the switch
  void coro_func(void *arg) {
      printf("step 1\n");
      coro_yield();  // ← YOU explicitly say "I'm done
  now"
      printf("step 2\n");
  }

  ---
  In our code specifically

  When you do:

  coroutine_t *a = coro_create(task_a, NULL, 0, 0);
  coroutine_t *b = coro_create(task_b, NULL, 0, 0);

  coro_resume(a);   // run a until it yields
  coro_resume(b);   // run b until it yields
  coro_resume(a);   // run a again

  Both a and b are coroutines. They're not threads.
  They're not even known to the OS. To the OS, you have
   one thread running main(). That one thread is
  manually jumping between a and b's stacks using our
  assembly context switch.

  The OS has no idea coroutines exist. It just sees one
   thread. Your application code is the one juggling
  the coroutines.

  ---
  Why coroutines for this project?

  - Speed: No OS overhead. Context switch is just
  stp/ldp instructions.
  - Scalability: You can have millions of coroutines
  without kernel overhead.
  - Go compatibility: Go uses M:N scheduling — many
  goroutines (coroutines) mapped to few OS threads.
  Same model.
  - Predictability: You control exactly when switching
  happens (no surprise preemption).



---



### I AM NEW. EXPLAIN EVEN MORE SIMPLER



❯ Ok explain it in even more detail and simpler. I am new

⏺ Coroutines vs Threads vs Processes — From Zero

  Let me start at the very beginning.

  ---
  What's a "program running"?

  When you run a program, the CPU executes instructions one at a time. Your code is a sequence of instructions.

  void foo() {
      printf("hello\n");      // instruction 1
      printf("world\n");      // instruction 2
      int x = 5;              // instruction 3
      printf("%d\n", x);      // instruction 4
  }

  The CPU goes: instruction 1 → instruction 2 → instruction 3 → instruction 4. Done.

  That's execution.

  ---
  The Problem: "I want to do two things at the same time"

  But what if I want to run two functions at the same time?

  void task_a() {
      printf("[A] step 1\n");
      printf("[A] step 2\n");
      printf("[A] step 3\n");
  }

  void task_b() {
      printf("[B] step 1\n");
      printf("[B] step 2\n");
      printf("[B] step 3\n");
  }

  int main() {
      // I want A and B to run "at the same time"
      // Like:
      // [A] step 1
      // [B] step 1
      // [A] step 2
      // [B] step 2
      // [A] step 3
      // [B] step 3
  }

  But the CPU only runs one instruction at a time. So how do we do this?

  Answer: We fake it by quickly switching between them.

  ---
  The Switching Trick

  Imagine you have a piece of paper for each task:

  Task A's notebook:
  □ [A] step 1
  □ [A] step 2
  □ [A] step 3

  Task B's notebook:
  □ [B] step 1
  □ [B] step 2
  □ [B] step 3

  You (the CPU) do this:
  1. Read Task A's notebook: "[A] step 1" → write it down (execute)
  2. Stop and put Task A's notebook away
  3. Take out Task B's notebook: "[B] step 1" → write it down
  4. Stop and put Task B's notebook away
  5. Take out Task A's notebook again: "[A] step 2" → write it down
  6. Stop and put Task A's notebook away
  7. Take out Task B's notebook: "[B] step 2" → write it down
  8. ... and so on

  Output:
  [A] step 1
  [B] step 1
  [A] step 2
  [B] step 2
  [A] step 3
  [B] step 3

  You did both! But you didn't run them truly in parallel — you switched very fast.

  ---
  What's in the "notebook"?

  The notebook is your state:
  - Where you are in the code (which line / instruction)
  - The values of all your variables
  - Where data is stored in memory
  - Everything needed to resume from where you left off

  In computer terms, this is called a context or execution context.

  ---
  Now here are the three ways to do this switching:

  ---
  1. PROCESS — The OS manages everything

  Let's say the OS (operating system — Windows, macOS, Linux) manages the switching.

  Your laptop
  ├── Process A (task_a running)
  │   ├── Memory: 0x0 to 0xFFFFFFFF (4 GB)
  │   ├── Stack for task_a
  │   ├── Variables
  │   └── Registers
  │
  ├── Process B (task_b running)
  │   ├── Memory: 0x0 to 0xFFFFFFFF (4 GB, DIFFERENT from A!)
  │   ├── Stack for task_b
  │   ├── Variables
  │   └── Registers
  │
  └── OS Kernel (the boss)
      └── Scheduler: "run Process A for 10ms, then B, then A, then B..."

  Key facts:

  - Separate memory worlds: Process A and Process B can't see each other's memory. They each think they own the entire 4GB address space.
  - The OS controls switching: Every 10 milliseconds (or when a process waits for I/O), the OS switches.
  - Your code doesn't know it's switching: You just write normal code. The OS yells "STOP!" and switches without asking.
  - Heavy weight: The OS does a lot of work switching (save memory tables, flush caches, etc.)
  - Isolated: Great for security (one program crashing doesn't kill another), but expensive.

  ---
  2. THREAD — Also OS-managed, but lighter

  Now the OS can do something smarter.

  Your laptop
  └── Process (single, shared memory)
      ├── Memory: 0x0 to 0xFFFFFFFF (shared by all threads)
      ├── Heap (shared by all threads)
      ├── Code (shared by all threads)
      │
      ├── Thread A (task_a running)
      │   ├── Stack for task_a (separate)
      │   ├── Registers (separate)
      │   └── Variables on its own stack
      │
      ├── Thread B (task_b running)
      │   ├── Stack for task_b (separate)
      │   ├── Registers (separate)
      │   └── Variables on its own stack
      │
      └── OS Kernel (the boss)
          └── Scheduler: "run Thread A for 10ms, then B, then A..."

  Key facts:

  - Shared memory: Both threads can see and access the same data (the heap, global variables, etc.)
  - Separate stacks: Each thread has its own stack, but they're in the same process
  - Still OS-managed: The OS still does the switching, still "yells STOP!"
  - Lighter than processes: Faster to switch (less state to save), less memory per thread
  - But shared memory = complexity: Both threads can access the same variable at the same time. Chaos! Need locks (mutex, semaphore) to prevent corruption.

  Example of the problem:

  int counter = 0;

  void thread_a() {
      counter = counter + 1;  // read counter, add 1, write back
  }

  void thread_b() {
      counter = counter + 1;  // read counter, add 1, write back
  }

  // Main runs both threads
  // Expected: counter = 2
  // Actual: counter = 1 (if they interfere!)

  Why? Thread A reads counter (0), starts to add 1, but gets interrupted. Thread B reads counter (still 0), adds 1, writes back (counter = 1). Thread A wakes up, finishes adding
   1, writes back (counter = 1). Oops!

  This is why threads are complex — you need locks.

  ---
  3. COROUTINE — You manage the switching (what we're building)

  Now let's do it without the OS's help.

  Your laptop
  └── Process (single)
      ├── Memory: 0x0 to 0xFFFFFFFF (shared)
      ├── Heap (shared by all coroutines)
      ├── Code (shared by all coroutines)
      │
      ├── OS Thread (one)
      │   └── Scheduler: YOUR CODE
      │       ├── Coroutine A
      │       │   ├── Stack for A (64 KB, mmap'd)
      │       │   ├── Registers (saved in coro_ctx_t)
      │       │   └── Variables
      │       │
      │       ├── Coroutine B
      │       │   ├── Stack for B (64 KB, mmap'd)
      │       │   ├── Registers (saved in coro_ctx_t)
      │       │   └── Variables
      │       │
      │       └── Main code (scheduler)
      │           coro_resume(A)  // ← YOU decide
      │           coro_resume(B)  // ← YOU decide

  Key facts:

  - No OS involvement: The OS doesn't know coroutines exist. It just sees one thread.
  - You decide switching: YOU write coro_resume(a), then coro_resume(b). The OS doesn't interrupt.
  - Shared memory: All coroutines share the heap (like threads).
  - Separate stacks: Each coroutine has its own 64 KB stack, but YOU (not the OS) manage them.
  - Super lightweight: Switching is just assembly instructions — save 13 registers, load 13 registers. Done. ~10-50 CPU cycles.
  - Cooperative: Coroutines must voluntarily yield. A coroutine can't be interrupted mid-function.

  ---
  Let me show the actual code difference

  PROCESS (OS manages)

  #include <unistd.h>

  int counter = 0;

  void task_a() {
      for (int i = 0; i < 1000000; i++) {
          counter++;
      }
  }

  int main() {
      if (fork() == 0) {
          // Child process (Process B)
          task_a();
          exit(0);
      } else {
          // Parent process (Process A)
          task_a();
          wait(NULL);  // Wait for child
      }
      printf("counter = %d\n", counter);
  }

  Run this: you'll get counter = 1000000. But it printed twice — once in Process A, once in Process B. They're separate worlds!

  ---
  THREAD (OS manages)

  #include <pthread.h>

  int counter = 0;

  void* task_a(void *arg) {
      for (int i = 0; i < 1000000; i++) {
          counter++;  // DANGER: both threads access this!
      }
      return NULL;
  }

  int main() {
      pthread_t t;
      pthread_create(&t, NULL, task_a, NULL);
      task_a(NULL);  // Main is also a thread
      pthread_join(t, NULL);
      printf("counter = %d\n", counter);  // Might be wrong!
  }

  Run this: you'll get counter = something less than 2000000 (usually). The threads interfered.

  ---
  COROUTINE (You manage)

  #include "coroutine.h"

  int counter = 0;

  void task_a(void *arg) {
      for (int i = 0; i < 1000000; i++) {
          counter++;
          if (i % 100000 == 0) {
              coro_yield();  // ← YOU decide when to switch
          }
      }
  }

  int main() {
      coroutine_t *a = coro_create(task_a, NULL, 0, 0);
      coroutine_t *b = coro_create(task_a, NULL, 0, 0);

      while (a->state != CORO_DONE || b->state != CORO_DONE) {
          if (a->state == CORO_READY) coro_resume(a);
          if (b->state == CORO_READY) coro_resume(b);
      }

      printf("counter = %d\n", counter);  // Guaranteed 2000000 (no interference!)
      coro_destroy(a);
      coro_destroy(b);
  }

  Run this: you'll get counter = 2000000. Guaranteed! No race conditions because only one coroutine runs at a time (on the same OS thread).

  ---
  Why are coroutines safe but threads aren't?

  Thread scenario:
  Thread A: read counter (0)
            add 1
            [OS interrupts here!]

  Thread B: read counter (still 0!)
            add 1
            write counter = 1

  Thread A: write counter = 1

  Coroutine scenario:
  Coroutine A: read counter (0)
               add 1
               [you call coro_yield()]
               write counter = 1

  Coroutine B: read counter (1)
               add 1
               [you call coro_yield()]
               write counter = 2

  With coroutines, you control exactly when switching happens. You can make sure a coroutine finishes the whole read-modify-write before yielding. No interruption = no race
  condition.

  ---
  Summary Table

  ┌────────────────────────────────┬────────────────────────────────┬──────────────────────────┬──────────────────────────────┐
  │                                │            Process             │          Thread          │          Coroutine           │
  ├────────────────────────────────┼────────────────────────────────┼──────────────────────────┼──────────────────────────────┤
  │ Who switches?                  │ OS (you can't control)         │ OS (you can't control)   │ You (you control)            │
  ├────────────────────────────────┼────────────────────────────────┼──────────────────────────┼──────────────────────────────┤
  │ Shared memory?                 │ No (separate worlds)           │ Yes (same heap)          │ Yes (same heap)              │
  ├────────────────────────────────┼────────────────────────────────┼──────────────────────────┼──────────────────────────────┤
  │ Each has its own stack?        │ Yes                            │ Yes                      │ Yes                          │
  ├────────────────────────────────┼────────────────────────────────┼──────────────────────────┼──────────────────────────────┤
  │ How expensive is a switch?     │ Very expensive (~1000s cycles) │ Expensive (~100s cycles) │ Very cheap (~10-50 cycles)   │
  ├────────────────────────────────┼────────────────────────────────┼──────────────────────────┼──────────────────────────────┤
  │ Can you have many?             │ No (OS limits: ~100s)          │ Maybe (~1000s)           │ Yes! (~millions)             │
  ├────────────────────────────────┼────────────────────────────────┼──────────────────────────┼──────────────────────────────┤
  │ Race conditions?               │ No (separate memory)           │ Yes (need locks!)        │ No (only one runs at a time) │
  ├────────────────────────────────┼────────────────────────────────┼──────────────────────────┼──────────────────────────────┤
  │ When do you control switching? │ Never                          │ Never                    │ Always                       │
  └────────────────────────────────┴────────────────────────────────┴──────────────────────────┴──────────────────────────────┘

  ---
  What's happening in our code right now?

  coro_resume(a);  // "Run coroutine A until it yields"
  printf("[main] A yielded, resuming B\n");
  coro_resume(b);  // "Run coroutine B until it yields"

  - We have one OS thread (the main thread)
  - We have two coroutines (A and B)
  - Main is the scheduler — it decides who runs
  - When A yields, main resumes
  - Then main resumes B
  - No OS interference — pure software

  The OS thinks only main is running. It has no idea coroutines A and B exist!


---



### DUMB AHH



❯ When you say Memory, heap, code, stack, variabels, registers. Arent they all part of memory itself

⏺ YES! You're exactly right!

  This is an excellent question and shows you're thinking clearly. Let me clarify the terminology.

  ---
  Everything IS memory

  All of this — heap, stack, code, variables, registers — is ultimately stored in physical hardware.

  But we categorize them by location and purpose. Think of it like a city:

  All buildings are made of bricks.
  But we organize them by purpose:
    - Residential zone (houses)
    - Commercial zone (stores)
    - Industrial zone (factories)
    - Roads

  All bricks, different purposes and locations.

  Same with memory.

  ---
  The Memory Map

  When a program runs, its virtual address space is divided into regions. On a 64-bit machine, you have addresses from 0x0 to 0xFFFFFFFFFFFFFFFF.

  Here's how they're organized (roughly):

  Address Space (from low to high)
  ┌─────────────────────────────────────────┐
  │ 0x0                                     │
  │                                         │  ← Reserved, can't use
  ├─────────────────────────────────────────┤
  │ TEXT SEGMENT (Code)                     │  ← Read-only
  │ 0x400000 (example)                      │  ← Your instructions live here
  │ printf(), main(), task_a(), etc.        │
  ├─────────────────────────────────────────┤
  │ INITIALIZED DATA SEGMENT                │  ← Read/Write
  │ 0x600000 (example)                      │  ← Global variables with values
  │ int global_x = 5;  (the "5" is here)    │
  ├─────────────────────────────────────────┤
  │ UNINITIALIZED DATA (BSS)                │  ← Read/Write
  │ 0x601000 (example)                      │  ← Global variables without values
  │ int global_y;  (gets zeroed)            │
  ├─────────────────────────────────────────┤
  │ HEAP (grows upward ↑)                   │  ← Read/Write
  │ 0x700000 (example)                      │  ← malloc(), calloc() go here
  │ Used by: malloc("hello"), new object    │
  ├─────────────────────────────────────────┤
  │                                         │  ← Free space (can expand)
  ├─────────────────────────────────────────┤
  │ STACK (grows downward ↓)                │  ← Read/Write
  │ 0x7FFFFFFF (example)                    │  ← Local variables go here
  │ void foo() { int x; } ← x is here       │
  ├─────────────────────────────────────────┤
  │ 0xFFFFFFFFFFFFFFFF                      │
  │                                         │  ← Kernel space (can't access)
  └─────────────────────────────────────────┘

  ---
  What each region actually is

  All of these regions are RAM (memory chips). But we name them by what's stored there:

  TEXT (Code)

  int main() {
      printf("hello\n");
  }

  The CPU fetches bytes from the TEXT region. Those bytes ARE the instructions (stp x19, x20, ret, etc.). It's just data — binary 1s and 0s that the CPU
  interprets as "do this instruction."

  Location: Low addresses (example: 0x400000)
  Stored in: RAM, marked read-only (you can't modify code at runtime)
  Who puts it there: The OS when it loads your program

  ---
  INITIALIZED DATA

  int global_x = 5;

  This variable is global (not in a function). The value 5 is baked into your binary file. When the OS loads the program, it reads that 5 from disk and puts it
  into RAM at some address.

  Location: Example 0x600000
  Stored in: RAM
  Value: 5 (just bytes in memory)
  Who puts it there: The OS when loading the program (from your binary file)

  ---
  BSS (Uninitialized Data)

  int global_y;  // no value

  Same as above, but the value isn't specified. So the OS just allocates space and zeros it out.

  Location: Example 0x601000
  Stored in: RAM
  Value: 0 (all zeros)
  Who puts it there: The OS (just allocates and zeros)

  ---
  HEAP

  int *ptr = malloc(100);  // allocate 100 bytes
  ptr[0] = 42;
  ptr[1] = 99;

  When you call malloc, it says "give me some memory from the heap region." It returns a pointer (an address, like 0x700000). You write data there.

  Location: Example 0x700000 and growing upward
  Stored in: RAM
  Managed by: You (malloc/free)
  Lifetime: Until you free() it (or program ends)
  Values: Anything you put there

  In our coroutine project:
  void *mem = mmap(NULL, total, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  We're asking the OS for a chunk of the heap region. That chunk becomes a coroutine's stack.

  ---
  STACK

  void foo() {
      int x = 5;      // ← x lives here
      int y = 10;     // ← y lives here
      bar();          // ← foo's return address pushed here
  }

  void bar() {
      int z = 20;     // ← z lives here
  }

  When foo() is called, the CPU allocates space on the stack for x and y. When foo() calls bar(), the CPU allocates space for z on the same stack (just lower
  address, since stack grows downward).

  Location: Example 0x7FFFFFFF and growing downward
  Stored in: RAM
  Managed by: The CPU automatically (push/pop)
  Lifetime: Until the function returns
  SP (stack pointer): A register that points to the current top

  In our coroutine project:
  void *mem = mmap(NULL, total, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  c->stack = (char *)mem + CORO_GUARD_PAGE_SIZE;
  We allocate our own stack region (64 KB) using mmap. Each coroutine gets one.

  ---
  Variables — where do they actually live?

  Local variable (in a function):
  void foo() {
      int x = 5;  // ← goes on the STACK
  }
  Stored in: STACK region of RAM

  Global variable:
  int global_x = 5;  // ← goes in INITIALIZED DATA
  int global_y;      // ← goes in BSS
  Stored in: INITIALIZED DATA or BSS region of RAM

  Dynamic allocation:
  int *ptr = malloc(sizeof(int));
  *ptr = 5;  // ← goes on the HEAP
  Stored in: HEAP region of RAM

  ---
  Registers — the Exception

  Registers are NOT in RAM. They're physically inside the CPU chip itself.

  CPU Chip
  ┌────────────────────────────────────┐
  │ x0  ┌──┐                           │
  │     │42│  ← physically on the chip │
  │ x1  ├──┤                           │
  │     │99│  ← NOT in RAM             │
  │ x19 ├──┤                           │
  │     │10│                           │
  │ sp  ├──┤                           │
  │     │0x6FF0│ ← points to RAM        │
  │ ... └──┘                           │
  └────────────────────────────────────┘

  RAM (separate from CPU)
  ┌────────────────────────────────────┐
  │ Address 0x6FF0: [some value]       │
  │ Address 0x6FF8: [some value]       │
  │ ...                                │
  └────────────────────────────────────┘

  Registers are fast because they're on the chip, not across the bus to RAM.

  When we do:
  stp x19, x20, [x0, #0]

  We're copying values FROM registers (on the chip) TO RAM (the address x0 points to).

  ---
  So to answer your question

  Are they all memory?

  Yes, except registers. Here's the breakdown:

  ┌──────────────────┬──────────┬────────────┬───────────────────────┐
  │       Name       │  Where?  │    Type    │         Size          │
  ├──────────────────┼──────────┼────────────┼───────────────────────┤
  │ TEXT (code)      │ RAM      │ Read-only  │ Varies                │
  ├──────────────────┼──────────┼────────────┼───────────────────────┤
  │ INITIALIZED DATA │ RAM      │ Read/write │ Known at compile time │
  ├──────────────────┼──────────┼────────────┼───────────────────────┤
  │ BSS              │ RAM      │ Read/write │ Known at compile time │
  ├──────────────────┼──────────┼────────────┼───────────────────────┤
  │ HEAP             │ RAM      │ Read/write │ Grows at runtime      │
  ├──────────────────┼──────────┼────────────┼───────────────────────┤
  │ STACK            │ RAM      │ Read/write │ Grows at runtime      │
  ├──────────────────┼──────────┼────────────┼───────────────────────┤
  │ REGISTERS        │ CPU chip │ Read/write │ Fixed (64-bit each)   │
  └──────────────────┴──────────┴────────────┴───────────────────────┘

  Everything except registers is in RAM, but we organize it by:
  - Location in the address space (low to high)
  - Purpose (code, data, temporary work)
  - Lifetime (permanent, entire program, or just while a function runs)
  - Who manages it (OS, you with malloc, CPU with stack)

  ---
  Why this distinction matters in coroutines

  When we save a coroutine's context with coro_ctx_swap, we're saving the registers (which are on the CPU chip).

  stp x19, x20, [x0, #0]  // x19 and x20 from CPU chip → RAM

  The stack and heap are already in RAM. They don't need to be saved. They just sit there, untouched, waiting.

  When we restore:
  ldp x19, x20, [x1, #0]  // x19 and x20 from RAM → CPU chip

  We copy them back into the registers. The coroutine resumes as if nothing happened.

  The stack was never saved/restored — it was already in RAM the whole time.

  That's why sp (which points to the stack in RAM) is so important. By saving sp, we're saying "remember where in the stack this coroutine was". When we restore
   sp, the coroutine can find its stack data again.

---


### CRYSTAL CLEAR


⏺ Why Registers Need Saving But Stack Doesn't

  Let me make this crystal clear with a concrete example.

  ---
  The Setup: Two Coroutines

  Coroutine A:
    int x = 10;
    int y = 20;
    coro_yield();

  Coroutine B:
    int a = 100;
    int b = 200;
    coro_yield();

  When A runs, its local variables x and y are on A's stack.
  When B runs, its local variables a and b are on B's stack.

  ---
  The Problem: Registers are Shared

  The CPU has only one set of registers. They're not duplicated per coroutine.

  CPU Chip (one only)
  ┌────────────────────────────────┐
  │ x19 = [current value]          │
  │ x20 = [current value]          │
  │ x21 = [current value]          │
  │ sp  = [current address]        │
  │ ... only ONE of each           │
  └────────────────────────────────┘

  Think of the CPU registers like a single notepad. Only one notepad exists. Whoever is using the CPU gets to write on it.

  ---
  Coroutine A is running

  Time: T=0
  Coroutine A is executing.

  CPU Registers (the notepad):
    x19 = 10      ← value of x from coroutine A
    x20 = 20      ← value of y from coroutine A
    sp  = 0x5000  ← points to A's stack in RAM

  RAM (A's Stack):
    Address 0x5000: [some of A's data]
    Address 0x5008: [more of A's data]
    ...

  RAM (B's Stack) — not being used:
    Address 0x6000: [B's data, sitting idle]
    Address 0x6008: [more of B's data, sitting idle]
    ...

  The CPU's registers hold A's values. A's stack is in RAM (sitting there). B's stack is also in RAM (just sitting there, ignored).

  ---
  A calls coro_yield()

  Time: T=1
  A is yielding. We need to switch to B.

  PROBLEM: The CPU has only one x19 register.
           It currently holds x19=10 (A's value).
           But B also needs x19!

  SOLUTION: Save A's registers somewhere so we can load B's registers.

  ---
  We save A's registers into A's context struct

  stp x19, x20, [x0, #0]   // Save A's register values

  This means: "Take the current CPU registers (which have A's values), write them to RAM."

  After saving (but registers still have A's values):

  CPU Registers (still the notepad):
    x19 = 10      ← still here

  coro_ctx_t for A (in RAM):
    .x19 = 10     ← NOW COPIED HERE
    .x20 = 20     ← NOW COPIED HERE
    .sp  = 0x5000 ← NOW COPIED HERE

  RAM (A's Stack) — UNCHANGED:
    Address 0x5000: [some of A's data]
    Address 0x5008: [more of A's data]

  Key: We didn't move A's stack! It's still in RAM at address 0x5000. We just copied the pointer to it (sp) into the struct.

  ---
  Now we load B's registers

  ldp x19, x20, [x1, #0]   // Load B's saved register values

  This means: "Take B's saved registers from RAM, write them into the CPU's physical registers."

  After loading:

  CPU Registers (now have B's values):
    x19 = 100     ← B's value, loaded from coro_ctx_t for B
    x20 = 200     ← B's value, loaded from coro_ctx_t for B
    sp  = 0x6000  ← B's stack pointer, loaded from coro_ctx_t for B

  coro_ctx_t for A (in RAM) — UNCHANGED:
    .x19 = 10     ← still here (saved)
    .x20 = 20     ← still here (saved)
    .sp  = 0x5000 ← still here (saved)

  RAM (A's Stack) — COMPLETELY UNCHANGED:
    Address 0x5000: [some of A's data]
    Address 0x5008: [more of A's data]

  RAM (B's Stack) — now we know where it is:
    Address 0x6000: [B's data]
    Address 0x6008: [more of B's data]

  B is now running. The CPU's registers have B's values.

  ---
  The key insight: What got saved and what didn't?

  SAVED (into coro_ctx_t):
  - x19 (register)
  - x20 (register)
  - sp (register)
  - All other registers

  NOT SAVED:
  - A's stack (it's in RAM, sitting untouched)
  - A's variables (they're on the stack, sitting untouched)

  ---
  Why didn't we need to save the stack?

  Because the stack is not in the registers. It's in RAM.

  The CPU doesn't "run" the stack. The CPU only runs the registers.

  When you have a local variable:
  void task_a() {
      int x = 10;  // ← x is on the stack in RAM
      // ...
  }

  The variable x is data in RAM. The CPU doesn't execute it or care about it directly. The CPU only cares about the register that points to x — the
  stack pointer (sp).

  So we save sp. The actual stack data is already in RAM and doesn't need to be saved.

  ---
  Analogy: The Clipboard

  Think of it like this:

  Registers = your working notepad (on your desk)
  ┌──────────────────┐
  │ x19: 10          │
  │ x20: 20          │
  │ sp: 0x5000       │
  │ ...              │
  └──────────────────┘

  Stack = files in a filing cabinet (in the corner)
  ┌──────────────────────┐
  │ File Cabinet        │
  │ ┌────────────────┐  │
  │ │ Folder A       │  │
  │ │ page 1, page 2 │  │
  │ │ page 3, page 4 │  │
  │ │ ...            │  │
  │ │ (at address    │  │
  │ │  0x5000)       │  │
  │ └────────────────┘  │
  │                     │
  │ ┌────────────────┐  │
  │ │ Folder B       │  │
  │ │ page 1, page 2 │  │
  │ │ ...            │  │
  │ │ (at address    │  │
  │ │  0x6000)       │  │
  │ └────────────────┘  │
  └──────────────────────┘

  When you switch from A to B:

  1. Save the notepad (registers)
    - Write down what's on your desk: "x19=10, x20=20, sp=0x5000"
    - Store it in an envelope labeled "A's context"
  2. The filing cabinet (stack) doesn't move
    - Folder A stays in the cabinet
    - You just don't access it for now
  3. Load B's notepad (registers)
    - Take envelope labeled "B's context"
    - Read it: "x19=100, x20=200, sp=0x6000"
    - Write it on your desk
  4. Now you're working with B's data
    - Your notepad has B's values
    - sp=0x6000 points to Folder B in the cabinet
    - You access Folder B's data
  5. What happened to Folder A?
    - Still in the cabinet! Untouched!
    - When you switch back to A, you load sp=0x5000, and suddenly you can see Folder A again

  ---
  Concrete Example with Real Memory

  Initial state (A is running):

  CPU Registers:
    x19 = 10
    sp  = 0x5000

  RAM:
    0x5000: [A's data]
    0x5001: [A's data]
    0x5002: [A's data]
    0x6000: [B's data] ← B's stack, but we're ignoring it
    0x6001: [B's data]
    0x6002: [B's data]

  Save A's registers:

  coro_ctx_t A (in RAM):
    .x19 = 10       ← COPIED FROM CPU
    .sp  = 0x5000   ← COPIED FROM CPU

  RAM:
    0x5000: [A's data]  ← UNCHANGED
    0x5001: [A's data]  ← UNCHANGED
    0x5002: [A's data]  ← UNCHANGED
    0x6000: [B's data]  ← UNCHANGED
    0x6001: [B's data]  ← UNCHANGED
    0x6002: [B's data]  ← UNCHANGED

  Load B's registers:

  CPU Registers:
    x19 = 100       ← LOADED FROM coro_ctx_t B
    sp  = 0x6000    ← LOADED FROM coro_ctx_t B

  RAM:
    0x5000: [A's data]  ← UNCHANGED (but we can't see it anymore because sp=0x6000)
    0x5001: [A's data]  ← UNCHANGED
    0x5002: [A's data]  ← UNCHANGED
    0x6000: [B's data]  ← NOW we can see it (sp points here)
    0x6001: [B's data]
    0x6002: [B's data]

  Key observation:
  - A's data (0x5000-0x5002) never moved
  - A's data never got "saved" anywhere else
  - It's still in RAM, just not being used right now
  - When we restore sp=0x5000, we can access it again immediately

  ---
  Why this is efficient

  If we had to save the entire stack:

  A's entire 64 KB stack → copy to somewhere
  B's entire 64 KB stack → copy into the working area
  = 128 KB copied every switch
  = SLOW

  What we actually do:

  Just swap 13 registers (13 × 8 bytes = 104 bytes)
  = FAST

  The stack doesn't move. We just remember where it is (sp).

  ---
  Final insight: SP is the master key

  When you save sp:
  mov x2, sp
  str x2, [x0, #96]   // save sp

  You're saving the address of the stack. Not the stack itself.

  When you restore sp:
  ldr x2, [x1, #96]   // load sp
  mov sp, x2

  You're restoring the address. Now the CPU knows: "Your stack is at this address. Go access it."

  The stack data was never moved. It was always in RAM. You just forgot where it was (because you were working with B's stack). Now you remember again.

✻ Churned for 35s



---



 Excellent question! YES, but with a big difference.

  The OS does the same basic mechanism (save registers, leave stack in RAM), but it does way more.

  ---
  What the OS does when switching processes

  When the OS switches from Process A to Process B:

  Old Process A running:
    CPU Registers: x19=10, x20=20, sp=0x5000, ...
    Virtual Memory: sees addresses 0x0 to 0xFFFFFFFF as A's world
    Page Tables: (the translation table)

  OS interrupts:
    [STOP Process A]
    Save A's registers into A's PCB (Process Control Block)
    Save A's page tables
    Save A's interrupt state
    Save A's TLB (cache of page translations)
    Save A's CPU cache hints
    ... many other things

    [START Process B]
    Load B's registers from B's PCB
    Load B's page tables
    Load B's interrupt state
    Load B's TLB
    Flush B's CPU cache
    ... many other things

  New Process B running:
    CPU Registers: x19=100, x20=200, sp=0x6000, ...
    Virtual Memory: sees addresses 0x0 to 0xFFFFFFFF as B's world

  ---
  What we do with coroutines

  When we switch from Coroutine A to Coroutine B (in the same process, same OS thread):

  Old Coroutine A running:
    CPU Registers: x19=10, x20=20, sp=0x5000, ...
    Virtual Memory: SAME (both A and B see the same address space)
    Page Tables: SAME
    Interrupt state: SAME
    TLB: SAME
    CPU cache: SAME

  We manually call coro_ctx_swap:
    [STOP A]
    Save A's 13 registers

    [START B]
    Load B's 13 registers

  New Coroutine B running:
    CPU Registers: x19=100, x20=200, sp=0x6000, ...
    Virtual Memory: SAME
    Page Tables: SAME
    Interrupt state: SAME
    TLB: SAME
    CPU cache: SAME

  ---
  The key difference: Virtual Address Space

  Processes: Different virtual address spaces

  When Process A and Process B are running, they each think they own the entire address space:

  Process A thinks:
    Address 0x5000 → points to A's stack

  Process B thinks:
    Address 0x5000 → points to B's stack (completely different physical RAM!)

  The same virtual address (0x5000) points to different physical RAM!

  The OS maintains page tables for each process that do this translation:

  Process A's Page Tables:
    Virtual 0x5000 → Physical RAM location X

  Process B's Page Tables:
    Virtual 0x5000 → Physical RAM location Y

  When switching processes, the OS must swap out the page tables. Otherwise, Process B would try to access address 0x5000 and get Process A's
  data by mistake!

  This is why process switching is expensive — the OS must:
  1. Save page tables
  2. Flush the TLB (Translation Lookaside Buffer — a cache of address translations)
  3. Flush CPU cache (because data at physical address X belongs to A)

  Coroutines: Same virtual address space

  Both Coroutine A and B are in the same process, so they share the same page tables:

  Both A and B share one page table:
    Virtual 0x5000 → Physical RAM location X (A's stack)
    Virtual 0x6000 → Physical RAM location Y (B's stack)

  When we switch from A to B, we don't need to change page tables. We just restore sp:

  A was using: sp = 0x5000  (points to A's stack)
  B needs to use: sp = 0x6000  (points to B's stack)

  Just restore sp. The page tables already know where both are!

  ---
  Comparison Table

  ┌────────────────────────┬────────────────────────┬──────────────────────┬───────────────────────┐
  │                        │ OS switching Processes │ OS switching Threads │ Coroutines (our code) │
  ├────────────────────────┼────────────────────────┼──────────────────────┼───────────────────────┤
  │ Save/load registers    │ Yes                    │ Yes                  │ Yes                   │
  ├────────────────────────┼────────────────────────┼──────────────────────┼───────────────────────┤
  │ Save/load page tables  │ Yes                    │ No (same process)    │ No (same process)     │
  ├────────────────────────┼────────────────────────┼──────────────────────┼───────────────────────┤
  │ Flush TLB              │ Yes                    │ No                   │ No                    │
  ├────────────────────────┼────────────────────────┼──────────────────────┼───────────────────────┤
  │ Flush CPU cache        │ Yes                    │ No                   │ No                    │
  ├────────────────────────┼────────────────────────┼──────────────────────┼───────────────────────┤
  │ Who controls switching │ OS (preemptive)        │ OS (preemptive)      │ You (cooperative)     │
  ├────────────────────────┼────────────────────────┼──────────────────────┼───────────────────────┤
  │ CPU cycles to switch   │ ~1000+                 │ ~100+                │ ~10-50                │
  └────────────────────────┴────────────────────────┴──────────────────────┴───────────────────────┘

  ---
  Why is this important?

  The OS does all that extra work because:

  1. Processes are isolated — they must not see each other's memory
  2. Page tables translate virtual → physical addresses — must be swapped per process
  3. CPU cache is per-physical-address — flushing it is expensive

  With coroutines:

  1. Same process — same page tables
  2. Both stacks are in the same address space — no translation needed
  3. Same cache — no flushing needed

  ---
  Visual: What's happening under the hood

  Process switch (expensive):

  Process A (virtual address 0x5000 = physical RAM X):
    Page Table A: 0x5000 → Physical X
    sp = 0x5000

  [OS takes over]
  Save registers, page table A
  Flush TLB (the cache)
  Flush CPU cache

  Process B (virtual address 0x5000 = physical RAM Y):
    Page Table B: 0x5000 → Physical Y
    sp = 0x5000
  [OS loads registers, page table B]

  Thread switch (less expensive):

  Thread A (in Process P, virtual 0x5000 = physical RAM X):
    Page Table P: 0x5000 → Physical X
    sp = 0x5000

  [OS takes over]
  Save registers
  (no page table change — Thread B is in same process P!)

  Thread B (in Process P, virtual 0x5000 = physical RAM X):
    Page Table P: 0x5000 → Physical X (same!)
    sp = 0x6000
  [OS loads registers]

  Coroutine switch (very cheap):

  Coroutine A:
    Page Table (shared): 0x5000 → Physical X
    sp = 0x5000

  [No OS involvement!]
  Save registers
  (no page table change — both in same process!)

  Coroutine B:
    Page Table (shared): 0x5000 → Physical X (same!)
    sp = 0x6000
  [Load registers]

  ---
  The answer to your question

  Yes, the OS does essentially the same thing with registers and stacks.

  But the OS also has to handle virtual address space switching, which is expensive. We don't have to do that with coroutines because:

  - Processes need isolation → different virtual address spaces
  - Coroutines don't → they're all in the same process, same address space

  So the OS's approach is overkill for coroutines. We can do the same "job" (switch execution) much faster by just swapping registers.

  ---
  In our code

  When we do:
  coro_ctx_swap(&A->ctx, &B->ctx);

  We're doing the minimal register-swapping part that the OS does. We skip all the expensive page table / cache / TLB stuff because we don't need
   it.

  If the OS switched coroutines instead of letting us do it:
  pthread_create(task_a);  // OS creates a thread
  pthread_create(task_b);  // OS creates a thread
  // OS switches between them every 10ms
  // Expensive! All that page table stuff is unnecessary!

  It would work, but you'd pay the price of process/thread switching for something that only needs register swapping.

  That's why coroutines are so fast.


---



❯ So, using the same OS thread address space ( the big stack) chunking into 64kb and using them all

⏺ Not quite — let me clarify the memory layout

  You're close, but there's an important distinction. We're not chunking the OS thread's stack. We're creating separate stacks using
   mmap.

  ---
  The Memory Layout

  Process Virtual Address Space (0x0 to 0xFFFFFFFF)

  ┌──────────────────────────────────────────────────┐
  │ TEXT (code)                                      │
  ├──────────────────────────────────────────────────┤
  │ HEAP (malloc'd, global variables, etc.)          │
  ├──────────────────────────────────────────────────┤
  │                                                  │
  │ OS Thread 1's STACK (allocated by OS)            │
  │ ├─ 1-8 MB, depends on OS                         │
  │ ├─ This is where main() and coro_resume live     │
  │ └─ Grows downward                                │
  │                                                  │
  ├──────────────────────────────────────────────────┤
  │ Coroutine A's STACK (allocated via mmap)         │
  │ ├─ 64 KB (guard page + usable stack)             │
  │ ├─ SEPARATE from thread stack                    │
  │ ├─ Allocated at some other address              │
  │ └─ Grows downward                                │
  ├──────────────────────────────────────────────────┤
  │ Coroutine B's STACK (allocated via mmap)         │
  │ ├─ 64 KB (guard page + usable stack)             │
  │ ├─ SEPARATE from thread stack                    │
  │ ├─ Allocated at yet another address             │
  │ └─ Grows downward                                │
  ├──────────────────────────────────────────────────┤
  │ Coroutine C's STACK (allocated via mmap)         │
  │ ├─ 64 KB                                         │
  │ └─ ... and so on                                 │
  │                                                  │
  └──────────────────────────────────────────────────┘

  ---
  The Thread's Stack vs Coroutine Stacks

  OS Thread's Stack (allocated by OS, used by scheduler)

  int main() {  // ← running on OS thread's stack
      coroutine_t *a = coro_create(task_a, NULL, 0, 0);
      coroutine_t *b = coro_create(task_b, NULL, 0, 0);

      coro_resume(a);   // ← scheduler code here
      coro_resume(b);   // ← scheduler code here
  }

  The OS automatically allocates a stack when it creates the thread. On macOS, that's typically 8 MB. When main() and coro_resume()
  run, they use this stack.

  This stack is huge — you only need a small portion of it.

  Coroutine's Stacks (allocated via mmap, used by task functions)

  void task_a(void *arg) {  // ← running on Coroutine A's mmap'd stack
      printf("[A] step 1\n");
      coro_yield();
  }

  When you call coro_create(task_a, ...), this happens:

  size_t total = CORO_GUARD_PAGE_SIZE + c->stack_size;  // 4 KB + 64 KB
  void *mem = mmap(NULL, total, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);

  We ask the OS for NEW memory using mmap. This is completely separate from the thread's stack. It goes somewhere else in the
  address space.

  Thread Stack (8 MB, OS-allocated):
    0x7FFF0000 ← top
    [main() locals]
    [coro_resume() locals]
    ...
    0x7FFF0000 - 8MB

  Coroutine A's Stack (64 KB, mmap'd):
    0x12340000 ← top (completely different address!)
    [task_a() locals]
    ...
    0x12340000 - 64KB

  Coroutine B's Stack (64 KB, mmap'd):
    0x56780000 ← top (completely different address!)
    [task_b() locals]
    ...
    0x56780000 - 64KB

  ---
  Why separate stacks with mmap instead of chunking the thread stack?

  Good question! Why not do this:

  // DON'T do this:
  char *thread_stack = ...;  // OS thread's 8MB stack
  c->stack = thread_stack + 2_MB;  // take a 64KB chunk

  Answer: Multiple reasons

  1. Guard pages don't work

  We want to use mprotect to create a guard page:

  mprotect(mem, CORO_GUARD_PAGE_SIZE, PROT_NONE);

  This sets a page to PROT_NONE (inaccessible). If a coroutine overflows its stack, it hits the guard page and crashes with SIGSEGV.

  The problem: If you chunk the thread's stack, you can't protect just one chunk. The thread's stack is a continuous region that the
   OS itself is managing. You can't arbitrarily mprotect parts of it without breaking the OS's ability to use it.

  2. mmap guarantees page alignment

  When you use mmap, the OS returns a page-aligned address. This is important for:
  - Guard pages (must be page-aligned)
  - Performance (page boundaries matter)
  - Portability

  If you chunk a shared stack, you lose these guarantees.

  3. Independence

  With separate mmap'd stacks, each coroutine's stack is completely independent. You can:
  - Free one without affecting others
  - mprotect independently
  - Track memory usage per coroutine

  With chunking, they're entangled.

  4. Safety

  With separate stacks, a coroutine overflow only affects that coroutine. With chunking, it could trash the next coroutine's stack.

  ---
  What's actually happening in our code

  coroutine_t *coro_create(...) {
      // 1. Allocate struct (in heap)
      coroutine_t *c = calloc(1, sizeof(coroutine_t));

      // 2. Ask OS for 68 KB via mmap (NOT from thread stack!)
      void *mem = mmap(NULL, 68_KB, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);

      // 3. Make first 4 KB inaccessible
      mprotect(mem, 4_KB, PROT_NONE);

      // 4. Usable stack starts after guard
      c->stack = (char *)mem + 4_KB;

      // 5. Set up context to jump to this stack
      coro_ctx_init(&c->ctx, trampoline, c->stack, 64_KB);

      return c;
  }

  Each call to coro_create asks the OS: "Give me 68 KB somewhere in the address space." The OS finds a free region and hands back a
  pointer.

  These regions are independent. They don't come from the thread's stack.

  ---
  Diagram: Where everything lives

  ┌─ Process's Virtual Address Space ──────────────────┐
  │                                                    │
  │  OS Thread Stack (8 MB)                            │
  │  ┌──────────────────────────────────────────┐     │
  │  │ main() code                              │     │
  │  │ coro_resume(a) { ... }  ← scheduler code │     │
  │  │ coro_resume(b) { ... }                   │     │
  │  │ (main's local variables live here)       │     │
  │  └──────────────────────────────────────────┘     │
  │                                                    │
  │  [empty space in address space]                   │
  │                                                    │
  │  Coroutine A's Stack (64 KB, mmap'd)              │
  │  ┌──────────────────────────────────────────┐     │
  │  │ Guard page (PROT_NONE)                   │     │
  │  ├──────────────────────────────────────────┤     │
  │  │ task_a() code                            │     │
  │  │ (A's local variables live here)          │     │
  │  └──────────────────────────────────────────┘     │
  │                                                    │
  │  [empty space in address space]                   │
  │                                                    │
  │  Coroutine B's Stack (64 KB, mmap'd)              │
  │  ┌──────────────────────────────────────────┐     │
  │  │ Guard page (PROT_NONE)                   │     │
  │  ├──────────────────────────────────────────┤     │
  │  │ task_b() code                            │     │
  │  │ (B's local variables live here)          │     │
  │  └──────────────────────────────────────────┘     │
  │                                                    │
  │  [empty space in address space]                   │
  │                                                    │
  │  Coroutine C's Stack (64 KB, mmap'd)              │
  │  ┌──────────────────────────────────────────┐     │
  │  │ ...                                      │     │
  │  └──────────────────────────────────────────┘     │
  │                                                    │
  └────────────────────────────────────────────────────┘

  ---
  Summary

  ┌───────────────┬────────────────────────────────────┬──────────────────────────────────────┐
  │               │            Thread Stack            │           Coroutine Stacks           │
  ├───────────────┼────────────────────────────────────┼──────────────────────────────────────┤ │ Allocated by  │ OS (when thread created)           │ You (via mmap in coro_create)        │
  ├───────────────┼────────────────────────────────────┼──────────────────────────────────────┤
  │ Size          │ ~1-8 MB                            │ 64 KB each                           │
  ├───────────────┼────────────────────────────────────┼──────────────────────────────────────┤
  │ Number        │ 1 per thread                       │ Many (one per coroutine)             │
  ├───────────────┼────────────────────────────────────┼──────────────────────────────────────┤
  │ Used by       │ Scheduler code (main, coro_resume) │ Coroutine code (task_a, task_b, ...) │
  ├───────────────┼────────────────────────────────────┼──────────────────────────────────────┤
  │ Can mprotect? │ No (OS is using it)                │ Yes! (isolated with guard page)      │
  ├───────────────┼────────────────────────────────────┼──────────────────────────────────────┤
  │ Address       │ Fixed (allocated by OS)            │ Varies (wherever mmap puts them)     │
  └───────────────┴────────────────────────────────────┴──────────────────────────────────────┘

  The thread's stack is for the scheduler. The coroutine stacks are for the workers.








---



❯ So, we basically doing calloc into the heap and using it as corotuine's stack? I mean technically, it is just memory from
  somehwhere in the entire RAM. And becuase we are dealing with registers here, we call it "stack" instead of corotuine's heap?

⏺ Great insight! But with an important clarification.

  We're NOT using calloc for the stacks. We're using mmap. But your deeper question is excellent.

  ---
  What we're actually doing

  coroutine_t *c = calloc(1, sizeof(coroutine_t));  // ← heap (small struct)

  void *mem = mmap(NULL, 68_KB, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);  // ← NOT from heap!
  c->stack = (char *)mem + CORO_GUARD_PAGE_SIZE;      // ← points to mmap'd memory

  - c (the struct) — allocated from heap via calloc
  - c->stack (the 64 KB region) — allocated from mmap (still RAM, but directly from OS, not from heap)

  ---
  Why mmap instead of malloc/calloc?

  You're right that malloc would also work in terms of "it's just RAM":

  // This would technically work:
  c->stack = malloc(64 * 1024);

  But here's the problem:

  malloc packs allocations together

  Heap (managed by malloc):
  ┌──────────────────────────────────────────┐
  │ [A][B][C][D][E][F][G]                    │
  │  ↑  ↑  ↑  ↑  ↑  ↑  ↑                      │
  │  └──┬──┘  └──┬──┘  └──┬──┘                │
  │     │       │        │                   │
  │  Coro A  Coro B  Coro C                  │
  │  (64KB)  (64KB)  (64KB)                  │
  │                                          │
  │ All in ONE large malloc page             │
  └──────────────────────────────────────────┘

  Now we want to protect coroutine A's stack with mprotect:

  mprotect(a_stack, 64_KB, PROT_NONE);

  PROBLEM: Coroutine B is in the same page! mprotect works at page granularity (4 KB per page). You can't protect just part of a
  shared page. If you try:

  BEFORE mprotect:
  Page 1: [A's data][B's data] ← shared page, readable
          ↑                ↑
          0x5000          0x6000

  AFTER mprotect(a_stack, 64KB, PROT_NONE):
  Page 1: [????][????]  ← ENTIRE page becomes PROT_NONE!
          ↑    ↑ ↑      ↑
          |    | └──────┘ B is now inaccessible too!
          A is protected, but B is killed!

  You've just crashed coroutine B by accident.

  mmap gives you dedicated pages

  void *mem = mmap(NULL, 68_KB, ...);

  mmap says: "Give me 68 KB aligned to page boundaries, and nobody else uses those pages."

  mmap pages (dedicated to each coroutine):
  ┌──────────────────────────────┐
  │ Page 1: [A's guard][A's stack] │  ← only A
  ├──────────────────────────────┤
  │ Page N: [B's guard][B's stack] │  ← only B
  ├──────────────────────────────┤
  │ Page M: [C's guard][C's stack] │  ← only C
  └──────────────────────────────┘

  Now mprotect works:
  mprotect(a_guard, 4KB, PROT_NONE);  ← only A's guard becomes inaccessible
                                         B and C unaffected!

  ---
  Your insight: "It's all just memory, why call it stack?"

  You're partially right. But there's important distinction:

  Terminology vs Reality

  REALITY: It's all RAM
    - malloc'd heap = RAM
    - mmap'd memory = RAM
    - OS thread stack = RAM
    - Coroutine stack = RAM

  TERMINOLOGY: We name them by PURPOSE and BEHAVIOR
    - "Stack" = LIFO data structure (grows downward, push/pop)
    - "Heap" = allocator-managed pool (grow/shrink at any time)
    - Coroutine's 64 KB region = we use it like a stack (LIFO), so we call it a stack

  The "stack" name comes from how we use it

  A stack is a data structure where:
  - Push = add to top
  - Pop = remove from top
  - LIFO = Last In, First Out
  - Grows downward on ARM64

  Our coroutine's 64 KB region:

  void task_a(void *arg) {
      int x = 5;        // pushed onto stack
      int y = 10;       // pushed onto stack
      bar();            // sp moves down
      // sp moves up
      return;           // x and y popped
  }

  The sp register points into this region and moves downward as we push locals. That's stack behavior.

  So we call it a "stack" because it acts like a stack, not because of where it's physically allocated.

  ---
  The better way to think about it

                TYPE OF ALLOCATION          USE CASE

  malloc/calloc → HEAP allocator            "I need some memory, don't care about protection"

  mmap          → OS direct allocation      "I need memory with special properties
                                             (guard pages, page alignment, mprotect)"

  OS thread     → Implicit (automatic)      "Created when thread starts, managed by OS"
  stack         → during thread creation

  Coroutine     → mmap'd region used like   "I need memory arranged as a stack
  stack         → a stack (LIFO)            (grows downward, LIFO) with guard pages"

  ---
  What's the difference, really?

  ┌──────────────────┬─────────────────────┬───────────────────────┬────────────────────────────────────┐
  │      Aspect      │       malloc        │    mmap for stacks    │          OS thread stack           │
  ├──────────────────┼─────────────────────┼───────────────────────┼────────────────────────────────────┤
  │ Who allocates    │ malloc library      │ OS (via mmap syscall) │ OS (implicit when creating thread) │
  ├──────────────────┼─────────────────────┼───────────────────────┼────────────────────────────────────┤
  │ Returned address │ Any, unaligned      │ Page-aligned          │ Fixed, known at thread creation    │
  ├──────────────────┼─────────────────────┼───────────────────────┼────────────────────────────────────┤
  │ Can mprotect?    │ NO (shared pages)   │ YES (dedicated pages) │ NO (OS using it)                   │
  ├──────────────────┼─────────────────────┼───────────────────────┼────────────────────────────────────┤
  │ Grow/shrink      │ Flexible            │ Fixed size            │ Fixed size                         │
  ├──────────────────┼─────────────────────┼───────────────────────┼────────────────────────────────────┤
  │ Overhead         │ Low                 │ Higher (syscall)      │ None (pre-allocated)               │
  ├──────────────────┼─────────────────────┼───────────────────────┼────────────────────────────────────┤
  │ Use case         │ General allocations │ Stacks with guards    │ Scheduler code                     │
  └──────────────────┴─────────────────────┴───────────────────────┴────────────────────────────────────┘

  ---
  Bottom line

  You're absolutely correct in principle:

  "It's all just memory from RAM"

  Yes! 100%. All three (heap, mmap'd stacks, thread stack) are physical RAM.

  But we distinguish them by:
  1. How they're allocated (malloc vs mmap vs OS)
  2. How we use them (random access vs stack-like vs stack-like)
  3. What protections we need (none vs guard pages vs none)

  The word "stack" doesn't mean "allocated from the thread's stack region." It means "used as a stack data structure" — LIFO, grows
  downward, etc.

  We call it a "coroutine stack" because it's memory we're using as a stack for a coroutine, not because it comes from some special
  "stack memory pool."




---





❯ What is even the difference between heap and mmap

⏺ Excellent question. This is the key to understanding why we use mmap instead of malloc.

  Both give you RAM, but they work completely differently.

  ---
  HEAP (malloc/calloc)

  What it is: A memory allocator that manages a shared pool of memory.

  void *ptr = malloc(100);  // give me 100 bytes

  How malloc works:

  1. malloc asks the OS: "Give me a big chunk of memory" (example: 1 MB)
  2. OS gives malloc a 1 MB region
  3. malloc divides that 1 MB into pieces and keeps track of which are free/used
  4. When you call malloc(100), malloc finds a free 100-byte piece and gives it to you
  5. malloc remembers: "bytes 0x1000-0x1064 are in use by user"

  Heap (1 MB region, managed by malloc):
  ┌──────────────────────────────────────────┐
  │ [malloc header]                          │
  ├──────────────────────────────────────────┤
  │ [your malloc(100)]                       │ ← you got this
  ├──────────────────────────────────────────┤
  │ [someone else's malloc(50)]               │ ← packed right next to yours
  ├──────────────────────────────────────────┤
  │ [free space that malloc can reuse]        │
  ├──────────────────────────────────────────┤
  │ [another malloc(200)]                     │
  ├──────────────────────────────────────────┤
  │ [free space]                              │
  └──────────────────────────────────────────┘

  Key point: Everything is packed together. malloc is efficient with memory but sacrifices isolation.

  ---
  MMAP (direct OS allocation)

  What it is: You ask the OS directly for memory pages. OS gives you entire pages, nobody else gets them.

  void *ptr = mmap(NULL, 68 * 1024, ...);  // give me 68 KB of pages

  How mmap works:

  1. You call mmap()
  2. OS says: "OK, I'll reserve pages 0x12345000 through 0x12356000 for YOU ALONE"
  3. OS returns: 0x12345000 (page-aligned)
  4. Nobody else can touch those pages
  5. You can do anything with those pages: read, write, protect, unmap

  Virtual Address Space (managed by OS paging system):
  ┌────────────────────────────────────────┐
  │ Page 0x12340000: [Your mmap region 1]  │ ← only you
  ├────────────────────────────────────────┤
  │ Page 0x12341000: [Your mmap region 1]  │ ← only you
  ├────────────────────────────────────────┤
  │ Page 0x12342000: [Someone else's data] │ ← NOT yours
  ├────────────────────────────────────────┤
  │ Page 0x12343000: [Your mmap region 2]  │ ← only you
  └────────────────────────────────────────┘

  Key point: Complete isolation. You own entire pages. You can protect them with mprotect.

  ---
  Side-by-side comparison

  ┌─────────────────┬─────────────────────────────────────┬───────────────────────────────────────────┐
  │     Aspect      │            malloc (Heap)            │                   mmap                    │
  ├─────────────────┼─────────────────────────────────────┼───────────────────────────────────────────┤
  │ Who allocates   │ malloc library (in user space)      │ OS kernel (syscall)                       │
  ├─────────────────┼─────────────────────────────────────┼───────────────────────────────────────────┤
  │ Granularity     │ Bytes (or small chunks)             │ Pages (4 KB each)                         │
  ├─────────────────┼─────────────────────────────────────┼───────────────────────────────────────────┤
  │ Packing         │ Dense (allocations packed together) │ Sparse (you get whole pages)              │
  ├─────────────────┼─────────────────────────────────────┼───────────────────────────────────────────┤
  │ Page sharing    │ YES (multiple allocations per page) │ NO (you own entire pages)                 │
  ├─────────────────┼─────────────────────────────────────┼───────────────────────────────────────────┤
  │ Can mprotect?   │ NO (breaks malloc)                  │ YES (only affects your pages)             │
  ├─────────────────┼─────────────────────────────────────┼───────────────────────────────────────────┤
  │ Speed           │ Fast (no syscall)                   │ Slower (syscall overhead)                 │
  ├─────────────────┼─────────────────────────────────────┼───────────────────────────────────────────┤
  │ Memory overhead │ Low (little metadata)               │ High (wasted space if using <1 page)      │
  ├─────────────────┼─────────────────────────────────────┼───────────────────────────────────────────┤
  │ Use case        │ General allocations (small objects) │ Special needs (protection, large regions) │
  └─────────────────┴─────────────────────────────────────┴───────────────────────────────────────────┘

  ---
  Concrete example: Why malloc breaks with mprotect

  // Using malloc (WRONG):
  int *stack_a = malloc(64 * 1024);
  int *stack_b = malloc(64 * 1024);

  // In memory:
  // 0x5000-0x5010: malloc overhead
  // 0x5010-0x5010+64KB: stack_a
  // 0x5010+64KB-0x5010+128KB: stack_b  ← SAME PAGE as stack_a!

  // Now protect stack_a's guard page:
  mprotect(stack_a, 4096, PROT_NONE);

  // What happens:
  // mprotect affects the entire PAGE, not just the 4KB you wanted
  // Page contains: [malloc overhead][start of stack_a][start of stack_b]
  // Now mprotect PROT_NONE applies to the ENTIRE page
  // So malloc ITSELF becomes inaccessible
  // And stack_b becomes inaccessible
  // CRASH!

  ---
  Concrete example: Why mmap works

  // Using mmap (CORRECT):
  void *mem_a = mmap(NULL, 68 * 1024, ...);
  void *mem_b = mmap(NULL, 68 * 1024, ...);

  // In memory (at page boundaries):
  // 0x12345000-0x12347000: mem_a (your pages)
  // 0x12348000-0x1234A000: mem_b (different pages!)

  // Protect mem_a's guard page:
  mprotect(mem_a, 4096, PROT_NONE);

  // What happens:
  // mprotect affects page 0x12345000 only
  // mem_a's guard page: PROT_NONE ✓
  // mem_b: unaffected ✓
  // WORKS!

  ---
  Why use malloc instead of mmap?

  malloc is better when:
  - You need lots of small allocations (objects, structs)
  - You DON'T need isolation or protection
  - You care about memory efficiency (less wasted space)
  - You want speed (no syscall overhead)

  Example:
  struct node {
      int data;
      struct node *next;
  };
  // Create 1000 nodes
  for (int i = 0; i < 1000; i++) {
      node[i] = malloc(sizeof(struct node));  // ← efficient
  }

  ---
  Why use mmap instead of malloc?

  mmap is better when:
  - You need isolation (own pages)
  - You need protection (mprotect, guard pages)
  - You're allocating large contiguous regions (stacks, buffers)
  - You need page alignment
  - You care about security/safety more than memory efficiency

  Example:
  void *stack = mmap(NULL, 64 * 1024, ...);  // ← can mprotect
  mprotect(stack, 4096, PROT_NONE);          // ← guard page

  ---
  The memory efficiency tradeoff

  malloc:
  Need 64 KB:
  Actual memory used: ~64 KB + 16 bytes overhead = 64 KB

  mmap:
  Need 64 KB:
  Requested: 68 KB (64 KB stack + 4 KB guard)
  Actual memory used: 72 KB (rounded to nearest page)
  Wasted: 4 KB (the unused part of the last page)

  malloc wins on efficiency. But for coroutines, we need mmap because we need guard pages.

  ---
  Bottom line

  ┌──────────────────────────────────────────────────┬────────┐
  │                  What you need                   │  Use   │
  ├──────────────────────────────────────────────────┼────────┤
  │ General memory (small objects)                   │ malloc │
  ├──────────────────────────────────────────────────┼────────┤
  │ Protection (guard pages, mprotect)               │ mmap   │
  ├──────────────────────────────────────────────────┼────────┤
  │ Isolation (nobody else touches your pages)       │ mmap   │
  ├──────────────────────────────────────────────────┼────────┤
  │ Memory efficiency                                │ malloc │
  ├──────────────────────────────────────────────────┼────────┤
  │ Large contiguous regions with special properties │ mmap   │
  └──────────────────────────────────────────────────┴────────┘

  For coroutine stacks: We use mmap because we need guard pages. malloc won't work because guard pages require page-level isolation,
   and malloc shares pages.

  If we didn't need guard pages, we could use malloc. But we do, so we use mmap.



---



 No. A page is typically 4 KB (4096 bytes).

  Some systems have larger pages (called "huge pages"), but standard is 4 KB.

  ---
  Standard page size

  On most systems (ARM64, x86-64, Linux, macOS):
  1 page = 4 KB = 4096 bytes

  On some systems (Intel with huge pages):
  1 huge page = 2 MB or 1 GB
  (but these are optional, not default)

  ---
  Why 4 KB?

  Historical reasons + practical balance:

  - Too small (1 KB): Wastes CPU resources managing too many pages
  - Too large (64 KB): Wastes RAM if you only need a tiny bit
  - 4 KB: Sweet spot

  ---
  How pages work in our coroutine code

  void *mem = mmap(NULL, 68 * 1024, ...);  // ask for 68 KB

  The OS rounds up to the nearest page:

  68 KB = 69,632 bytes

  Pages needed:
    Page 1: 0x12345000 (4 KB)   ┐
    Page 2: 0x12346000 (4 KB)   ├─ 68 KB total
    Page 3: 0x12347000 (4 KB)   │  (17 pages + 1 partial)
    ...                         │
    Page 17: 0x1234B000 (4 KB)  ┤
    Page 18: 0x1234C000 (768 B) ┘  ← partial page

  Actually allocated: 18 pages × 4 KB = 72 KB

  mmap returns: 0x12345000 (page-aligned)

  ---
  Guard page is 4 KB

  #define CORO_GUARD_PAGE_SIZE 4096  // one page

  mprotect(mem, CORO_GUARD_PAGE_SIZE, PROT_NONE);

  This makes one page (4 KB) inaccessible.

  ---
  Example with real addresses

  Coroutine A (68 KB from mmap):

  Page 0x12345000: [Guard page - PROT_NONE]  ← 4 KB
  Page 0x12346000: [Stack data]               ← 4 KB
  Page 0x12347000: [Stack data]               ← 4 KB
  Page 0x12348000: [Stack data]               ← 4 KB
  ... (13 more pages of stack)
  Page 0x1234B000: [Stack data + unused]      ← 4 KB (last page, partly used)

  Total: 18 pages = 72 KB allocated (68 KB usable + 4 KB guard)

  If the stack overflows and tries to write to 0x12345000 or below, it hits the guard page → SIGSEGV.

  ---
  Page size on different systems

  Linux (x86-64):     4 KB (standard)
  Linux (ARM64):      4 KB (standard)
  macOS (x86-64):     4 KB (standard)
  macOS (ARM64/M1):   4 KB (standard)
  Windows:            4 KB (standard)

  Huge pages (optional on some):
  Linux:              2 MB or 1 GB
  Windows:            2 MB or 1 GB

  You can check on your system:

  getconf PAGE_SIZE    # Linux/macOS

  On most modern systems, this prints: 4096 (4 KB)

  ---
  Why page size matters for mprotect

  mprotect(address, size, protection);

  The OS rounds DOWN to page boundaries:

  mprotect(0x12345678, 100, PROT_NONE);

  Actually protects:
  0x12345000 (start of page containing 0x12345678)
  to
  0x12346000 (end of that page)

  So you always protect whole pages, even if you ask for less.

  That's why we make the guard page exactly 4096 bytes — one full page, no more, no less.


### TO_DO 

----

❯ ❯ getconf PAGE_SIZE
  16384


⏺ Interesting! Your system has 16 KB pages, not 4 KB.

  This is actually important and reveals a potential issue in our code.

  ---
  What this means

  Your system:
  1 page = 16,384 bytes = 16 KB

  Our code:
  #define CORO_GUARD_PAGE_SIZE 4096  // ← only 4 KB!

  Problem: The guard page is smaller than an actual page!

  ---
  Why this matters

  When you call mprotect, it works on full pages:

  mprotect(mem, 4096, PROT_NONE);

  On your system with 16 KB pages:

  Your request: "Make 4 KB inaccessible"

  What actually happens:
  OS rounds UP to nearest page boundary
  Protects: entire 16 KB page

  Guard page (4 KB):  [PROT_NONE]
  Stack starts (12 KB into page):  [also PROT_NONE!]
                                    ↑ THIS IS A PROBLEM!

  You wanted to protect just the guard, but the OS protected the first 16 KB, which includes part of your actual stack!

  ---
  This is a bug in our code

  Our code assumes 4 KB pages:

  #define CORO_GUARD_PAGE_SIZE 4096
  #define CORO_DEFAULT_STACK_SIZE (64 * 1024)

  size_t total = CORO_GUARD_PAGE_SIZE + c->stack_size;  // 4 KB + 64 KB = 68 KB
  void *mem = mmap(NULL, total, ...);
  mprotect(mem, CORO_GUARD_PAGE_SIZE, PROT_NONE);      // ← assumes page size!

  On a system with 16 KB pages, this doesn't work correctly.

  ---
  How to fix it

  Get the page size at runtime:

  #include <unistd.h>

  long page_size = sysconf(_SC_PAGE_SIZE);  // returns 16384 on your system

  size_t total = page_size + c->stack_size;  // 16 KB guard + 64 KB stack
  void *mem = mmap(NULL, total, ...);
  mprotect(mem, page_size, PROT_NONE);      // protect exactly one page

  ---
  Check if our current code works on your system

  The current code might still work, but less safely:

  What we allocated:
    Total: 68 KB (4 KB guard + 64 KB stack)

  What your OS did:
    Rounded up to: 80 KB (5 pages × 16 KB)
    Protected first page: 16 KB

  Result:
    Guard: 16 KB protected (more than intended, but OK)
    Stack: 52 KB left (less than intended, but probably OK for simple cases)
    Wasted: 12 KB unused

  It might work, but it's not ideal. The guard page is bigger than needed, and we're wasting memory.

  ---
  Different page sizes in the wild

  System                    | Page Size
  --------------------------|----------
  Most x86-64 Linux/macOS   | 4 KB
  Apple Silicon (M1/M2/M3) | 16 KB
  Some ARM systems          | 16 KB or 64 KB
  Power systems             | 4 KB or 64 KB

  Your machine is probably Apple Silicon or an ARM system with 16 KB pages.

  ---
  What we should have done from the start

  #include <unistd.h>

  /* Get page size at runtime */
  static long get_page_size(void) {
      static long page_size = 0;
      if (page_size == 0) {
          page_size = sysconf(_SC_PAGE_SIZE);
          if (page_size <= 0) page_size = 4096;  // fallback
      }
      return page_size;
  }

  void coro_create(...) {
      long page_size = get_page_size();

      size_t total = page_size + CORO_DEFAULT_STACK_SIZE;
      void *mem = mmap(NULL, total, ...);

      mprotect(mem, page_size, PROT_NONE);  // protect one full page

      c->stack = (char *)mem + page_size;   // stack starts after guard
      c->stack_size = CORO_DEFAULT_STACK_SIZE;
  }

  ---
  Should we fix this?

  Yes, we should update the code to handle different page sizes. This would make it portable to:
  - 4 KB page systems (standard x86-64)
  - 16 KB page systems (Apple Silicon, some ARM)
  - 64 KB page systems (some ARM/Power systems)

  Want me to update src/coroutine.c to fix this?


---
