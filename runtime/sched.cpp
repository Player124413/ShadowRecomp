// Fiber based implementation of the Dolphin OS context switch primitives.
#include "rt_sched.h"
#include "funcs.h"
#include "plat.h"
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#elif defined(__ANDROID__)
// Android: Bionic never implemented ucontext (there is no getcontext/makecontext there), so a fiber is a real thread
// that stays parked on its own semaphore. Only one of them runs at a time - a switch posts the next fiber's semaphore
// and waits for its own - so this is the same cooperative scheduling as elsewhere, only with the switch done by the
// kernel. 16 MB of guest stack per fiber is reserved the same way as on the PC.
#include <cerrno>
#include <pthread.h>
#include <semaphore.h>
#else
// Android and Linux: cooperative fibers on ucontext. The guest's threads (OSCreateThread / OSSaveContext) switch
// exactly the same way as on the PC; only the switch primitive differs.
#include <ucontext.h>
#include <sys/mman.h>
#endif

ExcFrame* g_exc_top = nullptr;
bool g_trace_sched = false;

namespace {
enum Mode { M_NONE = 0, M_RETURN, M_UNWIND, M_START };

struct Fiber {
  int id = 0;
#ifdef _WIN32
  void* handle = nullptr;          // Windows fiber
#elif defined(__ANDROID__)
  sem_t sem{};                     // the switch primitive: whoever wants us running posts this
  pthread_t thread{};              // our own thread (fiber 0 is the caller's, so no thread of its own)
  bool own_thread = false;
#else
  ucontext_t ctx{};                // POSIX: the saved machine context
  void* stack = nullptr;           // its own stack (the main fiber has none: it is the caller's)
  size_t stack_size = 0;
#endif
  Cpu* cpu = nullptr;
  ExcFrame* exc_top = nullptr;
  int mode = M_NONE;
  uint32_t start_pc = 0, start_lr = 0;
  bool dead = false;      // ran OSExitThread: never resumed again, reclaimed by reap_dead()
};

std::vector<Fiber*> g_fibers;
Fiber* g_cur = nullptr;
std::unordered_map<uint32_t, Fiber*> g_owner;      // OSContext (physical) -> owning fiber

constexpr uint32_t kMask = RT_RAM_MASK;

#ifdef _WIN32
void CALLBACK fiber_main(void* p) {
  Fiber* f = (Fiber*)p;
#else
Fiber* g_starting = nullptr;       // the fiber a freshly made context starts (makecontext takes ints only)
void fiber_entry(Fiber* f);
void fiber_entry_trampoline() { fiber_entry(g_starting); }
void fiber_entry(Fiber* f) {
#endif
  logf("[sched] fiber %d starts at %08X lr=%08X r1=%08X\n", f->id, f->start_pc, f->start_lr, f->cpu->r[1]);
  Cpu& c = *f->cpu;
  g_exc_top = nullptr;
  uint32_t lr = f->start_lr;
  call_addr(c, f->start_pc);
  // entry returned: the real CPU would 'blr' into OSExitThread
  logf("[sched] fiber %d entry returned\n", f->id);
  f->dead = true;   // lr is OSExitThread (reached through call_addr, so the override below does not see it)
  c.lr = lr;
  call_addr(c, lr);
  rt_unimpl(c, 0, "thread exit returned");
}

void reap_dead();

// ---- the switch primitive -----------------------------------------------------------------------------------------
#ifdef _WIN32
void* fiber_create(Fiber* f) { return CreateFiberEx(1 << 16, 16 << 20, 0, fiber_main, f); }
void fiber_destroy(Fiber* f) { if (f->handle) DeleteFiber(f->handle); }
void fiber_switch(Fiber* from, Fiber* to) { (void)from; SwitchToFiber(to->handle); }
void fiber_init_main(Fiber*) {}
#elif defined(__ANDROID__)
constexpr size_t kFiberStack = 16u << 20;   // as on the PC: the recompiled guest code uses big frame stacks
void* fiber_thread(void* p) {
  Fiber* f = (Fiber*)p;
  sem_wait(&f->sem);                 // parked until someone switches to us
  fiber_entry(f);
  return nullptr;
}
void* fiber_create(Fiber* f) {
  if (sem_init(&f->sem, 0, 0) != 0) return nullptr;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, kFiberStack);
  const int rc = pthread_create(&f->thread, &attr, fiber_thread, f);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    sem_destroy(&f->sem);
    return nullptr;
  }
  f->own_thread = true;
  return f;
}
void fiber_destroy(Fiber* f) {
  if (!f->own_thread) return;        // fiber 0 runs on the caller's thread; its stack is not ours
  // A fiber is only ever reclaimed once it can no longer be resumed. Wake it so its thread sees that and exits, then
  // wait for it: freeing the semaphore under a parked thread would be a use-after-free.
  sem_post(&f->sem);
  pthread_join(f->thread, nullptr);
  sem_destroy(&f->sem);
  f->own_thread = false;
}
void fiber_switch(Fiber* from, Fiber* to) {
  sem_post(&to->sem);
  while (sem_wait(&from->sem) != 0 && errno == EINTR) {
  }
  if (from->dead) pthread_exit(nullptr);   // woken up to be reclaimed: this thread has nothing left to run
}
void fiber_init_main(Fiber* f) { sem_init(&f->sem, 0, 0); }
#else
constexpr size_t kFiberStack = 16u << 20;   // as on the PC: the recompiled guest code uses big frame stacks
void* fiber_create(Fiber* f) {
  void* stack = mmap(nullptr, kFiberStack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (stack == MAP_FAILED) return nullptr;
  f->stack = stack;
  f->stack_size = kFiberStack;
  getcontext(&f->ctx);
  f->ctx.uc_stack.ss_sp = stack;
  f->ctx.uc_stack.ss_size = kFiberStack;
  f->ctx.uc_link = nullptr;
  g_starting = f;
  makecontext(&f->ctx, fiber_entry_trampoline, 0);
  return f;
}
void fiber_destroy(Fiber* f) {
  if (f->stack) munmap(f->stack, f->stack_size);
  f->stack = nullptr;
}
void fiber_switch(Fiber* from, Fiber* to) { swapcontext(&from->ctx, &to->ctx); }
void fiber_init_main(Fiber*) {}
#endif

void switch_to(Fiber* t, int mode) {
  Fiber* me = g_cur;
  if (g_trace_sched) logf("[sched] switch %d -> %d mode %d\n", me->id, t->id, mode);
  me->exc_top = g_exc_top;
  t->mode = mode;
  g_cur = t;
  g_exc_top = t->exc_top;
  fiber_switch(me, t);
  // resumed
  g_cur = me;
  g_exc_top = me->exc_top;
  reap_dead();
}

// Free the fibers of guest threads that called OSExitThread (each loader job runs on a new OS thread, so these
// pile up: 16 MB of reserved stack per fiber). Safe once we are running on another fiber.
void reap_dead() {
  for (size_t i = 0; i < g_fibers.size(); ++i) {
    Fiber* f = g_fibers[i];
    if (!f || !f->dead || f == g_cur || f->exc_top) continue;   // never free a fiber with a pending exception frame
    for (auto it = g_owner.begin(); it != g_owner.end();) it = it->second == f ? g_owner.erase(it) : std::next(it);
    fiber_destroy(f);
    free(f->cpu);
    delete f;
    g_fibers[i] = nullptr;
  }
}
}  // namespace

