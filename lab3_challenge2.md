# Lab3 Challenge2: 信号量实现说明

## 一、分析流程与大致思路

### 1.1 需求分析
本实验要求通过修改 PKE 内核和系统调用，为用户程序提供信号量（Semaphore）功能，支持：
- 信号量的分配（创建）
- P 操作（等待/减操作）
- V 操作（信号/加操作）

当 P 操作发现信号量值不足时，进程需要进入等待状态，并触发进程调度，让出 CPU。

### 1.2 代码结构分析
首先对 PKE 内核的相关模块进行了梳理：

| 模块 | 关键文件 | 作用 |
|------|----------|------|
| 系统调用 | `kernel/syscall.h`, `kernel/syscall.c` | 定义系统调用号，分发并执行具体系统调用 |
| 进程管理 | `kernel/process.h`, `kernel/process.c` | 定义进程结构体 `process`，管理进程状态（FREE/READY/RUNNING/BLOCKED/ZOMBIE） |
| 调度器 | `kernel/sched.h`, `kernel/sched.c` | 维护就绪队列，实现 `schedule()` 进程切换 |
| 用户库 | `user/user_lib.h`, `user/user_lib.c` | 提供用户态系统调用封装 |
| 测试程序 | `user/app_semaphore.c` | 使用 `sem_new`/`sem_P`/`sem_V` 进行父子进程同步 |

### 1.3 设计思路
基于以上分析，确定了如下实现方案：

1. **在内核中维护一个全局的信号量表**：使用固定大小的数组（`NSEM = 16`），每个信号量包含 `value`（当前值）、`used`（是否被占用）、`wait_head`（等待队列头指针）。结构极小，不会引起 `kernel_size` 问题。
2. **复用进程的 `queue_next` 指针构建等待队列**：进程结构体中已有 `queue_next` 字段，当进程因 P 操作阻塞时，将其链入对应信号量的等待队列。由于进程被阻塞时不会在就绪队列中，因此不会与就绪队列冲突。
3. **P 操作阻塞时触发调度**：当 `value < 0` 时，将当前进程状态设为 `BLOCKED`，加入等待队列，然后调用 `schedule()` 切换到其他就绪进程。
4. **V 操作唤醒等待者**：当 `value <= 0` 时，说明有进程在等待，从等待队列头部取出一个进程，将其状态改为 `READY` 并插入就绪队列。
5. **添加系统调用号并封装用户接口**：在内核添加 3 个系统调用号，在用户库添加对应的封装函数。

---

## 二、具体修改内容

### 2.1 `kernel/syscall.h`

**添加内容**：
```c
// added for semaphore
#define SYS_user_sem_new (SYS_user_base + 6)
#define SYS_user_sem_P   (SYS_user_base + 7)
#define SYS_user_sem_V   (SYS_user_base + 8)
```

**说明**：为信号量的三个操作分配了系统调用号，紧跟在已有系统调用之后。

---

### 2.2 `kernel/syscall.c`

这是本次实验的核心修改文件，实现了信号量的全部内核逻辑。

#### 2.2.1 添加的数据结构

```c
#define NSEM 16

typedef struct {
  int value;        // 信号量当前值
  int used;         // 是否已被分配
  process *wait_head;  // 阻塞等待队列头
} sem_t;

static sem_t sems[NSEM];
```

- `sems[]` 是静态全局数组，存放在内核 BSS 段，自动初始化为 0。
- 整个数组大小仅约 256 字节，非常轻量。

#### 2.2.2 `sys_user_sem_new(int init_val)`

**功能**：创建并初始化一个信号量。

**工作原理**：
- 遍历 `sems[]` 数组，查找第一个 `used == 0` 的条目。
- 将其 `used` 置为 1，`value` 设为 `init_val`，`wait_head` 置为 `NULL`。
- 返回该信号量在数组中的索引（作为信号量 ID）。
- 若数组已满，返回 `-1`。

**被调用位置**：通过 `do_syscall()` 的 `SYS_user_sem_new` 分支，由用户程序调用 `sem_new()` 触发。

#### 2.2.3 `sys_user_sem_P(int sem_id)`

**功能**：执行 P 操作（wait/down）。

**工作原理**：
1. 参数检查：若 `sem_id` 非法或信号量未分配，返回 `-1`。
2. 将信号量的 `value` 减 1。
3. 判断：
   - 若 `value >= 0`：资源充足，P 操作直接成功返回 `0`。
   - 若 `value < 0`：资源不足，当前进程需要阻塞等待：
     - 将 `current->status` 设为 `BLOCKED`。
     - 将当前进程通过 `queue_next` 链入该信号量的等待队列尾部（FIFO）。
     - 手动设置 `current->trapframe->regs.a0 = 0`，确保进程被唤醒返回用户态时 `a0` 寄存器值为 0（表示成功）。
     - 调用 `schedule()` 触发进程调度，切换到就绪队列中的下一个进程。

