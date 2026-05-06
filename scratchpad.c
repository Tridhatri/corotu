#pragma once
#include <stdint.h>
#include <stddef.h>
#include <ucontext.h>

#define  STACK_SIZE (64 * 1024)
#define CORO_GAURD_PAGE_SIZE 4096


typedef enum {
    CORO_READY = 0,
    CORO_RUNNING = 1,
    CORO_BLOCKED = 2, 
    CORO_DONE = 3,
  }coro_state_t;

typedef struct coroutine {
  ucontext_t ctx;
  void * stack;
  size_t stack_size;
  void (*fn) (void*);
  void *arg;
  int priority;
  uint64_tt deadline_ns;
  int cpu_affinity;
  coro_state_t state;
  struct coroutine *next;
}coroutine_t;


coroutine_t* coro_create(void (*fn)(void*), void *arg, int priority, uint64)t deadline){

  coroutine_t c = (*coroutine_t)calloc(1,sizeof(coroutine_t));
  //## WHy calloc(1 ), instead of calloc(0 )
  c->fn = fn;  
  c->arg = arg;
  c->priority = priority;
  c->deadline_ns = deadline_ns;
  
  
  get_context(&c->ctx);
  c->stack = NULL;
  c->stack_size = STACK_SIZE;
  c->cpu_affinity =  0;
  c->state = CORO_READY;
  c->next = NULL;
  
  size_t total = CORO_GAURD_PAGE_SIZE + c->stack_size;

  void* mem = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE| MAP_ANONYMOUS, -1, 0);
  //## Why mmap first arg is NULL? what does the flags mean?
  // Does MAP_PRIVATE mean thtat it is private to the process and any modifications to it, 
  // does not let other processes see it?
  // I mean does it mean, the memory map vanish after the exit of the process?
  // Munmap also explain.
  

  if(mem == MAP_FAILED) {
    free(c);
    return NULL;
  }

  if(mprotect(mem, CORO_GUARD_PAGE_SIZE, PROT_NONE) != 0) {
      munmap(mem, total);
      // Munmap is used 
      free(c);
      return NULL;

  }

  getcontext(&c->ctx);

  c->ctx.uc_stack.ss_sp = c->stack;
  c->ctx.uc_stack>ss_size = c->stack_size;




}

static long get_page_size(void){
  static long page_size = 0;
  if(page_size == 0){

    page_size = sysconf(_SC_PAGE_SIZE);
    if(page_size <=0) page_size = 4096;
  }
  return page_size;
}
// static is used for internal linkeage.
// Which means the value will not be changed throughout the file.
//
