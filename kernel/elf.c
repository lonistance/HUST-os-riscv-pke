/*
 * routines that scan and load a (host) Executable and Linkable Format (ELF) file
 * into the (emulated) memory.
 */

#include "elf.h"
#include "string.h"
#include "riscv.h"
#include "spike_interface/spike_utils.h"


typedef struct elf_info_t {
  spike_file_t *f;
  process *p;
} elf_info;

elf_symbol elf_symbols[MAX_ELF_SYMBOLS];
int elf_symbol_count = 0;
f_name_addr func_names[MAX_ELF_SYMBOLS];

//
// the implementation of allocater. allocates memory space for later segment loading
//
static void *elf_alloc_mb(elf_ctx *ctx, uint64 elf_pa, uint64 elf_va, uint64 size) {
  // directly returns the virtual address as we are in the Bare mode in lab1_x
  return (void *)elf_va;
}

//
// actual file reading, using the spike file interface.
//
static uint64 elf_fpread(elf_ctx *ctx, void *dest, uint64 nb, uint64 offset) {
  elf_info *msg = (elf_info *)ctx->info;
  // call spike file utility to load the content of elf file into memory.
  // spike_file_pread will read the elf file (msg->f) from offset to memory (indicated by
  // *dest) for nb bytes.
  return spike_file_pread(msg->f, dest, nb, offset);
}

//
// init elf_ctx, a data structure that loads the elf.
//
elf_status elf_init(elf_ctx *ctx, void *info) {
  ctx->info = info;

  // load the elf header
  if (elf_fpread(ctx, &ctx->ehdr, sizeof(ctx->ehdr), 0) != sizeof(ctx->ehdr)) return EL_EIO;

  // check the signature (magic value) of the elf
  if (ctx->ehdr.magic != ELF_MAGIC) return EL_NOTELF;

  return EL_OK;
}

//
// load the elf segments to memory regions as we are in Bare mode in lab1
//
elf_status elf_load(elf_ctx *ctx) {
  // elf_prog_header structure is defined in kernel/elf.h
  elf_prog_header ph_addr;
  int i, off;

  // traverse the elf program segment headers
  for (i = 0, off = ctx->ehdr.phoff; i < ctx->ehdr.phnum; i++, off += sizeof(ph_addr)) {
    // read segment headers
    if (elf_fpread(ctx, (void *)&ph_addr, sizeof(ph_addr), off) != sizeof(ph_addr)) return EL_EIO;

    if (ph_addr.type != ELF_PROG_LOAD) continue;
    if (ph_addr.memsz < ph_addr.filesz) return EL_ERR;
    if (ph_addr.vaddr + ph_addr.memsz < ph_addr.vaddr) return EL_ERR;

    // allocate memory block before elf loading
    void *dest = elf_alloc_mb(ctx, ph_addr.vaddr, ph_addr.vaddr, ph_addr.memsz);

    // actual loading
    if (elf_fpread(ctx, dest, ph_addr.memsz, ph_addr.off) != ph_addr.memsz)
      return EL_EIO;
  }

  return EL_OK;
}

typedef union {
  uint64 buf[MAX_CMDLINE_ARGS];
  char *argv[MAX_CMDLINE_ARGS];
} arg_buf;

//
// returns the number (should be 1) of string(s) after PKE kernel in command line.
// and store the string(s) in arg_bug_msg.
//
static size_t parse_args(arg_buf *arg_bug_msg) {
  // HTIFSYS_getmainvars frontend call reads command arguments to (input) *arg_bug_msg
  long r = frontend_syscall(HTIFSYS_getmainvars, (uint64)arg_bug_msg,
      sizeof(*arg_bug_msg), 0, 0, 0, 0, 0);
  kassert(r == 0);

  size_t pk_argc = arg_bug_msg->buf[0];
  uint64 *pk_argv = &arg_bug_msg->buf[1];

  int arg = 1;  // skip the PKE OS kernel string, leave behind only the application name
  for (size_t i = 0; arg + i < pk_argc; i++)
    arg_bug_msg->argv[i] = (char *)(uintptr_t)pk_argv[arg + i];

  //returns the number of strings after PKE kernel in command line
  return pk_argc - arg;
}

//
// read section name string from shstrtab
//
void elf_read_sect_name(elf_ctx *ctx, char *dest, uint32 name_offset,
                               elf_sect_header *shstr_sh) {
  uint64 shstrtab_data_size = shstr_sh->sh_size;
  uint64 shstrtab_data_offset = shstr_sh->sh_offset;
  char shstrtab_data[MAX_SECTION_DATA_LEN];

  // read the whole shstrtab section
  elf_fpread(ctx, shstrtab_data, shstrtab_data_size, shstrtab_data_offset);

  // copy the section name to dest
  uint32 i = 0;
  while (i + name_offset < shstrtab_data_size) {
    dest[i] = shstrtab_data[name_offset + i];
    if (shstrtab_data[name_offset + i] == '\0') break;
    i++;
  }
  dest[i] = '\0';
}

int mystrncpy(char *dest, const char *src, size_t n) {
  size_t i;
  for (i = 0; i < n - 1 && src[i] != '\0'; i++) {
    dest[i] = src[i];
  }
  dest[i] = '\0';
  return i;
}

