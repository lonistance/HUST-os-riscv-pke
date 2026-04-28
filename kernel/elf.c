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

char debug_line_buff[MAX_SECTION_DATA_LEN * 2]; // double the buffer size to be safe
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

// leb128 (little-endian base 128) is a variable-length
// compression algoritm in DWARF
void read_uleb128(uint64 *out, char **off) {
    uint64 value = 0; int shift = 0; uint8 b;
    for (;;) {
        b = *(uint8 *)(*off); (*off)++;
        value |= ((uint64)b & 0x7F) << shift;
        shift += 7;
        if ((b & 0x80) == 0) break;
    }
    if (out) *out = value;
}
void read_sleb128(int64 *out, char **off) {
    int64 value = 0; int shift = 0; uint8 b;
    for (;;) {
        b = *(uint8 *)(*off); (*off)++;
        value |= ((uint64_t)b & 0x7F) << shift;
        shift += 7;
        if ((b & 0x80) == 0) break;
    }
    if (shift < 64 && (b & 0x40)) value |= -(1 << shift);
    if (out) *out = value;
}
// Since reading below types through pointer cast requires aligned address,
// so we can only read them byte by byte
void read_uint64(uint64 *out, char **off) {
    *out = 0;
    for (int i = 0; i < 8; i++) {
        *out |= (uint64)(**off) << (i << 3); (*off)++;
    }
}
void read_uint32(uint32 *out, char **off) {
    *out = 0;
    for (int i = 0; i < 4; i++) {
        *out |= (uint32)(**off) << (i << 3); (*off)++;
    }
}
void read_uint16(uint16 *out, char **off) {
    *out = 0;
    for (int i = 0; i < 2; i++) {
        *out |= (uint16)(**off) << (i << 3); (*off)++;
    }
}

