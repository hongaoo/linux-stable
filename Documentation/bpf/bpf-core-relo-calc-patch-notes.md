# CO-RE 深一层：relo 求值与指令 patch

这份文档继续沿 [Documentation/bpf/bpf-core-apply-notes.md](Documentation/bpf/bpf-core-apply-notes.md) 往下钻，专门看两块：

- [bpf_core_calc_relo_insn](tools/lib/bpf/relo_core.c#L1297)
- [bpf_core_patch_insn](tools/lib/bpf/relo_core.c#L1041)

目标是把 CO-RE 的“候选找到之后发生什么”讲清楚：

- local spec 是怎么解析出来的
- 多 candidate 之间怎么消歧
- field/type/enum relocation 是怎么求值的
- 最后不同类型的 BPF 指令是怎么被 patch 的

## 1. 先抓一句总话

如果把这一层压成一句话，可以这样记：

> `bpf_core_calc_relo_insn()` 负责把一条 CO-RE relocation 从“本地类型 + access string”解释成目标内核上的唯一 relocation 结果，而 `bpf_core_patch_insn()` 负责把这个结果安全地改写回具体 BPF 指令。

## 2. `bpf_core_calc_relo_insn()` 的主结构

在 [tools/lib/bpf/relo_core.c](tools/lib/bpf/relo_core.c#L1297) 里，`bpf_core_calc_relo_insn()` 大致做四件事：

1. 解析 local spec
2. 对每个 candidate 尝试 spec match
3. 对匹配 candidate 计算 relocation 结果
4. 检查多个 candidate 的结果是否一致

它的本质不是“直接算偏移”，而是“先解释、再匹配、再消歧、最后产出唯一结果”。

## 3. `bpf_core_parse_spec()` 为什么是第一步

在 [tools/lib/bpf/relo_core.c](tools/lib/bpf/relo_core.c#L151) 的 `bpf_core_parse_spec()`，access string 会被解析成结构化 spec。

这一步很关键，因为编译器给出的 relocation 信息并不是 verifier 直接好用的最终形式。

它需要先被拆成：

- raw spec
- 更高层的 accessor/spec 表示
- 累积 bit offset

也就是说，CO-RE 不是直接拿着原始 access string 就算，而是先把它编译成中间表示。

## 4. local spec 为什么重要

`bpf_core_calc_relo_insn()` 一开始就先对 local BTF 做 `bpf_core_parse_spec()`，得到 `local_spec`。

这是因为 CO-RE 不是单纯比较“目标长什么样”，而是必须先精确知道：

- 程序原本想访问本地类型语义中的哪一层字段/数组/枚举值

只有 local intent 先被表达清楚，后面 target match 才有意义。

## 5. `bpf_core_spec_match()` 在 candidate 上做什么

在 `bpf_core_calc_relo_insn()` 里，对每个 candidate 都会调 `bpf_core_spec_match()`。

它的含义是：

- 看本地 spec 所表达的访问意图，能不能在这个 target candidate 上找到语义对应物

如果不匹配，这个 candidate 会被直接丢掉；如果匹配，才进入下一步 relocation 求值。

所以 candidate 搜索只是按名字/kind 粗筛，真正决定“这个 candidate 能不能用”的，是 spec match。

## 6. 多 candidate 为什么还要强制结果一致

`bpf_core_calc_relo_insn()` 里最重要的一点之一，是它不满足于“有多个 candidate 都能 match 就行”。

它还要求：

- 多个 matching candidate 的 bit offset 一致
- relocation decision 一致
- `new_val` 一致

否则就会报 ambiguity。

这非常关键，因为 CO-RE 的目标是产出唯一、安全的 patch 结果，而不是“有几种可能都行”。

## 7. `bpf_core_calc_relo()` 的角色

在 [tools/lib/bpf/relo_core.c](tools/lib/bpf/relo_core.c#L824) 的 `bpf_core_calc_relo()`，可以看到 relocation 求值会根据 kind 再分成三类：

- field-based relocation
- type-based relocation
- enumval-based relocation

这意味着 CO-RE 不是只有“字段偏移修正”，而是覆盖了：

- 字段偏移/大小/符号性
- 类型存在性/匹配性/大小/ID
- 枚举值是否存在/值是多少

## 8. field relocation 真正在算什么

在 [bpf_core_calc_field_relo](tools/lib/bpf/relo_core.c#L679) 里，可以看到 field relocation 会产出：

- `byte_off`
- `byte_sz`
- signedness
- bitfield 相关 shift 信息

这说明字段 CO-RE 不只是“把 off 改成另一个 off”，它还会为 bitfield、load/store 大小调整等问题准备完整信息。

## 9. 为什么 bitfield 会变复杂

`bpf_core_calc_field_relo()` 对 bitfield 做了特殊处理，因为 bitfield 的：

- bit offset
- byte offset
- byte size
- left/right shift

都可能和普通字段不同，而且编译器与目标布局之间的歧义更大。

所以 bitfield relocation 在 CO-RE 里天然比普通字段更敏感，也更容易触发保守处理或 patch 失败。

## 10. `EUCLEAN` 为什么会触发 poison

在 `bpf_core_calc_relo()` 里，`-EUCLEAN` 被当成一种特殊信号：

- 请求对指令做 poison，而不是立刻把整个流程当成普通 hard error

这背后的语义是：

- 这条 relocation 可能在某些 guarded dead code 路径里不可用
- 与其立刻全局失败，不如先把指令毒化成无效形式
- 后续如果它确实不可达，verifier 会把它消掉

这也是 CO-RE 与 verifier 协同设计的一个很典型例子。

## 11. `bpf_core_patch_insn()` 支持哪些指令类

在 [tools/lib/bpf/relo_core.c](tools/lib/bpf/relo_core.c#L1041) 的函数注释里，已经明确列出它支持的几类可 patch 指令：

- 立即数赋值
- 立即数 ALU
- `ldimm64`
- `LDX`
- `STX`
- `ST`

这说明 CO-RE patch 的目标不是任意 BPF 指令，而是那几类与 layout / immediate / offset 直接相关的可重定位指令。

## 12. patch 前为什么还要做 expected value 验证

`bpf_core_patch_insn()` 在很多路径里都会先检查：

- 当前 insn 里的 `imm` / `off` 是否仍然等于它期望的 original value

这样做的意义是：

- 防止链接或其它预处理之后，当前指令已经和 relocation 最初假定的状态脱节

如果期望值对不上，CO-RE 不会盲 patch，而是直接报错。

所以 patch 不是“无脑覆盖”，而是“先确认上下文仍然正确，再改写”。

## 13. load/store 的 mem size 为什么还可能一起调整

在 `LDX/ST/STX` 路径里，`bpf_core_patch_insn()` 不只会改 offset，还可能根据 `orig_sz` / `new_sz` 去调整 mem size。

但它只在非常有限的安全场景下允许这么做，例如：

- 指针读写的某些安全情况
- unsigned integer 某些可零扩展场景

否则就会把这次 patch 视为不安全，甚至走 poison。

这说明 CO-RE 不是仅仅“改偏移”，而是在谨慎维护 load/store 语义是否仍然成立。

## 14. poison 指令到底在干什么

在 [bpf_core_poison_insn](tools/lib/bpf/relo_core.c#L983) 里，可以看到 poison 的做法是：

- 把目标 insn 改成一个带特定无效 helper ID 的调用

如果这条指令最终仍然可达，verifier 就会报错；如果它在 dead code 中，后续优化/验证阶段可能把它消掉。

所以 poison 不是“沉默忽略失败”，而是把失败显式编码进程序里，交给 verifier 再做最终裁决。

## 15. `TYPE_ID_LOCAL` 为什么特殊

在 `bpf_core_calc_relo_insn()` 里，`BPF_CORE_TYPE_ID_LOCAL` 是单独 special case。

原因是它本质上不需要 target candidate 搜索，也不依赖跨内核匹配；它只是把本地 type id 留在程序语义里，所以处理路径比其它 relocation 简单得多。

这再次说明 CO-RE relocation 并不是一套完全统一的处理模型，而是有一些 kind 带着明确特例。

## 16. 用一条流水线记住这两步

可以把这层逻辑压成下面这条顺序：

1. `bpf_core_parse_spec()` 解析 local access spec
2. 对每个 candidate 做 `bpf_core_spec_match()`
3. 对匹配 candidate 做 `bpf_core_calc_relo()`
4. 强制多个 candidate 的结果一致，否则报 ambiguity
5. `bpf_core_patch_insn()` 校验原值并 patch 到目标指令
6. 若 relocation 失败但允许 guarded dead path，则 poison 指令

## 17. 一句话压缩版

如果把这条线压成一句话，可以这样记：

> `bpf_core_calc_relo_insn()` 负责把本地类型访问意图解析成唯一、无歧义的目标 relocation 结果，而 `bpf_core_patch_insn()` 负责在验证原始指令上下文仍然正确的前提下，把这个结果安全地改写回具体 BPF 指令；做不到时，就显式 poison 留给 verifier 最终裁决。 

如果想继续把 “candidate 为什么算匹配” 这一层单独拆开，看 `bpf_core_spec_match()`、`bpf_core_match_member()`、`bpf_core_fields_are_compat()` 和 `__bpf_core_types_match()` 如何定义 field/type match 规则，可以继续看补充文档 [Documentation/bpf/bpf-core-spec-match-notes.md](Documentation/bpf/bpf-core-spec-match-notes.md)。
