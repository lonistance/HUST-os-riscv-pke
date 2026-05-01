# Lab2 Challenge3：多核内存管理实验报告

## 1. 实验目标

在已具备多核并发启动能力的基础上（lab1_challenge3 的多核机制已重做），为引入虚拟内存的 riscv-pke 添加**多核物理内存并发控制**与**虚拟地址空间隔离**。

核心要求：
- `spike -p2` 启动两核，分别运行 `app_alloc0` 和 `app_alloc1`
- 物理页分配互斥：同一物理页不能被两核同时分配
- 虚拟地址隔离：每个进程的 `naive_malloc` 虚拟地址连续且独立，互不重叠
- 内核页表和物理内存管理初始化仅执行一次

---

## 2. 分析思路

单核 lab2 的内存管理代码直接搬到多核会遇到 4 个新问题：

| 问题 | 现象 | 解决思路 |
|-----|------|---------|
| **内核初始化重复执行** | `pmm_init()` 和 `kern_vm_init()` 被两个核各执行一次，导致空闲页列表和内核页表被重复初始化/覆盖，引发 `map_pages` panic | 仅 hart 0 执行初始化，其余核通过屏障同步等待 |
| **物理页分配竞态** | `alloc_page()` 操作全局链表 `g_free_mem_list` 无锁保护，两核可能同时取到同一个空闲节点 | 用 `amoswap` 实现自旋锁，保护 `alloc_page` / `free_page` |
| **虚拟地址全局共享** | 全局变量 `g_ufree_page` 管理用户虚拟地址起点，多进程下虚拟地址重叠，导致页表映射冲突 | 将 `ufree_page` 移入 `process` 结构体，每个进程独立管理 |
| **alloc_page 打印硬编码 hartid** | 原代码 `uint64 hartid = 0` 导致无论哪个核分配都显示 `hartid = 0` | 改为 `read_tp()` 获取实际核号 |

---

## 3. 分文件修改说明

### 3.1 `kernel/sync_utils.h` —— 新增自旋锁

```c
typedef struct {
  int locked;
} spinlock;

static inline void spinlock_acquire(spinlock *lk) {
  int expected;
  do {
    asm volatile("amoswap.w %0, %1, (%2)\n"
                 : "=r"(expected)
                 : "r"(1), "r"(&lk->locked)
                 : "memory");
  } while (expected);
}

static inline void spinlock_release(spinlock *lk) {
  asm volatile("amoswap.w x0, x0, (%0)\n"
               :
               : "r"(&lk->locked)
               : "memory");
}
```

**原理**：
- RISC-V `amoswap.w` 是原子交换指令。`spinlock_acquire` 将 `1` 原子写入 `lk->locked`，并返回旧值。若旧值为 `1`，说明锁已被占用，循环重试；若旧值为 `0`，则获取锁成功。
- `spinlock_release` 用 `amoswap.w x0, x0, (addr)` 将 `0` 原子写回，释放锁。`x0` 是零寄存器，效果等价于原子清零。
- 多核并发时，只有一个核能成功将 `locked` 从 `0` 变为 `1`，其余核在 `do-while` 中自旋等待，从而保证 `g_free_mem_list` 操作的原子性。

---

### 3.2 `kernel/pmm.c` —— 物理页分配并发保护

#### 修改点 1：加锁保护空闲链表
```c
static spinlock mem_lock;

void pmm_init() {
  // ... 原有代码 ...
  create_freepage_list(free_mem_start_addr, free_mem_end_addr);
  spinlock_init(&mem_lock);
}

void *alloc_page(void) {
  spinlock_acquire(&mem_lock);
  list_node *n = g_free_mem_list.next;
  uint64 hartid = read_tp();
  if (vm_alloc_stage[hartid]) {
    sprint("hartid = %ld: alloc page 0x%x\n", hartid, n);
  }
  if (n) g_free_mem_list.next = n->next;
  spinlock_release(&mem_lock);
  return (void *)n;
}

void free_page(void *pa) {
  // ... 合法性检查 ...
  spinlock_acquire(&mem_lock);
  list_node *n = (list_node *)pa;
  n->next = g_free_mem_list.next;
  g_free_mem_list.next = n;
  spinlock_release(&mem_lock);
}
```