//
// load elf_strlab to get function name from address
//
void elf_load_symbol(elf_ctx *ctx) {
  elf_header ehdr = ctx->ehdr;
  uint64 shoff = ehdr.shoff;
  uint16 shentsize = ehdr.shentsize;
  uint16 shnum = ehdr.shnum; 
  uint16 shstrndx = ehdr.shstrndx;
  elf_sect_header sh_addr;
  elf_sect_header sh_strtab;
  elf_sect_header sh_symtab;
  elf_sect_header shstr_sh;
  uint64 shstr_pos = shoff + (uint64)shstrndx * shentsize;
  elf_fpread(ctx, &shstr_sh, sizeof(shstr_sh), shstr_pos);
  char shstrtab_data[MAX_SYMBOL_NAME_LEN];
  for (int i = 0; i < shnum; i++) {
    uint64 sh_pos = shoff + (uint64)i * shentsize;
    elf_fpread(ctx, &sh_addr, sizeof(sh_addr), sh_pos);
    elf_read_sect_name(ctx, shstrtab_data, sh_addr.sh_name, &shstr_sh);
    //sprint("Section %d name: %s\n", i, shstrtab_data);
    if (strcmp(shstrtab_data, ".strtab") == 0) {
      sh_strtab = sh_addr;
    } else if (strcmp(shstrtab_data, ".symtab") == 0) {
      sh_symtab = sh_addr;
    }
  }
  uint64 symtab_size = sh_symtab.sh_size;
  uint64 symtab_offset = sh_symtab.sh_offset;
  uint64 symtab_entsize = sh_symtab.sh_entsize;
  uint64 strtab_size = sh_strtab.sh_size;
  uint64 strtab_offset = sh_strtab.sh_offset;
  uint64 sym_count = symtab_size / symtab_entsize;
  elf_symbol sym;
  for(int i=0; i<sym_count; i++) {
    uint64 sym_pos = symtab_offset + (uint64)i * symtab_entsize;
    elf_fpread(ctx, &sym, sizeof(sym), sym_pos);
    if ((sym.st_info & 0xf) != 2) continue; // only process function type symbol
    if (sym.st_size == 0) continue;
    char sym_name[MAX_SYMBOL_NAME_LEN];
    uint32 name_offset = sym.st_name;
    uint32 j = 0;
    while (j + name_offset < strtab_size) {
      elf_fpread(ctx, &sym_name[j], 1, strtab_offset + name_offset + j);
      if (sym_name[j] == '\0') break;
      j++;
    }
    sym_name[j] = '\0';
    elf_symbols[elf_symbol_count] = sym;
    mystrncpy(func_names[elf_symbol_count].name, sym_name, MAX_SYMBOL_NAME_LEN);
    func_names[elf_symbol_count].addr = sym.st_value;
    elf_symbol_count++;
    if (elf_symbol_count >= MAX_ELF_SYMBOLS) break;
  }
  for(int i=0; i<elf_symbol_count; i++) {
    sprint("Function %d: name=%s, addr=0x%lx\n", i,
      func_names[i].name, func_names[i].addr);
  }
}

//
// load the elf of user application, by using the spike file interface.
//
void load_bincode_from_host_elf(process *p) {
  arg_buf arg_bug_msg;

  // retrieve command line arguements
  size_t argc = parse_args(&arg_bug_msg);
  if (!argc) panic("You need to specify the application program!\n");

  sprint("Application: %s\n", arg_bug_msg.argv[0]);

  //elf loading. elf_ctx is defined in kernel/elf.h, used to track the loading process.
  elf_ctx elfloader;
  // elf_info is defined above, used to tie the elf file and its corresponding process.
  elf_info info;

  info.f = spike_file_open(arg_bug_msg.argv[0], O_RDONLY, 0);
  info.p = p;
  // IS_ERR_VALUE is a macro defined in spike_interface/spike_htif.h
  if (IS_ERR_VALUE(info.f)) panic("Fail on openning the input application program.\n");

  // init elfloader context. elf_init() is defined above.
  if (elf_init(&elfloader, &info) != EL_OK)
    panic("fail to init elfloader.\n");

  // load elf. elf_load() is defined above.
  if (elf_load(&elfloader) != EL_OK) panic("Fail on loading elf.\n");

  elf_load_symbol(&elfloader);

  // entry (virtual, also physical in lab1_x) address
  p->trapframe->epc = elfloader.ehdr.entry;

  // close the host spike file
  spike_file_close( info.f );

  sprint("Application program entry point (virtual address): 0x%lx\n", p->trapframe->epc);
}

// map function address to name
int f_addr_to_name(uint64 addr) {
  if (elf_symbol_count <= 0) return -1;
  
  int best = -1;
  uint64 best_addr = 0;

  for (int i = 0; i < elf_symbol_count; i++) {
    uint64 a = func_names[i].addr;
    uint64 size = elf_symbols[i].st_size;

    if (a == 0) continue;
    if (a > addr) continue;

    /* 选择距离 addr 最近且<=addr 的符号 */
    if (best == -1 || a > best_addr) {
      if (size > 0) {
        /* 如果符号有大小，优先确保 addr 在范围内 */
        if (addr < a + size) {
          best = i;
          best_addr = a;
        } else {
          /* 仍可作为备选（如果没有更好的匹配） */
          if (best == -1) {
            best = i;
            best_addr = a;
          }
        }
      } else {
        best = i;
        best_addr = a;
      }
    }
  }

  return best;
}
