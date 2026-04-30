# bpf_check 主流程：verifier 的总入口是怎么推进的

这份文档专门顺着 [bpf_check](kernel/bpf/verifier.c#L19900) 往下读一次 verifier 的主流程，目标不是解释每个细节函数，而是把“BPF_PROG_LOAD 进入 verifier 之后到底会按什么顺序推进”压成一条清楚的流水线。

## 1. 先抓一句总话

如果把 `bpf_check()` 压成一句话，可以这样记：

> `bpf_check()` 先把程序和元数据整理成 verifier 可分析的内部环境，再做 BTF/subprog/CFG/数据流准备，随后分别验证 main program 和 subprog，最后再做一轮指令修补、优化和运行时选择。

这意味着 verifier 并不是“拿到字节码就直接逐条模拟”，而是分成明显的前处理、主体验证和后处理三大段。

## 2. 第一段：创建 verifier 环境

在 [kernel/bpf/verifier.c](kernel/bpf/verifier.c#L19900) 开头，`bpf_check()` 先做的是建立 `bpf_verifier_env`。

这里会准备：

- `env`
- `insn_aux_data`
- `succ`
- `env->prog`
- `env->ops`
- 各种 capability / token 相关开关

这一步的意义是：把来自 syscall 层的 `bpf_prog` 对象，包装成 verifier 后续所有分析都依赖的统一工作上下文。

## 3. 第二段：日志、fd array 和基础运行条件

在正式分析程序前，`bpf_check()` 还会初始化：

- verifier log
- `process_fd_array()`
- alignment / privilege / test flags
- `explored_states` 哈希表

这里最值得注意的是 [process_fd_array](kernel/bpf/verifier.c#L19634)。

它会把用户传进来的 fd array 提前解析成：

- used maps
- used BTFs

这说明 verifier 并不是只分析“程序文本”，它也会把程序可能依赖的外部对象先纳入分析环境。

## 4. 第三段：early BTF 进入点

在 [kernel/bpf/verifier.c](kernel/bpf/verifier.c#L19992) 的 `bpf_check_btf_info_early()`，BTF 会在 very early 阶段进入 verifier 流程。

这个函数的实现不在 verifier.c，而在 [kernel/bpf/check_btf.c](kernel/bpf/check_btf.c#L410)。

它做的核心事情是：

- 通过 `prog_btf_fd` 取到 program BTF
- 写入 `prog->aux->btf`
- 先做 `func_info` 的 early 校验

这一步很重要，因为它说明 BTF 不是 verifier 的末尾附属品，而是后续 subprog 解析和函数原型理解的前置材料。

## 5. 第四段：subprog 建图与基本合法性检查

接下来 `bpf_check()` 会走：

- `add_subprog_and_kfunc()`
- `check_subprogs()`

其中 [check_subprogs](kernel/bpf/verifier.c#L3155) 做的是非常基础但很关键的事情：

- 检查 jump 不得跨 subprog 边界
- 记录 tail call / ld_abs 等特征
- 检查 subprog 末尾必须是 exit 或合法跳转

这一步还不是完整 CFG 验证，但它先把“程序被拆成哪些函数边界”这件事固定下来。

## 6. 第五段：完整 BTF 信息进入点

在 [kernel/bpf/verifier.c](kernel/bpf/verifier.c#L20004) 的 `bpf_check_btf_info()`，verifier 再继续处理完整 BTF 元信息。

对应实现仍然在 [kernel/bpf/check_btf.c](kernel/bpf/check_btf.c#L438)。

这里会继续验证：

- `func_info`
- `line_info`
- `core_relo`

也就是说：

- early BTF 更像“先把 program BTF 接进来，先验证最基本的 func 边界材料”
- late BTF 更像“把函数、源码行、CO-RE relocation 全部真正接到 verifier 里”

## 7. 第六段：指令解析与 CFG 准备

在进入主体状态模拟前，还会经过：

- `check_and_resolve_insns()`
- `bpf_check_cfg()`
- `bpf_compute_postorder()`
- `bpf_stack_liveness_init()`

这说明 verifier 不是先做抽象执行，再顺便看控制流；它会先把控制流和遍历顺序准备好。

其中：

- `bpf_check_cfg()` 对应 CFG/DAG 合法性
- `bpf_compute_postorder()` 为后续数据流分析和子程序排序准备遍历顺序

## 8. 第七段：attach 语义检查真正进入 verifier

在 [kernel/bpf/verifier.c](kernel/bpf/verifier.c#L20030) 的 `check_attach_btf_id()`，程序类型、attach 类型、attach_btf_id 对应的目标函数语义，才真正与 verifier 主体接上。

这个函数会根据 program type 做不同处理，例如：

- tracing
- LSM
- EXT
- struct_ops

它不只是检查 `attach_btf_id` 合不合法，还会：

- 解析 attach target
- 填充 `attach_func_proto`
- 在 `EXT` 场景下继承目标程序类型语义
- 计算 trampoline key

这一步说明 attach 语义并不是 syscall.c 单独决定完就结束了，verifier 里还要把它转成后续分析真正可用的目标函数模型。

## 9. 第八段：真正进入数据流准备期

在主体 `do_check` 之前，`bpf_check()` 还会连续做一串分析准备：

- `bpf_compute_const_regs()`
- `bpf_prune_dead_branches()`
- `sort_subprogs_topo()`
- `bpf_compute_scc()`
- `bpf_compute_live_registers()`
- `mark_fastcall_patterns()`

这串函数很能体现 verifier 的真实面貌：

- 它不是一遍简单解释器
- 它更像一个带多阶段静态分析的编译器前端/中端

尤其是 [sort_subprogs_topo](kernel/bpf/verifier.c#L3205) 与 SCC/live-register 分析，说明 verifier 会先整理调用图和活跃性信息，再进入真正的逐路径状态证明。

## 10. 第九段：主体验证分成 main 和 subprog 两块

真正的主体验证入口是：

- [do_check_main](kernel/bpf/verifier.c#L18882)
- [do_check_subprogs](kernel/bpf/verifier.c#L18832)

`bpf_check()` 的顺序是：

1. 先验证 main program
2. 再验证 global subprogs

这里的设计很关键。

main program 是程序真实入口；而 global subprog 会按“已被调用且尚未验证”的原则，反复遍历直到全部验证完。

这也是为什么 verifier 不是简单“按函数文本顺序验证每个函数”。

## 11. `do_check_subprogs()` 的要点是什么

在 [kernel/bpf/verifier.c](kernel/bpf/verifier.c#L18832) 里，`do_check_subprogs()` 的核心思路是：

- 只验证 global subprog
- 只验证真正被调用的 subprog
- 每次验证出新的 global subprog 后，再回头扫一遍

这样做的目的是把“全局函数原型安全”和“被主程序实际触达的调用图”结合起来，而不是无差别验证所有文本片段。

## 12. 第十段：主体验证后并没有结束

`do_check_main()` / `do_check_subprogs()` 返回成功之后，`bpf_check()` 还会继续走后处理路径：

- `bpf_remove_fastcall_spills_fills()`
- `check_max_stack_depth()`
- `bpf_optimize_bpf_loop()`
- dead code sanitize / remove
- `bpf_convert_ctx_accesses()`
- `bpf_do_misc_fixups()`
- `bpf_opt_subreg_zext_lo32_rnd_hi32()`
- `bpf_fixup_call_args()`

这说明 verifier 通过之后，程序还会被进一步修补和规范化，才会变成最终准备交给 runtime/JIT 的版本。

## 13. `convert_pseudo_ld_imm64()` 和 runtime 选择为什么放最后

在 [kernel/bpf/verifier.c](kernel/bpf/verifier.c#L20163) 之后，可以看到：

- `convert_pseudo_ld_imm64(env)`
- `adjust_btf_func(env)`
- `__bpf_prog_select_runtime(...)`

这里说明两件事：

1. verifier 期间程序里还允许保留某些 pseudo 形式，等验证通过后再转成最终形式
2. verifier 的终点不是“给出 yes/no”，而是把程序整理到可进入最终 interpreter/JIT runtime 的状态

## 14. 用一条流水线记住 `bpf_check()`

可以把 [bpf_check](kernel/bpf/verifier.c#L19900) 压成下面这条顺序：

1. 建立 verifier env
2. 初始化 log / fd array / privilege flags
3. early BTF 接入
4. subprog/kfunc 边界发现
5. 完整 BTF、line_info、CO-RE relocation 接入
6. CFG/postorder/stack liveness 准备
7. attach_btf_id 与目标函数语义检查
8. const reg / dead branch / topo / SCC / live register 准备
9. `do_check_main()` + `do_check_subprogs()`
10. stack depth / loop / dead code / ctx access / misc fixup / zext / call arg fixup
11. pseudo 指令转换、BTF 调整、runtime 选择

## 15. 一句话压缩版

如果把这条线再压成一句话，可以这样记：

> `bpf_check()` 不是单次解释执行，而是一条多阶段验证流水线：先接入程序/BTF/attach 语义并建立 CFG 与子程序关系，再执行主体状态证明，最后做指令修补、优化和 runtime 选择，把程序从“待验证对象”变成“可执行 BPF 程序对象”。

如果想继续把主体状态证明本身拆开，看 `do_check()` 如何逐指令推进、如何用 `push_stack()` 处理分支、如何用 visited-state 剪枝，可以继续看补充文档 [Documentation/bpf/bpf-do-check-notes.md](Documentation/bpf/bpf-do-check-notes.md)。