/*
* analyzis the data in the debug_line section
*
* the function needs 3 parameters: elf context, data in the debug_line section
* and length of debug_line section
*
* make 3 arrays:
* "process->dir" stores all directory paths of code files
* "process->file" stores all code file names of code files and their directory path index of array "dir"
* "process->line" stores all relationships map instruction addresses to code line numbers
* and their code file name index of array "file"
*/
void make_addr_line(elf_ctx *ctx, char *debug_line, uint64 length) {
   process *p = ((elf_info *)ctx->info)->p;
    p->debugline = debug_line;
    // directory name char pointer array
    p->dir = (char **)((((uint64)debug_line + length + 7) >> 3) << 3); int dir_ind = 0, dir_base;
    // file name char pointer array
    p->file = (code_file *)(p->dir + 64); int file_ind = 0, file_base;
    // table array
    p->line = (addr_line *)(p->file + 64); p->line_ind = 0;
    char *off = debug_line;
    while (off < debug_line + length) { // iterate each compilation unit(CU)
        debug_header *dh = (debug_header *)off; off += sizeof(debug_header);
        dir_base = dir_ind; file_base = file_ind;
        // get directory name char pointer in this CU
        while (*off != 0) {
            p->dir[dir_ind++] = off; while (*off != 0) off++; off++;
        }
        off++;
        // get file name char pointer in this CU
        while (*off != 0) {
            p->file[file_ind].file = off; while (*off != 0) off++; off++;
            uint64 dir; read_uleb128(&dir, &off);
            p->file[file_ind++].dir = dir - 1 + dir_base;
            read_uleb128(NULL, &off); read_uleb128(NULL, &off);
        }
        off++; addr_line regs; regs.addr = 0; regs.file = 1; regs.line = 1;
        // simulate the state machine op code
        for (;;) {
            uint8 op = *(off++);
            switch (op) {
                case 0: // Extended Opcodes
                    read_uleb128(NULL, &off); op = *(off++);
                    switch (op) {
                        case 1: // DW_LNE_end_sequence
                            if (p->line_ind > 0 && p->line[p->line_ind - 1].addr == regs.addr) p->line_ind--;
                            p->line[p->line_ind] = regs; p->line[p->line_ind].file += file_base - 1;
                            p->line_ind++; goto endop;
                        case 2: // DW_LNE_set_address
                            read_uint64(&regs.addr, &off); break;
                        // ignore DW_LNE_define_file
                        case 4: // DW_LNE_set_discriminator
                            read_uleb128(NULL, &off); break;
                    }
                    break;
                case 1: // DW_LNS_copy
                    if (p->line_ind > 0 && p->line[p->line_ind - 1].addr == regs.addr) p->line_ind--;
                    p->line[p->line_ind] = regs; p->line[p->line_ind].file += file_base - 1;
                    p->line_ind++; break;
                case 2: { // DW_LNS_advance_pc
                            uint64 delta; read_uleb128(&delta, &off);
                            regs.addr += delta * dh->min_instruction_length;
                            break;
                        }
                case 3: { // DW_LNS_advance_line
                            int64 delta; read_sleb128(&delta, &off);
                            regs.line += delta; break; } case 4: // DW_LNS_set_file
                        read_uleb128(&regs.file, &off); break;
                case 5: // DW_LNS_set_column
                        read_uleb128(NULL, &off); break;
                case 6: // DW_LNS_negate_stmt
                case 7: // DW_LNS_set_basic_block
                        break;
                case 8: { // DW_LNS_const_add_pc
                            int adjust = 255 - dh->opcode_base;
                            int delta = (adjust / dh->line_range) * dh->min_instruction_length;
                            regs.addr += delta; break;
                        }
                case 9: { // DW_LNS_fixed_advanced_pc
                            uint16 delta; read_uint16(&delta, &off);
                            regs.addr += delta;
                            break;
                        }
                        // ignore 10, 11 and 12
                default: { // Special Opcodes
                             int adjust = op - dh->opcode_base;
                             int addr_delta = (adjust / dh->line_range) * dh->min_instruction_length;
                             int line_delta = dh->line_base + (adjust % dh->line_range);
                             regs.addr += addr_delta;
                             regs.line += line_delta;
                             if (p->line_ind > 0 && p->line[p->line_ind - 1].addr == regs.addr) p->line_ind--;
                             p->line[p->line_ind] = regs; p->line[p->line_ind].file += file_base - 1;
                             p->line_ind++; break;
                         }
            }
        }
endop:;
    }
    //for (int i = 0; i < p->line_ind; i++)
    //    sprint("%p %d %d\n", p->line[i].addr, p->line[i].line, p->line[i].file);
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
  uint64 shstrtab_data_size = shstr_sh->size;
  uint64 shstrtab_data_offset = shstr_sh->offset;
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

//
//load .debug_line section and make the address-line number-file name table for later debug use
//
void elf_load_debug_info(elf_ctx *ctx) {
  elf_header *ehdr = &ctx->ehdr;
  uint64 shoff = ehdr->shoff;
  uint16 shentsize = ehdr->shentsize;
  uint16 shnum = ehdr->shnum; 
  uint16 shstrndx = ehdr->shstrndx;
  elf_sect_header esh_debug_line;   //debug line section
  elf_sect_header shstr_esh;        // section header string table section
  uint64 shstr_pos = shoff + (uint64)shstrndx * shentsize;
  elf_fpread(ctx, &shstr_esh, sizeof(shstr_esh), shstr_pos);
  char shstrtab_data[MAX_ELFSH_NAME_LEN];
  uint64 sh_pos;
  for (int i = 0; i < shnum; i++) {
    sh_pos = shoff + (uint64)i * shentsize;
    elf_fpread(ctx, &esh_debug_line, sizeof(esh_debug_line), sh_pos);
    elf_read_sect_name(ctx, shstrtab_data, esh_debug_line.name, &shstr_esh);
    sprint("Section %d name: %s\n", i, shstrtab_data);
    if (strcmp(shstrtab_data, ".debug_line") == 0) 
      break;
  }
  elf_fpread(ctx, debug_line_buff, esh_debug_line.size, esh_debug_line.offset);
  make_addr_line(ctx, debug_line_buff, esh_debug_line.size);
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

  // entry to laod .debug_line section and make the address-line number-file name table for later debug use
  elf_load_debug_info(&elfloader);

  // entry (virtual, also physical in lab1_x) address
  p->trapframe->epc = elfloader.ehdr.entry;

  // added @lab1_challenge2: load .debug_line section for source line information
  if (elfloader.ehdr.shoff != 0 && elfloader.ehdr.shstrndx != 0) {
    elf_sect_header shstrhdr;
    uint64 shstr_offset = elfloader.ehdr.shoff + elfloader.ehdr.shstrndx * sizeof(shstrhdr);
    if (elf_fpread(&elfloader, &shstrhdr, sizeof(shstrhdr), shstr_offset) == sizeof(shstrhdr)) {
      char *tmp_buf = (char *)0x81400000;
      if (shstrhdr.size > 0 && shstrhdr.size <= 0x10000) {
        elf_fpread(&elfloader, tmp_buf, shstrhdr.size, shstrhdr.offset);

        for (int i = 0; i < elfloader.ehdr.shnum; i++) {
          elf_sect_header sh;
          uint64 sh_offset = elfloader.ehdr.shoff + i * sizeof(sh);
          if (elf_fpread(&elfloader, &sh, sizeof(sh), sh_offset) == sizeof(sh)) {
            if (strcmp(tmp_buf + sh.name, ".debug_line") == 0) {
              elf_fpread(&elfloader, tmp_buf, sh.size, sh.offset);
              make_addr_line(&elfloader, tmp_buf, sh.size);
              break;
            }
          }
        }
      }
    }
  }

  // close the host spike file
  spike_file_close( info.f );

  sprint("Application program entry point (virtual address): 0x%lx\n", p->trapframe->epc);
}

//
// added @lab1_challenge2: print the source code line text
//
void print_source_line(char *dir, char *file, int line) {
  char path[256];
  int i = 0;
  while (*dir && i < 255) path[i++] = *dir++;
  if (i < 255) path[i++] = '/';
  while (*file && i < 255) path[i++] = *file++;
  path[i] = '\0';

  spike_file_t *f = spike_file_open(path, O_RDONLY, 0);
  if (IS_ERR_VALUE(f)) return;

  char *buf = (char *)0x81500000;
  ssize_t total = 0;
  ssize_t n;
  while ((n = spike_file_read(f, buf + total, 0x10000 - total)) > 0) {
    total += n;
    if (total >= 0x10000) break;
  }
  spike_file_close(f);

  if (total == 0) return;
  buf[total] = '\0';

  int current_line = 1;
  char *line_start = buf;
  for (int j = 0; j < total; j++) {
    if (buf[j] == '\n') {
      if (current_line == line) {
        char line_buf[256];
        int len = &buf[j] - line_start;
        if (len > 255) len = 255;
        memcpy(line_buf, line_start, len);
        line_buf[len] = '\0';
        sprint("%s\n", line_buf);
        return;
      }
      current_line++;
      line_start = &buf[j + 1];
    }
  }
}
