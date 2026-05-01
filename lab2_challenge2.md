# PKE 内核堆管理优化修改说明

## 一、分析流程与整体思路

### 1.1 原始问题分析
原始 PKE 内核的 `sys_user_allocate_page` 每次系统调用只分配一整页（4KB）物理内存，并将整页映射到用户虚拟地址空间。这种"按页分配"策略导致：
- 两次 `better_malloc(100)` 和 `better_malloc(50)` 会占用两个不同的页面
- `better_free` 后直接释放整页，无法重用页内剩余空间
- 无法满足"两次申请在同页"且"释放后重用"的测试要求

### 1.2 设计思路
参考 Linux 内存分配中的堆管理策略，在内核中引入**内存控制块（MCB, Memory Control Block）**机制，将页级分配优化为字节级细粒度分配：
1. **虚拟地址空间管理**：为每个进程维护独立的堆顶指针 `heap_top`，按需扩展堆虚拟地址范围
2. **物理页按需映射**：当 `heap_top` 扩展到未映射的新页面时，动态分配物理页并建立映射
3. **空闲块管理**：使用 MCB 链表记录每个分配块的状态，支持首次适应（First-Fit）重用策略
4. **细粒度分配/释放**：`malloc` 按字节分配并记录元数据，`free` 仅标记块为空闲供后续重用

### 1.3 关键数据结构
```c
typedef struct mcb_t {
  uint64 addr;           // 分配块的起始虚拟地址
  uint64 size;           // 分配块的大小（8字节对齐）
  int used;              // 是否已使用（1=使用，0=空闲）
  struct mcb_t *next;    // 链表指针
} mcb;
```

---

## 二、具体修改内容

### 2.1 `kernel/process.h`

**修改类型**：数据结构扩展与函数声明

**添加内容**：

1. **MCB 结构定义**（第 21-27 行）：
   ```c
   typedef struct mcb_t {
     uint64 addr;
     uint64 size;
     int used;
     struct mcb_t *next;
   } mcb;
   ```
   每个 `mcb` 节点记录一个已分配或已释放的内存块元数据，通过链表串联形成进程的全局块表。

2. **进程结构扩展**（第 30-40 行）：
   ```c
   typedef struct process_t {
     uint64 kstack;
     pagetable_t pagetable;
     trapframe* trapframe;
     // heap management
     uint64 heap_top;
     mcb *mcb_list;
   } process;
   ```
   - `heap_top`：当前堆的顶部虚拟地址，下一次分配从此地址开始
   - `mcb_list`：指向 MCB 链表头部的指针

3. **函数声明**（第 45-47 行）：
   ```c
   uint64 heap_alloc(process *proc, uint64 size);
   void heap_free(process *proc, uint64 addr);
   ```

**使用位置**：被 `kernel/syscall.c` 中的系统调用处理函数调用。

---

### 2.2 `kernel/kernel.c`

**修改类型**：进程初始化

**添加内容**（第 61-63 行）：
```c
// initialize heap management for the process
proc->heap_top = USER_FREE_ADDRESS_START;
proc->mcb_list = NULL;
```

**位置**：`load_user_program()` 函数中，在 `load_bincode_from_host_elf(proc)` 之后、建立用户栈映射之前。

**作用**：加载用户 ELF 程序后，将进程堆起始位置初始化为 `USER_FREE_ADDRESS_START`（0x100000），并将 MCB 链表置空，为后续堆分配做准备。

---

### 2.3 `kernel/process.c`

**修改类型**：新增三个核心函数

#### 函数 1：`ensure_heap_mapped`

```c
static int ensure_heap_mapped(pagetable_t pt, uint64 va, uint64 size);
```

**调用位置**：仅在 `heap_alloc` 内部被调用。

**工作原理**：
1. 计算虚拟地址范围 `[va, va+size]` 所覆盖的所有页面边界
2. 遍历每个页面，通过 `page_walk(pt, p, 0)` 检查该虚拟页是否已有有效 PTE
3. 若某页未映射，调用 `alloc_page()` 分配物理页，再通过 `user_vm_map()` 建立用户空间可读可写映射
4. 若物理内存耗尽则触发 `panic`

**关键意义**：这是"按需分页"的核心实现，保证 `heap_top` 扩展时虚拟地址背后总有物理页支撑。

---

#### 函数 2：`heap_alloc`

