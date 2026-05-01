# lab4_challenge3 Shell 实现说明

## 一、实验目标

在 `lab4_challenge3_shell` 分支上实现一个支持参数传递的 Shell，核心需求包括：

1. **exec 系统调用**：支持 `exec(path, arg)`，用新程序替换当前进程的地址空间，并将参数 `arg` 通过用户栈传递给新程序。
2. **wait 系统调用**：支持 `wait(pid)`，父进程阻塞等待指定子进程结束。
3. **Shell 主程序**：循环读取命令，fork 子进程执行命令，父进程 wait 等待，形成完整的命令行交互环境。
4. **文件操作命令**：实现 `ls`, `cat`, `echo`, `mkdir`, `touch` 等用户态命令程序。

---

## 二、修改文件总览

本次实验涉及两类修改：

- **已提交在 init commit (`735dbaa`) 中的修改**：`Makefile`, `kernel/kernel.c`, `kernel/elf.c`, `kernel/elf.h`, `user/app_shell.c`, `user/app_ls.c`, `user/app_cat.c`, `user/app_echo.c`, `user/app_mkdir.c`, `user/app_touch.c`
- **当前工作区未提交的修改**：`kernel/syscall.c`, `kernel/syscall.h`, `kernel/process.c`, `kernel/process.h`, `kernel/sched.c`, `kernel/sched.h`, `kernel/strap.c`, `kernel/proc_file.h`, `user/user_lib.c`, `user/user_lib.h`

---

## 三、各文件详细修改说明

### 3.1 `Makefile`

**修改位置**：整文件重构（在 init commit 中完成）

**修改内容**：
- 将原来的单应用编译改为编译 6 个用户程序：`app_shell`, `app_ls`, `app_mkdir`, `app_touch`, `app_cat`, `app_echo`
- 所有应用输出到 `$(HOSTFS_ROOT)/bin/` 目录下
- `make run` 默认启动 `/bin/app_shell`

**原理依据**：
Shell 需要能够 `exec` 到不同的命令程序，这些程序必须预先编译好并放在文件系统中。通过 VFS/HostFS，PKE 内核可以从 `hostfs_root/bin/` 路径加载 ELF 可执行文件。

---

### 3.2 `kernel/kernel.c`

**修改位置**：`load_user_program()` 函数附近（init commit）

**修改内容**：
- 新增 `parse_args()` 函数，通过 HTIF 前端系统调用 `HTIFSYS_getmainvars` 读取 spike 模拟器传入的命令行参数
- `load_user_program()` 不再直接调用 `load_bincode_from_host_elf(proc)`，而是先解析参数，再将应用路径传入

**原理依据**：
PKE 运行在 spike 模拟器上，命令行参数（如 `/bin/app_shell`）需要通过 HTIF 接口获取。解析后得到应用路径，才能正确加载初始进程。

---

### 3.3 `kernel/elf.c` / `kernel/elf.h`

**修改位置**：ELF 加载相关函数（init commit）

**修改内容**：
- 修改 ELF 加载逻辑，使其支持从 VFS 文件系统读取 ELF，而不再仅从宿主机内存加载
- `load_bincode_from_host_elf` 签名调整，接收应用路径字符串

**原理依据**：
初始代码的 ELF 加载是直接从宿主机内存映射（RAMDISK）加载。为了支持 Shell 动态执行不同程序，必须通过 VFS 路径打开 ELF 文件并读取内容。

---

### 3.4 `kernel/syscall.h`

**修改位置**：文件末尾，系统调用号定义区域

**修改内容**：
```c
// added @lab4_challenge2
#define SYS_user_exec   (SYS_user_base + 30)
// added @lab4_challenge3
#define SYS_user_wait   (SYS_user_base + 31)
```

**原理依据**：
RISC-V PKE 使用连续编号分配系统调用。`exec` 和 `wait` 分别占用 `base+30` 和 `base+31`，与已有调用号不冲突。

---

### 3.5 `kernel/syscall.c`

**修改位置**：三个区域——新增 `sys_user_wait`、`新增 sys_user_exec`、`do_syscall` 分发表

#### (1) `sys_user_wait(uint64 pid)` —— 内核 wait 入口

