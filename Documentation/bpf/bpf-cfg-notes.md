# kernel/bpf/cfg.c 说明

这份文档专门解释 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c) 在 verifier 架构中的角色，以及它内部几个关键算法和数据结构分别在做什么。

## 1. 先定位 cfg.c 的职责

`cfg.c` 最适合被理解成 verifier 的“控制流图分析模块”。

它处理的核心问题不是寄存器数值或 helper 参数，而是：

- 这段 BPF 程序对应的控制流图是什么
- 图里有哪些边
- 有没有非法回边
- 有没有不可达指令
- 特殊控制流例如 callback、indirect goto、abnormal return 该怎么建模
- 后续图算法需要的 postorder、SCC 等结构怎么计算

所以它回答的是“程序图长什么样”，不是“图上状态怎么变”。

## 2. 文件开头的 DFS 伪代码就是阅读地图

[kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L9) 开头直接给了 non-recursive DFS 伪代码，这不是装饰说明，而是整个文件的阅读地图。

它已经把算法的关键元素定义清楚了：

- vertex 是指令
- edge 是控制流跳转或 fall-through
- `DISCOVERED` / `EXPLORED` 表示 DFS 状态
- back-edge 表示回边，也就是 loop 检测的关键线索

因此读 `cfg.c` 最有效的办法不是先看零散函数，而是先把这段伪代码和后面的状态位编码对起来。

## 3. insn_state 和 insn_stack 是第一遍 DFS 的核心状态

在 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L93) 的 `push_insn()` 里可以直接看到：

- `env->cfg.insn_state`
- `env->cfg.insn_stack`

它们分别承担：

- `insn_state`：记录某条指令当前处于未访问、已发现还是已探索，以及某些边是否已打标
- `insn_stack`：显式模拟 DFS 栈，避免递归

这和文件开头的伪代码是一一对应的。

## 4. push_insn() 为什么是第一遍最关键的函数

`push_insn()` 的价值在于：它把“图遍历中的一条边”统一建模了。

它主要做几件事：

- 检查这条边是否已经处理过
- 检查目标指令是否越界
- 对 branch 目标做 prune/jmp point 标记
- 决定这是 tree-edge、back-edge 还是 forward/cross-edge
- 把新发现的节点压入 DFS 栈

如果目标节点已经是 `DISCOVERED` 状态，就意味着出现了回边；在不允许 loop 的情况下，这里就会报错。

所以“BPF verifier 用 DFS 找回边”在代码里最直接的落点就是这个函数。

## 5. visit_insn() 是 CFG 语义分发点

[kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L417) 的 `visit_insn()` 可以看成“控制流图层面的指令语义解释器”。

它按指令类别决定下一步如何扩展图：

- 非跳转指令只有 fall-through 边
- `EXIT` 终止探索
- `CALL` 需要特殊建模
- `JA` 可能是直接跳，也可能是 gotox
- 条件跳转会生成两条边

因此它的作用不是检查寄存器，而是决定“这条指令在 CFG 上产生哪些后继节点”。

## 6. 为什么 CALL 在 cfg.c 里并不简单

在 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L142) 的 `visit_func_call_insn()` 里可以看到，函数调用在 CFG 层面要同时处理：

- 调用后继续执行的 fall-through
- 被调子程序入口
- prune point / jmp point 标记
- callee 效应合并

这说明 verifier 的 CFG 层并没有把 call 当成“普通顺序执行的一步”，而是在显式建模跨子程序控制流。

## 7. gotox 为什么要有 jump table

在 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L346) 的 `visit_gotox_insn()`，可以看到它处理的是“conditional jump with N edges”。

这和普通条件跳转只分两条边不同，它需要一个 jump table 来表示可能目标集合。

相关辅助函数包括：

- `jt_from_map()`
- `jt_from_subprog()`
- `create_jt()`

这些逻辑说明：`cfg.c` 不只是支持最基础的 if/else 图结构，它也在建模更动态的多目标跳转场景。

## 8. 为什么 jump table 要检查是否落在当前 subprog 内

在 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L318) 的 `create_jt()` 里，会专门检查 jump table 的每个目标是否仍然落在当前 subprogram 范围内。

这一步非常关键，因为它保证：

