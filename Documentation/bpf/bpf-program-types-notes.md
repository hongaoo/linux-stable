# BPF program type 与 attach point 补充说明

这份文档补充主文档里 program type / attach point 那一层。核心目标是回答一个经常被低估的问题：为什么同样都是 BPF 字节码，挂到不同位置后，程序的输入、返回值、可用 helper/kfunc、甚至 verifier 允许的访问方式都会变化。

## 1. 先抓住最核心的一句话

在 Linux BPF 里，ISA 决定“能写出什么指令”，而 program type 决定“这段程序到底在什么语境里执行”。

这意味着 BPF 真正的执行语义，不是只有字节码本身决定的，而是由下面几件事一起决定：

- program type
- attach point / hook
- context 结构
- return value 语义
- 可用 helper / kfunc 集合
- verifier 针对该类型的规则

因此，program type 是 Linux BPF 里最接近“业务语义入口”的东西。

## 2. 什么是 program type

program type 可以理解成：

> 这段 BPF 程序属于哪一种执行类别，以及内核会用哪套规则解释它。

它至少决定下面这些问题：

- 程序会在什么事件或路径上被触发
- `R1` 初始时指向什么上下文对象
- 允许访问哪些上下文字段
- 可以调用哪些 helper / kfunc
- 返回值会被解释成什么动作或结果
- verifier 该用哪套规则去验证程序

所以 program type 不是一个“标签”，而是整个执行契约的一部分。

## 3. 什么是 attach point / hook

如果说 program type 是执行类别，那么 attach point 或 hook 可以理解成：

- 这段程序被挂到内核哪条具体路径上
- 它会在内核执行过程的哪个时刻被调用

例如：

- 网络数据路径上的某个阶段
- 某个 tracepoint
- 某个 LSM hook
- 某个 cgroup 相关操作点
- 某个 iterator 的遍历回调点

attach point 让 program type 从“抽象执行类别”落到“实际触发位置”。

## 4. program type 和 attach point 的关系

两者不能完全等同，但也不能分开看。

一个更准确的理解是：

- program type 决定大类语义
- attach point 决定具体挂载位置

例如同属 tracing 范畴，可能 attach 到：

- kprobe
- tracepoint
- raw tracepoint
- fentry/fexit 一类入口

它们都属于“观察或插入某类执行路径”的思路，但具体上下文、能力、成本和语义仍然不同。

## 5. 为什么同样的字节码换个 program type 语义会完全不同

因为 program type 会同时改变三件根本性的事：

### 5.1 输入变了

也就是 `R1` 初始指向的 context 不同。

例如：

- 在网络路径里，context 可能和 skb/xdp 包数据相关
- 在 LSM 路径里，context 可能是某个安全钩子的参数集合
- 在 tracing 路径里，context 可能对应函数参数或 tracepoint 上下文

### 5.2 返回值意义变了

同一个整数返回值，在不同 program type 中含义可能完全不同：

- 可能表示放行/拒绝
- 可能表示重定向/丢包
- 可能表示继续执行/覆盖结果
- 可能只是一个观测结果或辅助返回码

### 5.3 可用能力变了

不同 program type 可调用的 helper / kfunc、允许访问的对象和 verifier 规则都不同。

所以 Linux BPF 不是“任意 BPF 程序 + 任意 hook”，而是“特定类型的程序只能在特定语义约束下运行”。

## 6. context 为什么这么重要

在 BPF 中，context 是程序和内核事件之间的第一接触面。

程序一开始能看到什么、能从哪里拿数据、能否修改某些字段，很多时候都由 context 决定。

更关键的是，context 不是“裸指针”。verifier 会把它当成带类型和访问规则的对象处理。

所以 context 同时影响：

- 程序能读到什么
- 程序能写到什么
- verifier 能否证明这些访问安全

这也是为什么理解某个 program type 时，最先该看的通常不是它能做多少事，而是：

- context 是什么
- 哪些字段允许访问
- 返回值怎么解释

## 7. return value 为什么不能只看成普通整数

在 Linux BPF 里，返回值经常带有强业务语义。

