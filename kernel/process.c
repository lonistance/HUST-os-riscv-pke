/*
 * Utility functions for process management. 
 *
 * Note: in Lab1, only one process (i.e., our user application) exists. Therefore, 
 * PKE OS at this stage will set "current" to the loaded user application, and also
 * switch to the old "current" process after trap handling.
 */

#include "riscv.h"
#include "strap.h"
#include "config.h"
#include "process.h"
#include "elf.h"
#include "string.h"
#include "vmm.h"
#include "pmm.h"
#include "memlayout.h"
#include "util/functions.h"
#include "spike_interface/spike_utils.h"

//Two functions defined in kernel/usertrap.S
extern char smode_trap_vector[];
extern void return_to_user(trapframe *, uint64 satp);

// current points to the currently running user-mode application.
process* current = NULL;

// points to the first free page in our simple heap. added @lab2_2
uint64 g_ufree_page = USER_FREE_ADDRESS_START;

//
// switch to a user-mode process
//
void switch_to(process* proc) {
  assert(proc);
  current = proc;

  // write the smode_trap_vector (64-bit func. address) defined in kernel/strap_vector.S
  // to the stvec privilege register, such that trap handler pointed by smode_trap_vector
  // will be triggered when an interrupt occurs in S mode.
  write_csr(stvec, (uint64)smode_trap_vector);

  // set up trapframe values (in process structure) that smode_trap_vector will need when
  // the process next re-enters the kernel.
  proc->trapframe->kernel_sp = proc->kstack;      // process's kernel stack
  proc->trapframe->kernel_satp = read_csr(satp);  // kernel page table
  proc->trapframe->kernel_trap = (uint64)smode_trap_handler;

  // SSTATUS_SPP and SSTATUS_SPIE are defined in kernel/riscv.h
  // set S Previous Privilege mode (the SSTATUS_SPP bit in sstatus register) to User mode.
  unsigned long x = read_csr(sstatus);
  x &= ~SSTATUS_SPP;  // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE;  // enable interrupts in user mode

  // write x back to 'sstatus' register to enable interrupts, and sret destination mode.
  write_csr(sstatus, x);

  // set S Exception Program Counter (sepc register) to the elf entry pc.
  write_csr(sepc, proc->trapframe->epc);

  // make user page table. macro MAKE_SATP is defined in kernel/riscv.h. added @lab2_1
  uint64 user_satp = MAKE_SATP(proc->pagetable);

  // return_to_user() is defined in kernel/strap_vector.S. switch to user mode with sret.
  // note, return_to_user takes two parameters @ and after lab2_1.
  return_to_user(proc->trapframe, user_satp);
}

//
// ensure virtual address range [va, va+size] is mapped to physical pages
//
static int ensure_heap_mapped(pagetable_t pt, uint64 va, uint64 size) {
  if (size == 0) return 0;

  uint64 start = ROUNDDOWN(va, PGSIZE);
  uint64 end = ROUNDDOWN(va + size - 1, PGSIZE);

  for (uint64 p = start; p <= end; p += PGSIZE) {
    pte_t *pte = page_walk(pt, p, 0);
    if (pte == 0 || (*pte & PTE_V) == 0) {
      void *pa = alloc_page();
      if (pa == 0) {
        panic("ensure_heap_mapped: out of physical memory");
        return -1;
      }
      user_vm_map(pt, p, PGSIZE, (uint64)pa,
                  prot_to_type(PROT_WRITE | PROT_READ, 1));
    }
  }
  return 0;
}

//
// allocate memory from process heap with size bytes
//
uint64 heap_alloc(process *proc, uint64 size) {
  if (size == 0) return 0;

  // align size to 8 bytes
  uint64 alloc_size = ROUNDUP(size, 8);

  // first-fit: search for a free block
  mcb *cur = proc->mcb_list;
  while (cur) {
    if (!cur->used && cur->size >= alloc_size) {
      cur->used = 1;
      return cur->addr;
    }
    cur = cur->next;
  }

  // no free block found, allocate from heap_top
  uint64 addr = proc->heap_top;

  if (ensure_heap_mapped(proc->pagetable, addr, alloc_size) != 0) {
    return 0;
  }

  // create new MCB
  mcb *new_mcb = (mcb *)alloc_page();
  if (new_mcb == 0) {
    panic("heap_alloc: out of memory for MCB");
    return 0;
  }
  new_mcb->addr = addr;
  new_mcb->size = alloc_size;
  new_mcb->used = 1;
  new_mcb->next = NULL;

  // append to mcb list
  if (proc->mcb_list == NULL) {
    proc->mcb_list = new_mcb;
  } else {
    mcb *tail = proc->mcb_list;
    while (tail->next) tail = tail->next;
    tail->next = new_mcb;
  }

  proc->heap_top = addr + alloc_size;

  return addr;
}

//
// free memory block at addr
//
void heap_free(process *proc, uint64 addr) {
  mcb *cur = proc->mcb_list;
  while (cur) {
    if (cur->addr == addr && cur->used) {
      cur->used = 0;
      return;
    }
    cur = cur->next;
  }
  panic("heap_free: invalid addr 0x%lx", addr);
}