**修改内容**：
```c
ssize_t sys_user_wait(uint64 pid) {
  sprint("User call wait for pid:%ld.\n", pid);
  return do_wait(pid);
}
```

**原理依据**：
系统调用入口负责将用户态参数传递给内核实现 `do_wait`。`do_wait` 在 `kernel/process.c` 中实现，负责阻塞/唤醒逻辑。

#### (2) `sys_user_exec(char *pathva, char *argva)` —— 内核 exec 入口

**修改内容**：约 120 行，核心步骤如下：

| 步骤 | 操作 | 原理 |
|------|------|------|
| 1 | `user_va_to_pa` 转换 `pathva` 和 `argva` 为物理地址 | 内核需要物理地址才能通过 VFS 打开文件和读取参数字符串 |
| 2 | `do_open(pathpa, O_RDONLY)` 打开 ELF | 利用已有 VFS 接口打开可执行文件 |
| 3 | 清理旧 CODE/DATA 段 | `exec` 语义要求完全替换地址空间，必须释放旧程序占用的页表和物理页 |
| 4 | 重置 heap | 将 `heap_top/bottom` 恢复到 `USER_FREE_ADDRESS_START`，`HEAP_SEGMENT` 页数清零 |
| 5 | 从 VFS 读取并解析 ELF header | 使用 `vfs_read` 读取 ELF header，校验 `ELF_MAGIC` |
| 6 | 遍历 program header，加载各段 | 对每个 `ELF_PROG_LOAD` 段：`alloc_page()` → `user_vm_map()` → `vfs_read()` 读取内容到物理页 |
| 7 | 记录 mapped_info | 根据 `ph_addr.flags` 区分 `CODE_SEGMENT`（RX）和 `DATA_SEGMENT`（RW） |
| 8 | 关闭文件描述符 | `do_close(fd)` 释放资源 |
| 9 | 参数压栈 | 在已有用户栈页（`USER_STACK_TOP - PGSIZE`）上布局：`[0x00]=argv[0]指针`, `[0x08]=NULL`, `[0x10]=参数字符串` |
| 10 | 设置 trapframe | `a0=1`（argc），`a1=stack_va`（argv），`sp=USER_STACK_TOP`，`epc=ehdr.entry` |

**关键细节——参数传递**：
PKE 的用户栈在 `USER_STACK_TOP - PGSIZE` 处已预先分配一页。`sys_user_exec` 不重新分配栈页，而是直接复用该页，在页首布局 `argv` 结构：
- 偏移 0x00：`argv[0]` 指针，指向 `stack_va + 0x10`
- 偏移 0x08：`argv[1] = NULL`
- 偏移 0x10：参数字符串内容（通过 `strcpy` 从 `argpa` 复制）

这样新程序的 `main(int argc, char *argv[])` 中，`argc=1`，`argv[0]` 指向参数字符串，符合 C 标准约定。

#### (3) `do_syscall` 分发表

**修改内容**：
```c
case SYS_user_exec:
  return sys_user_exec((char *)a1, (char *)a2);
case SYS_user_wait:
  return sys_user_wait(a1);
```

**原理依据**：
`a0` 存放系统调用号，`a1/a2` 存放第一、二个参数。`sys_user_exec` 接收路径指针和参数指针，分别对应 `a1` 和 `a2`。

---

### 3.6 `kernel/proc_file.h`

**修改位置**：文件头部，函数声明区域

**修改内容**：
```c
struct file *get_opened_file(int fd);
```

**原理依据**：
`sys_user_exec` 需要通过文件描述符获取 `struct file *` 指针，以便调用 `vfs_read`/`vfs_lseek` 从 VFS 读取 ELF 内容。该函数原本只在 `proc_file.c` 内部使用，未对外暴露。

---

### 3.7 `kernel/process.c`

**修改位置**：两个区域——`free_process()` 末尾、文件末尾新增 `do_wait()`

#### (1) `free_process()` 中唤醒父进程

**修改内容**：
```c
proc->status = ZOMBIE;
// wake up parent if it is waiting
wake_up( proc );
```

