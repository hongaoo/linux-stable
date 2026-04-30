# do_check_insn：单条指令语义与条件跳转收窄

这份文档继续沿 [Documentation/bpf/bpf-do-check-notes.md](Documentation/bpf/bpf-do-check-notes.md) 往下钻，但只盯住两件事：

- [do_check_insn](kernel/bpf/verifier.c#L17567) 如何按指令类别分发验证
- [check_cond_jmp_op](kernel/bpf/verifier.c#L16285) 如何在真假分支上收窄寄存器状态

目标是把 verifier 的“单条指令语义执行”从大循环里剥出来，解释为什么条件跳转在 verifier 里不只是跳转，而是一次状态分裂和状态收窄。

## 1. 先抓一句总话

如果把这一层压成一句话，可以这样记：

> `do_check_insn()` 负责把当前 BPF 指令翻译成对抽象程序状态的局部变换；其中条件跳转最特殊，因为它会把一个状态分成真假两个分支，并分别细化寄存器范围、指针空值性和对象关系。

## 2. `do_check_insn()` 的职责很明确

在 [kernel/bpf/verifier.c](kernel/bpf/verifier.c#L17567) 的 `do_check_insn()`，verifier 基本按指令 class 做分发：

- `BPF_ALU` / `BPF_ALU64` -> 算术/逻辑操作
- `BPF_LDX` -> 读内存
- `BPF_STX` / `BPF_ST` -> 写内存
- `BPF_JMP` / `BPF_JMP32` -> 调用、跳转、退出
- `BPF_LD` -> `ld_imm64` / `ld_abs` 等特殊路径

所以它本质上是 verifier 的“单条指令语义调度器”。

## 3. 为什么条件跳转是最特别的一类指令

普通 ALU/load/store 大多只是：

- 在当前 state 上直接更新寄存器、栈或内存抽象

但条件跳转不是。

条件跳转需要同时回答两件事：

- 当前条件在静态上是否已经可判定
- 如果不可完全判定，真假两边的状态分别会被收窄成什么样

这就是 [check_cond_jmp_op](kernel/bpf/verifier.c#L16285) 这么重要的原因。

## 4. `check_cond_jmp_op()` 的主结构

`check_cond_jmp_op()` 大致可以拆成这几步：

1. 检查源/目的寄存器是否可读
2. 为常量比较准备 fake scalar reg
3. 复制真假分支的寄存器快照
4. 调 `is_branch_taken()` 看分支能否提前判定
5. 若不能提前判定，则 `push_stack()` 出另一分支
6. 在真假两边分别写回收窄后的寄存器状态

这说明 verifier 处理条件跳转的重点不是“更新 pc”，而是“先算条件对状态的约束”。

## 5. `is_branch_taken()` 在做什么

在 [kernel/bpf/verifier.c](kernel/bpf/verifier.c#L15764) 的 `is_branch_taken()`，verifier 会先尝试静态判断当前条件是否已经必真或必假。

它会区分：

- 包数据指针比较
- 指针和 0 的比较
- 标量和标量比较

如果能静态推出结果，就根本不需要真的分成两条路径都继续跑。

这一步本质上就是路径剪枝的前置版：

- 能提前判定分支方向，就直接只保留一条真实路径

## 6. 标量比较为什么会看这么多范围信息

在 [is_scalar_branch_taken](kernel/bpf/verifier.c#L15520) 里，可以看到 verifier 会同时看：

- `var_off`
- `umin/umax`
- `smin/smax`
- 32 位子范围

原因是：单靠某一个范围系统经常不足以排除或确认一条分支。

verifier 需要综合位级不确定性和有符号/无符号区间，才能尽量早地判定分支真假。

## 7. `simulate_both_branches_taken()` 在补什么

当分支真假无法仅靠当前范围直接判定时，verifier 会走 [simulate_both_branches_taken](kernel/bpf/verifier.c#L15493)。

它会：

- 在 FALSE 分支上按反条件收窄两个寄存器
- 在 TRUE 分支上按原条件收窄两个寄存器
- 同步并检查两边是否出现不可能的范围

如果某一边收窄后出现范围矛盾，那一边就是 dead branch。

这说明 verifier 对分支的理解，不只是“现在不知道真假”，而是“尝试把两个世界都模拟一下，看哪个世界自相矛盾”。

## 8. `regs_refine_cond_op()` 是分支收窄的核心

在 [kernel/bpf/verifier.c](kernel/bpf/verifier.c#L15829) 的 `regs_refine_cond_op()`，可以看到不同条件会对范围做不同方向的收窄，例如：

- `JEQ` 会把两边区间交起来
- `JNE` 会在某些常量边界上去掉一个值
- `JLT/JLE/JSGT/...` 会收窄 signed/unsigned 上下界
- `JSET` 会收窄 tnum 位级信息

这一步是真正把“条件语句的逻辑含义”翻译成“寄存器抽象状态变化”的地方。

## 9. 为什么 NULL check 会改写指针类型

在 [kernel/bpf/verifier.c](kernel/bpf/verifier.c#L16045) 的 `mark_ptr_or_null_reg()` 和 [mark_ptr_or_null_regs](kernel/bpf/verifier.c#L16084) 里，可以看到：

- 如果某个寄存器是 `PTR_TO_MAP_VALUE_OR_NULL` 一类可空指针
- 经过 `JEQ/JNE reg, 0` 这样的判断后
- verifier 会在两边分支里分别把它改写成 NULL 标量或非 NULL 指针

这一步非常关键，因为 BPF 里的判空并不是“只留一条布尔事实”，而是直接改变寄存器类型系统。

## 10. 为什么要“批量”改写 related regs

`mark_ptr_or_null_regs()` 不是只改当前比较寄存器，而是会在整个 verifier state 里扫描：

- 同一 `id` 的相关寄存器

原因是多个寄存器可能是同一来源对象的别名视图。

如果只改一个寄存器，不改共享同一对象身份的其它寄存器，就会让状态不一致。

所以在 verifier 里，判空其实是一种“按对象身份传播的类型收窄”。

## 11. `regs_bounds_sanity_check_branches()` 为什么存在

在 [kernel/bpf/verifier.c](kernel/bpf/verifier.c#L15989) 的 `regs_bounds_sanity_check_branches()`，verifier 会在分支收窄后检查真假分支寄存器范围是否违反内部不变量。

这一步的意义是：

- 一旦收窄逻辑产生了自相矛盾的 abstract value
- 说明这个分支不成立，或者有实现错误

因此它更像 verifier 内部的一道“抽象状态健康检查”。

## 12. `push_stack()` 在条件跳转里的角色

在 [check_cond_jmp_op](kernel/bpf/verifier.c#L16378) 之后，可以看到 verifier 会把另一条分支用 `push_stack()` 压起来。

然后：

- 当前路径继续走一边
- 栈里保存另一边的完整 state 快照

所以条件跳转的“另一半世界”并没有丢掉，而是被完整保存起来等 DFS 回溯时继续验证。

## 13. 为什么指针比较会被严格限制

在 `check_cond_jmp_op()` 里还能看到很多 pointer comparison 限制，例如：

- 一些非允许场景下的 pointer comparison 会直接被拒绝
- 某些 packet pointer 场景是例外，会走专门逻辑

这说明 verifier 不是把所有指针都当整数来比较，而是只接受少数它知道如何正确建模语义的比较形式。

这也是“指针类型不是普通整数”的又一个直接体现。

## 14. `do_check_insn()` 与 `do_check()` 的关系怎么记

最简单的记法是：

- `do_check()` 决定走哪条路径、何时回溯、何时剪枝
- `do_check_insn()` 决定当前这条指令对 state 做什么

条件跳转正好是两者交界最强的地方：

- 单条指令语义会决定状态如何分叉
- 路径调度又会决定哪一边先走、哪一边后走

## 15. 一句话压缩版

如果把这条线压成一句话，可以这样记：

> `do_check_insn()` 负责把单条 BPF 指令翻译成抽象状态变换，而 `check_cond_jmp_op()` 则把条件跳转变成“真假两边分别收窄状态”的操作；因此在 verifier 里，分支不是简单改写控制流，而是直接改写寄存器的范围、空值性和对象语义。 

如果想继续把 `do_check_insn()` 里非条件分支的几类核心路径单独拆开，例如 helper/kfunc 调用如何重写 caller-saved 寄存器、load/store 如何经由 `check_mem_access()` 更新抽象状态、`BPF_EXIT` 如何进入返回值和引用泄漏检查，可以继续看补充文档 [Documentation/bpf/bpf-do-check-insn-ops-notes.md](Documentation/bpf/bpf-do-check-insn-ops-notes.md)。
