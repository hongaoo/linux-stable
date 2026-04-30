# BPF 文档总结：从最小系统到完整架构

本文是对 `Documentation/bpf` 目录的压缩总结，并在原始文档基础上补充一条更适合工程理解的主线：先把 BPF 当作一个最小可运行系统来看，再逐层扩展到 Linux 内核里的完整 BPF 架构。

## 1. 先说结论

BPF 可以分成两部分来理解：

- 第一部分是“BPF 这台机器本身”
- 第二部分是“Linux 如何把这台机器接入内核、约束它、并把它扩展成通用内核可编程框架”

如果只保留最小集合，BPF 其实只需要下面几个元素：

- 一套指令集 ISA
- 一套寄存器与调用约定 ABI

补充理解：这里可以把 ABI 先粗略理解成“这台 BPF 虚拟机内部的函数调用规则”。也就是说，BPF 程序不是随意使用寄存器的，而是要遵守一套固定约定：哪些寄存器负责传参，哪个寄存器保存返回值，哪些寄存器在调用后还能继续使用，哪些寄存器需要自己保存，以及栈应当如何通过帧指针访问。没有这套约定，BPF 就只能是一串孤立指令，很难支持 helper 调用、子程序调用和较复杂的控制流组织。

再具体一点，BPF ABI 主要回答下面几个问题：

- 参数从哪里传入
- 返回值放在哪里
- 哪些寄存器是 caller-saved，哪些是 callee-saved
- 栈空间如何寻址
- 调用前后寄存器状态如何变化

因此，这里的“寄存器与调用约定 ABI”可以直接看成：BPF 机器内部关于寄存器职责分配和函数调用行为的一套统一规则。

如果把这套规则再压缩成最直观的寄存器视图，可以先记下面这一版：

- `R0`：返回值寄存器，helper 调用和程序 `EXIT` 的结果都放这里
- `R1-R5`：参数寄存器，用来向 helper、kfunc 或 BPF 子程序传参
- `R6-R9`：callee-saved 寄存器，跨调用时需要保持
- `R10`：只读帧指针，BPF 栈访问以它为基准

这套设计有两个直接效果：

- BPF 程序在调用 helper 或其他函数时，不需要临时发明一套传参协议
- JIT 可以更直接地把 BPF 寄存器映射到真实硬件寄存器，减少额外搬运开销

也正因为如此，BPF 从一开始就不是单纯“按顺序解释的一串过滤规则”，而是被设计成可以组织函数调用、局部状态和较复杂控制流的受限执行模型。

如果想继续把这一层展开到“caller-saved / callee-saved 怎么理解、为什么 R10 只读、helper 调用前后寄存器状态怎么变化、这套 ABI 和 JIT 的关系”，可以继续看补充文档 [Documentation/bpf/bpf-abi-notes.md](Documentation/bpf/bpf-abi-notes.md)。
- 一个加载入口
- 一个安全检查器

这里的 ABI 不是指 Linux 用户态程序常说的系统调用 ABI 或 glibc ABI，而是指 BPF 虚拟机自己的二进制约定：寄存器如何分工、函数调用时参数和返回值放在哪些寄存器、哪些寄存器需要调用者或被调用者保存、栈如何通过帧指针访问。

而 Linux 真正让 BPF 变成完整体系的部分是：

- verifier
- program type / attach point
- maps
- helpers / kfuncs
- BTF
- libbpf 与用户态加载流程

一句话概括：

> BPF 的底层是一个寄存器虚拟机，Linux 在它上面叠加了安全模型、状态模型、类型系统、挂载点和装载工具链，最终把它做成了内核中的通用可编程执行框架。

---

## 2. 文档分层

`Documentation/bpf` 并不是一篇连续教程，而是按层次拆开的。

### 2.1 标准化层

这一层回答的是：**什么是 BPF 本身**。

关键文档：

- `standardization/instruction-set.rst`
- `standardization/abi.rst`
- `standardization/index.rst`

这一层定义了：

- 指令编码
- 算术、跳转、访存、原子操作的语义
- 寄存器布局
- 调用约定
- 宽指令和立即数编码
- conformance groups，例如 `base32`、`base64`、`atomic64`