```c
uint64 heap_alloc(process *proc, uint64 size);
```

**调用位置**：`kernel/syscall.c` 中的 `sys_user_allocate_page(uint64 size)`。

**工作原理**：
1. **对齐处理**：将请求大小按 8 字节向上对齐，避免未对齐访问
2. **首次适应搜索**：遍历 `proc->mcb_list`，查找第一个满足 `!used && size >= alloc_size` 的空闲块
   - 若找到，将其标记为 `used = 1` 并返回该块地址（**实现内存重用**）
3. **堆顶扩展**：若链表中没有合适空闲块：
   - 取 `addr = proc->heap_top`
   - 调用 `ensure_heap_mapped` 确保 `[addr, addr+alloc_size]` 已映射物理页
   - 使用 `alloc_page()` 分配一页作为新 MCB 节点（实验场景下简单可行）
   - 将新 MCB 追加到链表尾部
   - 推进 `proc->heap_top += alloc_size`
   - 返回 `addr`

**测试场景验证**：
- `better_malloc(100)` → 对齐为 104，返回 0x100000，`heap_top` 变为 0x100068
- `better_malloc(50)` → 对齐为 56，链表无空闲块，返回 0x100068，两地址差 104 ≤ 512（**同页**）
- `better_free(0x100000)` → 第一个 MCB 标记为 `used = 0`
- `better_malloc(50)` → 首次适应命中第一个空闲块（大小 104 ≥ 56），返回 0x100000（**精确重用**）

---

#### 函数 3：`heap_free`

```c
void heap_free(process *proc, uint64 addr);
```

**调用位置**：`kernel/syscall.c` 中的 `sys_user_free_page(uint64 va)`。

**工作原理**：
1. 遍历 `proc->mcb_list`，查找 `addr` 匹配且 `used == 1` 的节点
2. 找到后将其 `used` 置为 0，表示该块进入空闲状态
3. 若未找到匹配节点，触发 `panic` 防止非法释放

**注意**：当前实现未做物理页回收和相邻空闲块合并。因为测试场景仅涉及单页内的少量分配，且进程生命周期结束即系统关闭，该简化不影响功能正确性。

---

### 2.4 `kernel/syscall.c`

**修改类型**：系统调用适配

**修改内容**：

1. **`sys_user_allocate_page` 改造**（第 42-44 行）：
   ```c
   uint64 sys_user_allocate_page(uint64 size) {
     return heap_alloc(current, size);
   }
   ```
   - 原实现：忽略 `size`，直接分配一整页
   - 新实现：将用户传入的 `size` 传递给 `heap_alloc`，实现真正的字节级堆分配

2. **`sys_user_free_page` 改造**（第 49-52 行）：
   ```c
   uint64 sys_user_free_page(uint64 va) {
     heap_free(current, va);
     return 0;
   }
   ```
   - 原实现：调用 `user_vm_unmap` 直接释放整页物理内存
   - 新实现：调用 `heap_free` 仅标记对应 MCB 为空闲，保留物理页和映射供重用

3. **`do_syscall` 参数传递**（第 66 行）：
   ```c
   case SYS_user_allocate_page:
     return sys_user_allocate_page(a1);
   ```
   - 原实现：`sys_user_allocate_page()` 无参数
   - 新实现：将寄存器 `a1`（即用户传入的 `n`）传入内核，使 `better_malloc(n)` 的请求大小生效

---

## 三、编译与运行验证

在项目根目录执行：
```bash
make clean && make && make run
```

**预期输出**：
```
hello, world!!!
User exit with code:0.
System is shutting down with exit code 0.
```

**验证点**：
1. 两次 `better_malloc` 返回地址差值 ≤ 512（同页）
2. `better_free` 后再次 `better_malloc` 返回原地址（重用成功）
3. 进程正常退出，退出码为 0

---

## 四、文件修改清单

| 文件路径 | 修改类型 | 主要内容 |
|---------|---------|---------|
| `kernel/process.h` | 修改 | 添加 MCB 结构、扩展 process 结构、声明堆管理函数 |
| `kernel/kernel.c` | 修改 | 在进程加载时初始化 `heap_top` 和 `mcb_list` |
| `kernel/process.c` | 修改 | 添加 `ensure_heap_mapped`、`heap_alloc`、`heap_free` 实现 |
| `kernel/syscall.c` | 修改 | 适配系统调用，将页级分配改为字节级堆分配 |
