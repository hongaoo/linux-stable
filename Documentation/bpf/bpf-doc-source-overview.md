# BPF 文档到源码实现总对照

这份文档把当前已经整理的 BPF 文档主线，粗粒度映射到 Linux 内核里的实现目录和关键文件，目的是提供一张“从概念到实现”的总览图，而不是覆盖所有细节。

## 1. 总体对应关系

可以先用下面这张简化表来记：

| 文档层主题 | 关注点 | 主要源码方向 |
| --- | --- | --- |
| ISA / ABI | 指令、寄存器、调用约定 | `include/uapi/linux/bpf.h`、JIT/解释器相关实现 |
| verifier | 安全证明、状态模拟 | `kernel/bpf/verifier.c`、`kernel/bpf/cfg.c` |
| program type | 执行语义、context、返回值 | `kernel/bpf/` 各 program type 相关实现、各子系统 hook |
| maps | 状态容器与访问 | `kernel/bpf/` 各 map 实现文件 |
| helpers | 稳定能力入口 | `kernel/bpf/helpers.c` 及 verifier helper 检查逻辑 |
| kfuncs | 受控开放的内核能力 | `kernel/bpf/verifier.c` + 各子系统注册的 kfunc 集 |
| BTF / CO-RE | 类型描述、重定位 | `kernel/bpf/btf.c`、libbpf/loader 路径、BTF UAPI |
| libbpf / syscall | 用户态装载流程 | `kernel/bpf/syscall.c`、`tools/lib/bpf/` |

## 2. 文档主线到源码目录

### 2.1 标准化层

文档：

- [Documentation/bpf/standardization/instruction-set.rst](Documentation/bpf/standardization/instruction-set.rst)
- [Documentation/bpf/standardization/abi.rst](Documentation/bpf/standardization/abi.rst)

源码上最相关的不是单一文件，而是几类位置：

- `include/uapi/linux/bpf.h`：用户可见 BPF 指令和 UAPI 常量
- 各架构 BPF JIT 实现：把 BPF ISA 映射到目标机器码
- 解释器路径与校验路径：体现指令语义如何被消费

这一层更像“规范来源”，不完全对应单个实现文件。

### 2.2 verifier 层

文档：

- [Documentation/bpf/verifier.rst](Documentation/bpf/verifier.rst)
- [Documentation/bpf/bpf-verifier-notes.md](Documentation/bpf/bpf-verifier-notes.md)

源码：

- [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c)
- [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c)

分工可以压成：

- `cfg.c`：控制流图、可达性、回边、postorder、SCC
- `verifier.c`：寄存器/栈/指针/引用/参数/返回值的状态级安全证明

### 2.3 program type / attach point 层

文档：

- [Documentation/bpf/programs.rst](Documentation/bpf/programs.rst)
- [Documentation/bpf/bpf-program-types-notes.md](Documentation/bpf/bpf-program-types-notes.md)

源码：

- `kernel/bpf/verifier.c` 里的 `bpf_verifier_ops[]`
- 各 program type 对应的 verifier ops
- 各子系统 attach 点所在源码，例如网络、LSM、trace、cgroup 路径

这一层没有单一总文件，因为它天然跨多个子系统。

### 2.4 maps 层

文档：

- [Documentation/bpf/maps.rst](Documentation/bpf/maps.rst)
- 各 `map_*.rst`
- [Documentation/bpf/bpf-maps-helpers-kfuncs-notes.md](Documentation/bpf/bpf-maps-helpers-kfuncs-notes.md)

源码：

- `kernel/bpf/` 下各 map 实现文件，例如 cpumap、devmap、sockmap、struct_ops map 等
- [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c) 中的 map 访问检查逻辑

这一层体现的是“对象实现 + verifier 访问约束”双重结构。

### 2.5 helpers 层

文档：

- [Documentation/bpf/helpers.rst](Documentation/bpf/helpers.rst)
- `bpf-helpers(7)`

源码：

- helper 本体实现
- [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c) 中 helper prototype 与参数检查逻辑

理解 helper 时，最好总是把“函数本体”和“verifier 如何认识它”一起看。

### 2.6 kfunc 层

文档：

- [Documentation/bpf/kfuncs.rst](Documentation/bpf/kfuncs.rst)
- [Documentation/bpf/bpf-maps-helpers-kfuncs-notes.md](Documentation/bpf/bpf-maps-helpers-kfuncs-notes.md)

源码：

- [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c) 中 kfunc 元数据、BTF、原型解析与校验逻辑
- 各子系统内通过 BTF/kfunc 集注册开放给 BPF 的函数

这一层本质上是“verifier + BTF + 子系统注册”的组合。

### 2.7 BTF / CO-RE 层

文档：

- [Documentation/bpf/btf.rst](Documentation/bpf/btf.rst)
- [Documentation/bpf/llvm_reloc.rst](Documentation/bpf/llvm_reloc.rst)
- [Documentation/bpf/bpf-btf-core-notes.md](Documentation/bpf/bpf-btf-core-notes.md)

源码方向：

- `kernel/bpf/` 中的 BTF 相关实现
- libbpf 在 `tools/lib/bpf/` 中的对象解析与 relocation 逻辑
- syscall / loader 路径与内核 BTF 消费逻辑

这一层天然跨内核和用户态工具链。

### 2.8 用户态装载层

文档：

- [Documentation/bpf/syscall_api.rst](Documentation/bpf/syscall_api.rst)
- [Documentation/bpf/libbpf/libbpf_overview.rst](Documentation/bpf/libbpf/libbpf_overview.rst)

