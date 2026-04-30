# do_check_insn 继续拆：helper、load/store、exit

这份文档继续沿 [Documentation/bpf/bpf-do-check-insn-notes.md](Documentation/bpf/bpf-do-check-insn-notes.md) 往下钻，但不再讲条件跳转，而是专门看 `do_check_insn()` 里另外几条最关键的单指令路径：

- [check_load_mem](kernel/bpf/verifier.c#L6585)
- [check_store_reg](kernel/bpf/verifier.c#L6618)
- [check_helper_call](kernel/bpf/verifier.c#L10262)
- [process_bpf_exit_full](kernel/bpf/verifier.c#L17415)
- 以及 [do_check_insn](kernel/bpf/verifier.c#L17567) 里对 `BPF_LD` / `BPF_CALL` / `BPF_EXIT` 的分发

目标是把 verifier 对“非分支指令”的语义执行讲清楚：它们不是简单地看一眼 opcode，而是把内存模型、调用约束、返回值语义和引用生命周期一起推进。

## 1. 一句总话

如果先压成一句话，可以这样记：

> `do_check_insn()` 对 helper/load/store/exit 这几类指令的处理，本质上是在维护三件事：内存访问是否合法、调用后寄存器和引用状态如何变化、程序退出时返回值和资源状态是否满足 program type 规则。

## 2. `do_check_insn()` 如何分发这些路径

在 [kernel/bpf/verifier.c](kernel/bpf/verifier.c#L17567) 里，这几条路径的入口很清晰：

- `BPF_LDX` -> `check_load_mem()`
- `BPF_STX` / `BPF_ST` -> `check_store_reg()` 或 `check_mem_access()`
- `BPF_JMP/BPF_JMP32 + BPF_CALL` -> `check_func_call()` / `check_kfunc_call()` / `check_helper_call()`
- `BPF_EXIT` -> `process_bpf_exit_full()`
- `BPF_LD` -> `check_ld_abs()` / `check_ld_imm()`

所以这里最重要的不是 opcode 分类本身，而是每个分支都接到一套不同的状态更新规则。

## 3. `check_load_mem()` 并不只是“允许读内存”

在 [check_load_mem](kernel/bpf/verifier.c#L6585) 里，路径大致是：

1. 检查 `src_reg` 可读
2. 检查 `dst_reg` 作为写目标是否合法
3. 调 `check_mem_access()` 验证 `(src + off)` 的读是否合法
4. 由 `check_mem_access()` 更新 `dst_reg` 的抽象状态
5. `save_aux_ptr_type()` 记录这条指令看到的 pointer type
6. 再做一次 `reg_bounds_sanity_check()`

这说明 load 指令对 verifier 来说不是“返回一个整数”，而是一次类型传播。

## 4. 为什么 load 会调用 `save_aux_ptr_type()`

`save_aux_ptr_type()` 的目的，是记住这条 load 指令在不同路径上看到的 base pointer type。

这样后续如果同一条 load 指令在另一条路径上被拿去读另一种不兼容 pointer，verifier 可以拒绝它。

这点很关键，因为 verifier 关心的不是“这次 load 单独合法吗”，而是“同一条指令在所有可能路径上是否语义一致”。

## 5. `check_store_reg()` 的重点是写目标，不是源值本身

在 [check_store_reg](kernel/bpf/verifier.c#L6618) 里，可以看到核心步骤是：

- 检查 `src_reg` 可读
- 检查 `dst_reg` 作为被写内存基址是否合法
- 调 `check_mem_access()` 验证 `(dst + off)` 是否可写
- 记录这条 store 指令看到的 pointer type

所以 store 路径的核心不是“值从哪来”，而是“你到底在往谁写、这个位置能不能写”。

## 6. `BPF_ST` 为什么还要单独走一条路

`BPF_ST` 是立即数写内存，因此在 [do_check_insn](kernel/bpf/verifier.c#L17567) 里没有走 `check_store_reg()`，而是直接：

- 检查 `dst_reg`
- 调 `check_mem_access()` 验证写目标
- 再 `save_aux_ptr_type()`

区别在于：

- `BPF_STX` 是寄存器到内存
- `BPF_ST` 是立即数到内存

但两者都共享同一个更本质的问题：目标内存是否合法。

## 7. `check_helper_call()` 首先在验证“你能不能调这个 helper”

在 [check_helper_call](kernel/bpf/verifier.c#L10262) 里，第一层不是参数检查，而是 helper 能力入口检查：

- 这个 helper ID 是否存在
- 当前 program type 是否允许调用
- 是否要求 GPL 程序
- sleepable helper 是否处在 sleepable context

这说明 helper 并不是“只要知道 ID 就能调”，而是能力面本身受 program type 和上下文约束。

## 8. helper 参数检查是 verifier 的另一条主线

`check_helper_call()` 接着会逐个调用 `check_func_arg()` 检查 `R1-R5`。

这一步的本质是：

- helper prototype 并不只是文档说明
- verifier 真正拿它来决定参数类型、可空性、读写方向、buffer 大小、对象引用关系是否成立

因此 helper 调用是 verifier 类型系统的一个重要出口，而不是单纯的“外部函数调用”。

## 9. 为什么 helper 调用后 caller-saved 寄存器会被清掉

在 `check_helper_call()` 后半段，可以看到 verifier 会 reset caller-saved regs，并重新定义 `R0`。

这是 BPF ABI 在 verifier 里的直接落地：

- `R1-R5` 作为参数寄存器，helper 调用后不能再假定保留原值
- `R0` 作为返回值寄存器，被重新赋予新的抽象类型

所以 helper call 对 verifier 来说，本质上是一次 ABI 边界穿越。

## 10. `R0` 的返回类型为什么这么复杂

在 [check_helper_call](kernel/bpf/verifier.c#L10478) 附近的返回值处理里，可以看到 helper return type 不只是整数，还可能是：

- `SCALAR_VALUE`
- `PTR_TO_MAP_VALUE`
- `PTR_TO_SOCKET`
- `PTR_TO_MEM`
- `PTR_TO_BTF_ID`
- 以及各种带 `PTR_MAYBE_NULL`、`MEM_RDONLY`、`MEM_ALLOC` 之类 flag 的组合

这说明 helper 调用其实是 verifier 类型系统的重要“生产者”。

很多后续判空、解引用、引用跟踪，都是从这里把 `R0` 的语义引入程序状态中的。

## 11. helper 调用还会改变引用生命周期

`check_helper_call()` 里还有一条特别重要的线：

- release helper 会释放 reference
- 某些 helper 会建立 callback state
- 某些 helper 会改变 packet/data pointer 有效性

所以 helper 不只是在产出返回值，它还可能：

- 修改整个 verifier state 里的引用图
- 打开新的 callback frame
- 让已有指针失效

这就是为什么 helper call 必须由 verifier 内建语义处理，而不能只当“黑盒函数”。

## 12. `check_ld_imm()` / `check_ld_abs()` 为什么也是特殊路径

在 [check_ld_imm](kernel/bpf/verifier.c#L16524) 和 [check_ld_abs](kernel/bpf/verifier.c#L16646) 里，`BPF_LD` 类指令并没有按普通 load/store 处理。

原因是它们经常承载特殊语义：

- `ld_imm64` 可能装入常量、map、BTF ID、pseudo func 等
- `ld_abs/ld_ind` 更像特殊 helper-like 访问，会重置 caller-saved regs，并在 `R0` 写结果

也就是说，`BPF_LD` 的某些 opcode 在 verifier 里更接近“语义化内建操作”，而不是普通内存读。

## 13. `process_bpf_exit_full()` 为什么是退出语义的真正入口

在 [process_bpf_exit_full](kernel/bpf/verifier.c#L17415) 里，verifier 对 `BPF_EXIT` 的处理并不是“程序结束”。

它至少要做三件事：

- 检查 reference/resource leak
- 如果是子程序返回，做 frame exit 准备
- 如果是最终返回，检查 `R0` 是否满足 program type 的返回值约束

所以 exit 指令在 verifier 里本质上是一次“最终一致性检查”。

## 14. 为什么退出时还要区分 main prog、subprog、callback

`process_bpf_exit_full()` 会区分：

- nested function exit
- regular global subprogram exit
- main program exit
- async/exception callback exit

原因是不同退出点的语义不同：

- 子程序主要关心 frame 收束
- 主程序关心 program type 的 return code 语义
- callback 还可能有自己的 scheduling/exception 约束

这说明 `EXIT` 在 verifier 里并不是统一的一种返回语义，而是跟当前执行上下文强绑定。

## 15. 返回值检查其实是 program type 语义的一部分

`process_bpf_exit_full()` 最终会调用 [check_return_code](kernel/bpf/verifier.c#L16867) 或全局子程序的返回检查逻辑。

这里 verifier 会验证：

- `R0` 是否已初始化
- `R0` 是否是标量而不是泄漏地址的 pointer
- `R0` 是否落在当前 program type 允许的值域里

所以 return value 不是“程序员自己的约定”，而是 hook 语义的一部分。

## 16. 用三条线记住这些路径

可以把这几类路径压成三条主线：

- load/store：通过 `check_mem_access()` 维护内存模型和 pointer type 一致性
- helper/call：通过 prototype、ABI 和返回类型维护寄存器/引用状态
- exit：通过 leak check 和 return code check 做路径收束

## 17. 一句话压缩版

如果把这条线压成一句话，可以这样记：

> verifier 对 helper、load/store、exit 的处理，本质上是在维护“内存是否合法、调用边界后寄存器和引用怎么变、退出时结果是否符合 hook 语义”这三件事；因此这些指令在 `do_check_insn()` 里都不是普通 opcode，而是完整的抽象状态转换入口。 
