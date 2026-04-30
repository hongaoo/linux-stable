# BPF maps、helpers 与 kfuncs 补充说明

这份文档把主文档里的“状态平面”和“能力平面”合起来讲，并顺手把它们和内核源码里的关键实现入口对上。重点不是列 API 清单，而是回答三个更核心的问题：

- map 为什么是 BPF 的状态平面
- helper 和 kfunc 到底分别在解决什么问题
- verifier 是怎么把这两类能力接进安全模型里的

## 1. 先说整体关系

可以先把这三者放进一条简单链路里：

- map 负责保存状态
- helper / kfunc 负责让程序读写状态或调用内核能力
- verifier 负责约束这些访问和调用必须在安全条件下发生

所以这三者不是并列的 API 分类，而是一套协作机制。

## 2. 为什么说 map 是“状态平面”

如果没有 map，大多数 BPF 程序只能做到：

- 收到一个上下文
- 做一些即时计算
- 返回一个动作

但它很难做到：

- 跨次执行保存统计信息
- 维护策略配置
- 维护对象索引
- 向用户态输出事件数据
- 支撑重定向/队列/转发表一类共享结构

map 解决的正是“程序执行之外的持久或共享状态”。

从这个角度看，map 不只是一个内核数据结构集合，而是 BPF 程序和用户态控制面之间的共享状态面。

## 3. 文档里的 map 和源码里的 map 是怎么接上的

文档入口在 [Documentation/bpf/maps.rst](/home/hongao/github-workspace/linux-stable/Documentation/bpf/maps.rst)。

这份文档在概念上主要讲：

- map 是用户态和内核态共享状态的容器
- 用户态通过 `bpf()` syscall 创建和操作 map
- BPF 程序通过 helper 来访问 map

源码层面，最直观的连接点之一其实出现在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L115) 开头大段注释里：它直接拿 `bpf_map_lookup_elem()` 举例说明 helper 原型约束、参数检查和返回值类型是怎么进入 verifier 模型的。

也就是说，map 在源码里并不是“独立一块功能”，而是通过：

- map 对象本身
- helper 原型
- verifier 的参数与返回类型规则

共同进入 BPF 执行模型。

## 4. helper 在这套模型里做什么

helper 可以理解成“为 BPF 暴露的一组相对稳定的内核服务入口”。

它们的特点通常是：

- 调用方式统一，走 BPF 调用 ABI
- 有明确的参数类型约束
- verifier 对它们有比较成熟的理解模型
- 能力更偏通用、稳定、场景无关的抽象

典型例子包括：

- map lookup/update/delete
- 时间获取
- 调试输出
- skb/xdp 处理
- 字符串/内存辅助操作

所以 helper 是“被 verifier 熟悉的一组系统服务”。

## 5. kfunc 在这套模型里做什么

kfunc 更接近“把内核中的某些真实函数以受控方式开放给 BPF”。

它和 helper 的核心差异不在调用形式，而在能力来源和稳定性边界：

- helper 更像稳定公共服务接口
- kfunc 更像受控开放的内核原生能力

因此，kfunc 的特点往往是：

- 更灵活，表达能力更强
- 强依赖 BTF
- verifier 需要更多元数据来检查参数和返回类型
- 稳定性通常弱于 helper

文档入口在 [Documentation/bpf/kfuncs.rst](/home/hongao/github-workspace/linux-stable/Documentation/bpf/kfuncs.rst)。

## 6. helper 和 kfunc 最大的共同点

虽然来源不同，但它们都不是“普通函数调用”。

它们都依赖：

- BPF ABI 提供统一调用形式
- program type 决定哪些调用被允许
- verifier 检查参数、返回值和对象生命周期

所以从执行模型看，helper 和 kfunc 都是“受 verifier 监管的能力入口”。

## 7. verifier 怎么理解 map helper

从 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L115) 开始那段注释可以看得很清楚：

- helper 有 prototype
- prototype 定义参数类型和返回类型
- verifier 会把这些类型规则映射到寄存器状态上

例如对 `bpf_map_lookup_elem()`：

- 第一个参数必须是 map 指针
- 第二个参数必须是指向 key 的有效栈内存
- 返回值会被建模成 `PTR_TO_MAP_VALUE_OR_NULL`

这意味着 helper 的真正威力不只是“能调用”，而是“verifier 知道调用之后寄存器状态该怎样演化”。

