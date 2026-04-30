# CO-RE 内核侧核心：candidate 搜索与指令 patch

这份文档继续沿 [Documentation/bpf/bpf-btf-load-verifier-notes.md](Documentation/bpf/bpf-btf-load-verifier-notes.md) 往下钻，但只盯住 CO-RE 在内核里的核心动作：

- [bpf_core_find_cands](kernel/bpf/btf.c#L9408)
- [bpf_core_apply](kernel/bpf/btf.c#L9495)

目标是把 CO-RE 从“加载时会自动重定位”这句抽象话，压缩成源码里的几个明确步骤：

- 目标候选类型是怎么找的
- 名字匹配为什么不是简单字符串相等
- relocation 是怎么被计算的
- 最后又是怎么 patch 回具体指令的

## 1. 先抓一句总话

如果把 CO-RE 内核侧主流程压成一句话，可以这样记：

> CO-RE 在内核里的核心动作是：先根据本地 BTF 里的类型引用去 vmlinux 或模块 BTF 里找兼容候选，然后用这些候选计算目标布局下的 relocation 结果，最后把结果 patch 到对应 BPF 指令上。

## 2. 为什么 `check_core_relo()` 不是终点

在 [kernel/bpf/check_btf.c](kernel/bpf/check_btf.c#L341) 的 `check_core_relo()`，verifier/load 路径只是读取 relocation record 并逐条调用：

- [bpf_core_apply](kernel/bpf/btf.c#L9495)

所以 `check_core_relo()` 更像入口调度器；真正的 CO-RE 语义处理发生在 `bpf_core_apply()` 里。

## 3. `bpf_core_apply()` 做的事其实很工整

在 [kernel/bpf/btf.c](kernel/bpf/btf.c#L9495) 看 `bpf_core_apply()`，它的结构非常清晰：

1. 读取本地 type id
2. 如有必要，先找 target candidates
3. 调 `bpf_core_calc_relo_insn()` 计算 relocation 结果
4. 调 `bpf_core_patch_insn()` 把结果写回指令

所以它本质上是“候选搜索 + 重定位求值 + 指令 patch”三段式流程。

## 4. 为什么有些 relocation 需要 candidate，有些不需要

在 `bpf_core_apply()` 开头会看到：

- `need_cands = relo->kind != BPF_CORE_TYPE_ID_LOCAL`

这说明并不是所有 CO-RE relocation 都需要去目标内核里找候选类型。

只有那些真正依赖“目标类型语义”的 relocation，才需要 candidate 搜索；而 purely local 的那类，不需要跨 BTF 匹配。

这也反映出 CO-RE 不是一个单一 relocation 种类，而是一组以类型语义为核心的 relocation 家族。

## 5. `bpf_core_find_cands()` 在找什么

在 [kernel/bpf/btf.c](kernel/bpf/btf.c#L9408) 的 `bpf_core_find_cands()`，内核会根据本地 BTF 里的一个 `local_type_id` 去找 target candidates。

它不是简单“拿 type id 去另一边查同号 ID”，而是按这些条件找：

- kind 相同
- essential name 相同
- 先查 vmlinux BTF
- 找不到再查 module BTF

这说明 CO-RE 的核心不是“ID 对 ID”，而是“语义匹配后再重定位”。

## 6. 为什么名字匹配不是直接全名相等

这里最值得注意的是：

- [bpf_core_essential_name_len](kernel/bpf/btf.c#L9203)

CO-RE 并不总是直接用完整名字匹配，而是先提取 essential name。

这样做的原因是要兼容某些 flavor / variant 命名模式，例如带 `___` 分隔的变体名。

也就是说，CO-RE 在名称层面也不是死板字符串匹配，而是在做一种“忽略 flavor 噪音后的语义匹配”。

## 7. 为什么先查 vmlinux，再查 module

`bpf_core_find_cands()` 的顺序是：

1. 先查 vmlinux BTF
2. 如果 vmlinux 有候选，就不再查 module
3. 如果 vmlinux 没候选，再去遍历 module BTF

这很符合工程预期，因为大多数稳定目标类型首先都来自主内核 BTF。

只有当主内核没找到时，才需要扩大到模块 BTF 空间里搜索。

## 8. candidate cache 为什么存在

在 [kernel/bpf/btf.c](kernel/bpf/btf.c#L9234) 往后，可以看到：

- `vmlinux_cand_cache`
- `module_cand_cache`
- `check_cand_cache()`
- `populate_cand_cache()`

这说明 candidate 搜索在内核里被明确当成了值得缓存的昂贵操作。

原因很直接：

- CO-RE relocation 可能很多
- 相同类型名/kind 的查询会反复发生
- 全量扫描 BTF 类型表和模块集合代价不低

所以 CO-RE 并不是“纯功能逻辑”，实现上也很重视搜索性能。

## 9. 为什么还要特别处理模块卸载

在 `bpf_core_find_cands()` 和 `bpf_core_apply()` 里能看到：

- candidate cache mutex
- 模块 BTF 引用保护
- module cache purge 逻辑

这说明 module BTF 带来一个额外问题：

- 候选不只是“找到就行”
- 还必须保证在 relocation 计算期间目标 BTF 不会被并发卸载掉

因此 CO-RE 的实现并不只是类型匹配，还包含了内核对象生命周期管理。

## 10. `bpf_core_calc_relo_insn()` 在抽象上做什么

虽然这次没有继续往下展开 `bpf_core_calc_relo_insn()` 本体，但从 `bpf_core_apply()` 的位置关系可以很明确地看出，它负责的是：

- 根据本地 BTF、relocation spec、候选 target type
- 算出目标内核布局下应该使用的最终 relocation 结果

可以把它理解成 CO-RE 的“求值器”。

而 `bpf_core_patch_insn()` 则更像“写回器”。

## 11. `bpf_core_patch_insn()` 为什么重要

CO-RE 最后必须落回一条真实 BPF 指令，不然前面所有匹配和计算都只是纸面分析。

所以 [bpf_core_patch_insn](kernel/bpf/btf.c#L9560) 的意义在于：

- 把求得的 target relocation 结果真正 patch 到程序里的目标 insn 上

这一步标志着 CO-RE 从“语义匹配”走到了“代码改写”。

## 12. 为什么 `specs` 会分配临时内存

在 `bpf_core_apply()` 开头还能看到一段说明：

- 需要分配一块临时内存，把 LLVM 风格的 spec 转成内核侧更好处理的结构

这说明 CO-RE 在内核里并不是直接拿原始字符串 spec 就算，而是要先把它解析成结构化中间表示。

这也符合编译器式管线的典型模式：

- 原始 spec
- 结构化中间表示
- 求值结果
- 指令 patch

## 13. `bpf_core_types_match()` 与 candidate 搜索的关系

在 [kernel/bpf/btf.c](kernel/bpf/btf.c#L9188) 的 `bpf_core_types_match()` 可以看出，CO-RE 运行时并不是只按名字选 candidate。

名字只是第一层筛选；更深层仍然要靠类型兼容/匹配逻辑来判断这个 target type 是否真的可以作为 relocation 目标。

因此 candidate 搜索和最终 relocation 计算的关系大致是：

- 名字/种类搜索先缩小集合
- 类型匹配与 spec 计算再决定最终是否成立

## 14. 用一条流水线记住 `bpf_core_apply()`

可以把 [bpf_core_apply](kernel/bpf/btf.c#L9495) 压成下面这条顺序：

1. 取本地 relocation 对应的 `type_id`
2. 如有必要，用 `bpf_core_find_cands()` 找目标候选
3. 用 `bpf_core_calc_relo_insn()` 计算 target relocation 结果
4. 用 `bpf_core_patch_insn()` 把结果 patch 到目标指令

## 15. 把它和高层 BTF/CO-RE 叙述怎么接起来

如果把它和 [Documentation/bpf/bpf-btf-core-notes.md](Documentation/bpf/bpf-btf-core-notes.md) 放在一起看，会更容易理解：

- 高层文档讲的是“为什么 CO-RE 能 compile once, run everywhere”
- 这份源码对照讲的是“内核里具体是怎么在 BTF 上找候选、算偏移并改指令的”

也就是说，CO-RE 的工程化抽象最终在内核里落实成了明确的 candidate 搜索与 patch 流水线。

## 16. 一句话压缩版

如果把这条线压成一句话，可以这样记：

> `bpf_core_apply()` 是 CO-RE 在内核里的核心执行点：它先按类型 kind 和 essential name 去 vmlinux/模块 BTF 中找候选，再根据 relocation spec 计算目标结果，最后把结果 patch 到对应 BPF 指令上。 

如果想继续把 `bpf_core_calc_relo_insn()` 和 `bpf_core_patch_insn()` 本体拆开，看 spec 解析、candidate 歧义消解、field/type/enum relocation 求值，以及 load/store/alu/ldimm64 各类指令如何被 patch，可以继续看补充文档 [Documentation/bpf/bpf-core-relo-calc-patch-notes.md](Documentation/bpf/bpf-core-relo-calc-patch-notes.md)。