例如不同类型的程序里，`R0` 可能表示：

- 丢弃/放行
- 拒绝/允许
- 重定向到哪个目标
- 是否继续链式执行
- 向上一个钩子返回什么状态

这意味着 `R0` 虽然在 ABI 层面总是返回值寄存器，但在 program type 层面，它其实是“该类程序与内核路径之间的控制信号”。

## 8. helper / kfunc 集为什么总是和 program type 绑定

helper / kfunc 看起来像“能力库”，但 Linux 不会把所有能力都开放给所有 program type。

原因很直接：

- 不同执行路径的安全要求不同
- 不同上下文对象的可访问性不同
- 某些 helper 只对特定数据路径有意义
- 某些 kfunc 只在特定对象模型下才安全

所以 program type 决定的不只是“程序挂哪”，还决定“程序在那个位置被允许做什么”。

## 9. verifier 为什么也依赖 program type

verifier 不是在真空中验证程序，它需要知道：

- 当前 `R1` 是什么 context
- 当前类型允许哪些 helper / kfunc
- 某些上下文字段是否可读或可写
- 某种返回值在这里是否合法

这意味着 verifier 并不是拿着统一模板去审所有程序，而是在某个 program type 的语义上下文里验证程序。

所以 program type 不是 verifier 之外的附加信息，而是 verifier 决策的输入之一。

## 10. 用几个典型场景来理解

### 10.1 网络路径

网络相关 program type 往往最强调：

- 包数据边界访问
- 重定向
- context 中的网络元数据
- 返回值对应的数据路径动作

例如 `redirect.rst` 这类文档讨论的重点就不是“BPF 怎么算术”，而是“返回什么动作、配合什么 map、会在数据路径上产生什么效果”。

### 10.2 LSM 路径

LSM BPF 程序更像是在安全钩子上做策略和审计控制。

重点变成：

- hook 参数是什么
- 返回值是否允许某操作继续
- 如何借助 BTF/CO-RE 访问内核对象字段
- attach 到哪个 LSM hook

也就是说，这时 program type 的语义已经非常接近“安全扩展点”，而不是单纯事件观察点。

### 10.3 tracing 路径

tracing 类程序更强调：

- 观察哪个函数或事件
- 能拿到哪些参数
- 如何读取内核对象状态
- 是否只是观测，还是能参与更复杂交互

在这里，返回值通常不像网络/安全路径那样直接控制放行或拒绝，而更偏向观测与记录语义。

## 11. 为什么说 program type 是 Linux BPF 的“业务语义入口”

ISA 只提供了低级表达能力，但真正把程序放到哪条内核路径上、让它参与什么业务逻辑，是 program type 决定的。

因此可以把它看成 Linux BPF 的“语义入口层”：

- 它把抽象字节码接到具体内核事件上
- 它把返回值从普通整数提升成动作信号
- 它把上下文从无类型输入变成受约束对象
- 它把 helper/kfunc 从通用调用变成场景化能力集合

## 12. 读 program type 文档时建议抓住的四个问题

看任何一个 `prog_*.rst`，优先找下面四件事：

1. 触发点是什么
2. `R1` 对应什么 context
3. 返回值表示什么
4. 允许用哪些 helper / kfunc / map 模式

只要把这四个问题答清楚，这个 program type 的基本编程模型就已经建立起来了。

## 13. program type、ABI、verifier 的边界

这三者经常同时出现，但解决的是三类不同问题：

- ABI：怎么传参、怎么返回、哪些寄存器跨调用保持
- program type：程序处在哪个执行语境里
- verifier：在这个语境下，这段程序是否安全

因此更准确的关系是：

- ABI 定义运行形式
- program type 定义业务语义
- verifier 定义安全边界

## 14. 一句话压缩版

如果把 program type / attach point 压成一句话，可以这样记：

> 在 Linux BPF 中，program type 和 attach point 共同决定程序被挂到哪条内核路径、拿到什么 context、返回值代表什么动作、能调用哪些能力，以及 verifier 应该按什么语义去验证它。
