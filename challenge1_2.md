# PKE Lab1 Challenge2 修改分析文档

## 一、实验目标

修改 PKE 内核，使得用户程序在发生异常时，内核能够输出：
1. 触发异常的用户程序的**源文件名和对应代码行号**
2. 该行对应的**源代码文本**

输出格式示例：
```
Runtime error at user/app_errorline.c:13
  asm volatile("csrw sscratch, 0");
Illegal instruction!
System is shutting down with exit code -1.
```

## 二、核心思路概述

实验的核心在于利用 ELF 文件中的 **`.debug_line` 段**（DWARF 调试信息）建立"指令地址 → 源文件行号"的映射关系。整体分为三个阶段：

1. **加载阶段**：在用户程序加载时，读取 ELF 的 `.debug_line` 段，调用 `make_addr_line` 解析出 `dir`、`file`、`line` 三个数组
2. **异常处理阶段**：当异常发生时，根据异常地址（`sepc` 或 `mepc`）在 `line` 数组中二分查找对应的源文件和行号
3. **源码输出阶段**：通过 Spike 文件接口打开源文件，读取并输出对应行的源代码文本

---

## 三、修改文件与详细说明

### 3.1 `kernel/elf.c`

#### 3.1.1 读取 `.debug_line` Section（`load_bincode_from_host_elf` 函数中）

**位置**：`load_bincode_from_host_elf` 函数末尾，`spike_file_close` 之前

**代码逻辑**：
```c
if (elfloader.ehdr.shoff != 0 && elfloader.ehdr.shstrndx != 0) {
    // 1. 读取 Section Name String Table 的 header
    elf_sect_header shstrhdr;
    uint64 shstr_offset = elfloader.ehdr.shoff + elfloader.ehdr.shstrndx * sizeof(shstrhdr);
    if (elf_fpread(&elfloader, &shstrhdr, sizeof(shstrhdr), shstr_offset) == sizeof(shstrhdr)) {
        char *tmp_buf = (char *)0x81400000;
        if (shstrhdr.size > 0 && shstrhdr.size <= 0x10000) {
            // 2. 读取 string table 到临时缓冲区
            elf_fpread(&elfloader, tmp_buf, shstrhdr.size, shstrhdr.offset);
            // 3. 遍历所有 section header，寻找 .debug_line
            for (int i = 0; i < elfloader.ehdr.shnum; i++) {
                elf_sect_header sh;
                uint64 sh_offset = elfloader.ehdr.shoff + i * sizeof(sh);
                if (elf_fpread(&elfloader, &sh, sizeof(sh), sh_offset) == sizeof(sh)) {
                    if (strcmp(tmp_buf + sh.name, ".debug_line") == 0) {
                        // 4. 读取 .debug_line 数据到缓冲区，调用 make_addr_line 解析
                        elf_fpread(&elfloader, tmp_buf, sh.size, sh.offset);
                        make_addr_line(&elfloader, tmp_buf, sh.size);
                        break;
                    }
                }
            }
        }
    }
}
```

**工作原理解析**：
- ELF 文件的 Section Header Table 起始偏移为 `ehdr.shoff`，共 `shnum` 个条目，每个大小为 `shentsize`
- Section 名称存放在 **Section Header String Table** 中，该表的索引由 `ehdr.shstrndx` 给出
- 先读取 string table 到临时缓冲区 `0x81400000`，然后遍历所有 section header，通过 `strcmp(tmp_buf + sh.name, ".debug_line")` 匹配名称
- 找到 `.debug_line` 段后，将其数据读到同一缓冲区（覆盖 string table），调用 `make_addr_line` 解析

**内存地址选择**：
- 使用固定地址 `0x81400000` 作为 `.debug_line` 数据缓冲区
- 原因：用户程序加载在 `0x81000000` 附近，`USER_STACK=0x81100000`，`USER_KSTACK=0x81200000`，`USER_TRAP_FRAME=0x81300000`。`0x81400000` 处于空闲区域，安全可用
- `make_addr_line` 会将 `dir`（64 个 `char*`）、`file`（64 个 `code_file`）、`line`（变长数组）依次放置在 `debug_line` 数据之后，因此缓冲区后需预留足够空间

#### 3.1.2 新增 `print_source_line` 函数

**位置**：`elf.c` 文件末尾，`load_bincode_from_host_elf` 函数之后

**函数签名**：`void print_source_line(char *dir, char *file, int line)`

**功能**：通过 Spike HTIF 文件接口读取源文件，输出指定行号的源代码文本

**工作流程**：
1. **路径拼接**：将 `dir` 和 `file` 拼接为 `dir/file` 格式的完整路径（存放到 256 字节局部数组 `path` 中）
2. **打开文件**：调用 `spike_file_open(path, O_RDONLY, 0)` 打开 host 文件系统中的源文件
3. **读取内容**：循环调用 `spike_file_read`，将文件内容读取到固定地址 `0x81500000` 的缓冲区（最多 64KB）
4. **按行查找**：遍历缓冲区，以 `\n` 为分隔符计数行号，找到目标行后：
   - 将该行内容复制到 `line_buf`（最多 255 字节）
   - 调用 `sprint("%s\n", line_buf)` 输出（保留源代码原有的缩进空格）
