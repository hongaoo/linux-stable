# BTF 在 load 与 verifier 中的源码入口

这份文档把 BTF 从“概念层”继续往下落到源码入口，重点串起这条链：

- `BPF_PROG_LOAD` 怎么把 program BTF / attach BTF 带进内核
- `check_btf.c` 怎么把 func_info / line_info / CO-RE relocation 接进 verifier
- `check_attach_btf_id()` 怎么把 attach 语义落到具体目标函数

目标不是穷举 BTF 子系统全部实现，而是把 BTF 在 load/verifier 路径里的关键落点串成一条工程可读的路线图。

## 1. 先抓一句总话

如果把 BTF 在 load/verifier 中的角色压成一句话，可以这样记：

> BTF 在 `BPF_PROG_LOAD` 里先以 program BTF 和 attach BTF 的形式进入内核，在 `check_btf.c` 中被解析成 func_info、line_info 和 CO-RE relocation 的验证材料，再在 verifier 主流程里和 attach target、subprog 原型、日志/源码定位一起参与程序语义验证。

## 2. 第一条入口：program BTF 通过 `prog_btf_fd` 进入

program 自身的 BTF 不是在 libbpf 文档里抽象存在的，它在 verifier 里有很明确的入口：

- [kernel/bpf/check_btf.c](kernel/bpf/check_btf.c#L410)

也就是 `bpf_check_btf_info_early()`。

这一步会：

- 通过 `attr->prog_btf_fd` 拿到 BTF 对象
- 写入 `env->prog->aux->btf`

这说明 program BTF 是在 verifier 早期就被挂到 `prog->aux` 上的，后面所有和函数原型、line info、日志定位相关的逻辑，都是基于这一步建立起来的。

## 3. 第二条入口：attach BTF 通过 `BPF_PROG_LOAD` 进入

attach 目标相关的 BTF 则是在 syscall 层先进入：

- [kernel/bpf/syscall.c](kernel/bpf/syscall.c#L2939)
- [kernel/bpf/syscall.c](kernel/bpf/syscall.c#L2998)

在 [bpf_prog_load](kernel/bpf/syscall.c#L2864) 里，内核会解析：

- `attach_prog_fd`
- `attach_btf_obj_fd`
- `attach_btf_id`

然后把解析出来的 attach BTF 写入：

- `prog->aux->attach_btf`
- `prog->aux->attach_btf_id`

这说明 BTF 在 load 路径里至少有两类不同来源：

- program BTF
- attach target BTF

两者用途不同，但都会进入 verifier 后续语义检查。

## 4. 为什么 `bpf_check_btf_info_early()` 要在 subprog 之前

在 [kernel/bpf/verifier.c](kernel/bpf/verifier.c#L19992) 里，`bpf_check_btf_info_early()` 发生在：

- `add_subprog_and_kfunc()` 之前
- `check_subprogs()` 之前

这顺序非常重要。

原因是：

- 后续 subprog 识别和函数原型一致性检查，需要先有 BTF func_info 材料

所以 early 阶段的含义不是“只做一半 BTF”，而是“先把后续结构分析所必须的 BTF 材料接进来”。

## 5. `check_btf_func_early()` 在校验什么

在 [kernel/bpf/check_btf.c](kernel/bpf/check_btf.c#L19) 的 `check_btf_func_early()`，可以看到 early 阶段主要检查的是：

- `func_info_rec_size`
- `func_info_cnt`
- 每条 `func_info` 的 `insn_off`
- `type_id` 是否真指向 BTF function

它最终会把：

- `prog->aux->func_info`
- `prog->aux->func_info_cnt`

填起来。

这一步的本质是：先把“程序里有哪些函数边界、它们声称对应哪些 BTF 函数类型”这件事建立起来。

## 6. `bpf_check_btf_info()` 在补什么

真正更完整的 BTF 验证是在：

- [kernel/bpf/check_btf.c](kernel/bpf/check_btf.c#L438)

也就是 `bpf_check_btf_info()`。

它会继续串三件事：

- `check_btf_func()`
- `check_btf_line()`
- `check_core_relo()`

这里就能看出 BTF 在 verifier 里的三种典型用途：

- 描述函数原型
- 描述源码位置
- 描述 CO-RE 需要重定位的地方

## 7. `check_btf_func()` 为什么是 subprog 语义桥梁

在 [kernel/bpf/check_btf.c](kernel/bpf/check_btf.c#L113) 的 `check_btf_func()` 里，代码会检查：

- `func_info_cnt` 是否等于 `subprog_cnt`
- `func_info[].insn_off` 是否和 `subprog_info[].start` 对齐
- 对应返回类型是否允许 `LD_ABS` / `tail_call`

这说明 `func_info` 不是给调试器看的附属注释，而是 verifier 用来把：

- BTF 函数原型
- 实际 BPF subprog 边界

绑定在一起的结构材料。

## 8. `check_btf_line()` 在 verifier 里有什么实际价值

在 [kernel/bpf/check_btf.c](kernel/bpf/check_btf.c#L210) 的 `check_btf_line()`，内核会验证：

- `line_info` 记录顺序
- `insn_off` 合法性
- `file_name_off` / `line_off` 是否有效
- 每个 subprog 是否有对应 line info 起点

它最终写入：

- `prog->aux->linfo`
- `prog->aux->nr_linfo`

这使得 verifier log、bpftool、JIT line info、源码定位等能力，都能把“某条 BPF 指令”映射回“源文件某行”。

所以 line info 虽然不直接决定程序是否安全，但它是现代 BPF 可诊断性的关键基础设施。

## 9. `check_core_relo()` 是 CO-RE 在 verifier 路径里的直接落点

在 [kernel/bpf/check_btf.c](kernel/bpf/check_btf.c#L341) 的 `check_core_relo()`，CO-RE 真正进入 verifier 路径。

这里会：

- 从 attr 读取 `core_relos`
- 校验每条 relocation record
- 最终调用 [bpf_core_apply](kernel/bpf/btf.c#L9495)

这意味着 CO-RE 在内核侧并不是一个模糊概念，而是明确地：

- 读取 relocation record
- 基于 program BTF 和目标 BTF 计算结果
- 直接 patch 到程序指令上

## 10. `bpf_core_apply()` 为什么值得知道

虽然大多数阅读这批文档时不需要深入整个 BTF 子系统，但 [kernel/bpf/btf.c](kernel/bpf/btf.c#L9495) 的 `bpf_core_apply()` 很值得记住，因为它是内核侧 CO-RE patch 的关键落点。

从架构上看，`check_core_relo()` 更像：

- verifier / load path 里的调度入口

而 `bpf_core_apply()` 更像：

- 真正执行类型匹配、candidate 选择和指令 patch 的核心位置

## 11. attach BTF 在 verifier 里怎么继续发挥作用

attach BTF 不会在 syscall.c 写进 `prog->aux` 后就结束，它会继续进入：

- [kernel/bpf/verifier.c](kernel/bpf/verifier.c#L19528)

也就是 `check_attach_btf_id()`。

这里 verifier 会根据：

- `prog->aux->attach_btf`
- `prog->aux->attach_btf_id`
- `dst_prog`

去解析真正的 attach target。

对 tracing / LSM / EXT / struct_ops 程序来说，这一步非常关键，因为程序能否成立，不只是看字节码本身，还要看它到底打算附着到哪个函数/对象语义上。

## 12. `check_attach_btf_id()` 最终产出什么

在 [kernel/bpf/verifier.c](kernel/bpf/verifier.c#L19528) 之后，`check_attach_btf_id()` 不只是报错或放行，它还会把 attach target 的结果写回 `prog->aux`，例如：

- `attach_func_proto`
- `attach_func_name`
- `mod`
- `dst_trampoline`

这说明 attach BTF 最终会变成 verifier 和后续 trampoline/link 逻辑真正消费的运行时语义材料，而不是只做一个“ID 合法性检查”。

## 13. program BTF、attach BTF、vmlinux BTF 三者怎么区分

在源码路径里，最好把这三类 BTF 分开记：

- program BTF：来自 `prog_btf_fd`，挂在 `prog->aux->btf`
- attach BTF：来自 `attach_btf_obj_fd` 或 vmlinux fallback，挂在 `prog->aux->attach_btf`
- in-kernel vmlinux BTF：通过 [bpf_get_btf_vmlinux](kernel/bpf/verifier.c#L19608) 获取，供 attach target 与 CO-RE 等场景使用

这三者经常一起出现，但职责不同：

- program BTF 描述“我自己是什么”
- attach BTF 描述“我要挂到谁”
- vmlinux BTF 描述“当前内核实际有什么类型语义”

## 14. 用一条流水线记住 BTF 在 load/verifier 里的位置

可以把这条路径记成：

1. `BPF_PROG_LOAD` 在 syscall 层解析 `attach_btf` / `attach_btf_id`
2. verifier 早期用 `prog_btf_fd` 取到 program BTF
3. `check_btf_func_early()` 先把 func_info 接进来
4. subprog 识别完成后，`check_btf_func()` / `check_btf_line()` / `check_core_relo()` 继续补全
5. `check_core_relo()` 通过 `bpf_core_apply()` 真正应用 CO-RE patch
6. `check_attach_btf_id()` 再把 attach target 语义和 trampoline 信息落实到 `prog->aux`

## 15. 一句话压缩版

如果把这条线压成一句话，可以这样记：

> BTF 在内核里不是单一对象，而是以 program BTF、attach BTF 和目标内核 BTF 三种角色进入 `BPF_PROG_LOAD` 与 verifier 流程：`check_btf.c` 用它们建立函数/源码/CO-RE 语义材料，`check_attach_btf_id()` 再把 attach 目标函数模型落实成 verifier 与 trampoline 真正消费的程序语义。 

如果想继续把 CO-RE 的内核侧核心动作拆开，看 `bpf_core_find_cands()` 如何找目标类型候选、`bpf_core_apply()` 如何计算 relocation 并 patch 指令，可以继续看补充文档 [Documentation/bpf/bpf-core-apply-notes.md](Documentation/bpf/bpf-core-apply-notes.md)。