**原理**：
- `alloc_page` 和 `free_page` 都操作全局链表 `g_free_mem_list`。若不保护，以下竞态可能发生：
  1. 核 A 读取 `g_free_mem_list.next = P`
  2. 核 B 同时读取 `g_free_mem_list.next = P`
  3. 核 A 设置 `g_free_mem_list.next = P->next`
  4. 核 B 也设置 `g_free_mem_list.next = P->next`
  5. 两核拿到同一个物理页 `P`，后续写入同一个物理地址，数据互相覆盖，最终触发异常或输出错乱。
- 自旋锁确保上述 4 步对单个核是原子的，另一核在 `spinlock_acquire` 处阻塞，直到锁释放。

#### 修改点 2：修正 hartid
```c
// 修改前
uint64 hartid = 0;

// 修改后
uint64 hartid = read_tp();
```

**原理**：`vm_alloc_stage[hartid]` 控制是否打印物理页分配信息。硬编码为 `0` 会导致所有核的打印都显示 `hartid = 0`，无法区分是哪个核的用户进程在申请内存。

---

### 3.3 `kernel/process.h` —— 虚拟地址按进程隔离

```c
typedef struct process_t {
  uint64 kstack;
  pagetable_t pagetable;
  trapframe* trapframe;
  uint64 ufree_page;   // 每个进程独立的虚拟地址分配起点
} process;
```

**原理**：
- 单核 lab2 使用全局变量 `g_ufree_page` 管理用户堆虚拟地址。所有进程共享同一个起点，导致后创建的进程会覆盖先创建进程的虚拟地址映射。
- 将 `ufree_page` 放入 `process` 结构体后，每个进程拥有独立的虚拟地址游标。进程 A 从 `0x400000` 开始分配，进程 B 也从 `0x400000` 开始分配，但映射到**不同的物理页**，两进程的虚拟地址空间互不干扰。

---

### 3.4 `kernel/kernel.c` —— 内核初始化单次执行 + 多核程序加载

#### 修改点 1：初始化仅 hart 0 执行，全核同步后开页表
```c
int s_start(void) {
  int hartid = read_tp();
  // ...
  if (hartid == 0) {
    pmm_init();
    kern_vm_init();
  }

  static volatile int barrier_counter = 0;
  sync_barrier(&barrier_counter, NCPU);

  enable_paging();
  // ...
}
```

**原理**：
- `pmm_init()` 构建全局空闲页链表，`kern_vm_init()` 创建内核页表并映射内核代码段和数据段。这两个操作只能执行一次。
- 若 hart 1 也执行 `pmm_init()`，会重新初始化 `g_free_mem_list`，导致 hart 0 之前分配的页被错误地放回空闲列表；若再执行 `kern_vm_init()`，会创建第二个内核页目录，并试图映射已被映射的地址，触发 `map_pages` 的 `PTE_V` 断言失败。
- `sync_barrier` 使用 `amoadd.w` 原子递增计数器，未到达目标核数的核在 `lw` 循环中自旋，直到所有核都到达屏障。hart 0 完成初始化后，hart 1 才能安全地 `enable_paging()` 并继续执行。

#### 修改点 2：`load_user_program` 按核初始化独立资源
```c
void load_user_program(process *proc, int hartid) {
  // ... 分配 trapframe, pagetable, kstack, user stack ...
  proc->ufree_page = USER_FREE_ADDRESS_START;   // 关键：每个进程独立的虚拟地址起点
  // ...
  load_bincode_from_host_elf(proc, hartid);
  // ...
}
```

