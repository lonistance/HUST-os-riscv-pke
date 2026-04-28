/*
 * contains the implementation of all syscalls.
 */

#include <stdint.h>
#include <errno.h>

#include "util/types.h"
#include "syscall.h"
#include "string.h"
#include "process.h"
#include "util/functions.h"

#include "spike_interface/spike_utils.h"
#include "spike_interface/atomic.h"

//
// implement the SYS_user_print syscall
//
ssize_t sys_user_print(const char* buf, size_t n) {
  int hartid = read_tp();
  sprint("hartid = %d: %s\n", hartid, buf);
  return 0;
}

//
// implement the SYS_user_exit syscall
//
static volatile int finished_harts = 0;
static spinlock_t exit_lock = SPINLOCK_INIT;

ssize_t sys_user_exit(uint64 code) {
  int hartid = read_tp();
  sprint("hartid = %d: User exit with code:%d.\n", hartid, code);

  spinlock_lock(&exit_lock);
  finished_harts++;
  spinlock_unlock(&exit_lock);

  if (hartid == 0) {
    while (finished_harts < NCPU) {
      // spin wait for other harts to finish
    }
    sprint("hartid = %d: shutdown with code:%d.\n", hartid, code);
    shutdown(code);
  } else {
    while (1) {
      asm volatile("wfi");
    }
  }
}

//
// [a0]: the syscall number; [a1] ... [a7]: arguments to the syscalls.
// returns the code of success, (e.g., 0 means success, fail for otherwise)
//
long do_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long a6, long a7) {
  switch (a0) {
    case SYS_user_print:
      return sys_user_print((const char*)a1, a2);
    case SYS_user_exit:
      return sys_user_exit(a1);
    default:
      panic("Unknown syscall %ld \n", a0);
  }
}
