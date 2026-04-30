# CO-RE 再拆一层：spec match 与兼容性规则

这份文档继续沿 [Documentation/bpf/bpf-core-relo-calc-patch-notes.md](Documentation/bpf/bpf-core-relo-calc-patch-notes.md) 往下钻，专门看 CO-RE 里最容易被一笔带过、但实际上决定 candidate 能否成立的那一层：

- [bpf_core_spec_match](tools/lib/bpf/relo_core.c#L558)
- [bpf_core_match_member](tools/lib/bpf/relo_core.c#L486)
- [bpf_core_fields_are_compat](tools/lib/bpf/relo_core.c#L413)
- [__bpf_core_types_match](tools/lib/bpf/relo_core.c#L1511)
- 以及 [bpf_core_names_match](tools/lib/bpf/relo_core.c#L1439)

目标是把下面这个问题讲清楚：

- 一个 candidate 在名字上看起来像是对的，为什么最后可能还是不 match

## 1. 一句总话

如果先压成一句话，可以这样记：

> CO-RE 的 candidate 之所以“真的算匹配”，不是因为名字碰上了，而是因为 `spec_match` 能沿着本地访问路径在目标 BTF 中找到语义对应物，并且字段或类型兼容性规则允许这种对应关系成立。

## 2. `bpf_core_spec_match()` 是 candidate 真正的判决点

在 [tools/lib/bpf/relo_core.c](tools/lib/bpf/relo_core.c#L558) 的 `bpf_core_spec_match()` 里，输入是：

- 已经解析好的 local spec
- 一个具体 target candidate

输出不是简单的 true/false，而是：

- 这个 candidate 是否匹配
- 如果匹配，生成完整 target spec
- 同时算出 low-level/high-level access 路径和 bit offset

所以它本质上是“把本地访问意图映射到目标类型结构”的解释器。

## 3. `bpf_core_names_match()` 只做 essential-name 级别比较

在 [bpf_core_names_match](tools/lib/bpf/relo_core.c#L1439) 里，CO-RE 的名字比较不是普通全字符串比较，而是先取 essential name。

这意味着：

- flavor suffix 会被忽略
- 真正用于 match 的是 flavor-less 语义名

所以 candidate 搜索和后续 member/type match 其实都建立在“essential name 相等”这个更宽松、也更符合 CO-RE 目标的规则上。

## 4. 为什么名字相等还不够

名字相等只说明：

- 这个 target 成员或类型看起来像本地想找的那个对象

但 CO-RE 还要进一步确认：

- 它的种类是否兼容
- 递归结构是否兼容
- 如果是 field access，对应字段类型是否允许这样 relocation

所以名字只是门票，不是最终判决。

## 5. `bpf_core_match_member()` 在做什么

在 [bpf_core_match_member](tools/lib/bpf/relo_core.c#L486) 里，CO-RE 会针对一个高层 named field accessor：

- 在 target struct/union 中递归枚举成员
- 如果遇到匿名嵌套 struct/union，会继续深入
- 如果找到同名字段，再检查字段类型兼容性

这说明 field match 不是“按索引对位”，而是“按名字递归搜到语义上对应的字段”。

## 6. 匿名 struct/union 为什么需要递归搜索

匿名嵌套成员会让本地和目标的物理布局层次不一致：

- 本地可能是一层字段
- 目标可能把它包进匿名 struct/union

如果 CO-RE 只按同层索引匹配，就会大量误判不兼容。

所以 `bpf_core_match_member()` 会穿过匿名 composite，把“命名字段的语义路径”找回来。

## 7. `bpf_core_fields_are_compat()` 定义了 field relocation 的宽松兼容规则

在 [bpf_core_fields_are_compat](tools/lib/bpf/relo_core.c#L413) 里，可以看到 field compatibility 比严格 type equality 宽松很多。

它大致允许：

- composite 和 composite 互相兼容
- pointer 和 pointer 兼容
- float 和 float 兼容
- int 主要排除旧式 bitfield-like int，其余比较宽松
- array 递归看 element type
- enum/fwd 用名字或匿名规则做兼容

这说明 field relocation 要解决的是“能不能继续沿这条路径访问对应字段”，而不是“本地和目标类型是否完全相等”。

## 8. 为什么 field compatibility 会比 type match 更宽

这是 CO-RE 的核心设计取向之一。

如果 field access relocation 也要求完整结构严格等价，那么大量仅做了 harmless 重排或包装的内核类型都会导致 CO-RE 失败。

所以 field compatibility 更像：

- 对访问路径而言，目标侧还能不能承接这条访问语义

而不是：

- 这两个类型在形式上是否完全一样

## 9. `__bpf_core_types_match()` 处理的是更严格的“类型匹配”

在 [__bpf_core_types_match](tools/lib/bpf/relo_core.c#L1511) 里，规则明显更严格。

它会递归看：

- int 的大小和 signedness
- pointer/array 的目标类型
- struct/union 的成员名字与递归类型
- enum 的 symbolic name 与 size
- function proto 的参数个数、顺序和返回值

这更接近“这两个类型在 CO-RE 语义上是否可视作同一种 type”。

## 10. behind pointer 的规则为什么特殊

`__bpf_core_types_match()` 里有一个很关键的概念：behind pointer。

它的意义是：

- 一旦某层已经穿过 pointer，后面某些结构匹配可以更保守地只要求名字和 kind 对得上，而不必把整个深层结构完全展开到头

这是一种非常实用的折中：

- 既能保证指向对象的大方向没错
- 又避免把深层布局差异放大成不必要的不兼容

## 11. enum match 为什么看 symbolic name，不看 numeric value

在 [bpf_core_enums_match](tools/lib/bpf/relo_core.c#L1456) 和 `__bpf_core_types_match()` 的 enum 规则里，重点是：

- local enum 的 symbolic variant 在 target 里都能找到
- size 兼容

但并不要求 numeric value 相同。

这是因为 CO-RE 的 type match 关注的是“语义集合是否对应”，而枚举值的具体数值变化通常应由 enum-value relocation 去解决，不应在 type match 阶段过早失败。

## 12. composite match 为什么不是逐字段一一严格对齐

在 [bpf_core_composites_match](tools/lib/bpf/relo_core.c#L1479) 可以看到，本地每个成员都需要在目标里找到同名成员，但目标可以有更多成员。

这代表的不是严格等价，而是：

- local 需要的语义结构在 target 中仍然存在

因此 target 新增字段通常不会破坏 match，只要不会让 local 关心的那些成员失去可追踪性。

## 13. `spec_match` 为什么会同时构造 target spec

`bpf_core_spec_match()` 在判断匹配的同时，还不断填充：

- `targ_spec->spec`
- `targ_spec->raw_spec`
- `targ_spec->bit_offset`

因为后面 relocation 求值和 patch 不是只需要一个“匹配/不匹配”的布尔值，而是需要：

- 目标字段到底落在哪
- 精确 offset 是多少
- 访问路径是什么

所以 spec match 本身就是“匹配 + 目标路径重建”。

## 14. field/type/enum 三类 relocation 在匹配阶段就已经分流

在 `bpf_core_spec_match()` 里能看到：

- type-based relocation 会走 type compatibility / type matches
- enumval-based relocation 会遍历枚举项名字
- field-based relocation 会走 member 递归匹配

这意味着 CO-RE 不是先统一 match 再统一求值，而是一开始就根据 relocation kind 进入不同解释器。

## 15. 用一句流程记住这层逻辑

可以把这一层记成：

1. 用 essential name 先做名字级筛选
2. 对 field relocation，递归穿透匿名 composite 找同名 member
3. 用 `bpf_core_fields_are_compat()` 判断这条 field 路径能不能成立
4. 对 type relocation，用 `__bpf_core_types_match()` 或 compat 规则做更严格递归匹配
5. 一旦匹配成立，同时构造 target spec 和 bit offset

## 16. 一句话压缩版

如果把这条线压成一句话，可以这样记：

> CO-RE 的 `spec_match` 不是简单的名字比较，而是沿着本地 access path 在目标 BTF 里递归重建一条语义等价路径；其中 field relocation 用较宽松的字段兼容性规则保证“访问还能成立”，而 type relocation 用更严格的递归类型匹配保证“类型语义仍然成立”。 