**原理**：
- 每个核调用 `load_user_program` 时传入自己的 `user_apps[hartid]` 和 `hartid`，分配独立的物理页作为页目录、内核栈、用户栈。
- `proc->ufree_page = USER_FREE_ADDRESS_START` 确保两个进程的用户堆都从 `0x400000` 开始，但由于页表独立且物理页不同，实现虚拟地址空间隔离。

---

### 3.5 `kernel/syscall.c` —— 虚拟地址分配使用进程私有游标

```c
uint64 sys_user_allocate_page() {
  void* pa = alloc_page();
  uint64 va = current->ufree_page;   // 使用当前进程的私有虚拟地址起点
  current->ufree_page += PGSIZE;     // 递增当前进程的游标
  user_vm_map((pagetable_t)current->pagetable, va, PGSIZE, (uint64)pa,
         prot_to_type(PROT_WRITE | PROT_READ, 1));
  sprint("hartid = %d: vaddr 0x%x is mapped to paddr 0x%x\n", read_tp(), va, pa);
  return va;
}
```

**原理**：
- 单核版本使用全局 `g_ufree_page`。多核下若继续使用全局变量，进程 A 分配一页后全局游标前进，进程 B 再分配时拿到的是下一个虚拟地址。但进程 B 的页表中也映射了进程 A 的虚拟地址（如果进程 B 之前没有用过的话），或者更糟的是，两个进程的页表映射到同一个虚拟地址，但物理页不同，导致数据不可见或相互覆盖。
- 改为 `current->ufree_page` 后，每个进程的虚拟地址分配完全独立。`app_alloc0` 和 `app_alloc1` 各自从 `0x400000` 开始，依次得到 `0x400000`, `0x401000`, `0x402000`... 但由于 `user_vm_map` 将各自的虚拟地址映射到**不同的物理页**，两进程的数据完全隔离。

---

## 4. 关键问题排查

### 问题 1：`map_pages fails on mapping va ... to pa ...` panic
- **根因**：`s_start` 中 `read_tp()` 返回 `0`，导致 hart 1 误判自己是 hart 0，也执行了 `pmm_init()` 和 `kern_vm_init()`，重复初始化破坏了页表和空闲链表。
- **深层原因**：`m_start` 期间调用的子函数可能将 `tp`（调用者保存寄存器）当作临时寄存器使用，导致 `mentry.S` 中设置的 `tp = mhartid` 在进入 S-mode 前被破坏。
- **修复**：在 `m_start` 的 `mret` 前显式执行 `write_tp(hartid)`，确保 S-mode 一定能读到正确的核号。

### 问题 2：两核分配到的物理页相同，输出错乱
- **根因**：`alloc_page()` 未加锁，两核并发时可能同时摘取同一个链表节点。
- **修复**：`sync_utils.h` 中新增 `spinlock`，在 `alloc_page` 和 `free_page` 首尾加锁保护。

### 问题 3：`app_alloc0` 的输出不是 `0,1,2,3,4`，而是乱序或重复
- **根因**：全局 `g_ufree_page` 导致两进程虚拟地址重叠，页表映射到不同物理页，一个进程写入的数据在另一个进程的虚拟地址上不可见。
- **修复**：将 `ufree_page` 移入 `process` 结构体，实现按进程的虚拟地址空间隔离。

---

## 5. 编译与运行验证

```bash
export PATH="/home/momo/riscv64-elf-gcc/bin:$PATH"
make clean && make
make run
```

输出验证要点：
- `hartid = 0:` 和 `hartid = 1:` 的 `user frame` / `kstack` 地址不同（物理页隔离）
- `alloc page` 地址在两核间递减且互不重复（如 `0x87fa4000`, `0x87fa3000`...）
- `vaddr 0x00400000` 在两核中分别映射到**不同**的 `paddr`
- `app_alloc0` 读到 `0,1,2,3,4`；`app_alloc1` 读到 `5,6,7,8,9`