这部分可以理解为“BPF 处理器手册”。它尽量描述可移植的 BPF 语义，而不是 Linux 私有实现细节。

### 2.2 Linux 核心机制层

这一层回答的是：**BPF 如何安全地跑在 Linux 内核里**。

关键文档：

- `verifier.rst`
- `syscall_api.rst`
- `linux-notes.rst`
- `classic_vs_extended.rst`

这一层定义了：

- 程序如何通过 `bpf()` syscall 进入内核
- verifier 如何证明程序安全
- Linux 对标准 ISA 的额外语义和限制
- eBPF 相比 classic BPF 的根本变化

### 2.3 内核能力与状态层

这一层回答的是：**BPF 程序如何读写状态、调用内核能力、和外界交互**。

关键文档：

- `maps.rst`
- `map_*.rst`
- `helpers.rst`
- `kfuncs.rst`
- `cpumasks.rst`
- `fs_kfuncs.rst`

这一层定义了：

- map 这一通用状态抽象
- helper 这一稳定内核服务接口
- kfunc 这一更接近内核原生函数的扩展机制
- 特定对象族的高阶操作能力

### 2.4 程序类型与挂载点层

这一层回答的是：**程序到底在什么时机执行，输入输出是什么**。

关键文档：

- `programs.rst`
- `prog_*.rst`
- `bpf_iterators.rst`
- `redirect.rst`

这里定义了不同 program type 的：

- context 结构
- return value 语义
- 可以调用哪些 helper / kfunc
- attach 到内核哪条路径

这层本质上决定了 BPF 的“业务语义”。

### 2.5 类型系统与工程化层

这一层回答的是：**BPF 如何从“能跑”变成“能工程化交付”**。

关键文档：

- `btf.rst`
- `libbpf/index.rst`
- `libbpf/libbpf_overview.rst`
- `llvm_reloc.rst`

这一层提供：

- 类型信息
- 调试与符号信息
- CO-RE 重定位基础
- 用户态加载与附着的工程化接口

---

## 3. 最小 BPF 系统

如果完全不考虑 Linux 的生态，只保留“最小可执行 BPF 系统”，那它大致长这样：

### 3.1 指令集

BPF 是寄存器虚拟机，不是栈机。

- 通用寄存器：`R0` 到 `R9`
- 只读帧指针：`R10`
- 支持 32 位和 64 位算术
- 支持跳转、调用、返回
- 支持内存 load/store
- 支持 wide immediate

关键点：BPF 的设计非常偏向 JIT，一大目标就是让字节码能够较直接地映射到真实硬件寄存器。

### 3.2 调用约定

这里说的“寄存器与调用约定 ABI”，本质上就是 BPF 自身的函数调用规则，而不是某个具体 CPU 架构的用户态 ABI。

按 ABI 文档：

- `R0`：返回值
- `R1-R5`：参数寄存器
- `R6-R9`：callee-saved
- `R10`：栈帧指针

这让 BPF 不只是“过滤器字节码”，而是具备函数调用和子程序组织能力的指令系统。

### 3.3 加载接口

最小系统必须有一个入口把字节码送入运行时。在 Linux 里，这个入口是 `bpf()` syscall。

最核心的两类对象是：

- program
- map

没有 program，就没有执行体；没有 map，就几乎没有可持续状态。

### 3.4 安全检查

如果没有 verifier，BPF 就只是“内核里的另一种不安全代码注入方式”。

verifier 的作用不是简单检查指令合法，而是证明：

- 所有路径都会终止
- 所有寄存器读取都已初始化
- 所有内存访问都在已知合法边界内
- 指针类型不会被错误混用
- helper / kfunc 调用参数满足约束

所以从体系结构上说，**verifier 不是外围组件，而是 BPF 运行时定义的一部分**。

如果想把这一层继续展开到“verifier 实际在证明什么、为什么它不只是语法检查、寄存器类型和指针类型是怎么参与验证的、为什么 helper/kfunc 调用也受 verifier 约束”，可以继续看补充文档 [Documentation/bpf/bpf-verifier-notes.md](Documentation/bpf/bpf-verifier-notes.md)。

---

## 4. Linux 如何把最小系统扩成完整架构

### 4.1 verifier：安全边界