源码：

- `kernel/bpf/syscall.c`
- `tools/lib/bpf/`

这里是一条清晰的“内核入口 + 用户态 loader”双端链路。

## 3. 当前最关键的一组源码入口

如果只挑少数最值得先建立地图感的文件，我建议优先记住：

- [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c)
- [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c)
- [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c)

再往外扩：

- `kernel/bpf/` 下各 map 实现
- 各子系统中的 attach/hook 实现
- `tools/lib/bpf/` 中的 libbpf 装载逻辑

## 4. 和当前专题文档的关系

这份总对照页可以和下面这些文档配合使用：

- 总览：[Documentation/bpf/bpf-architecture-summary.md](Documentation/bpf/bpf-architecture-summary.md)
- ABI：[Documentation/bpf/bpf-abi-notes.md](Documentation/bpf/bpf-abi-notes.md)
- verifier：[Documentation/bpf/bpf-verifier-notes.md](Documentation/bpf/bpf-verifier-notes.md)
- verifier 源码对照：[Documentation/bpf/bpf-verifier-source-map.md](Documentation/bpf/bpf-verifier-source-map.md)
- verifier 指针/map 访问细化：[Documentation/bpf/bpf-pointer-map-access-notes.md](Documentation/bpf/bpf-pointer-map-access-notes.md)
- helper/kfunc 返回值与 R0 写回：[Documentation/bpf/bpf-helper-kfunc-ret-notes.md](Documentation/bpf/bpf-helper-kfunc-ret-notes.md)
- verifier 总入口与主流程：[Documentation/bpf/bpf-check-main-flow-notes.md](Documentation/bpf/bpf-check-main-flow-notes.md)
- do_check 主循环与状态剪枝：[Documentation/bpf/bpf-do-check-notes.md](Documentation/bpf/bpf-do-check-notes.md)
- do_check_insn 与条件跳转收窄：[Documentation/bpf/bpf-do-check-insn-notes.md](Documentation/bpf/bpf-do-check-insn-notes.md)
- do_check_insn 的 helper/load-store/exit 路径：[Documentation/bpf/bpf-do-check-insn-ops-notes.md](Documentation/bpf/bpf-do-check-insn-ops-notes.md)
- CFG 细节：[Documentation/bpf/bpf-cfg-notes.md](Documentation/bpf/bpf-cfg-notes.md)
- program type：[Documentation/bpf/bpf-program-types-notes.md](Documentation/bpf/bpf-program-types-notes.md)
- maps/helpers/kfuncs：[Documentation/bpf/bpf-maps-helpers-kfuncs-notes.md](Documentation/bpf/bpf-maps-helpers-kfuncs-notes.md)
- BTF/CO-RE：[Documentation/bpf/bpf-btf-core-notes.md](Documentation/bpf/bpf-btf-core-notes.md)
- BTF 在 load/verifier 中的源码入口：[Documentation/bpf/bpf-btf-load-verifier-notes.md](Documentation/bpf/bpf-btf-load-verifier-notes.md)
- CO-RE candidate 搜索与 patch：[Documentation/bpf/bpf-core-apply-notes.md](Documentation/bpf/bpf-core-apply-notes.md)
- CO-RE relocation 求值与指令改写：[Documentation/bpf/bpf-core-relo-calc-patch-notes.md](Documentation/bpf/bpf-core-relo-calc-patch-notes.md)
- CO-RE spec match 与兼容性规则：[Documentation/bpf/bpf-core-spec-match-notes.md](Documentation/bpf/bpf-core-spec-match-notes.md)
- syscall/load/attach 入口：[Documentation/bpf/bpf-syscall-load-path-notes.md](Documentation/bpf/bpf-syscall-load-path-notes.md)
- BPF_PROG_LOAD 细化路径：[Documentation/bpf/bpf-prog-load-path-notes.md](Documentation/bpf/bpf-prog-load-path-notes.md)

## 5. 一条推荐使用方式

如果你现在正从源码往回理解文档，我建议用下面顺序：

1. 先看 [Documentation/bpf/bpf-doc-source-overview.md](Documentation/bpf/bpf-doc-source-overview.md)
2. 再看 [Documentation/bpf/bpf-verifier-source-map.md](Documentation/bpf/bpf-verifier-source-map.md)
3. 然后看 [Documentation/bpf/bpf-cfg-notes.md](Documentation/bpf/bpf-cfg-notes.md) 对照 [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c)
4. 再回看 [Documentation/bpf/bpf-architecture-summary.md](Documentation/bpf/bpf-architecture-summary.md)，把整体层次装回去

如果你是从文档往源码走，则反过来：

1. [Documentation/bpf/bpf-architecture-summary.md](Documentation/bpf/bpf-architecture-summary.md)
2. [Documentation/bpf/bpf-study-index.md](Documentation/bpf/bpf-study-index.md)
3. 按主题进入对应专题文档
4. 最后回到本页按主题找实现入口

## 6. 一句话压缩版

如果把这份总对照页压成一句话，可以这样记：

> BPF 文档描述的是分层语义模型，而源码实现则大致分散在 `kernel/bpf/` 的 verifier / CFG / syscall / map 相关逻辑、各子系统 hook 实现，以及 `tools/lib/bpf/` 的用户态 loader 中；理解时最好始终沿“概念层 -> 关键对象 -> 入口文件”的方式往下走。
