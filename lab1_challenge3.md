# Lab1 Challenge3：riscv-pke 双核并发启动实验报告

## 1. 实验目标

将单核 riscv-pke 操作系统扩展为支持 **2 个 HART（硬件线程，即 CPU）并发运行**：
- **Hart 0** 执行 `app0`（加载地址 `0x81000000`）
- **Hart 1** 执行 `app1`（加载地址 `0x85000000`）
- 通过 `spike -p2` 启动两核，所有内核 `sprint` 输出需带 `hartid` 前缀
- 所有核执行完毕后，由 **hart 0** 负责关闭模拟器

约束：仅修改 `kernel/` 目录下的现有文件，不允许新建代码文件。

---

## 2. 整体分析思路

单核代码直接搬到多核环境会遇到以下 6 类问题，需要逐一解决：

| 问题类别 | 具体表现 | 解决思路 |
|---------|---------|---------|
| **启动同步** | `spike_file_init()`、`init_dtb()` 等 HTIF 初始化只能执行一次 | 仅 hart 0 执行，hart 1 自旋等待同步标志 |
| **内存隔离** | 两核的 trapframe、kstack、user stack 若共用同一地址会相互覆盖 | 按 `hartid` 做固定地址偏移，各核独立 |
| **进程管理** | 全局单变量 `current` 无法区分当前是哪个核在运行 | 改为按核数组 `currents[NCPU]`，通过宏按 `hartid` 索引 |
| **时钟中断** | 全局 `g_ticks` 导致两核共享计时，互相干扰 | 改为按核数组 `g_ticks[NCPU]`，各核独立计数 |
| **用户态 hartid 丢失** | `return_to_user` 会从 trapframe 恢复所有寄存器，包括 `tp`；首次进入用户态时 `tp=0`，导致内核 `read_tp()` 无法识别核号 | 在 `load_user_program` 时将 `trapframe->regs.tp` 初始化为 `hartid` |
| **退出同步** | 单核 `exit` 直接 `shutdown`，多核下会强制终止其他核 | 引入完成计数器，hart 0 等待所有核完成后再 `shutdown` |
| **syscall 传参错误** | 代码仓库中某次错误提交将 `handle_syscall` 的 syscall number 从 `a0` 改为了 `a7`，与用户库实际传参不一致 | 恢复为 `a0` 作为 syscall number |

---

## 3. 分文件修改说明

### 3.1 `kernel/machine/minit.c` —— 多核启动与 M-mode 中断上下文隔离

#### 修改点 1：`g_itrframe` 改为按核数组
```c
// 修改前
riscv_regs g_itrframe;

// 修改后
riscv_regs g_itrframe[NCPU];
```

**原理**：M-mode 发生中断时，`mtrap_vector.S` 会把寄存器保存到 `mscratch` 指向的 `g_itrframe`。单核时只需一份；多核下若两核同时进入 M-mode trap，会互相覆盖上下文。改为数组后，每个核有独立的保存区。

#### 修改点 2：HTIF / spike 设备初始化仅执行一次
```c
static volatile int init_done = 0;

void m_start(uintptr_t hartid, uintptr_t dtb) {
  if (hartid == 0) {
    spike_file_init();
    init_dtb(dtb);
    __sync_synchronize();
    init_done = 1;
  } else {
    while (!init_done) {
      // spin wait for hart 0 to finish initialization
    }
    __sync_synchronize();
  }
  sprint("In m_start, hartid:%d\n", hartid);
  // ... 其余代码每个核都执行
}
```

**原理**：
- `spike_file_init()` 创建 stdin/stdout/stderr 的 HTIF fd；`init_dtb()` 解析设备树获取内存大小和 HTIF 基址。这些全局状态只能初始化一次。
- `init_done` 是 `volatile int`，配合 `__sync_synchronize()` 内存屏障，确保 hart 0 的写入对 hart 1 可见。
- hart 1 在 `while (!init_done)` 中自旋，直到 hart 0 完成。

#### 修改点 3：`mscratch` 指向本核的 `g_itrframe`
```c
// 修改前
write_csr(mscratch, &g_itrframe);

// 修改后
write_csr(mscratch, &g_itrframe[hartid]);
```

**原理**：`mtrapvec` 通过 `csrrw a0, mscratch, a0` 取得中断帧地址，必须让每个核指向自己的那份。

---

