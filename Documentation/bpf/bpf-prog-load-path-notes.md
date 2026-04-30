# BPF_PROG_LOAD 到 verifier/BTF 的进入路径

这份文档把 [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c) 里的 `BPF_PROG_LOAD` 单独拎出来，顺着 [bpf_prog_load](kernel/bpf/syscall.c#L2864) 看一次完整进入路径：

- 用户态 attr 在哪里做基础检查
- attach_btf / dst_prog 在哪里绑定
- plain `bpf_prog` 对象何时分配
- 何时进入安全钩子
- 何时真正进入 verifier
- 成功后何时分配 ID 和 fd

## 1. 总锚点：`bpf_prog_load()`

`BPF_PROG_LOAD` 在 syscall 层的核心入口是：

- [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c#L2864)

如果要理解“用户态提交一段 BPF 程序后，内核先做什么、后做什么”，这就是最值得顺着往下读的函数。

## 2. 第一层：基础 attr 与 flag 检查

函数开头先做的是很传统但非常关键的一层：

- `CHECK_ATTR(BPF_PROG_LOAD)`
- `prog_flags` 白名单检查
- `bpf_prog_load_fixup_attach_type(attr)`

这里的重点不是复杂逻辑，而是：

- syscall 层先把用户态输入整理成内核可接受的语义形态

也就是说，在 verifier 看到程序之前，attr 本身已经先被过滤和规范化了一轮。

## 3. 第二层：token 与 capability 边界

在 [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c#L2888) 之后的一段里，可以看到：

- token fd 解析
- token 是否允许 `BPF_PROG_LOAD`
- token 是否允许该 `prog_type` / `expected_attach_type`
- `CAP_BPF`
- `CAP_NET_ADMIN`
- `CAP_PERFMON`

这说明 `BPF_PROG_LOAD` 并不是“只要能调用 syscall 就能进 verifier”。

程序能否进入后续装载流程，首先要跨过 syscall 层的权限边界。

## 4. 第三层：attach 目标解析

在 [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c#L2935) 一带，可以看到 `attach_prog_fd` / `attach_btf_obj_fd` / `attach_btf_id` 的处理。

这里 syscall 层会判断：

- 当前 attach 指向的是另一个 bpf_prog
- 还是某个 BTF 对象
- 或者直接回退到 vmlinux BTF

这一步的重要性在于：

- 程序不是孤立加载的
- 某些 prog type 的语义和 attach 目标/BTF 元信息直接相关

所以在 verifier 之前，程序的“将要附着到谁”就已经开始被组织起来了。

## 5. `bpf_prog_load_check_attach()` 在提前锁定语义合法性

在 [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c#L2974) 调用 [bpf_prog_load_check_attach](kernel/bpf/syscall.c#L2646) 时，内核会做 program type 和 attach type 的匹配检查。

这一步很关键，因为它说明：

- verifier 不是第一个理解程序语义的地方
- syscall 层已经先验证“你宣称自己是哪类程序，准备挂到什么语义位置”是否成立

因此 program load 在进入 verifier 之前，已经先做过一层“类型系统级别的外部语义检查”。

## 6. 第四层：plain `bpf_prog` 对象分配

在 [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c#L2986) 的 `bpf_prog_alloc()`，真正的内核 `bpf_prog` 对象开始被分配。

这一步之后，代码会继续填充：

- `expected_attach_type`
- `sleepable`
- `attach_btf`
- `attach_btf_id`
- `dst_prog`
- `dev_bound`
- `xdp_has_frags`

这说明 verifier 之后看到的 `prog` 已经不是一块裸指令缓存，而是一个带装载上下文的程序对象。

## 7. 第五层：从用户态复制 insns 和 license

在 [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c#L3009) 之后，`bpf_prog_load()` 会把：

- `insns`
- `license`

从用户态复制进来。

这两样都很关键：

- 指令当然是 verifier/JIT 的直接输入
- `license` 则决定程序是否能使用 GPL-only helper/kfunc

所以从装载视角看，license 不是附属信息，而是 capability/功能边界的一部分。

## 8. 第六层：签名、dev-bound、program type 绑定

在 verifier 之前，还会继续走一些准备步骤：

- 可选签名校验 [bpf_prog_verify_signature](kernel/bpf/syscall.c#L2800)
- dev-bound 初始化/继承
- `find_prog_type(type, prog)`
- 程序名和 load time 填充

这里特别值得注意的是 [find_prog_type](kernel/bpf/syscall.c#L2272)。

它的意义不是“再做一次普通检查”，而是把 `prog_type` 映射到后续真正控制 verifier/JIT/attach 语义的程序类型实现上。

## 9. 第七层：LSM/security 钩子在 verifier 之前

在 [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c#L3077) 的 `security_bpf_prog_load()`，安全模块有机会在 verifier 之前介入。

这说明 `BPF_PROG_LOAD` 的控制链条是：

- syscall 输入校验
- 权限检查
- 语义组织
- LSM 安全决策
- verifier

而不是“直接扔给 verifier 再说”。

## 10. 真正进入 verifier 的交接点在哪里

最关键的交接点就在：

- [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c#L3082)

也就是：

- `err = bpf_check(&prog, attr, uattr, uattr_size);`

这就是 `syscall.c` 向 verifier 交棒的地方。

从架构上可以把它理解成：

- `syscall.c` 负责把“用户请求”整理成“可验证的内核程序对象”
- `bpf_check()` 负责证明这个程序对象在目标语义下是安全可接受的

## 11. BTF 是怎么进入这条路径的

`BPF_PROG_LOAD` 到 verifier/BTF 的关系，不是只有 CO-RE 那么简单。

在 `bpf_prog_load()` 里，BTF 主要通过几条路进入：

- `attach_btf` / `attach_btf_id` 进入 `prog->aux`
- `dst_prog` 也可能把 attach 目标语义带进来
- 后续 `bpf_check()` 会在 verifier 侧利用 program BTF、func_info、line_info、attach BTF 等元信息

所以对 tracing、LSM、struct_ops、ext 这类程序来说，BTF 不是可选装饰，而是程序语义的一部分。

## 12. verifier 成功之后还没结束

在 `bpf_check()` 成功之后，`bpf_prog_load()` 还会继续做：

- `bpf_prog_mark_insn_arrays_ready()`
- `bpf_prog_alloc_id()`
- `bpf_prog_kallsyms_add()`
- `perf_event_bpf_event(... PROG_LOAD ...)`
- `bpf_audit_prog()`
- `bpf_prog_new_fd()`

这说明“verifier 通过”不是装载的终点，而只是进入“发布可见对象”的前提条件。

从生命周期上说：

- verifier 证明程序可以存在
- alloc_id/new_fd 才让它真正对外成为可引用内核对象

## 13. 为什么 `bpf_prog_alloc_id()` 之后的失败处理会变得不同

在 [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c#L3090) 后的注释里，源码明确强调：

- 一旦 `bpf_prog_alloc_id()` 成功，程序就已经对外可见

这意味着：

- 之前失败，还是私有对象清理
- 之后失败，就必须按“已暴露对象”的规则释放

这是一条很重要的生命周期边界，说明 load path 不只是“验证”问题，也是一条对象发布路径。

## 14. 用一条流水线记住整个 `BPF_PROG_LOAD`

可以把 [bpf_prog_load](kernel/bpf/syscall.c#L2864) 压成下面这条流水线：

1. 检查 attr 和 flags
2. 修正/验证 attach 语义
3. 检查 token 与 capability
4. 解析 attach_btf / dst_prog
5. 分配 plain `bpf_prog`
6. 复制 insns 与 license
7. 做签名、安全、program type 相关准备
8. 进入 `bpf_check()`
9. verifier 通过后再分配 ID、写 kallsyms、发 event、创建 fd

## 15. 和 BTF/CO-RE 文档怎么连起来看

如果把它和 [Documentation/bpf/bpf-btf-core-notes.md](/home/hongao/github-workspace/linux-stable/Documentation/bpf/bpf-btf-core-notes.md) 放在一起看，会更容易理解：

- BTF/CO-RE 解决的是“用户态对象描述如何映射到目标内核类型语义”
- `BPF_PROG_LOAD` 解决的是“这些元信息和程序指令如何被带入内核，并交给 verifier 解释”

也就是说，BTF 是语义材料，`bpf_prog_load()` 是把这些材料送进内核验证/装载管线的入口。

## 16. 一句话压缩版

如果把这条线压成一句话，可以这样记：

> `BPF_PROG_LOAD` 在 [bpf_prog_load](kernel/bpf/syscall.c#L2864) 中先完成 attr、权限、attach 目标和程序对象的准备，再通过 [bpf_check](kernel/bpf/syscall.c#L3082) 把程序交给 verifier/BTF 语义处理，最后在验证成功后分配 ID、发布事件并创建 fd，把它变成真正对外可见的 BPF 程序对象。
