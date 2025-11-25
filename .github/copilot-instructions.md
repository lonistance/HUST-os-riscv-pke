**Purpose**: Brief, actionable guidance for AI coding agents working on this PKE (Proxy Kernel for Education) repo.

- **Repository focus**: educational proxy OS (PKE) for RISC-V labs. Primary intent is small, incremental kernel exercises driven by user applications in `user/`.

**Big Picture**:
- **Kernel vs User**: `kernel/` contains the proxy kernel (incomplete on purpose). `user/` contains lab applications named `app_*` and a minimal user library. Start from `user/app_helloworld.c` to trace control flow into the kernel.
- **Emulator / Integration**: The repo targets the Spike RISC-V ISA simulator. The `spike_interface/` directory implements host/emulator glue (HTIF, dts parse, memory/file helpers).
- **Build outputs**: all intermediates and final artifacts live in `obj/` (e.g. `obj/riscv-pke`, `obj/app_helloworld`, `obj/util.a`, `obj/spike_interface.a`). Linker scripts are `kernel/kernel.lds` and `user/user.lds`.

**Important files to read first** (order for discovery):
- `user/app_helloworld.c` — example app and starting point for labs.
- `user/user_lib.c` / `user/user_lib.h` — how user-side syscalls are invoked (ecall).
- `kernel/syscall.c` — kernel syscall (trap) entry and dispatch.
- `kernel/strap_vector.S`, `kernel/machine/mentry.S` — low-level trap/entry assembly.
- `kernel/kernel.c`, `kernel/process.c` — kernel main loop and process management.
- `spike_interface/spike_htif.c`, `spike_interface/spike_memory.c` — emulator integration.
- `Makefile` — authoritative build rules, toolchain, and helpful flags (see `CFLAGS`, `CROSS_PREFIX`).

**Project-specific conventions & patterns**:
- Apps always named `app_*` and placed in `user/`. Tests/labs typically modify or extend those files.
- Linker scripts are per-domain: kernel uses `kernel/kernel.lds`, user apps use `user/user.lds`.
- Cross-toolchain prefix is `riscv64-unknown-elf-` (set in `Makefile` as `CROSS_PREFIX`) — tools (gcc/gdb/objdump) are expected to be the RISC-V baremetal toolchain.
- Compiler flags: `-nostdlib`, `-fno-builtin`, `-mcmodel=medany`, `-std=gnu99` etc. Avoid assuming standard C runtime behavior.
- Assembly files end with `.S` and are compiled by the same cross-compiler.

**Build / run / debug workflows (concrete)**:
- Ensure cross toolchain and Spike are installed and on `PATH`. Example:

```bash
export RISCV=/path/to/riscv-toolchain
export PATH=$RISCV/bin:$PATH
make        # builds kernel and user app into obj/
make run    # runs spike with kernel and user app
make gdb    # starts spike + openocd (if installed) and launches cross-gdb using .gdbinit
```

- `make` builds objects into `obj/` and archives `util.a` and `spike_interface.a` before linking the kernel (`obj/riscv-pke`) and `obj/app_helloworld`.
- The repo also includes a VS Code task that compiles the active C file; prefer the `make` flow for full-system builds.

**What AI agents should do when making changes**:
- Start from `user/` app to reproduce expected behavior, then step into kernel syscall/strap/vector to see where behavior should change.
- Preserve the build conventions: maintain linker scripts, do not assume standard libc or dynamic linking.
- When adding tests or example apps, follow `app_*` naming and add to `user/` and ensure `Makefile` wildcard picks them up.

**Integration & external deps**:
- Requires RISC-V cross toolchain (`riscv64-unknown-elf-*`) and Spike (`spike`). Optional: OpenOCD for `make gdb`.
- `Makefile` contains the exact invocation (see `CC`, `COMPILE`, `CFLAGS`). Use it rather than ad-hoc host gcc.

**Notes / gotchas discovered from the code**:
- Code compiles with `-nostdlib` and avoids libc — many usual helpers are absent or reimplemented under `user/` or `util/`.
- The repo is intentionally minimal: kernel is a teaching proxy kernel, not a full OS; do not add large dependencies unless necessary for a lab.
- README warns: do not merge challenge labs (branching policy) — respect lab branching when adding solutions.

If any section is unclear or you want more examples (e.g., a small walkthrough from `app_helloworld` to the trap handler), tell me which part and我会迭代补充。
