# Go-Like Coroutine Runtime in C

## The Problem with Go's Scheduler

Go's goroutine scheduler is M:N (many goroutines mapped to few OS threads) and does a lot of things right — work stealing, cooperative+preemptive scheduling, cheap stack growth. But it has one fundamental flaw:

**Every goroutine is equal. There are no priorities, no deadlines.**

This makes Go unsuitable for:
- Real-time or soft real-time systems
- Mixed-criticality workloads (a payment handler shouldn't wait behind a log flusher)
- Latency-sensitive pipelines where some tasks *must* finish before a deadline

We fix that by building a coroutine runtime in C with **priority scheduling + EDF (Earliest Deadline First)**.

---

## What We're Building

A coroutine runtime in C with:

1. **Coroutines** — lightweight execution contexts using `ucontext_t`, each with its own mmap'd stack and guard page
2. **Priority scheduling** — 8 priority levels (0 = highest). High-priority coroutines always run before low-priority ones
3. **EDF scheduling** — within a priority level, coroutines with a deadline are scheduled by who needs to finish soonest
4. **Work stealing** — each OS thread has its own run queues; idle threads steal from busy ones
5. **Channels** — typed, buffered channels for communication between coroutines (like Go's `make(chan T, n)`)
6. **CPU affinity** — optional pinning of coroutines to specific cores

---

## Architecture

```
Runtime
├── Worker Threads (M)           ← POSIX threads, one per core
│    └── Processor (P)           ← one per thread, like Go's P
│         ├── run_queue[8]       ← per-priority FIFO queues
│         └── deadline_heap      ← min-heap ordered by deadline (EDF)
└── global_queue                 ← overflow queue for new coroutines

Coroutine (G)
├── ucontext_t ctx               ← register state + stack pointer
├── void *stack                  ← mmap'd stack (default 64KB) + guard page
├── int priority                 ← 0 (urgent) to 7 (background)
├── uint64_t deadline_ns         ← 0 = no deadline, else CLOCK_MONOTONIC ns
├── int cpu_affinity             ← -1 = any, else core index
└── state                        ← READY | RUNNING | BLOCKED | DONE

Channel
├── void *buf                    ← ring buffer
├── size_t elem_size, capacity
├── head, tail, count
├── send_waitq                   ← coroutines blocked on send
└── recv_waitq                   ← coroutines blocked on recv
```

---

## File Structure

```
go-like/
├── Plan.md
├── Makefile
├── include/
│   ├── coroutine.h      — coroutine struct, create/yield/exit
│   ├── scheduler.h      — processor, run queues, EDF heap, sched_next
│   ├── channel.h        — channel struct, send/recv/close
│   └── runtime.h        — runtime_init/shutdown, go() macro
├── src/
│   ├── coroutine.c      — stack alloc (mmap + guard), ucontext setup, trampoline
│   ├── scheduler.c      — priority queues, EDF min-heap, work stealing, worker loop
│   ├── channel.c        — ring buffer, waitq blocking/unblocking
│   └── runtime.c        — thread pool init, global queue, go() implementation
└── examples/
    ├── priority_demo.c  — 10 coroutines at different priorities, verify order
    └── deadline_demo.c  — tasks with tight deadlines, show EDF beats round-robin
```

---

## Key APIs

```c
// Initialize the runtime with N worker threads
void runtime_init(int num_threads);
void runtime_shutdown(void);

// Create a coroutine (does not run it yet)
coroutine_t *coro_create(void (*fn)(void *), void *arg,
                          int priority, uint64_t deadline_ns);

// Cooperative yield — give up the CPU voluntarily
void coro_yield(void);

// Spawn a coroutine onto the scheduler
void sched_spawn(coroutine_t *c);

// Channels
channel_t *chan_make(size_t elem_size, int capacity);
void  chan_send(channel_t *ch, const void *val);   // blocks if full
int   chan_recv(channel_t *ch, void *out);          // blocks if empty
void  chan_close(channel_t *ch);

// Convenience macro (like Go's `go fn(arg)`)
#define go(fn, arg, prio, deadline_ns) \
    sched_spawn(coro_create(fn, arg, prio, deadline_ns))
```

---

## Scheduling Algorithm (`sched_next`)

```
1. Check EDF heap: if top coroutine has deadline_ns > 0 and deadline is approaching
   → pop and return it immediately (deadline-critical)

2. Otherwise, scan run_queue[0..7] from highest priority:
   → return the first non-empty queue's head

3. If local processor is empty → attempt work stealing:
   → pick a random other processor, steal from its lowest-priority non-empty queue
   → if still nothing, park the thread (futex/condvar wait)
```

---

## Build Plan (Implementation Order)

### Step 1 — Coroutine Foundation
- `coroutine.h` / `coroutine.c`
- mmap stack allocation with `PROT_NONE` guard page below stack
- `ucontext_t` setup with trampoline function
- `coro_yield()` swaps back to scheduler context
- Goal: single coroutine that can yield and be resumed

### Step 2 — Single-Threaded Scheduler
- `scheduler.h` / `scheduler.c`
- 8-level priority queue (array of FIFO linked lists)
- EDF min-heap (binary heap, keyed on `deadline_ns`)
- `sched_next()` implementing the algorithm above
- Single-threaded event loop: pick next → makecontext → swapcontext
- Goal: multiple coroutines running cooperatively on one thread

### Step 3 — Multi-Threaded Runtime
- `runtime.h` / `runtime.c`
- One `Processor` (P) per worker thread
- pthread pool, each thread runs its own scheduler loop
- Work stealing between processors
- Global overflow queue for newly spawned coroutines
- Mutex + condvar for thread parking
- Goal: coroutines distributed across all CPU cores

### Step 4 — Channels
- `channel.h` / `channel.c`
- Fixed-capacity ring buffer (malloc'd)
- `chan_send`: if full → block coroutine, add to `send_waitq`
- `chan_recv`: if empty → block coroutine, add to `recv_waitq`
- When space/data becomes available → unblock waiting coroutine by re-enqueuing it
- Goal: goroutine-style CSP communication

### Step 5 — Examples & Makefile
- `priority_demo.c`: spawn background + urgent coroutines, show urgent runs first
- `deadline_demo.c`: spawn tasks with deadlines, print which ones missed deadline
- Makefile with: `make`, `make asan` (AddressSanitizer), `make tsan` (ThreadSanitizer)

---

## Potential Pitfalls

| Issue | Mitigation |
|---|---|
| `ucontext_t` deprecated on macOS | Use it anyway (still works); add `-Wno-deprecated` |
| False sharing on per-processor structs | Pad `processor_t` to 64 bytes (cache line) |
| Stack overflow silently corrupts memory | Guard page with `PROT_NONE` → segfault on overflow |
| Work stealing causes starvation of low-prio work | Only steal if local queue empty; steal low-prio tasks |
| Channel waitq race with multi-thread scheduler | Per-channel mutex; coroutine unblocked by the sender/receiver thread |
| `CLOCK_MONOTONIC` resolution on macOS | Use `clock_gettime` with `CLOCK_MONOTONIC_RAW` for tighter deadlines |

---

## Verification

- `make asan && ./priority_demo` — output should show priority-0 tasks always finishing before priority-7 tasks
- `make asan && ./deadline_demo` — deadline miss count should be 0 or near-0 under normal load
- `make tsan && ./deadline_demo` — no data race warnings
- Valgrind `--tool=massif` to verify no stack memory leaks after coroutine teardown

---

## Benchmarks

All benchmarks live in `benchmarks/` and are built via `make bench`. Each prints a CSV row so results can be piped into a comparison table.

### Reference numbers (what we're competing against)

| Library | Context switches/sec (2 coroutines, macOS x86_64) |
|---|---|
| libaco (C, hand-rolled asm) | ~100M (≈10 ns/switch) |
| tbox (C) | 48.7M |
| libmill (C) | 19M |
| boost.context (C++) | 13.7M |
| **Go goroutines** | **6.4M (≈156 ns/switch)** ← our baseline to beat |
| libtask (C) | 6.2M |

Channel throughput reference (unbuffered, passes/sec):

| Library | Passes/sec |
|---|---|
| tbox | 10.9M |
| **Go** | **3.1M** ← baseline |
| libmill | 2.9M |
| libtask | 2.7M |

Sources: [tboox coroutine benchmark](https://tboox.org/2016/10/28/benchbox-coroutine/), [libaco](https://github.com/hnes/libaco), [TKONIY/co-benchmark](https://github.com/TKONIY/co-benchmark)

---

### Benchmark 1 — Context Switch Speed (`bench_ctx_switch.c`)

**What**: N coroutines ping-pong via `coro_yield()` in a tight loop for 10 seconds. Report switches/sec and ns/switch.

**Methodology** (mirrors tboox / libaco):
```
for N in {2, 100, 1000, 10000}:
    spawn N coroutines, each yielding in an infinite loop
    count total yields over 10s wall time
    report: yields/sec, ns/yield
```

**Target**: > 6.4M switches/sec (beat Go) at N=2; stay competitive at N=1000.

---

### Benchmark 2 — Channel Throughput (`bench_channel.c`)

**What**: One producer coroutine + one consumer coroutine, passing integers through a channel. Unbuffered (cap=0) and buffered (cap=256) variants.

**Methodology**:
```
producer: loop, chan_send(ch, &i)
consumer: loop, chan_recv(ch, &v)
run for 10s, report messages/sec
```

**Target**: > 3.1M passes/sec unbuffered (beat Go).

---

### Benchmark 3 — Scalability Curve (`bench_scale.c`)

**What**: Measure context switch throughput as coroutine count scales: 2, 10, 100, 1K, 10K, 100K. Plot degradation curve.

**Why**: Go maintains ~6.4M switches/sec from 2→1000 coroutines (work-stealer helps). We want to match or beat that flatness.

**Output**: CSV `n_coroutines, switches_per_sec` — plot with gnuplot or paste into a spreadsheet.

---

### Benchmark 4 — Deadline Miss Rate (`bench_deadline.c`) ← unique to us

**What**: The benchmark Go *cannot run*. Spawn M coroutines each with a tight deadline under increasing CPU load. Compare our EDF scheduler against a naive FIFO scheduler (simulating Go behavior).

**Methodology**:
```
for utilization in {50%, 75%, 90%, 95%, 100%, 110% (overload)}:
    spawn 100 coroutines with random deadlines in [1ms, 10ms]
    each coroutine does a fixed amount of CPU work
    record: deadlines_met / total, avg_lateness_ns

run twice: once with EDF enabled, once with EDF disabled (FIFO only)
```

**Expected results**:
- EDF: 100% deadlines met at ≤100% utilization (provably optimal)
- FIFO: deadline misses begin well below 100% utilization, unpredictable

---

### File additions for benchmarks

```
go-like/
└── benchmarks/
    ├── bench_ctx_switch.c   — benchmark 1
    ├── bench_channel.c      — benchmark 2
    ├── bench_scale.c        — benchmark 3
    ├── bench_deadline.c     — benchmark 4
    └── bench_common.h       — timing helpers (clock_gettime wrapper, CSV printer)
```

Add to Makefile:
```makefile
bench: $(BENCH_BINS)
    @for b in $(BENCH_BINS); do echo "--- $$b ---"; ./$$b; done
```