### 3.2 `kernel/process.h` / `kernel/process.c` —— 按核的 `current` 进程指针

#### 修改点：全局 `current` 改为数组 + 宏
```c
// kernel/process.h
// 修改前
extern process* current;

// 修改后
extern process* currents[NCPU];
#define current (currents[read_tp()])
```

```c
// kernel/process.c
// 修改前
process* current = NULL;

// 修改后
process* currents[NCPU] = {NULL};
```

**原理**：
- 多核下每个核必须独立知道自己正在运行哪个用户进程。
- `read_tp()` 读取 `tp` 寄存器（在 `mentry.S` 中被设为 `mhartid`）。通过宏定义，代码中所有原来的 `current = proc`、`current->trapframe` 等写法无需改动，编译时自动展开为按核索引。
- `switch_to(proc)` 中 `current = proc` 实际变成 `currents[read_tp()] = proc`，天然按核隔离。

---

### 3.3 `kernel/kernel.c` —— 用户态 `tp` 寄存器初始化为 `hartid`

#### 修改点：加载用户程序时写入 `tp`
```c
void load_user_program(process *proc, int hartid) {
  uint64 trapframe_addr = USER_TRAP_FRAME + hartid * 0x1000;
  uint64 kstack_addr    = USER_KSTACK    + hartid * 0x1000;
  uint64 stack_addr     = USER_STACK     + hartid * 0x1000;

  proc->trapframe = (trapframe *)trapframe_addr;
  memset(proc->trapframe, 0, sizeof(trapframe));
  proc->kstack = kstack_addr;
  proc->trapframe->regs.sp = stack_addr;
  proc->trapframe->regs.tp = hartid;   // <-- 新增

  load_bincode_from_host_elf(proc, hartid);
}
```

**原理**：
- `return_to_user`（`kernel/strap_vector.S`）在 `sret` 前会调用 `restore_all_registers`，从 trapframe 恢复包括 `tp` 在内的所有通用寄存器。
- `load_user_program` 里 `memset(..., 0, ...)` 会把 `tp` 清零。如果不显式设置，用户态的 `tp = 0`。
- 当用户态触发 `ecall` 进入 `smode_trap_handler` 时，内核需要通过 `read_tp()` 判断当前是哪个核。若 `tp = 0`，hart 1 的 trap 也会被误判为 hart 0，导致 `current` 指向错误的进程，最终引发指令访问错误或重复执行。
- **这是本实验中最隐蔽、最关键的 bug 之一。**

---

### 3.4 `kernel/strap.c` —— 时钟中断独立化与 syscall 修复

#### 修改点 1：`g_ticks` 数组化
```c
// 修改前
static uint64 g_ticks = 0;

// 修改后
static uint64 g_ticks[NCPU] = {0};
```

#### 修改点 2：`handle_mtimer_trap` 按核处理
```c
// 修改前
void handle_mtimer_trap() {
  sprint("Ticks %d\n", g_ticks);
  g_ticks++;
  write_csr(sip, read_csr(sip) & ~SIP_SSIP);
  // ... 重复代码
}

// 修改后
void handle_mtimer_trap() {
  int hartid = read_tp();
  sprint("hartid = %d: Ticks %d\n", hartid, g_ticks[hartid]);
  g_ticks[hartid]++;
  write_csr(sip, read_csr(sip) & ~SIP_SSIP);
}
```

**原理**：
- 每个核的 `CLINT_MTIMECMP` 是独立的（地址按 `hartid` 偏移）。M-mode timer 中断到来后，经 `handle_timer` 设置 SSIP 并交由 S-mode 处理。两核的时钟中断必须独立计数，否则一个核的 tick 会被另一个核重复统计。

#### 修改点 3：修复 `handle_syscall` 的 syscall number 参数
```c
// 修改前（错误提交引入）
long ret = do_syscall(regs->a7, regs->a0, regs->a1, ...);

// 修改后（恢复正确逻辑）
long ret = do_syscall(regs->a0, regs->a1, regs->a2, ...);
```

**原理**：
- 用户库 `user/user_lib.c` 的 `do_user_call` 把 `sysnum` 作为 C 函数第一个参数，由调用约定放入 `a0` 寄存器。
- 代码仓库某次错误提交将内核侧改为读 `a7` 作为 syscall number，导致 `do_syscall` 收到巨大的垃圾值，触发 `Unknown syscall` 后 panic。
- 同时删除了残留的旧 TODO 代码和第二次错误调用 `tf->regs.a0 = do_syscall(tf->regs.a0, ...)`。

