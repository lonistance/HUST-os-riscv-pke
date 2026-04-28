#include "kernel/riscv.h"
#include "kernel/process.h"
#include "kernel/elf.h"
#include "spike_interface/spike_utils.h"

//
// added @lab1_challenge2
//
static void print_error_line_mtrap() {
  extern process *current;
  if (!current || !current->line || current->line_ind <= 0) return;

  uint64 addr = read_csr(mepc);
  int l = 0, r = current->line_ind - 1, ans = -1;
  while (l <= r) {
    int mid = (l + r) / 2;
    if (current->line[mid].addr <= addr) {
      ans = mid;
      l = mid + 1;
    } else {
      r = mid - 1;
    }
  }

  if (ans >= 0) {
    addr_line *al = &current->line[ans];
    code_file *cf = &current->file[al->file];
    char *dir = current->dir[cf->dir];
    sprint("Runtime error at %s/%s:%d\n", dir, cf->file, (int)al->line);
    print_source_line(dir, cf->file, (int)al->line);
  }
}

static void handle_instruction_access_fault() {
  print_error_line_mtrap();
  panic("Instruction access fault!");
}

static void handle_load_access_fault() {
  print_error_line_mtrap();
  panic("Load access fault!");
}

static void handle_store_access_fault() {
  print_error_line_mtrap();
  panic("Store/AMO access fault!");
}

static void handle_illegal_instruction() {
  print_error_line_mtrap();
  panic("Illegal instruction!");
}

static void handle_misaligned_load() {
  print_error_line_mtrap();
  panic("Misaligned Load!");
}

static void handle_misaligned_store() {
  print_error_line_mtrap();
  panic("Misaligned AMO!");
}

// added @lab1_3
static void handle_timer() {
  int cpuid = 0;
  // setup the timer fired at next time (TIMER_INTERVAL from now)
  *(uint64*)CLINT_MTIMECMP(cpuid) = *(uint64*)CLINT_MTIMECMP(cpuid) + TIMER_INTERVAL;

  // setup a soft interrupt in sip (S-mode Interrupt Pending) to be handled in S-mode
  write_csr(sip, SIP_SSIP);
}

//
// handle_mtrap calls a handling function according to the type of a machine mode interrupt (trap).
//
void handle_mtrap() {
  show_debug(current);
  uint64 mcause = read_csr(mcause);
  switch (mcause) {
    case CAUSE_MTIMER:
      handle_timer();
      break;
    case CAUSE_FETCH_ACCESS:
      handle_instruction_access_fault();
      break;
    case CAUSE_LOAD_ACCESS:
      handle_load_access_fault();
    case CAUSE_STORE_ACCESS:
      handle_store_access_fault();
      break;
    case CAUSE_ILLEGAL_INSTRUCTION:
      // TODO (lab1_2): call handle_illegal_instruction to implement illegal instruction
      // interception, and finish lab1_2.
      //panic( "call handle_illegal_instruction to accomplish illegal instruction interception for lab1_2.\n" );
      handle_illegal_instruction();
      break;
    case CAUSE_MISALIGNED_LOAD:
      handle_misaligned_load();
      break;
    case CAUSE_MISALIGNED_STORE:
      handle_misaligned_store();
      break;

    default:
      print_error_line_mtrap();
      sprint("machine trap(): unexpected mscause %p\n", mcause);
      sprint("            mepc=%p mtval=%p\n", read_csr(mepc), read_csr(mtval));
      panic( "unexpected exception happened in M-mode.\n" );
      break;
  }
}
