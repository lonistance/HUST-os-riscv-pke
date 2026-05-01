# Lab3 Challenge3: Fork 写时复制（COW）机制实现文档

## 一、问题分析

### 1.1 原始行为
在修改之前，`do_fork` 在处理 `HEAP_SEGMENT` 时会直接为子进程的每一个堆页分配新的物理页面，并将父进程堆数据完整复制过去。这导致：
- fork 后子进程的堆物理地址与父进程始终不同
- 即使子进程只读不写，也浪费了物理内存和复制开销

### 1.2 目标行为
实现写时复制（Copy-On-Write, COW）：
1. **fork 时**：子进程与父进程共享相同的物理堆页，不立即复制
2. **读操作时**：父子访问同一物理地址，无需额外开销
3. **写操作时**：触发页故障（Store Page Fault），内核为写入方分配新物理页并复制数据，再重新映射

### 1.3 关键问题
- **如何标记 COW 页？** → 利用 RISC-V PTE 的 RSW（Reserved for Software）位
- **如何区分 COW 页故障与普通栈增长故障？** → 检查 PTE 的 COW 标志位
- **共享页何时释放？** → 引入物理页引用计数，仅当计数降为 0 时才释放
- **父子页表权限如何设置？** → fork 后共享页均设为只读 + COW 标记；写入后恢复为可读写

---

## 二、修改文件及详细说明

### 2.1 `kernel/riscv.h`

**修改内容：**
```c
#define PTE_COW (1L << 8)  // copy-on-write (software-defined)
```

**原理依据：**
- RISC-V Sv39 页表项中，bit 0~7 为标准硬件定义位（V/R/W/X/U/G/A/D）
- bit 8~9 为 RSW（Reserved for Software），供软件自由使用
- 选用 bit 8 作为 COW 标记位，不会影响硬件页表遍历和权限检查

---

### 2.2 `kernel/pmm.h` 与 `kernel/pmm.c`

**修改内容：**
在 `pmm.h` 中增加引用计数接口声明：
```c
void incr_page_ref(uint64 pa);
void decr_page_ref(uint64 pa);
int get_page_ref(uint64 pa);
void set_page_ref(uint64 pa, int val);
```

在 `pmm.c` 中实现：
```c
#define MAX_PHY_PAGES (PKE_MAX_ALLOWABLE_RAM / PGSIZE)
static uint8_t page_ref_count[MAX_PHY_PAGES];

static inline int pa2idx(uint64 pa) {
  return (pa - DRAM_BASE) / PGSIZE;
}

void incr_page_ref(uint64 pa) { ... }
void decr_page_ref(uint64 pa) { ... }
int get_page_ref(uint64 pa) { ... }
void set_page_ref(uint64 pa, int val) { ... }
```

并在 `pmm_init()` 中初始化：
```c
memset(page_ref_count, 0, sizeof(page_ref_count));
```

**原理依据：**
- PKE 管理的最大物理内存为 `PKE_MAX_ALLOWABLE_RAM = 128MB`，共 `128MB / 4KB = 32768` 个物理页
- 使用 `uint8_t` 数组记录每个物理页的引用次数，最多支持 255 个引用（远超过 PKE 的 32 进程上限）
- 物理页地址通过 `(pa - DRAM_BASE) / PGSIZE` 映射为数组索引
- 引用计数是判断共享页能否安全释放的唯一依据：仅当引用计数为 0 时，该物理页才真正空闲

---

### 2.3 `kernel/vmm.c`

**修改内容：**
重写 `user_vm_unmap`：
```c
void user_vm_unmap(pagetable_t page_dir, uint64 va, uint64 size, int free) {
  uint64 first, last;
  for (first = ROUNDDOWN(va, PGSIZE), last = ROUNDDOWN(va + size - 1, PGSIZE);
       first <= last; first += PGSIZE) {
    pte_t *pte = page_walk(page_dir, first, 0);
    if (pte == 0 || (*pte & PTE_V) == 0) {
      panic("user_vm_unmap: va 0x%lx not mapped\n", first);
    }
    uint64 pa = PTE2PA(*pte);
    if (free) {
      decr_page_ref(pa);
      if (get_page_ref(pa) == 0) {
        free_page((void*)pa);
      }
    }
    *pte = 0;  // 清除页表项
  }
}
```