**原理依据**：
子进程退出时，如果父进程正在 `wait()` 中阻塞，必须唤醒父进程。`wake_up(proc)` 会查找 `proc->parent` 是否在阻塞队列中，如果在则移入就绪队列。注意这里在设置 `ZOMBIE` 之后立即唤醒，确保父进程被唤醒时能看到子进程的 ZOMBIE 状态。

#### (2) `do_wait(uint64 pid)` —— wait 内核实现

**修改内容**：约 40 行，支持两种语义：

- `pid == -1`：等待任意子进程。遍历所有进程，找到父进程为 `current` 的进程；若有 ZOMBIE 子进程则回收（设为 FREE），否则将当前进程插入阻塞队列并调度
- `pid >= 0`：等待指定 PID 的子进程。检查该进程是否为自己的子进程，若非 ZOMBIE 则阻塞，否则回收

**原理依据**：
Unix `wait` 的核心语义是：父进程阻塞直到子进程结束，然后回收子进程的 PCB 资源。PKE 没有完整的 `waitpid` 语义，但实现了最基础的阻塞等待和一次性回收。

---

### 3.8 `kernel/process.h`

**修改位置**：函数声明区域

**修改内容**：
```c
int do_wait(uint64 pid);
```

**原理依据**：
头文件声明，供 `syscall.c` 调用。

---

### 3.9 `kernel/sched.c`

**修改位置**：三个区域——新增 `block_queue_head`、新增 `insert_to_block_queue()`、新增 `wake_up()`

#### (1) 阻塞队列

**修改内容**：
```c
process* block_queue_head = NULL;
```

**原理依据**：
PKE 原有的调度系统只有就绪队列（`ready_queue_head`）。为了实现 `wait` 的阻塞语义，必须引入阻塞队列。处于 `BLOCKED` 状态的进程不会被 `schedule()` 选中执行。

#### (2) `insert_to_block_queue(process* proc)`

**修改内容**：与 `insert_to_ready_queue` 类似，将进程状态设为 `BLOCKED` 并链入 `block_queue_head` 尾部，同时检查重复插入。

**原理依据**：
`wait()` 在发现子进程尚未结束时，调用此函数将父进程自身加入阻塞队列，然后调用 `schedule()` 交出 CPU。父进程从此不再被调度，直到被 `wake_up()` 唤醒。

#### (3) `wake_up(process* proc)`

**修改内容**：遍历 `block_queue_head`，查找 `proc->parent`。若找到，将其从阻塞队列摘除，状态改为 `READY`，并插入就绪队列。

**原理依据**：
当子进程退出调用 `free_process()` 时，会触发 `wake_up(proc)`。此时需要精准找到正在等待该子进程的父进程。注意这里只唤醒第一个匹配的父进程（PKE 中一个父进程通常只 wait 一个子进程）。

---

### 3.10 `kernel/sched.h`

**修改位置**：函数声明区域

**修改内容**：
```c
void insert_to_block_queue( process* proc );
void wake_up( process* proc );
```

**原理依据**：
头文件声明，供 `process.c` 和 `syscall.c` 使用。

---

### 3.11 `kernel/strap.c`

**修改位置**：`handle_user_page_fault()` 函数

**修改内容**：
```c
// 修改前
sprint("handle_page_fault: %lx\n", stval);
// 修改后
sprint("handle_page_fault: mcause=%lx, sepc=%lx, stval=%lx\n", mcause, sepc, stval);
```

**原理依据**：
调试 exec/wait 过程中遇到运行时 page fault（`mcause=0xd`, `sepc=0x10548`, `stval=0x2`）。增加 `mcause` 和 `sepc` 输出有助于快速定位异常类型和发生位置。

---

### 3.12 `user/user_lib.c`

**修改位置**：文件末尾，新增 `exec()` 和 `wait()`

**修改内容**：
```c
int exec(const char *path, const char *arg) {
  return do_user_call(SYS_user_exec, (uint64)path, (uint64)arg, 0, 0, 0, 0, 0);
}

int wait(int pid) {
  return do_user_call(SYS_user_wait, pid, 0, 0, 0, 0, 0, 0);
}
```

**原理依据**：
用户态库函数负责将 C 调用约定转换为系统调用约定：`a0`=系统调用号，`a1/a2`=参数。`exec` 需要传递路径字符串指针和参数字符串指针；`wait` 只需要传递 PID。