---

### 3.5 `kernel/syscall.c` —— 多核同步退出

#### 修改点：`sys_user_exit` 引入完成计数器
```c
#include "spike_interface/atomic.h"

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
```

**原理**：
- 单核时 `exit` 直接调用 `shutdown()` 关闭 spike。多核下若任意核提前 `shutdown`，会强制终止仍在运行的其他核，导致输出不完整。
- 引入 `finished_harts` 计数器和 `exit_lock` 自旋锁（`atomic.h` 提供），确保所有核都调用 `sys_user_exit` 后，hart 0 才执行 `shutdown`。
- hart 1 退出后进入 `wfi` 低功耗等待，避免空转消耗 CPU。
- hart 0 在 `while (finished_harts < NCPU)` 中自旋等待。由于 `finished_harts` 是 `volatile`，且 `spinlock_unlock` 内含 `mb()` 内存屏障，hart 0 最终一定能读到 hart 1 的更新。

---

## 4. 未修改但已具备支持能力的文件

| 文件 | 说明 |
|-----|------|
| `kernel/config.h` | `NCPU` 已经定义为 `2`，无需修改。`USER_STACK` / `USER_KSTACK` / `USER_TRAP_FRAME` 的基地址在 `kernel.c` 的 `load_user_program` 中按 `hartid * 0x1000` 偏移，天然隔离。 |
| `kernel/elf.c` | `load_bincode_from_host_elf` 已经按 `hartid` 选择 `argv[0]` 或 `argv[1]`，支持加载两个 app。 |
| `kernel/machine/mentry.S` | 已经通过 `mhartid` 计算独立栈顶 `sp = stack0 + 4096 * (hartid + 1)`，并保存 `tp = mhartid`，无需修改。 |

---

## 5. 关键问题排查记录

### 5.1 "Unknown syscall 2166357912"  panic
- **根因**：`kernel/strap.c` 中 `handle_syscall` 错误地将 `regs->a7` 当作 syscall number，而用户库实际把 sysnum 放在 `a0`。
- **修复**：恢复为 `do_syscall(regs->a0, regs->a1, ...)`。

### 5.2 "Instruction access fault!" + app1 无输出
- **根因**：`load_user_program` 没有初始化 `trapframe->regs.tp`，导致 `return_to_user` 后用户态 `tp = 0`。hart 1 触发 `ecall` 后，内核 `read_tp()` 返回 0，`current` 被错误索引为 `currents[0]`（app0），`epc` 被写入 app1 的 PC 但保存到了 app0 的 trapframe，后续恢复时跑飞。
- **修复**：`proc->trapframe->regs.tp = hartid`。

### 5.3 评测超时（两核均在 `wfi` 中，无核调用 `shutdown`）
- **根因**：早期版本让"最后一个完成的核"调用 `shutdown`。若 hart 1 后完成，则 hart 1 调用 `shutdown`，hart 0 早已 `wfi`，spike 在多核 `wfi` 状态下对 HTIF exit 的响应可能不一致。
- **修复**：固定由 hart 0 负责 `shutdown`。hart 0 在 `sys_user_exit` 中自旋等待 `finished_harts == NCPU`，确保所有核安全结束后再关闭模拟器。

---

## 6. 编译与运行验证

```bash
export PATH="/home/momo/riscv64-elf-gcc/bin:$PATH"
make clean && make
make run
```

输出与预期完全一致：

```
HTIF is available!
(Emulated) memory size: 2048 MB
In m_start, hartid:0
hartid = 0: Enter supervisor mode...
hartid = 0: Application: obj/app0
hartid = 0: Application program entry point (virtual address): 0x0000000081000000
hartid = 0: Switch to user mode...
In m_start, hartid:1
hartid = 1: Enter supervisor mode...
hartid = 1: Application: obj/app1
hartid = 1: Application program entry point (virtual address): 0x0000000085000000
hartid = 1: Switch to user mode...
hartid = 0: >>> app0 is expected to be executed by hart0
hartid = 1: >>> app1 is expected to be executed by hart1
hartid = 0: User exit with code:0.
hartid = 1: User exit with code:0.
hartid = 0: shutdown with code:0.
System is shutting down with exit code 0.
```
