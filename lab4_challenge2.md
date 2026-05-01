# lab4_challenge2: 实现 exec 系统调用

## 实验目标
实现 `exec(path)` 系统调用，使得当前进程可以替换自身的内存映像，加载并执行一个新的 ELF 可执行文件。

## 涉及文件与修改

### 1. `kernel/syscall.h`
- 添加 `SYS_user_exec` 宏定义：
```c
#define SYS_user_exec (SYS_user_base + 30)
```

### 2. `kernel/syscall.c`
- 实现 `sys_user_exec(char *pathva)` 函数：
  1. 通过 `user_va_to_pa` 将用户态路径字符串转为物理地址
  2. 使用 `do_open(pathpa, O_RDONLY)` 打开目标 ELF 文件
  3. **清理旧的 CODE/DATA 段**：遍历 `current->mapped_info`，对 CODE_SEGMENT 和 DATA_SEGMENT：
     - 遍历每个页，通过 `page_walk` 找到 PTE
     - `free_page` 释放物理页
     - 将 PTE 清零（**必须清零**，否则后续 `user_vm_map` / `map_pages` 会因 `PTE_V` 已设置而 panic）
     - 将 `mapped_info[i]` 清零
  4. **重置 heap**：
     - `heap_top = heap_bottom = USER_FREE_ADDRESS_START`
     - `free_pages_count = 0`
     - `mapped_info[HEAP_SEGMENT].npages = 0`
  5. **加载新 ELF**：
     - 使用 `vfs_read`（**不能用 `do_read`**，`do_read` 内部使用 `strcpy`，遇到二进制数据中的 `\0` 会截断）
     - 读取 ELF header，检查 `magic`
     - 遍历 program header，对每个 `ELF_PROG_LOAD` 段：
       - `alloc_page()` 分配物理页，`memset` 清零
       - `user_vm_map` 建立映射（权限通常为 `PROT_WRITE | PROT_READ | PROT_EXEC`）
       - `vfs_read` 读取段内容到物理页
       - 根据 `ph_addr.flags` 记录到 `mapped_info`（CODE_SEGMENT 或 DATA_SEGMENT）
     - 设置 `current->trapframe->epc = ehdr.entry`
     - 设置 `current->trapframe->regs.sp = USER_STACK_TOP`
     - `do_close(fd)`
     - 返回 0
- 在 `do_syscall` 中添加 `SYS_user_exec` 的分发：
```c
case SYS_user_exec:
  return sys_user_exec((char *)a1);
```

**注意**：`exec` 从用户程序视角是"不返回"的。`sys_user_exec` 返回 0 后，trap handler 会继续执行 `switch_to(current)`，用户进程从新程序的 `epc` 开始执行。返回 -1 仅表示加载失败。

### 3. `user/user_lib.h` / `user/user_lib.c`
- 添加用户态封装：
```c
int exec(const char *path) {
  return do_user_call(SYS_user_exec, (uint64)path, 0, 0, 0, 0, 0, 0);
}
```

### 4. `kernel/elf.c`
- 修改 `load_bincode_from_host_elf`：
  - 改为接受显式路径参数 `const char *path`
  - 如果 `spike_file_open(path)` 失败，尝试在路径前加上 `./hostfs_root` 再打开（适配 VFS 路径解析）
```c
info.f = spike_file_open(path, O_RDONLY, 0);
if (IS_ERR_VALUE(info.f)) {
  char host_path[256];
  strcpy(host_path, "./hostfs_root");
  strcat(host_path, path);
  info.f = spike_file_open(host_path, O_RDONLY, 0);
  if (IS_ERR_VALUE(info.f)) panic("...");
}
```

### 5. `kernel/kernel.c`
- 修改 `load_user_program()`：
  - 调用 `load_bincode_from_host_elf(proc, arg_bug_msg.argv[0]);`
  - 替代原来无参的版本

### 6. `Makefile`
- 设置 `HOSTFS_ROOT := hostfs_root`
- 修改用户程序构建目标，输出到 `$(HOSTFS_ROOT)/bin/` 目录下
- 本实验需要构建两个应用：`app_exec` 和 `app_ls`
- `make run` 时启动 `spike $(KERNEL_TARGET) /bin/app_exec`

## 关键坑点

1. **`do_read` 不能用**：`do_read` 内部用 `strcpy` 拷贝缓冲区内容，遇到 ELF 文件中的 `\0` 字节会截断，导致加载失败。必须用 `vfs_read` 直接读取二进制数据。

2. **`map_pages` 会 panic**：如果旧 PTE 的 `PTE_V` 位仍然置位，`map_pages` 会 panic。因此在加载新程序前，必须手动遍历旧 CODE/DATA 段，将对应的 PTE 清零（同时释放物理页）。

3. **heap 必须重置**：如果不重置 `user_heap`，新程序可能会错误地继承旧程序的堆状态。

4. **路径解析**：用户程序通过 VFS 访问 `/bin/app_ls`，实际对应主机上的 `./hostfs_root/bin/app_ls`。`spike_file_open` 直接打开 `/bin/app_ls` 会失败，需要在 `load_bincode_from_host_elf` 中做 `./hostfs_root` 前缀回退。

5. **头歌平台注意事项**：
   - 头歌无法继承前面的工作，开始前需要确保 lab1-1 到 lab4-3 的所有基础代码已补全（不能留 panic）。
   - 如遇到编译报错，检查 `case` 语句后直接跟变量声明的情况（头歌编译器较老），可在声明前加 `;` 解决。