**被调用位置**：通过 `do_syscall()` 的 `SYS_user_sem_P` 分支，由用户程序调用 `sem_P()` 触发。

#### 2.2.4 `sys_user_sem_V(int sem_id)`

**功能**：执行 V 操作（signal/up）。

**工作原理**：
1. 参数检查：若 `sem_id` 非法或信号量未分配，返回 `-1`。
2. 将信号量的 `value` 加 1。
3. 判断：
   - 若 `value <= 0`：说明在加 1 之前至少有进程在等待，需要唤醒一个：
     - 从等待队列头部取出一个进程 `p`。
     - 将 `p->status` 改为 `READY`，并调用 `insert_to_ready_queue(p)` 将其加入全局就绪队列。
   - 若 `value > 0`：没有等待者，直接返回。
4. 返回 `0`。

**被调用位置**：通过 `do_syscall()` 的 `SYS_user_sem_V` 分支，由用户程序调用 `sem_V()` 触发。

#### 2.2.5 `do_syscall()` 的扩展

在 `do_syscall()` 的 `switch` 语句中新增了三个 `case`：
```c
case SYS_user_sem_new:
  return sys_user_sem_new(a1);
case SYS_user_sem_P:
  return sys_user_sem_P(a1);
case SYS_user_sem_V:
  return sys_user_sem_V(a1);
```

---

### 2.3 `user/user_lib.h`

**添加内容**：
```c
int sem_new(int init_val);
void sem_P(int sem_id);
void sem_V(int sem_id);
```

**说明**：为用户程序提供信号量操作的接口声明。

**被调用位置**：`user/app_semaphore.c` 中直接调用这些函数。

---

### 2.4 `user/user_lib.c`

**添加内容**：
```c
int sem_new(int init_val) {
  return do_user_call(SYS_user_sem_new, init_val, 0, 0, 0, 0, 0, 0);
}

void sem_P(int sem_id) {
  do_user_call(SYS_user_sem_P, sem_id, 0, 0, 0, 0, 0, 0);
}

void sem_V(int sem_id) {
  do_user_call(SYS_user_sem_V, sem_id, 0, 0, 0, 0, 0, 0);
}
```

**说明**：通过内联汇编指令 `ecall` 陷入内核，触发对应的系统调用。

**被调用位置**：
- `sem_new()` 在 `app_semaphore.c` 中被调用 3 次，用于创建 `main_sem` 和两个 `child_sem`。
- `sem_P()` / `sem_V()` 在循环中被调用，控制父子进程的打印顺序。

---

## 三、关键流程图解

### 3.1 P 操作阻塞与调度流程

```
用户态: sem_P(sem_id)
    |
    v
ecall 进入 S-mode
    |
    v
smode_trap_handler -> handle_syscall -> do_syscall -> sys_user_sem_P
    |
    v
value--
    |
    +-- value >= 0 --> 直接返回 0
    |
    +-- value < 0  --> current->status = BLOCKED
                      链入 sem.wait_queue
                      schedule()  // 切换到其他进程
```

### 3.2 V 操作唤醒流程

```
用户态: sem_V(sem_id)
    |
    v
ecall 进入 S-mode
    |
    v
sys_user_sem_V
    |
    v
value++
    |
    +-- value > 1 或队列为空 --> 直接返回
    |
    +-- value <= 0 --> 从 wait_queue 取出头部进程 p
                       p->status = READY
                       insert_to_ready_queue(p)
```

### 3.3 被唤醒进程的恢复

当阻塞进程 `p` 被 V 操作唤醒并插入就绪队列后：
1. 后续某次 `schedule()` 选中 `p`。
2. `switch_to(p)` 通过 `return_to_user()` 恢复 `p` 的上下文。
3. `p` 从上次 `ecall` 的下一条指令继续执行，仿佛 `sem_P()` 刚刚返回。

---

## 四、同步正确性说明

PKE 运行在单核环境（`NCPU = 1`），且 RISC-V 进入 S-mode trap 时硬件会自动清除 `SSTATUS_SIE`，即 S-mode 中断被关闭。因此：
- 系统调用 `sys_user_sem_P` / `sys_user_sem_V` 的执行是原子的。
- 不需要额外的自旋锁或关中断操作来保护信号量结构和等待队列。
- 进程的 `queue_next` 指针在进程阻塞时用于链接等待队列，在进程就绪时用于链接就绪队列，两种状态互斥，不会冲突。

---

## 五、测试验证

编译并运行 `app_semaphore`：

```bash
make run
```

输出结果：

```
Parent print 0
Child0 print 0
Child1 print 0
Parent print 1
Child0 print 1
Child1 print 1
...
Parent print 9
Child0 print 9
Child1 print 9
```

**结论**：三个进程严格按照 `sem_P`/`sem_V` 控制的顺序交替执行，说明信号量的分配、P 操作阻塞等待、V 操作唤醒以及进程调度均正确工作。
