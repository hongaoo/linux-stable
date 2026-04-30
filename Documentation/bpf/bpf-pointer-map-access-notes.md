# verifier 中的指针类型与 map value 访问

这份文档继续沿 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c) 深挖，重点只看一件事：为什么 verifier 把指针当成带语义的对象，而不是普通整数，以及 map value 访问为什么会同时牵涉范围检查、类型检查和 BTF field 约束。

## 1. 先说最关键的一句话

在 verifier 里，指针不是“一个地址值”，而是“一个带对象语义、边界信息和访问规则的寄存器状态”。

这也是为什么 BPF 程序里：

- 不是所有指针都能做任意算术
- 不是所有指针都能被 helper 接受
- 不是所有 map value 内部字段都能直接 load/store

因为 verifier 真正关心的是：这次访问指向的到底是什么对象，以及它在当前状态下是否仍然可证明安全。

## 2. verifier 眼里的几类关键指针

从 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c) 开头注释到后续 `check_mem_access()` 相关逻辑，可以抓住几类最关键的指针类型：

- `PTR_TO_CTX`
- `PTR_TO_STACK`
- `PTR_TO_MAP_VALUE`
- `PTR_TO_MAP_KEY`
- `PTR_TO_BTF_ID`
- `PTR_TO_MEM`

这些类型的差异，不只是名字不同，而是：

- 可访问区域不同
- 是否允许偏移不同
- 可传给哪些 helper/kfunc 不同
- verifier 会走的检查路径不同

所以在 verifier 里，“指针类型”本身就是安全策略的一部分。

## 3. `__check_mem_access()` 在做最底层的边界门槛

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L4411) 的 `__check_mem_access()`，可以把它看成最底层的“给定偏移和大小，是否落在允许内存区域里”的统一门槛。

它会根据寄存器类型输出不同报错语义，例如：

- map key 访问非法
- map value 访问非法
- packet 访问非法
- context 访问非法

这一步很重要，因为它说明 verifier 在最底层已经按对象类别区分内存区域，而不是统一把所有内存访问都当成“地址 + size”。

## 4. `check_mem_region_access()` 为什么要看最小值和最大值

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L4443) 的 `check_mem_region_access()`，可以看到 verifier 对“可能带变量偏移的访问”不会只检查一个值，而是会检查：

- `smin_value + off`
- `umax_value + off`

这说明它在做的是“整个可能偏移区间上的保守证明”。

换句话说，只要某个可能路径上的最小偏移或最大偏移会越界，verifier 就不能接受这次访问。

所以这不是“当前看起来没越界就行”，而是“在当前抽象状态允许的所有值里都不能越界”。

## 5. 为什么有些指针要求“原样传递”

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L4487) 的 `__check_ptr_off_reg()` 里，代码明确表达了一个规则：

- 某些指针类型只有在原始、未修改形式下才允许被解引用或传给 helper/kfunc

这里会拒绝几类情况：

- `var_off` 不是常量
- 偏移为负
- 在不允许 fixed offset 的场景下带了非零偏移

这就是文档里“trusted / original pointer form”那类说法在源码里的直接落点。

本质上，这是在保护 verifier 的对象语义：一旦偏移和来源关系失控，后续就难以继续证明安全。

## 6. map value 为什么要单独走 `check_map_access()`

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L4799) 的 `check_map_access()`，verifier 对 map value 做了比普通内存区域更细的检查。

它不只是检查：

- 访问是不是落在 value_size 之内

还会继续检查：

- map record 里是否有特殊 BTF 字段
- 这次 load/store 是否会碰到 kptr/uptr 等受限字段
- 是否是直接访问还是 helper 间接访问
- offset 是否常量、是否对齐到字段位置
- 访问大小是否满足要求，例如 kptr 必须用 `BPF_DW`

这说明 map value 不是一块“裸内存”，而是可能携带结构化对象语义的区域。

## 7. `map_mem_size()` 说明 map value 也不是总等于 `value_size`

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L4788) 的 `map_mem_size()`，可以看到 verifier 甚至不会一律把可访问大小简单等同于 `map->value_size`。

例如对 `BPF_MAP_TYPE_INSN_ARRAY`，它会按整个 `ips` 数组大小来计算。

这意味着 verifier 在 map value 访问这件事上，也是在跟具体 map 语义联动，而不是只套一个统一模板。

## 8. BTF field 为什么会让 map value 访问变复杂

