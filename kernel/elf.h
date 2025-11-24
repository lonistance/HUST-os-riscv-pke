#ifndef _ELF_H_
#define _ELF_H_

#include "util/types.h"
#include "process.h"

#define MAX_CMDLINE_ARGS 64
#define MAX_ELF_SYMBOLS 256
#define MAX_SYMBOL_NAME_LEN 32
#define MAX_SECTION_DATA_LEN 4096

// elf header structure
typedef struct elf_header_t {
  uint32 magic;
  uint8 elf[12];
  uint16 type;      /* Object file type */
  uint16 machine;   /* Architecture */
  uint32 version;   /* Object file version */
  uint64 entry;     /* Entry point virtual address */
  uint64 phoff;     /* Program header table file offset */
  uint64 shoff;     /* Section header table file offset */
  uint32 flags;     /* Processor-specific flags */
  uint16 ehsize;    /* ELF header size in bytes */
  uint16 phentsize; /* Program header table entry size */
  uint16 phnum;     /* Program header table entry count */
  uint16 shentsize; /* Section header table entry size */
  uint16 shnum;     /* Section header table entry count */
  uint16 shstrndx;  /* Section header string table index */
} elf_header;

// Program segment header.
typedef struct elf_prog_header_t {
  uint32 type;   /* Segment type */
  uint32 flags;  /* Segment flags */
  uint64 off;    /* Segment file offset */
  uint64 vaddr;  /* Segment virtual address */
  uint64 paddr;  /* Segment physical address */
  uint64 filesz; /* Segment size in file */
  uint64 memsz;  /* Segment size in memory */
  uint64 align;  /* Segment alignment */
} elf_prog_header;

/* ELF64 Section header (section table entry) */
typedef struct elf_sect_header_t {
  uint32 sh_name;      /* section name (string tbl index) */
  uint32 sh_type;      /* section type */
  uint64 sh_flags;     /* section flags */
  uint64 sh_addr;      /* virtual address in memory */
  uint64 sh_offset;    /* offset in file */
  uint64 sh_size;      /* size in bytes */
  uint32 sh_link;      /* link to another section */
  uint32 sh_info;      /* additional section information */
  uint64 sh_addralign; /* section alignment */
  uint64 sh_entsize;   /* entry size if section holds a table */
} elf_sect_header;

/* ELF64 Symbol table entry (Elf64_Sym) */
typedef struct elf_symbol_t {
  uint32 st_name;   /* symbol name (string tbl index) */
  uint8  st_info;   /* type and binding attributes */
  uint8  st_other;  /* no defined meaning, 0 */
  uint16 st_shndx;  /* section index */
  uint64 st_value;  /* symbol value (address) */
  uint64 st_size;   /* symbol size */
} elf_symbol;

#define ELF_MAGIC 0x464C457FU  // "\x7FELF" in little endian
#define ELF_PROG_LOAD 1

typedef enum elf_status_t {
  EL_OK = 0,

  EL_EIO,
  EL_ENOMEM,
  EL_NOTELF,
  EL_ERR,

} elf_status;

typedef struct elf_ctx_t {
  void *info;
  elf_header ehdr;
} elf_ctx;

typedef struct f_name_addr_t {
  char name[MAX_SYMBOL_NAME_LEN];
  uint64 addr;
} f_name_addr;

extern elf_symbol elf_symbols[MAX_ELF_SYMBOLS];
extern int elf_symbol_count;
extern f_name_addr func_names[MAX_ELF_SYMBOLS];

elf_status elf_init(elf_ctx *ctx, void *info);
elf_status elf_load(elf_ctx *ctx);

void elf_load_symbol(elf_ctx *ctx);
void load_bincode_from_host_elf(process *p);
int f_addr_to_name(uint64 addr);

#endif