## 8. map lookup 为什么是理解 verifier 的经典例子

`bpf_map_lookup_elem()` 之所以经典，是因为它正好把几个关键机制串起来了：

- 参数检查
- 栈初始化检查
- 返回可空指针
- 判空后的状态收窄

也就是说，一个常见 map helper 调用，已经同时体现了：

- map 是状态入口
- helper 是能力入口
- verifier 是安全证明器

这也是为什么源码注释喜欢拿它举例。

## 9. kfunc 为什么更强，也更“重”

kfunc 的能力之所以更强，是因为它不再局限于固定 helper 列表，而是能借助 BTF 和注册机制把更多内核函数安全地开放给 BPF。

但这也意味着它更“重”：

- 需要 BTF 支撑函数与类型识别
- 需要 flags 描述获取/释放/可空/可睡眠等属性
- verifier 需要做更复杂的对象与生命周期检查

在 [Documentation/bpf/kfuncs.rst](/home/hongao/github-workspace/linux-stable/Documentation/bpf/kfuncs.rst) 里，`KF_ACQUIRE`、`KF_RET_NULL`、`KF_RELEASE`、`KF_SLEEPABLE` 这些 flag，本质上就是把函数语义显式提供给 verifier。

## 10. kfunc 在源码里是怎么接进 verifier 的

从源码上看，kfunc 的接入比 helper 更动态。

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L2929) 附近可以看到 `fetch_kfunc_meta()` 一类逻辑，它负责：

- 找到对应的 BTF
- 解析 kfunc 原型
- 读取 kfunc flags

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L2992) 的 `bpf_add_kfunc_call()` 可以看到：

- kfunc 会被记录进 prog aux 的描述表
- 需要解析地址、BTF、函数模型
- 某些架构还需要 JIT 能支持 kfunc call

也就是说，helper 更像预定义能力；kfunc 更像“加载期解析并注册的扩展能力”。

## 11. helper 和 kfunc 为什么都要跟 program type 绑定

不是所有 program type 都能调用所有 helper / kfunc，这不是人为限制，而是安全模型要求。

因为不同 program type：

- 拥有不同 context
- 运行在不同内核路径
- 允许访问不同对象
- 对睡眠、锁、RCU、包数据等有不同限制

因此能力开放一定是场景化的。

这也是为什么“能不能调用某个 helper / kfunc”本质上不是 API 可见性问题，而是 program type + verifier 规则共同决定的问题。

## 12. map、helper、kfunc 三者的分工

可以把它们的分工压缩成这样：

- map：状态容器
- helper：稳定通用能力
- kfunc：受控开放的高级内核能力

再加上 verifier：

- verifier：确保前面三者的使用方式在当前程序语境中安全

这就是完整的“状态面 + 能力面 + 安全面”组合。

## 13. 为什么说 map 不只是存储，而是控制面的桥

从工程实践看，很多 BPF 应用并不是只跑一段程序，而是用户态和 BPF 程序长期协作：

- 用户态写配置到 map
- BPF 程序读取配置做决策
- BPF 程序把统计或事件写回 map / ringbuf
- 用户态读取结果再调整配置

所以 map 实际上同时承担：

- 配置下发
- 运行状态共享
- 结果回传

这就是为什么把它理解成“状态平面”比理解成“内核哈希表”更准确。

## 14. 读源码时建议抓住的几个点

如果想把这层和源码对上，建议优先抓这几个点：

1. 看 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L115) 对 map helper 参数/返回值的注释说明
2. 看 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L4799) 一类 `check_map_access()` 相关逻辑，理解 map value 访问为什么受严格约束
3. 看 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L2929) 到 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L3073) 一带，理解 kfunc 元数据、BTF、原型解析如何进入 verifier
4. 再回头看 [Documentation/bpf/maps.rst](/home/hongao/github-workspace/linux-stable/Documentation/bpf/maps.rst) 和 [Documentation/bpf/kfuncs.rst](/home/hongao/github-workspace/linux-stable/Documentation/bpf/kfuncs.rst)，会更容易把抽象和实现对上

## 15. 一句话压缩版

如果把这一层压成一句话，可以这样记：

> map 提供 BPF 的共享状态面，helper 提供相对稳定的通用能力入口，kfunc 提供更强但更依赖 BTF 和 verifier 语义的内核能力扩展，而 verifier 负责把这些状态和能力接进安全模型里。
