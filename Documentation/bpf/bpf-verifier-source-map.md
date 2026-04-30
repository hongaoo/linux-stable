# BPF verifier 文档到源码对照

这份文档把前面 verifier 的抽象说明，对照到当前内核源码里的关键实现位置。重点不是做逐行注释，而是回答：文档里说的“第一遍 CFG/DAG 检查”和“第二遍状态模拟”在源码里分别落在哪，当前打开的 `kernel/bpf/cfg.c` 又在整个 verifier 架构里扮演什么角色。

## 1. 先抓总入口

最值得先看的总入口是：

- [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c)
- [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c)

从当前代码组织看，verifier 已经不再把所有逻辑都塞进一个超大文件里，而是把 CFG 相关逻辑拆到了 `cfg.c`。

这样看会更清楚：

- `verifier.c` 更偏“整体验证框架 + 状态语义 + 寄存器/栈/引用规则”
- `cfg.c` 更偏“控制流图遍历、可达性、跳转结构、图算法辅助信息”

## 2. 文档里的“两遍验证”在源码里怎么对应

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L53) 这一大段注释里，已经把 verifier 的经典两阶段模型直接写出来了：

1. 第一遍是 depth-first search，用来检查程序是不是 DAG，是否有回边、不可达指令、非法跳转
2. 第二遍是从第一条指令开始沿所有路径做状态下降分析，也就是大家通常说的寄存器/栈/指针状态模拟

这段注释本身就是连接文档和源码的最好桥梁。

如果把它映射到文件：

- 第一遍主要落在 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c)
- 第二遍主要落在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c)

## 3. cfg.c 在 verifier 里到底负责什么

当前打开的 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c) 很适合被理解成“verifier 的控制流前置分析模块”。

它主要负责这些事情：

- 遍历 BPF 指令形成控制流图
- 检查跳转是否越界
- 检查是否存在回边
- 检查是否有不可达指令
- 记录某些图结构辅助信息，例如 postorder、SCC 等
- 对 gotox / callback / abnormal return 这类特殊控制流做建模

也就是说，`cfg.c` 处理的是“程序图长什么样”，而 `verifier.c` 更多处理“沿着这张图走的时候状态怎么变”。

## 4. 第一遍 CFG/DAG 检查的核心入口

在 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L551) 可以看到 `bpf_check_cfg()`，这是第一遍 CFG 检查最核心的入口之一。

从结构上看，它做的事情很符合文档里那段描述：

- 为指令状态和 DFS 栈分配空间
- 从入口点开始遍历控制流
- 调用 `visit_insn()` 按指令类型扩展边
- 遍历结束后检查是否有不可达指令

因此，如果你想从源码上理解“为什么 verifier 先做 DAG/可达性检查”，`bpf_check_cfg()` 就是最该先看的函数。

## 5. push_insn() 是第一遍图遍历的关键动作

在 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L93) 的 `push_insn()` 里，可以直接看到 DFS 风格遍历的核心逻辑。

它对应的其实就是文件开头那段“non-recursive DFS pseudo code”。

这里几个点尤其重要：

- 它会区分 `FALLTHROUGH` 和 `BRANCH`
- 会检查跳转目标是否越界
- 会根据 `insn_state` 区分未发现、已发现、已探索状态
- 遇到已发现节点时，会把它识别成 back-edge 并在不允许的情况下报错

这正好对应文档里“第一遍要拒绝 loop、非法跳转、不可达代码”的说法。

所以如果你在文档里看到“通过 DFS 检查 DAG”，在源码里最直接的落点就是 `push_insn()` 配合 `bpf_check_cfg()`。

## 6. 为什么 cfg.c 开头那段伪代码很重要

[kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L9) 开头直接写了非递归 DFS 伪代码，而且后面的状态编码和遍历方式都紧贴这段说明。

这很重要，因为它告诉你：

- 这里不是“顺便扫一下跳转”
- 而是在显式地把 BPF 程序当作一张图来处理

因此，理解 verifier 第一遍时，不要只把它看成“跳转合法性检查”，而应把它看成“控制流图构建与遍历”。

## 7. visit_insn() 是 CFG 扩边的调度点

在 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L417) 的 `visit_insn()`，可以把它看成“按指令语义决定 CFG 下一步怎么走”的分发点。

它会针对不同指令类型决定：

- 只有 fall-through
- 有 branch 和 fall-through 两条边
- 是函数调用，需要同时考虑 call 前进和 callee 入口
- 是特殊 gotox / callback / abnormal return 情况

这很像“控制流层面的指令语义解释器”。

换句话说，`visit_insn()` 不是在做寄存器状态模拟，而是在决定“图上有哪些边存在”。

## 8. 函数调用在 CFG 层面是怎么建模的

在 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L142) 的 `visit_func_call_insn()` 可以看到，函数调用在 CFG 层面不是一条简单边。

它至少会显式处理：

