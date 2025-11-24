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
#include "spike_interface/spike_file.h"
#include "kernel/elf.h"

//
// implement the SYS_user_print syscall
//
ssize_t sys_user_print(const char* buf, size_t n) {
  sprint(buf);
  return 0;
}

//
// implement the SYS_user_exit syscall
//
ssize_t sys_user_exit(uint64 code) {
  sprint("User exit with code:%d.\n", code);
  // in lab1, PKE considers only one app (one process). 
  // therefore, shutdown the system when the app calls exit()
  shutdown(code);
}

//
// implement the SYS_user_print_backtrace syscall
//
ssize_t sys_print_backtrace(uint64 depth) {
  trapframe *tf = current ? current->trapframe : NULL;
  if (!tf) {
    sprint("no current trapframe\n");
    return -1;
  }
  uint64 fp = tf->regs.s0;
  int printed = 0;
  for (uint64 i = 0; i < depth && fp; i++) {
    uint64 prev_fp = 0, saved_ra = 0;
    //prev_fp = *(uint64 *)(fp + 0);
    // if use this convention,it will skip some frames
    prev_fp = fp + 0x10; 
    // in our convention, the previous frame pointer is stored at offset 0x10
    saved_ra = *(uint64 *)(fp + 8);
    int idx = f_addr_to_name(saved_ra-1);
    if (idx >= 0) {
      //sprint("frame[%d]: fp=0x%lx saved_ra=0x%lx (%s) prev_fp=0x%lx\n",
      //  printed, fp, saved_ra, func_names[idx].name, prev_fp);
      sprint("%s\n", func_names[idx].name);
    }     
    printed++;
    if (prev_fp == 0 || prev_fp == fp) break;
    fp = prev_fp;
  }
  sprint("=== end backtrace (printed %d frames) ===\n", printed);
  return printed;
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
    case SYS_user_print_backtrace:
      return sys_print_backtrace(a1);
    default:
      panic("Unknown syscall %ld \n", a0);
  }
}