5. **关闭文件**：调用 `spike_file_close` 释放文件描述符

**关键设计**：
- 使用 `0x81500000` 作为文件读取缓冲区，与 `.debug_line` 的 `0x81400000` 分开，避免相互覆盖
- 如果文件不存在或无法打开（如某些系统头文件路径在 host 上不可访问），函数静默返回，不影响异常处理流程

---

### 3.2 `kernel/elf.h`

**修改内容**：在文件末尾添加 `print_source_line` 的函数声明

```c
void print_source_line(char *dir, char *file, int line);
```

**目的**：使 `strap.c` 和 `mtrap.c` 能够调用该函数，避免重复定义

---

### 3.3 `kernel/strap.c`

#### 3.3.1 新增头文件包含

```c
#include "elf.h"
```

用于访问 `print_source_line` 的函数声明。

#### 3.3.2 新增 `print_error_line` 函数

**位置**：`handle_mtimer_trap` 函数之后

**函数签名**：`static void print_error_line(uint64 addr)`

**功能**：根据异常地址查找并输出源文件信息

**工作流程**：
1. **安全检查**：确认 `current`、`current->line`、`current->line_ind` 均有效
2. **二分查找**：在 `current->line` 数组中查找不大于 `addr` 的最大地址条目
   - `line` 数组按地址升序排列（由 `make_addr_line` 保证）
   - 使用标准二分查找，时间复杂度 O(log n)
3. **信息输出**：
   - 通过 `line[ans].file` 索引到 `file` 数组，获取文件名和目录索引
   - 通过 `file.dir` 索引到 `dir` 数组，获取目录路径
   - 输出：`Runtime error at <dir>/<file>:<line>`
   - 调用 `print_source_line(dir, cf->file, line)` 输出源代码文本

**示例映射关系**：
- 对于 `app_errorline.c` 第 13 行的 `asm volatile(...)` 指令：
  - `line` 数组中存在 `addr=0x81000010, line=13, file=0`
  - `file[0] = {dir=0, file="app_errorline.c"}`
  - `dir[0] = "user"`
  - 最终输出：`Runtime error at user/app_errorline.c:13`

#### 3.3.3 在 `smode_trap_handler` 中调用

**位置**：`else` 分支（非系统调用、非定时器异常）

```c
} else {
    print_error_line(read_csr(sepc));
    sprint("smode_trap_handler(): unexpected scause %p\n", read_csr(scause));
    sprint("            sepc=%p stval=%p\n", read_csr(sepc), read_csr(stval));
    panic( "unexpected exception happened.\n" );
}
```

**说明**：
- S-mode 异常的异常地址保存在 `sepc` 寄存器中
- 被 `delegate_traps()` 委托到 S-mode 的异常包括：page fault、breakpoint、misaligned fetch、user ecall 等

---

### 3.4 `kernel/machine/mtrap.c`

#### 3.4.1 新增头文件包含

```c
#include "kernel/elf.h"
```

#### 3.4.2 新增 `print_error_line_mtrap` 函数

**位置**：文件开头，各异常处理函数之前

**函数签名**：`static void print_error_line_mtrap()`

**与 `print_error_line` 的区别**：
- M-mode 异常的异常地址保存在 `mepc` 寄存器中，而非传入参数
- 其余逻辑（二分查找、信息输出、源码打印）完全相同

#### 3.4.3 修改各 M-mode 异常处理函数

修改的函数列表：
- `handle_instruction_access_fault`
- `handle_load_access_fault`
- `handle_store_access_fault`
- `handle_illegal_instruction`
- `handle_misaligned_load`
- `handle_misaligned_store`
- `handle_mtrap` 的 `default` 分支

**修改方式**：在每个函数的 `panic` 调用之前插入 `print_error_line_mtrap()`

**示例**：
```c
static void handle_illegal_instruction() {
    print_error_line_mtrap();
    panic("Illegal instruction!");
}
```

**为什么需要修改 M-mode**：
- `delegate_traps()` 在 `kernel/machine/minit.c` 中只委托了部分异常到 S-mode
- **未委托**的异常（如 Illegal Instruction、Access Fault、Misaligned Load/Store）会在 M-mode 触发
- `app_errorline.c` 中的 `csrw sscratch, 0` 正是触发 **Illegal Instruction**，在 M-mode 被捕获
- 因此 M-mode 和 S-mode 的异常处理都需要支持源码定位

---

## 四、关键数据结构

### 4.1 `process` 结构体（`kernel/process.h`）