`verifier.rst` 是 Linux BPF 架构最核心的文档之一。

它做两件事：

- CFG/DAG 检查，拒绝不可达代码和非法控制流
- 状态模拟，跟踪寄存器、栈、指针、标量范围

Linux 中很多 BPF 能力之所以成立，不是因为“有这个指令”，而是因为 verifier 能静态证明这段程序对内核是安全的。

可以把 verifier 看成 BPF 的静态证明器。它让内核接受“来自用户态的不可信程序”成为可能。

### 4.2 program type：语义入口

同一份 BPF ISA，挂到不同位置，就变成完全不同的能力。

例如：

- 网络路径：XDP、tc、socket filter
- 追踪路径：kprobe、tracepoint、raw tracepoint、tracing
- 安全路径：LSM、cgroup hook
- 控制路径：sysctl、sockopt
- 遍历路径：iterators

program type 规定了三件根本性的事：

- `R1` 指向什么 context
- return value 代表什么动作
- 当前类型允许哪些 helper / kfunc / map 访问模式

因此 BPF 的“功能”主要不是由 ISA 决定，而是由 **program type + verifier rules + available helpers/kfuncs** 共同决定。

如果想把这一层继续展开到“program type 到底在定义什么、attach point 和 hook 的关系是什么、为什么同样的 BPF 字节码换个 program type 语义就完全不同、context / return value / helper 集为什么总是一起出现”，可以继续看补充文档 [Documentation/bpf/bpf-program-types-notes.md](Documentation/bpf/bpf-program-types-notes.md)。

### 4.3 maps：状态平面

map 是 BPF 世界中的通用状态容器，也是用户态和内核态之间的主要共享面。

基础作用：

- 保存配置
- 保存统计信息
- 保存索引/关联关系
- 作为事件通道或重定向表

常见分类：

- 通用存储：hash、array、lru hash、lpm trie
- 队列结构：queue、stack
- 事件通道：ringbuf
- 重定向与数据路径：devmap、cpumap、xskmap、sockmap
- 对象私有存储：sk_storage、cgroup_storage 等
- 组合结构：map-of-maps

从架构角度说，map 不只是“数据结构库”，而是 BPF 的状态平面。程序执行面和状态平面通过 helper 连接起来。

如果想把这一层继续展开到“map 为什么是状态平面、helper 和 kfunc 在能力模型上有什么差别、它们和 verifier/source code 是怎么接起来的”，可以继续看补充文档 [Documentation/bpf/bpf-maps-helpers-kfuncs-notes.md](Documentation/bpf/bpf-maps-helpers-kfuncs-notes.md)。

### 4.4 helpers 与 kfuncs：能力平面

这两者都让 BPF 调用内核能力，但定位不同。

#### helpers

helpers 是相对稳定的、按 ID 或 BTF ID 暴露给 BPF 的内核接口。

特点：

- 接口长期稳定性更强
- verifier 对参数规则更成熟
- 面向通用能力封装

典型 helper：

- map 查找/更新/删除
- 时间获取
- 调试输出
- skb/xdp 数据处理
- 字符串与内存辅助操作

#### kfuncs

kfuncs 更接近直接向 BPF 暴露内核函数。

特点：

- 灵活度更高
- 依赖 BTF
- 接口稳定性不承诺跨版本不变
- 通常要求更严格的 trusted pointer 规则

它实际上把 BPF 扩展能力从“固定 helper 集合”推进到“受控的内核函数开放模型”。

### 4.5 BTF：类型系统与可移植性基础

BTF 是完整 BPF 架构成熟起来的关键转折点。

它提供：

- 类型描述
- 函数签名
- 行号信息
- 全局变量与数据段元数据

BTF 的价值不只是调试，而是：

- 支持 map pretty print
- 支持 verifier / loader 理解对象类型
- 支持 CO-RE 重定位
- 支持基于 BTF ID 的 helper / kfunc / 类型关联

没有 BTF，现代 BPF 仍然能运行；但没有 BTF，就很难成为可维护、可移植、可大规模交付的工程体系。

如果想把这一层继续展开到“BTF 到底存了什么、为什么它不只是调试信息、CO-RE 为什么依赖 BTF、libbpf 和 LLVM relocation 如何参与这条链路”，可以继续看补充文档 [Documentation/bpf/bpf-btf-core-notes.md](Documentation/bpf/bpf-btf-core-notes.md)。

