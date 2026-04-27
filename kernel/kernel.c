/*
 * Supervisor-mode startup codes
 */

#include "riscv.h"
#include "string.h"
#include "elf.h"
#include "process.h"

#include "spike_interface/spike_utils.h"

// process is a structure defined in kernel/process.h
process user_app;

// global array to hold processes for each hart
process user_apps[NCPU];

//
// load the elf, and construct a "process" (with only a trapframe).
// load_bincode_from_host_elf is defined in elf.c
//
void load_user_program(process *proc, int hartid) {
  // calculate trapframe and kstack addresses based on hartid
  uint64 trapframe_addr = USER_TRAP_FRAME + hartid * 0x1000;
  uint64 kstack_addr = USER_KSTACK + hartid * 0x1000;
  uint64 stack_addr = USER_STACK + hartid * 0x1000;
  
  proc->trapframe = (trapframe *)trapframe_addr;
  memset(proc->trapframe, 0, sizeof(trapframe));
  proc->kstack = kstack_addr;
  proc->trapframe->regs.sp = stack_addr;

  // load_bincode_from_host_elf() is defined in kernel/elf.c
  load_bincode_from_host_elf(proc, hartid);
}

//
// s_start: S-mode entry point of riscv-pke OS kernel.
//
int s_start(void) {
  // Note: we use direct (i.e., Bare mode) for memory mapping in lab1.
  // which means: Virtual Address = Physical Address
  // therefore, we need to set satp to be 0 for now. we will enable paging in lab2_x.
  // 
  // write_csr is a macro defined in kernel/riscv.h
  write_csr(satp, 0);

  // get the current hartid
  int hartid = read_tp();
  sprint("hartid = %d: Enter supervisor mode...\n", hartid);
  
  // the application code (elf) is first loaded into memory, and then put into execution
  load_user_program(&user_apps[hartid], hartid);

  sprint("hartid = %d: Switch to user mode...\n", hartid);
  // switch_to() is defined in kernel/process.c
  switch_to(&user_apps[hartid]);

  // we should never reach here.
  return 0;
}