**原理依据：**
- 原始实现直接调用 `free_page`，未考虑共享场景：若多个进程共享同一物理页，释放会导致其他进程访问已回收的内存，造成严重错误
- 修改后，解除映射时先递减引用计数，仅当计数降为 0 才真正归还物理页到空闲链表
- 同时清除 PTE（`*pte = 0`），防止后续误访问

---

### 2.4 `kernel/syscall.c`

**修改内容：**
在 `sys_user_allocate_page` 中，映射完成后设置引用计数：
```c
user_vm_map((pagetable_t)current->pagetable, va, PGSIZE, (uint64)pa,
       prot_to_type(PROT_WRITE | PROT_READ, 1));
set_page_ref((uint64)pa, 1);  // 新增
return va;
```

**原理依据：**
- 用户通过 `naive_malloc` 首次申请堆页时，该物理页仅被当前进程引用
- 必须在此时将引用计数初始化为 1，否则后续 `do_fork` 中的 `incr_page_ref` 会基于错误的初始值累加

---

### 2.5 `kernel/process.c`

**修改内容：**
修改 `do_fork` 中 `HEAP_SEGMENT` 的处理逻辑：

```c
case HEAP_SEGMENT:
  // ... free_block_filter 逻辑不变 ...

  for (uint64 heap_block = parent->user_heap.heap_bottom;
       heap_block < parent->user_heap.heap_top; heap_block += PGSIZE) {
    if (free_block_filter[(heap_block - heap_bottom) / PGSIZE])
      continue;

    uint64 parent_pa = lookup_pa(parent->pagetable, heap_block);

    // 子进程映射到父进程同一物理页，只读 + COW
    user_vm_map((pagetable_t)child->pagetable, heap_block, PGSIZE, parent_pa,
                prot_to_type(PROT_READ, 1) | PTE_COW);

    // 父进程页表项也去写权限、置 COW 位
    pte_t *pte = page_walk(parent->pagetable, heap_block, 0);
    if (pte) {
      *pte = (*pte & ~PTE_W) | PTE_COW;
    }

    incr_page_ref(parent_pa);
  }

  child->mapped_info[HEAP_SEGMENT].npages = parent->mapped_info[HEAP_SEGMENT].npages;
  memcpy((void*)&child->user_heap, (void*)&parent->user_heap, sizeof(parent->user_heap));
  break;
```

**原理依据：**
- **共享映射**：子进程不再申请新物理页，而是直接映射到父进程的物理页，实现"先映射但不实际复制"
- **权限控制**：
  - 子进程 PTE：`PTE_R | PTE_U | PTE_COW`（可读、用户态、COW 标记），**无写权限**
  - 父进程 PTE：同样清除 `PTE_W` 并置 `PTE_COW`
  - 这样无论父子哪一方尝试写入，都会触发 Store Page Fault，进入内核 COW 处理流程
- **引用计数**：共享后调用 `incr_page_ref`，记录该物理页被多一个进程引用
- **跳过已释放块**：`free_block_filter` 逻辑保留，跳过用户已 `naive_free` 的堆块

---

### 2.6 `kernel/strap.c`

**修改内容：**
重写 `handle_user_page_fault` 的 `CAUSE_STORE_PAGE_FAULT` 处理：