### 4.6 libbpf：把加载流程工程化

从 `libbpf_overview.rst` 看，libbpf 把 BPF app 生命周期明确拆成四阶段：

- Open
- Load
- Attach
- Tear down

这四阶段非常重要，因为它揭示了完整 BPF 应用不只是“一段程序”，而是一组对象：

- BPF programs
- BPF maps
- global variables
- attach state

libbpf 的真正价值不是代替 syscall，而是把：

- ELF 解析
- map 创建
- relocation
- BTF/CO-RE 处理
- verifier 装载
- attach

统一成稳定的用户态流程。

这标志着 BPF 从“内核特性”演进成“可构建应用的运行平台”。

---

## 5. 一条完整的数据与控制流

把文档串起来后，可以把一次典型 BPF 应用运行过程理解为：

1. 开发者写 BPF C 程序和用户态控制程序
2. 编译器生成 BPF ELF，对象中包含字节码、maps、BTF、重定位信息
3. libbpf 打开对象，发现 program / map / global variable
4. libbpf 创建 maps，处理 CO-RE 和其他 relocation
5. 用户态通过 `bpf()` syscall 把 program load 到内核
6. verifier 静态证明程序安全
7. 程序 attach 到指定 hook
8. 内核事件触发该 hook，BPF 程序开始执行
9. 程序通过 helpers / kfuncs 访问状态、内核对象、时间、包数据等
10. 程序更新 maps 或产生事件
11. 用户态通过 map、ringbuf、perf/ring 通道读出结果并进行控制

这条链说明：

- program 决定执行逻辑
- map 决定状态交换
- verifier 决定安全边界
- BTF 决定可理解性和可移植性
- libbpf 决定工程落地效率

---

## 6. 为什么 eBPF 和 classic BPF 已经不是一回事

`classic_vs_extended.rst` 说明得很直接：eBPF 已经不是“增强版过滤器”这么简单。

关键变化包括：

- 从 2 个寄存器扩展到 10 个寄存器
- 从 32 位主模型扩展到 64 位主模型
- 引入明确调用约定
- 引入内核函数调用能力
- 更适合 JIT 一对一映射到硬件寄存器

所以 eBPF 的本质更接近：

> 一个可验证的、受约束的、适合 JIT 的内核内嵌 RISC 指令系统。

而 classic BPF 更接近特定场景下的过滤器字节码。

---

## 7. 标准化文档与 Linux 文档的边界

这个边界在阅读时很重要。

### 7.1 标准化文档关心什么

它主要关心：

- 指令怎么编码
- 语义怎么定义
- ABI 怎么约定
- 哪些能力属于通用 BPF 机器模型

### 7.2 Linux 文档关心什么

它主要关心：

- 哪些 program type 存在
- verifier 如何判定安全
- maps/helpers/kfuncs 如何工作
- BTF 如何编码和使用
- libbpf 如何加载
- 哪些是 Linux 特有扩展

### 7.3 这意味着什么

这意味着“会 BPF ISA”不等于“会 Linux BPF”。

真正写 Linux BPF 程序时，更重要的是理解：

- 你处在哪个 hook
- 你拿到什么 context
- verifier 允许你做什么
- 当前 program type 能用哪些 helper/kfunc/map

也就是说，Linux BPF 的重心不在“如何写指令”，而在“如何在约束模型内表达合法且有用的程序”。

---

## 8. 可以把完整架构画成五层

```text
+--------------------------------------------------+
| User Space Control Plane                         |
| libbpf / bpftool / app logic                     |
+--------------------------------------------------+
| Type + Relocation Plane                          |
| BTF / CO-RE / ELF metadata                       |
+--------------------------------------------------+
| Capability + State Plane                         |
| helpers / kfuncs / maps / global variables       |
+--------------------------------------------------+
| Execution Semantics Plane                        |
| program types / contexts / attach points         |
+--------------------------------------------------+
| Core VM + Safety Plane                           |
| ISA / ABI / verifier / JIT / interpreter         |
+--------------------------------------------------+
```

如果继续压缩：