void sched_init(Cpu& main_cpu) {
  Fiber* f = new Fiber();
  f->id = 0;
  fiber_init_main(f);
  f->cpu = &main_cpu;
  g_fibers.push_back(f);
  g_cur = f;
}

int sched_current_id() { return g_cur ? g_cur->id : 0; }

void sched_register_owner(uint32_t ctx) { g_owner[ctx & kMask] = g_cur; }

// ---------------------------------------------------------------- HLE overrides
void f_80371DA0(Cpu&);   // OSSaveContext (real implementation)

void hle_OSSaveContext(Cpu& c) {
  uint32_t ctx = c.r[3];
  f_80371DA0(c);                // stores the register file; r3 = 0
  g_owner[ctx & kMask] = g_cur;
}

void f_80371F54(Cpu&);   // OSInitContext (real implementation)
void f_80377DE4(Cpu&);   // OSExitThread (real implementation)

// OSExitThread never returns: it reschedules away from this thread for good. Mark the fiber so it gets freed.
void hle_OSExitThread(Cpu& c) {
  if (g_cur && g_cur->id != 0) g_cur->dead = true;
  f_80377DE4(c);
}

// OSCreateThread re-initialises an OSThread's context. If that OSThread struct was used before, its context is
// still mapped to the old (exited) fiber; loading it would resume the dead fiber instead of starting the new
// thread at its entry point. Forget the mapping so the next OSLoadContext starts a fresh fiber.
void hle_OSInitContext(Cpu& c) {
  uint32_t ctx = c.r[3];
  auto it = g_owner.find(ctx & kMask);
  if (it != g_owner.end()) {
    logf("[sched] OSInitContext ctx=%08X: dropping stale owner fiber %d\n", ctx, it->second->id);
    g_owner.erase(it);
  }
  f_80371F54(c);
}

