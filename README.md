# -os-riscv-pke
保存我在os实验所做的工作，预计会实现lab1-4的3个基础实验和第一个挑战实验，实验原项目参见https://gitee.com/hustos
指导在pke-doc目录下，源代码在riscv-pke下。所有关卡以通过头歌关卡为最低目标，因此存在很多可以改进的地方
3个基础实验的答案可以查看实验指导书（如果有），但那本书其实没什么用
头歌平台上没法继承前面的工作，开始每一关实验前，必须从lab1-1开始手动重做一遍工作（一般只要重做lab1-1等少数几个实验的工作，但为了以防万一，建议全部重做完）
每个关卡改动如下：
lab1-1: strap.c的handle_syscall函数
lab1-2: machine/mtrap.c的handle_mtrap函数
lab1-3: strap.c的handle_mtimer_trap函数

lab2-1: vmm.c的user_va_to_pa函数
lab2-2: vmm.c的user_vm_unmap函数
lab2-3: strap.c的handle_user_page_fault函数
（说明：头歌平台出现case语句直接接变量声明 case CONST_NUM:
                        declaration variable;
      会出现报错，解决方法是在声明前加一个分号; 这就是本地环境能过，头歌报错的原因）

lab1 challenge1: 这关要修改以下文件：
user_lib.h,user_lib.c：添加print_backtrace函数的定义与实现
syscall.h,syscall.c：添加并实现sys_print_backtrace函数，引用elf.h头
elf.h：添加数据结构段头表elf_sect_header，符号表项elf_symbol，用extern关键字定义好的“函数名-入口地址”结构体func_names，以及一些有必要的常量和外部变量（列出来的是必须实现的）
elf.c：整个实验的核心，需要定义elf_load_symbol函数，在load_bincode_from_host_elf函数中被调用，load_bincode_from_host_elf会读取elf头，并根据头的entry变量指定进程入口，这（可能）是唯一能读出elf头的机会（除非你彻底理解操作系统的每一条语句）。一个指导性的思路是将函数符号-地址对应的工作全部放在elf.c中做，再将结果保存在func_names中，传入func_names.c，从而使sys_print_backtrace函数根据返回地址查找函数符号名的工作不再直接依赖.strlab,.symtab。即“在elf.c读取一次，结果保存并传入syscall.c”。
完整步骤包括：1. elf_header ehdr = elf_ctx ctx->ehdr 读出elf头 2. 根据节头表读出“包含elf各节符号名的节”的节头表elf_sect_header shstr_sh 3. 找到节符号为.strlab和.symtab的节的节头表 4. 可以把.symbol节理解为符号表项elf_symbol数组，便利读取每个符号表项（得到函数入口地址和名字在.strlab的地址），建立函数符号-地址对 
另外需要指出，根据fp = tf->regs.s0; fp = *(uint64 *)fp; 追踪函数调用过程，得出的帧栈大小是实际的两倍（帧栈的实际大小为0x10b），建议直接用fp = fp + 0x10; 

lab2 challenge1: 实现函数功能一分钟，通过头歌半小时。甚至可以根据答案判断stval是否越界。但是头歌需要注释掉除strap.c的handle_user_page_fault外所有文件的print语句，宁可错杀，绝不放过。