- 间接跳转不会任意跨子程序乱跳
- CFG 的边界仍然维持在可验证的结构范围内

也就是说，这一层不是只在“收集候选目标”，还在保证图结构不破坏 subprog 边界。

## 9. abnormal return 为什么要单独建模

在 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L393) 的 `visit_abnormal_return_insn()`，文件把一些“隐式可能提前退出子程序”的情况单独建模了。

注释里提到的典型例子包括：

- `tail_call` 成功后
- `ld_abs` / `ld_ind` 失败时

这说明 `cfg.c` 并不只看显式写出来的 `EXIT`，而是会把某些隐藏控制流也补进 CFG。

这点很重要，因为 verifier 如果漏掉这些边，后续状态分析就会建立在不完整图之上。

## 10. bpf_check_cfg() 是第一遍的总入口

[kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L551) 的 `bpf_check_cfg()` 就是第一遍 CFG 检查的总入口。

它整体流程很清楚：

1. 分配 `insn_state` 和 `insn_stack`
2. 从第一条指令开始 DFS
3. 反复调用 `visit_insn()` 扩图
4. 节点探索完成后标记 `EXPLORED`
5. 最后检查是否还有不可达指令
6. 额外检查是否跳进 `ldimm64` 中间半条指令

这正好对应 verifier 文档里说的第一遍工作内容。

## 11. 为什么还要检查 ldimm64 的“半条指令”

在 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L619) 一带，可以看到 verifier 会额外检查是否跳进 `ldimm64` 的中间位置。

原因是 `ldimm64` 在 BPF 里占两条 64-bit 指令槽位，逻辑上却是一条宽指令。

所以 CFG 的合法性不只是“目标索引在范围内”，还包括“不能跳进一条宽指令的后半截”。

这类检查很能体现 `cfg.c` 的角色：它不仅关心抽象图，还关心图和指令编码边界之间是否一致。

## 12. postorder 为什么重要

在 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L644) 的 `bpf_compute_postorder()` 里，`cfg.c` 会为各 subprogram 计算指令后序遍历序列。

从图算法角度看，postorder 常常是：

- 做后续图分析的基础顺序
- 在某些数据流分析中更方便自底向上处理节点

这说明 `cfg.c` 并不是“检测完 loop 就结束”，而是在构造后续分析可以复用的图遍历结构。

## 13. SCC 为什么重要

在 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L697) 的 `bpf_compute_scc()` 里，文件继续计算 CFG 的 strongly connected components。

从图论角度看，SCC 的意义是把“相互可达的一组节点”归成一个组件。

在 BPF verifier 语境里，这种信息通常有助于：

- 更清楚地识别循环结构
- 为更高阶 CFG 分析提供基础
- 把某些路径结构按组件来理解，而不是孤立节点来理解

因此，`cfg.c` 已经不只是一个“前置合法性检查器”，而是逐渐在提供系统性的 CFG 分析能力。

## 14. cfg.c 和 verifier.c 的边界

把两者边界记清楚很重要：

- `cfg.c`：图、边、可达性、后继关系、图算法
- `verifier.c`：寄存器状态、栈状态、指针类型、helper/kfunc 参数、引用生命周期

换句话说：

- `cfg.c` 负责先把“路网”建出来
- `verifier.c` 再决定“沿着这些路走时每一步是否安全”

这也是为什么把 CFG 单独拆出来看，会比在 `verifier.c` 里硬啃全部逻辑更容易建立整体理解。

## 15. 读 cfg.c 的推荐顺序

建议按下面顺序读：

1. [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L9) 开头 DFS 伪代码
2. [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L93) 的 `push_insn()`
3. [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L417) 的 `visit_insn()`
4. [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L551) 的 `bpf_check_cfg()`
5. [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L644) 的 `bpf_compute_postorder()`
6. [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L697) 的 `bpf_compute_scc()`

这样读的好处是：先建立“图怎么被走一遍”，再理解“图怎么被进一步分析”。

## 16. 一句话压缩版

如果把 `cfg.c` 压成一句话，可以这样记：

> `kernel/bpf/cfg.c` 是 verifier 的控制流图分析模块，它先把 BPF 程序按图来建模和检查，再提供 jump table、postorder、SCC 等后续分析所需的结构信息。