一旦 map value 关联了 BTF record，verifier 就能知道内部哪些字段不是普通字节，而是特殊对象位点，例如：

- `BPF_KPTR_UNREF`
- `BPF_KPTR_REF`
- `BPF_KPTR_PERCPU`
- `BPF_UPTR`

这时问题就不再是“有没有越界”，而是“有没有以正确方式访问正确字段”。

所以 BTF 一进来，map value 的访问语义就从“字节数组”升级成了“带特殊字段约束的结构化对象”。

## 9. `check_map_kptr_access()` 体现了什么

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L4722) 的 `check_map_kptr_access()`，可以很清楚看到 verifier 对 kptr 的态度：

- kptr 不能随便像普通字段一样读写
- 访问模式必须受限于 `BPF_MEM`
- 某些 kptr 只允许 load，不允许 store
- store 时要检查寄存器里的对象类型是否和目标字段匹配
- load 时会把目标寄存器显式标成对应的 `PTR_TO_BTF_ID` 之类类型

这说明 kptr 访问本质上是一种“对象边界穿越”，而不是普通内存拷贝。

## 10. `map_kptr_match_type()` 为什么很关键

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L4515) 的 `map_kptr_match_type()`，可以看到 verifier 不满足于“都是某种指针就行”，而是会继续检查：

- 基础类型是否是 `PTR_TO_BTF_ID`
- 类型 flag 是否允许
- 指针偏移是否合法
- BTF struct id 是否匹配

这一步说明 verifier 对对象类型匹配是实打实认真做的，不是只看“看起来像个指针”。

因此 map 中的 kptr 是“类型化对象槽位”，不是“可以随便塞地址的 8 字节”。

## 11. 为什么 `PTR_TO_MAP_VALUE_OR_NULL` 这么关键

很多对 map 的理解，都是从 `bpf_map_lookup_elem()` 开始的，而它返回的是 `PTR_TO_MAP_VALUE_OR_NULL`。

这个类型重要在于它把三件事绑在一起了：

- map access
- NULL 检查
- 类型收窄

也就是说，一次 map lookup 之后，程序还没有真正拿到可随意访问的 map value；它只是拿到一个“可空的 map value 指针候选”。

只有经过判空之后，verifier 才能把它变成真正的 `PTR_TO_MAP_VALUE`。

这说明 verifier 里的 map 指针访问语义，一开始就是和控制流绑定的。

## 12. `PTR_TO_CTX`、`PTR_TO_STACK`、`PTR_TO_MAP_VALUE` 为什么要分开走

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L6323) 后的 `check_mem_access()` 分发里，可以看到不同指针类型会走不同访问路径：

- `PTR_TO_MAP_VALUE` 走 `check_map_access()`
- `PTR_TO_CTX` 走 context 访问规则
- `PTR_TO_STACK` 走栈访问规则

这说明 verifier 的内存模型本质上是“对象分区模型”，而不是“统一地址空间模型”。

也正因为如此，BPF 才能在低级语法下仍然维持较强的对象安全语义。

## 13. 这条线和 helper / kfunc 检查怎么汇合

指针类型与 map 访问检查，最终都会回流到 helper / kfunc 检查里。

因为 helper/kfunc 的参数并不是独立验证的，它们总是建立在寄存器当前类型状态之上。

例如：

- 某 helper 要求 `ARG_PTR_TO_MAP_VALUE`
- verifier 不只看“是不是指针”，而是看当前寄存器是否是符合要求的 map value 指针状态

所以“指针类型系统”和“helper/kfunc 参数检查”其实是一体两面：

- 前者定义对象状态
- 后者消费这些状态并施加调用约束

## 14. 读这部分源码的推荐顺序

建议按下面顺序读：

1. [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L4411) 的 `__check_mem_access()`
2. [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L4443) 的 `check_mem_region_access()`
3. [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L4487) 的 `__check_ptr_off_reg()`
4. [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L4722) 的 `check_map_kptr_access()`
5. [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L4799) 的 `check_map_access()`
6. 再回头看 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L6323) 的 `check_mem_access()` 总分发

这样读，能先把“底层边界检查”建立起来，再理解“map value 上的结构化约束”。

## 15. 一句话压缩版

如果把这条线压成一句话，可以这样记：

> verifier 里的指针不是普通地址，而是带对象语义和边界信息的状态；对 map value 的访问也不是单纯的越界检查，而是“范围 + 指针形态 + BTF 字段语义 + 特殊对象槽位规则”的组合验证。