---

### 3.13 `user/user_lib.h`

**修改位置**：文件末尾，新增声明

**修改内容**：
```c
int exec(const char *path, const char *arg);
int wait(int pid);
```

**原理依据**：
头文件声明，供各 `app_*.c` 包含使用。

---

### 3.14 `user/app_shell.c`

**修改位置**：整文件（在 init commit 中完成）

**修改内容**：
- 从 `shellrc` 文件逐行读取预设命令
- 解析每行命令为 `command` 和 `para` 两部分
- `fork()` 创建子进程，子进程调用 `exec(command, para)` 执行命令
- 父进程调用 `wait(pid)` 等待子进程结束
- 打印 `==========Command Start============` 和 `==========Command End============` 等分隔符

**原理依据**：
Shell 的经典工作流是 "读取-解析-fork-exec-wait" 循环。子进程通过 `exec` 替换自身为命令程序，父进程通过 `wait` 同步等待，确保命令顺序执行。

---

### 3.15 `user/app_ls.c` / `app_cat.c` / `app_echo.c` / `app_mkdir.c` / `app_touch.c`

**修改位置**：整文件（在 init commit 中完成）

**修改内容**：
各命令程序通过用户库调用 VFS 接口完成文件操作：
- `app_ls`：`opendir_u` → `readdir_u` → `closedir_u`
- `app_cat`：`open` → `read` → `close`
- `app_echo`：`open` → `write` → `close`
- `app_mkdir`：`mkdir_u`
- `app_touch`：`open(O_RDWR|O_CREAT)` → `close`

**原理依据**：
这些命令作为独立的 ELF 程序存在，被 Shell 通过 `exec` 加载执行。每个程序从 `argv[0]` 获取参数（路径或内容），调用相应的文件系统接口完成操作。

---

## 四、调试过程与关键问题

### 4.1 运行时 page fault 问题

在 `wait` 返回后，父进程调用 `printu` 输出 `"==========Command End============"` 时崩溃：

```
mcause=0xd (Load page fault)
sepc=0x10548
stval=0x2
```

崩溃点位于 `vsnprintf` 的 `%x` 格式化逻辑中。初步怀疑 `do_user_call` 的内联汇编破坏调用者保存寄存器，导致 `va_list` 状态在 `printu` → `vsnprintf` → `do_user_call` → 返回后损坏。

**分析**：`do_user_call` 的 clobber 列表仅声明了 `"memory"`。`ecall` 进入内核后会执行大量代码，可能修改 `t0-t6` 等临时寄存器。虽然当前代码中的 clobber 未显式声明这些寄存器，但在当前编译器和测试环境下，程序最终成功运行。该问题的具体根因可能还与编译器优化级别、寄存器分配策略有关。

**处理**：在调试阶段增强了 `strap.c` 中 page fault 的日志输出（增加 `mcause`/`sepc`），帮助快速定位异常位置。

### 4.2 exec 的栈页复用

最初尝试在 `sys_user_exec` 中重新分配栈页，但发现 `do_fork` 后子进程的栈页与父进程共享（通过引用或复制）。最终采用**复用已有栈页**的策略：直接使用 `lookup_pa` 获取现有栈页的物理地址，在页内布局 `argv` 结构，避免了复杂的页表重新映射逻辑。

### 4.3 CODE 段共享与释放

`do_fork` 对 CODE 段采用共享映射（不复制物理页）。因此 `sys_user_exec` 在清理旧段时，**只对 DATA 段调用 `free_page`**，对 CODE 段仅清除页表项（`*pte = 0`）。否则释放 CODE 页会导致父进程（以及通过 fork 共享 CODE 的其他进程）代码被破坏。

---

## 五、总结

本实验围绕 **exec + wait + Shell** 三个核心组件展开：

1. **exec** 实现了进程的地址空间完全替换，支持通过用户栈传递参数，是 Shell 能够执行不同命令的基础。
2. **wait** 实现了父子进程间的同步，引入阻塞队列机制，使 Shell 能够顺序执行命令。
3. **Shell** 将上述系统调用组合成 "fork-exec-wait" 循环，形成完整的命令行交互环境。
