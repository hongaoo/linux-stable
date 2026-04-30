# do_check：verifier 主循环、分支入栈与状态剪枝

这份文档继续沿 [bpf_check](kernel/bpf/verifier.c#L19900) 往下钻，但只盯住 verifier 的主体循环：

- [do_check](kernel/bpf/verifier.c#L17663)

目标是把“逐指令状态模拟”这件事从源码里的大循环压缩成一条可读主线，尤其是：

- verifier 怎么一条条推进指令
- 条件分支为什么会变成一棵 DFS 搜索
- `push_stack()` / `pop_stack()` 在干什么
- 状态剪枝为什么能显著降低复杂度

## 1. 先抓一句总话

如果把 `do_check()` 压成一句话，可以这样记：

> `do_check()` 维护一份当前 verifier state，并按 DFS 方式沿控制流逐指令推进；遇到分支时复制状态压栈，遇到已见等价状态时提前剪枝，直到所有可达路径都被证明安全或某条路径报错为止。

这说明 verifier 主循环本质上不是“解释程序执行”，而是在做“带状态缓存和剪枝的路径搜索”。

## 2. `do_check()` 的外形其实很简单

在 [kernel/bpf/verifier.c](kernel/bpf/verifier.c#L17663) 看 `do_check()`，最外层就是一个 `for (;;)` 无限循环。

每轮它主要做五件事：

1. 取当前 `env->insn_idx` 对应指令
2. 看当前状态是否可以被剪枝
3. 打印/记录日志和当前状态
4. 调 `do_check_insn()` 验证当前指令并更新状态
5. 如果当前路径结束，就 `pop_stack()` 回到之前挂起的分支继续验证

所以外层结构虽然大，但骨架其实非常稳定。

## 3. 当前正在验证什么，由哪几个值决定

`do_check()` 主循环里最关键的状态有三个：

- `env->insn_idx`
- `env->cur_state`
- `env->head`

可以这样理解：

- `insn_idx` 表示当前走到哪条指令
- `cur_state` 表示这条路径此刻的抽象程序状态
- `head` 表示 DFS 栈上还没处理完的其它路径分支

也就是说，verifier 不是只有“程序计数器”，它始终是在“指令位置 + 抽象状态 + 待探索分支栈”三者共同作用下推进。

## 4. 为什么这是 DFS，而不是 BFS

从 [push_stack](kernel/bpf/verifier.c#L1712) 和 [pop_stack](kernel/bpf/verifier.c#L1660) 的组织方式就能看出来，verifier 在主循环里更像深度优先搜索。

当一条路径走下去时，它会一直沿当前分支继续推进；只有当前路径结束、被剪枝或报错时，才会回到栈里挂起的另一条分支。

这种做法的直接好处是：

- 当前状态局部性更强
- 状态复制点比较明确
- 路径结束后可以自然回溯

## 5. `push_stack()` 在做什么

在 [kernel/bpf/verifier.c](kernel/bpf/verifier.c#L1712) 的 `push_stack()`，verifier 做的不是简单记录一个跳转地址，而是：

- 新建一个 `bpf_verifier_stack_elem`
- 记录目标 `insn_idx`
- 记录 `prev_insn_idx`
- 复制当前 verifier state
- 把它压到 `env->head`

所以压栈保存的是“一整份路径快照”，而不是只有一个程序计数器。

这正是 verifier 能在分支之后继续精确追踪寄存器/栈/引用状态的原因。

## 6. 为什么分支之后必须复制整个状态

因为条件分支两边可能对状态产生不同收窄。

例如：

- 一边判空后把 `PTR_TO_MAP_VALUE_OR_NULL` 收窄成 `PTR_TO_MAP_VALUE`
- 另一边则把它视为 NULL 路径

如果不复制状态，而是只保存跳转地址，那么两边分支上的寄存器类型和范围信息就会互相污染。

所以 verifier 的核心不是“路径切换”，而是“路径切换时状态必须分叉”。

## 7. `bpf_is_state_visited()` 为什么是复杂度关键

在 [kernel/bpf/verifier.c](kernel/bpf/verifier.c#L17701) 附近可以看到，`do_check()` 在 prune point 会调用 `bpf_is_state_visited()`。

这一步的意义是：

- 当前指令位置如果曾经来过
- 并且当时保存的状态已经不比现在更弱
- 那么当前路径就没有必要再继续展开

它本质上是在回答：

- “这个程序点，在这个状态精度下，我是不是已经证明过了？”

没有这一步，稍复杂一点的分支程序就会路径爆炸。

## 8. `explored_states` 是什么

`bpf_check()` 在前面准备阶段会初始化 `explored_states` 哈希表，而 `do_check()` 里的状态剪枝就依赖它。

可以把它理解成：

- 以“程序点 + 调用点关系”为索引的已验证状态缓存

当前路径每到关键 prune point，都会拿当前 state 去和这里缓存过的状态比较。

如果等价或更弱，就直接剪掉这条路径。

## 9. 为什么不是每条指令都做状态剪枝

`do_check()` 并不是在每条指令都无差别调用 visited-state 剪枝，而是只在合适的 prune point 做。

原因也很直接：

- 剪枝本身有成本
- 不是所有程序点都值得缓存/比较
- 某些程序点的信息量不足，剪枝收益有限

所以 verifier 做的是有选择的状态缓存，而不是“全程序点全状态 memoization”。

## 10. `do_check_insn()` 和 `do_check()` 的分工

可以把两者关系粗略理解成：

- `do_check()` 负责控制路径搜索和状态调度
- `do_check_insn()` 负责验证“当前这条指令对状态做了什么”

也就是说：

- `do_check()` 更像搜索引擎
- `do_check_insn()` 更像单条指令语义执行器

这两个层次分开，才让 verifier 既能处理复杂控制流，又能把指令语义保持在相对局部的实现里。

## 11. 条件分支在 verifier 里怎么展开

条件分支的核心动作不是“跳还是不跳”，而是：

- 当前状态根据条件被分成两份
- 两份状态分别按真假分支收窄
- 其中一份继续当前路径，另一份压栈等待回溯

这就是为什么在 verifier 里，分支既和 `push_stack()` 有关，也和寄存器范围/指针收窄有直接关系。

所以控制流分析和数据流分析在这里不是两层独立逻辑，而是紧密交织的。

## 12. `PROCESS_BPF_EXIT` 在主循环里意味着什么

`do_check_insn()` 有时不会简单返回“下一条指令”，而会返回：

- 当前路径已经结束

这时 `do_check()` 会走到 `process_bpf_exit` 那一段，做几件事：

- 标记当前 verifier state 已处理完
- 更新 branch count
- `pop_stack()` 回到之前挂起的分支

如果栈已经空了，整个 `do_check()` 才真正结束。

所以对 verifier 来说，一条路径“exit”并不代表整个程序验证结束，只代表当前 DFS 分支结束。

## 13. `branches` 计数在辅助什么

在 `push_stack()` 里还能看到 parent state 的 `branches` 会递增。

这说明 verifier 不只是维护一个裸 DFS 栈，它还在追踪：

- 某个父状态还有多少分支尚未处理完

这对路径回溯、状态释放和某些循环/收敛逻辑都很有帮助。

可以把它理解成 verifier 自己维护的一套“路径生命周期计数器”。

## 14. speculative state 为什么会被特殊处理

在 [do_check](kernel/bpf/verifier.c#L17663) 里还能看到 `state->speculative` 和 `nospec` 相关逻辑。

这部分说明 verifier 主循环并不只是在验证“架构语义下正常执行的路径”，还会对某些 speculative 场景施加额外约束，并在需要时插入 nospec 屏障相关标记。

所以 `do_check()` 不只是普通 CFG 路径搜索，还带着一层微架构安全相关的保守处理。

## 15. 为什么 `do_check()` 仍然需要复杂度上限

即便有状态剪枝，`do_check()` 仍然会检查：

- `env->insn_processed` 是否超过上限
- `push_stack()` 的 jump sequence 是否过深

这是因为剪枝只能缓解复杂度爆炸，不能保证所有恶意或极端路径都天然收敛得足够快。

所以 verifier 一直是“两手并用”：

- 用状态等价与收敛减少无效路径
- 用显式 complexity limit 避免最坏情况拖垮系统

## 16. 用一条流水线记住 `do_check()`

可以把 [do_check](kernel/bpf/verifier.c#L17663) 压成下面这条循环：

1. 取当前 `insn_idx` 的指令和当前 state
2. 在 prune point 检查 visited-state 是否可剪枝
3. 打印日志/状态
4. 调 `do_check_insn()` 更新状态或分叉
5. 当前路径结束则 `pop_stack()`
6. 栈空则整个验证完成

## 17. 一句话压缩版

如果把这条线压成一句话，可以这样记：

> `do_check()` 是 verifier 的 DFS 主循环：它以“当前指令 + 当前抽象状态”为核心，遇到分支就复制状态压栈，遇到已验证等价状态就提前剪枝，最终把所有可达路径都收敛成一组被证明安全的控制流状态。 

如果想继续把 `do_check()` 里“单条指令如何改变状态、条件跳转如何做真假分支收窄、NULL check 为什么会改写指针类型”单独拆开，可以继续看补充文档 [Documentation/bpf/bpf-do-check-insn-notes.md](Documentation/bpf/bpf-do-check-insn-notes.md)。