static void ctx_to_cpu(uint32_t ctx, Cpu& c) {
  for (int i = 0; i < 32; ++i) c.r[i] = ld32(ctx + 4 * i);
  set_crf(c, ld32(ctx + 0x80), 0xFF);
  c.lr = ld32(ctx + 0x84);
  c.ctr = ld32(ctx + 0x88);
  set_xer(c, ld32(ctx + 0x8C));
  c.srr0 = ld32(ctx + 0x198);
  c.srr1 = ld32(ctx + 0x19C);
  c.msr = c.srr1;
  for (int i = 1; i < 8; ++i) c.gqr[i] = ld32(ctx + 0x1A8 + 4 * (i - 1));
}

void hle_OSLoadContext(Cpu& c) {
  uint32_t ctx = c.r[3];
  if (g_trace_sched) logf("[sched] OSLoadContext ctx=%08X lr=%08X\n", ctx, c.lr);
  uint32_t key = ctx & kMask;
  uint32_t state = ld16(ctx + 0x1A2);
  uint32_t srr0 = ld32(ctx + 0x198);
  Fiber* target = nullptr;
  int mode = M_RETURN;

  if ((state & 2) && is_sentinel(srr0)) {
    // context saved by an exception: resume the fiber that was interrupted
    st16(ctx + 0x1A2, state & ~2u);
    int fid = sentinel_fid(srr0);
    if (fid >= (int)g_fibers.size()) rt_unimpl(c, 0, "OSLoadContext: bad sentinel fiber");
    target = g_fibers[fid];
    if (!target) rt_unimpl(c, 0, "OSLoadContext: sentinel names a reclaimed fiber");
    mode = M_UNWIND;
  } else {
    auto it = g_owner.find(key);
    if (it != g_owner.end()) {
      target = it->second;
      mode = M_RETURN;
    }
  }

  if (!target) {
    // brand new thread: start a fiber at ctx.srr0
    Fiber* f = new Fiber();
    f->id = (int)g_fibers.size();
    f->cpu = (Cpu*)calloc(1, sizeof(Cpu));
    ctx_to_cpu(ctx, *f->cpu);
    f->cpu->budget = 4000;
    f->start_pc = ld32(ctx + 0x198);
    f->start_lr = ld32(ctx + 0x84);
    fiber_create(f);
    g_fibers.push_back(f);
    g_owner[key] = f;
    target = f;
    mode = M_START;
  }

  if (target == g_cur) {
    if (mode == M_UNWIND) {
      if (!g_exc_top) rt_unimpl(c, 0, "OSLoadContext: unwind without exception frame");
      longjmp(g_exc_top->jb, 1);
    }
    // Loading our own voluntarily-saved context == returning from OSSaveContext a second time,
    // which for SelectThread simply leaves the function.
    return;
  }

  switch_to(target, mode);
  // somebody loaded *our* context
  if (g_cur->mode == M_UNWIND) {
    if (!g_exc_top) rt_unimpl(c, 0, "resumed for unwind without exception frame");
    longjmp(g_exc_top->jb, 1);
  }
  // M_RETURN: continue as if OSSaveContext had returned the second time
}

Cpu* sched_current_cpu() { return g_cur ? g_cur->cpu : nullptr; }