```c
case CAUSE_STORE_PAGE_FAULT: {
  pte_t *pte = page_walk(current->pagetable, stval, 0);
  if (pte && (*pte & PTE_V) && (*pte & PTE_COW)) {
    // 判定为 COW 页故障
    uint64 old_pa = PTE2PA(*pte);
    int ref = get_page_ref(old_pa);
    if (ref == 1) {
      // 仅当前进程引用，直接恢复写权限
      *pte = (*pte | PTE_W | PTE_D) & ~PTE_COW;
    } else {
      // 多个进程共享，需要复制新页
      void* new_pa = alloc_page();
      memcpy(new_pa, (void*)old_pa, PGSIZE);
      *pte = PA2PTE((uint64)new_pa) | prot_to_type(PROT_WRITE | PROT_READ, 1) | PTE_V;
      decr_page_ref(old_pa);
      set_page_ref((uint64)new_pa, 1);
    }
    flush_tlb();
    break;
  }

  // 非 COW 故障，按原逻辑处理（栈增长）
  void* pa = alloc_page();
  user_vm_map(current->pagetable, ROUNDDOWN(stval, PGSIZE), PGSIZE, (uint64)pa,
         prot_to_type(PROT_WRITE | PROT_READ, 1));
  break;
}
```

**原理依据：**
- **如何分辨 COW 场景**：检查 fault 地址对应的 PTE 是否存在（`PTE_V`）且带有 `PTE_COW` 标志。若是，则为 COW 写入；否则为普通的未映射页故障（如栈增长）。
- **引用计数为 1 时**：说明当前只有本进程还在使用该物理页（例如父进程已退出，或其他共享者已释放），无需复制新页，只需恢复写权限并清除 COW 标记，节省一次内存复制。
- **引用计数大于 1 时**：说明仍有其他进程共享该页，必须：
  1. 申请新物理页
  2. `memcpy` 复制旧页数据
  3. 修改当前进程 PTE 指向新页，权限设为可读写（`PTE_W | PTE_D`）
  4. 旧页引用计数减 1
  5. 新页引用计数置为 1
- **`flush_tlb()`**：修改页表后必须刷新 TLB，否则 CPU 可能仍使用旧的缓存页表项，导致重复故障
- **`PTE_D` 的重要性**：直接设置 `PTE_W` 时必须同时设置 `PTE_D`（Dirty），否则某些 RISC-V 模拟器/硬件会因缺少 Dirty 位而再次触发页故障

---

## 三、页表权限变化总结

| 阶段 | PTE 标志位 | 说明 |
|------|-----------|------|
| 父进程初始分配堆页 | `V \| R \| W \| D \| U \| A` | 正常可读写 |
| fork 后（共享期） | `V \| R \| U \| A \| COW` | 父子均为只读，带 COW 标记 |
| COW 触发后（独占） | `V \| R \| W \| D \| U \| A` | 恢复可读写，清除 COW |

---

## 四、共享页释放条件

**核心原则：引用计数降为 0 时释放。**

具体场景：
1. **父进程 fork 多个子进程**：每 fork 一次，共享物理页引用计数 +1
2. **子进程写入触发 COW**：子进程获得新页，旧页引用计数 -1
3. **进程调用 `naive_free`**：`user_vm_unmap` 递减引用计数，若降为 0 则调用 `free_page`
4. **父进程退出**：在本实现中，PKE 原版的 `free_process` 仅将进程状态置为 `ZOMBIE`，不主动释放堆页；但由于引用计数机制的存在，即使未来扩展为真正释放，也不会误释放仍被子进程引用的共享页

---

## 五、测试结果

运行命令：
```bash
spike obj/riscv-pke obj/app_cow
```

关键输出：
```
the physical address of parent process heap is: 0000000087faf000
...
the physical address of child process heap before copy on write is: 0000000087faf000
handle_page_fault: 0000000000400000
the physical address of child process heap after copy on write is: 0000000087fa0000
```

验证：
- **fork 后读取**：子进程与父进程物理地址相同（`87faf000`），证明共享成功
- **写入触发 COW**：输出 `handle_page_fault: 0000000000400000`，证明写权限缺失触发了页故障
- **COW 后写入**：子进程物理地址变为 `87fa0000`，与父进程不同，证明成功分配了新页并复制