- 最底层决定“能不能执行”
- 中间层决定“能做什么”
- 上层决定“怎么交付和维护”

---

## 9. 一个最小系统与一个完整系统的差别

### 9.1 最小系统

最小 BPF 系统只需要：

- 字节码
- 寄存器模型
- 调用约定
- 一个执行器
- 一个安全检查器

它可以执行有限逻辑，但几乎不具备现代内核可编程平台的实际价值。

### 9.2 完整系统

完整 Linux BPF 系统则需要额外引入：

- hook points
- program types
- map 体系
- helper/kfunc 体系
- BTF 类型系统
- 用户态 loader
- 调试、测试和观测工具链

只有这些都具备，BPF 才从“受限 VM”变成“内核平台能力”。

---

## 10. 阅读 Documentation/bpf 的推荐路线

如果目标是建立整体架构感，推荐按下面顺序读：

1. `index.rst`
2. `standardization/instruction-set.rst`
3. `standardization/abi.rst`
4. `classic_vs_extended.rst`
5. `verifier.rst`
6. `maps.rst`
7. `helpers.rst`
8. `kfuncs.rst`
9. `btf.rst`
10. `libbpf/libbpf_overview.rst`
11. `programs.rst`
12. 再按兴趣进入 `prog_*.rst` 和 `map_*.rst`

如果目标是写程序，则更实用的顺序是：

1. `libbpf/libbpf_overview.rst`
2. `btf.rst`
3. `programs.rst`
4. 对应的 `prog_*.rst`
5. `maps.rst`
6. 对应的 `map_*.rst`
7. `verifier.rst`

前者偏架构，后者偏开发。

---

## 11. 对原文档的几个补充理解

### 11.1 verifier 是“定义 BPF 可用边界”的组件

很多人会把 verifier 当成装载前的附加检查，但从 Linux BPF 架构上看，它更像语言语义的一部分。

因为在 Linux 中，“一个 BPF 程序是否成立”不只由 ISA 决定，还由 verifier 是否能证明它安全决定。

### 11.2 program type 比 ISA 更接近“真实能力边界”

两个程序即使都是 BPF 字节码，只要 program type 不同，context、返回值、helper 集和可访问对象都会不同。

所以 Linux BPF 的真实编程模型不是“通用汇编 + 任意能力”，而是“受 attach 场景强约束的受限程序模型”。

### 11.3 map 是状态平面，BTF 是类型平面，libbpf 是交付平面

这三个经常被分开看，但从架构上最好把它们并行理解：

- map 解决状态共享
- BTF 解决类型理解和跨内核可移植
- libbpf 解决对象发现、重定位、加载和附着

三者共同把 BPF 从“字节码”升级成“应用”。

### 11.4 Linux BPF 已经是“平台”而不是“单特性”

当一个系统同时具备：

- 多程序类型
- 多存储模型
- 类型元数据
- 生命周期管理
- 调试测试手段
- 工具链与库

它就已经不再是单点功能，而是平台。

BPF 在 Linux 中正是这样的平台。

---

## 12. 和源码实现对照时最值得关注的对象

如果后续要从文档转到源码，建议优先把这些对象对应起来：

- verifier 的寄存器状态跟踪
- BPF program 的 load / attach 生命周期
- map 的 fd 与内核对象关系
- BTF 类型 ID 与对象引用关系
- helper / kfunc 的调用约束
- program type 对 context 与返回值的解释

文档和源码之间最关键的桥梁不是某个 API，而是这些“对象关系”。

---

## 13. 最后的压缩版

如果必须把 `Documentation/bpf` 全部压成几句话：

1. BPF 的底层是一个面向 JIT 的寄存器虚拟机，ISA 和 ABI 定义了它的机器模型。
2. Linux 通过 verifier 把它变成可安全运行的不可信内核程序模型。
3. Linux 再通过 program type、maps、helpers、kfuncs 定义它的实际能力边界。
4. BTF 让它具备类型、自描述和 CO-RE 可移植能力。
5. libbpf 把 ELF、重定位、load、attach 封装成可交付的工程流程。
6. 因此，Linux BPF 不应只被理解为“字节码机制”，而应被理解为“内核中的通用可编程执行平台”。
