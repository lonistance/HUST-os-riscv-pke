#ifndef _SYNC_UTILS_H_
#define _SYNC_UTILS_H_

static inline void sync_barrier(volatile int *counter, int all) {

  int local;

  asm volatile("amoadd.w %0, %2, (%1)\n"
               : "=r"(local)
               : "r"(counter), "r"(1)
               : "memory");

  if (local + 1 < all) {
    do {
      asm volatile("lw %0, (%1)\n" : "=r"(local) : "r"(counter) : "memory");
    } while (local < all);
  }
}

// Simple spinlock using amoswap
typedef struct {
  int locked;
} spinlock;

static inline void spinlock_init(spinlock *lk) {
  lk->locked = 0;
}

static inline void spinlock_acquire(spinlock *lk) {
  int expected;
  do {
    asm volatile("amoswap.w %0, %1, (%2)\n"
                 : "=r"(expected)
                 : "r"(1), "r"(&lk->locked)
                 : "memory");
  } while (expected);
}

static inline void spinlock_release(spinlock *lk) {
  asm volatile("amoswap.w x0, x0, (%0)\n"
               :
               : "r"(&lk->locked)
               : "memory");
}

#endif