```c
typedef struct process_t {
    uint64 kstack;
    trapframe* trapframe;
    char *debugline;      // .debug_line 原始数据指针
    char **dir;           // 目录路径指针数组（最多 64 项）
    code_file *file;      // 文件信息数组（最多 64 项）
    addr_line *line;      // 地址-行号-文件索引映射数组（变长）
    int line_ind;         // line 数组的有效条目数
} process;
```

### 4.2 `code_file` 结构体

```c
typedef struct {
    uint64 dir;   // 在 dir 数组中的索引
    char *file;   // 文件名字符串指针（指向 .debug_line 段内部）
} code_file;
```

### 4.3 `addr_line` 结构体

```c
typedef struct {
    uint64 addr;  // 指令地址
    uint64 line;  // 源代码行号
    uint64 file;  // 在 file 数组中的索引
} addr_line;
```

**内存布局**（由 `make_addr_line` 构造）：
```
0x81400000:  [.debug_line raw data ...]
              [padding to 8-byte align]
              [dir[0..63] array]
              [file[0..63] array]
              [line[0..n] array (grows dynamically)]
```

---

## 五、完整工作流程

### 阶段 1：用户程序加载

1. `kernel/kernel.c` 中的 `load_user_program` 调用 `load_bincode_from_host_elf`
2. `load_bincode_from_host_elf` 通过 `elf_load` 加载程序的代码段和数据段
3. **新增**：读取 `.debug_line` section，调用 `make_addr_line` 解析 DWARF 数据
4. 解析完成后，`current->dir`、`current->file`、`current->line` 指向有效数组

### 阶段 2：用户程序执行异常

以 `app_errorline.c` 的非法指令为例：

1. U-mode 执行 `csrw sscratch, 0`（地址 `0x81000010`）
2. 处理器检测到非法指令，触发异常
3. `delegate_traps()` 未委托 `CAUSE_ILLEGAL_INSTRUCTION`，进入 **M-mode** `handle_mtrap`
4. `handle_mtrap` 调用 `handle_illegal_instruction`
5. `handle_illegal_instruction` 先调用 `print_error_line_mtrap()`

### 阶段 3：输出源码信息

1. `print_error_line_mtrap` 读取 `mepc = 0x81000010`
2. 在 `line` 数组中二分查找，找到 `addr <= 0x81000010` 的最大条目 → `line[2]`（addr=0x81000010, line=13, file=0）
3. 通过 `file[0]` 得到 `dir=0, file="app_errorline.c"`
4. 通过 `dir[0]` 得到 `"user"`
5. 输出：`Runtime error at user/app_errorline.c:13`
6. 调用 `print_source_line("user", "app_errorline.c", 13)`
7. `print_source_line` 打开 `user/app_errorline.c`，读取第 13 行
8. 输出：`  asm volatile("csrw sscratch, 0");`
9. 最后输出 panic 信息：`Illegal instruction!`

---

## 六、设计决策说明

### 6.1 为什么使用固定内存地址？

PKE 在 Lab1 处于 Bare Mode（无虚拟内存），且内核没有 `malloc`/`free` 等动态内存分配机制。因此使用固定物理地址是最简单可靠的方式：
- `0x81400000`：`.debug_line` 数据 + 解析后的数组
- `0x81500000`：源文件读取缓冲区（64KB）

### 6.2 为什么需要同时修改 M-mode 和 S-mode？

查看 `kernel/machine/minit.c` 中的 `delegate_traps()`：
```c
uintptr_t exceptions = (1U << CAUSE_MISALIGNED_FETCH) |
                       (1U << CAUSE_FETCH_PAGE_FAULT) |
                       (1U << CAUSE_BREAKPOINT) |
                       (1U << CAUSE_LOAD_PAGE_FAULT) |
                       (1U << CAUSE_STORE_PAGE_FAULT) |
                       (1U << CAUSE_USER_ECALL);
```

只有上述异常被委托到 S-mode。**Illegal Instruction、Access Fault、Misaligned Load/Store** 等异常仍由 M-mode 处理。因此两个模式都必须支持源码定位。

### 6.3 二分查找的合理性

`make_addr_line` 按指令地址递增顺序填充 `line` 数组，因此数组天然有序。二分查找 O(log n) 的效率远高于线性扫描 O(n)。

### 6.4 源文件读取失败的处理

`print_source_line` 在 `spike_file_open` 失败时直接返回。这是因为：
- 某些标准库头文件可能不在 host 文件系统的对应路径上
- 即使无法读取源码文本，行号信息仍然已输出，不影响核心功能

---

## 七、文件修改统计

| 文件 | 修改内容 |
|------|----------|
| `kernel/elf.c` | 加载 `.debug_line` 段；新增 `print_source_line` 函数 |
| `kernel/elf.h` | 声明 `print_source_line` 函数 |
| `kernel/strap.c` | 新增 `print_error_line`；在 `smode_trap_handler` 异常分支中调用 |
| `kernel/machine/mtrap.c` | 新增 `print_error_line_mtrap`；在所有 M-mode 异常处理函数中调用 |