- 调用后的 fall-through 路径
- 需要访问 callee 时的 branch 到被调子程序入口
- prune/jmp point 标记
- callee 效应合并

这说明 verifier 在第一遍里并不只是把 call 当普通顺序执行，而是把它纳入了跨子程序控制流图建模。

## 9. unreachable insn 的文档描述在源码哪里落地

文档里反复提到 verifier 会拒绝不可达指令，对应源码上最直观的位置就是 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L615)。

这里在 CFG 遍历完成后，会检查哪些指令状态仍然没有被访问到，并打印：

- `unreachable insn %d`

所以“不可达代码检测”不是概念性的描述，而是在 `bpf_check_cfg()` 尾部有直接实现和报错逻辑。

## 10. postorder 和 SCC 为什么会在 cfg.c 里出现

在 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L644) 和 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L697) 分别能看到：

- `bpf_compute_postorder()`
- `bpf_compute_scc()`

这说明 `cfg.c` 不只是做最基础的 DAG 检查，它还在为后续更复杂的图分析准备结构信息。

从架构角度看，这意味着 verifier 已经把“程序是一张图”这件事贯彻得比较彻底，而不是只在第一遍里临时 DFS 一次就结束。

## 11. 第二遍状态模拟主要落在哪里

和 `cfg.c` 相比，[kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c) 的核心职责更接近：

- 定义寄存器和栈状态语义
- 沿路径模拟每条指令对状态的影响
- 检查 helper / kfunc 参数约束
- 处理引用对象、可空指针、dynptr、iter 等高级对象状态
- 管理分支状态栈与状态剪枝

文件开头的大段注释本身就已经把这些语义讲得很清楚，例如：

- R1 初始为 `PTR_TO_CTX`
- 调 helper 后 R1-R5 会变成不可读
- `PTR_TO_MAP_VALUE_OR_NULL` 经判空后会收窄成可用指针
- helper 参数会根据 prototype 进行检查

所以第二遍的主线可以理解为：

> 按第一遍确认过的合法控制流图，逐步推进并验证每个点上的寄存器/栈/对象状态。

## 12. verifier.c 开头注释为什么值得反复读

[kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L53) 到前面一大段注释，价值非常高，因为它同时交代了：

- 两遍验证模型
- 寄存器角色
- 指针状态模型
- helper 参数检查思路
- 引用类型的生命周期约束

也就是说，这段注释本身就已经是“源码版 verifier 文档”。

如果要从抽象理解过渡到实现，这里通常是比直接扎进具体函数更好的入口。

## 13. 当前这套代码组织反映了什么架构思路

从 `cfg.c` 被独立拆出来这件事，本身就能看出 verifier 代码组织的一个趋势：

- 把“图结构与控制流问题”独立出来
- 把“状态语义和对象约束问题”留在 verifier 主逻辑里

这和我们前面的文档分层是高度一致的：

- CFG 层回答“程序图是否合法、哪些路径存在”
- 状态层回答“沿这些路径走时，每一步是否安全”

所以当前源码结构，某种程度上正好印证了前面文档里的那种理解方式。

## 14. 读源码时建议的顺序

如果你想把 verifier 文档和源码对上，建议顺序如下：

1. 先读 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L53) 开头总注释
2. 再读 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L9) 开头 DFS 伪代码
3. 再看 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L551) 的 `bpf_check_cfg()`
4. 再看 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L93) 的 `push_insn()` 和 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c#L417) 的 `visit_insn()`
5. 然后回到 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c)，按寄存器状态、helper 参数检查、引用跟踪这些主题继续读

这个顺序的好处是：先建立图，再理解状态，不容易把两层逻辑混在一起。

## 15. 一句话压缩版

如果把 verifier 的源码分工压成一句话，可以这样记：

> `kernel/bpf/cfg.c` 主要负责把 BPF 程序当控制流图来检查和整理，而 `kernel/bpf/verifier.c` 主要负责在这张图上做寄存器、栈、指针、helper/kfunc 约束等状态级安全证明。

如果想继续把 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c) 单独展开成“DFS、jump table、postorder、SCC 各自做什么”的算法说明，可以继续看补充文档 [Documentation/bpf/bpf-cfg-notes.md](Documentation/bpf/bpf-cfg-notes.md)。

如果想继续沿 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c) 深挖“寄存器状态、helper 参数检查、引用跟踪”这三条主线，可以继续看补充文档 [Documentation/bpf/bpf-verifier-core-notes.md](Documentation/bpf/bpf-verifier-core-notes.md)。

如果想继续把 [bpf_check](kernel/bpf/verifier.c#L19900) 这条总入口拆成“early BTF、subprog、CFG/postorder/live analysis、main/subprog do_check、后处理 fixup/optimize/runtime 选择”的完整流水线，可以继续看补充文档 [Documentation/bpf/bpf-check-main-flow-notes.md](Documentation/bpf/bpf-check-main-flow-notes.md)。
