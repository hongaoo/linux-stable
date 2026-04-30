# BPF 学习索引

这份索引把当前已经整理出来的 BPF 学习材料按“总览 -> 专题 -> 源码对照”组织起来，方便从不同入口进入。

## 1. 总览入口

- [Documentation/bpf/bpf-architecture-summary.md](Documentation/bpf/bpf-architecture-summary.md)

如果你还没有建立整体框架，优先从这篇开始。

## 2. 按主题阅读

### 2.1 机器模型与基础语义

- [Documentation/bpf/bpf-abi-notes.md](Documentation/bpf/bpf-abi-notes.md)
- [Documentation/bpf/bpf-btf-core-notes.md](Documentation/bpf/bpf-btf-core-notes.md)

### 2.2 安全验证

- [Documentation/bpf/bpf-verifier-notes.md](Documentation/bpf/bpf-verifier-notes.md)
- [Documentation/bpf/bpf-verifier-source-map.md](Documentation/bpf/bpf-verifier-source-map.md)
- [Documentation/bpf/bpf-verifier-core-notes.md](Documentation/bpf/bpf-verifier-core-notes.md)
- [Documentation/bpf/bpf-pointer-map-access-notes.md](Documentation/bpf/bpf-pointer-map-access-notes.md)
- [Documentation/bpf/bpf-helper-kfunc-ret-notes.md](Documentation/bpf/bpf-helper-kfunc-ret-notes.md)
- [Documentation/bpf/bpf-check-main-flow-notes.md](Documentation/bpf/bpf-check-main-flow-notes.md)
- [Documentation/bpf/bpf-do-check-notes.md](Documentation/bpf/bpf-do-check-notes.md)
- [Documentation/bpf/bpf-do-check-insn-notes.md](Documentation/bpf/bpf-do-check-insn-notes.md)
- [Documentation/bpf/bpf-do-check-insn-ops-notes.md](Documentation/bpf/bpf-do-check-insn-ops-notes.md)
- [Documentation/bpf/bpf-cfg-notes.md](Documentation/bpf/bpf-cfg-notes.md)

### 2.3 执行语义与挂载点

- [Documentation/bpf/bpf-program-types-notes.md](Documentation/bpf/bpf-program-types-notes.md)

### 2.4 状态面与能力面

- [Documentation/bpf/bpf-maps-helpers-kfuncs-notes.md](Documentation/bpf/bpf-maps-helpers-kfuncs-notes.md)

### 2.5 用户态到内核入口

- [Documentation/bpf/bpf-syscall-load-path-notes.md](Documentation/bpf/bpf-syscall-load-path-notes.md)
- [Documentation/bpf/bpf-prog-load-path-notes.md](Documentation/bpf/bpf-prog-load-path-notes.md)
- [Documentation/bpf/bpf-btf-load-verifier-notes.md](Documentation/bpf/bpf-btf-load-verifier-notes.md)
- [Documentation/bpf/bpf-core-apply-notes.md](Documentation/bpf/bpf-core-apply-notes.md)
- [Documentation/bpf/bpf-core-relo-calc-patch-notes.md](Documentation/bpf/bpf-core-relo-calc-patch-notes.md)
- [Documentation/bpf/bpf-core-spec-match-notes.md](Documentation/bpf/bpf-core-spec-match-notes.md)

## 3. 从官方文档回看

如果你想把这些总结重新映射回原始文档，建议优先看：

- [Documentation/bpf/index.rst](Documentation/bpf/index.rst)
- [Documentation/bpf/standardization/instruction-set.rst](Documentation/bpf/standardization/instruction-set.rst)
- [Documentation/bpf/standardization/abi.rst](Documentation/bpf/standardization/abi.rst)
- [Documentation/bpf/verifier.rst](Documentation/bpf/verifier.rst)
- [Documentation/bpf/maps.rst](Documentation/bpf/maps.rst)
- [Documentation/bpf/kfuncs.rst](Documentation/bpf/kfuncs.rst)
- [Documentation/bpf/btf.rst](Documentation/bpf/btf.rst)
- [Documentation/bpf/programs.rst](Documentation/bpf/programs.rst)
- [Documentation/bpf/libbpf/libbpf_overview.rst](Documentation/bpf/libbpf/libbpf_overview.rst)

## 4. 从源码切入

如果你更想直接从实现看，推荐先走下面这条线：

1. [Documentation/bpf/bpf-verifier-source-map.md](Documentation/bpf/bpf-verifier-source-map.md)
2. [Documentation/bpf/bpf-cfg-notes.md](Documentation/bpf/bpf-cfg-notes.md)
3. [Documentation/bpf/bpf-maps-helpers-kfuncs-notes.md](Documentation/bpf/bpf-maps-helpers-kfuncs-notes.md)

对应源码入口：

- [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c)
- [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c)

## 5. 两条推荐路线

### 5.1 架构路线

1. [Documentation/bpf/bpf-architecture-summary.md](Documentation/bpf/bpf-architecture-summary.md)
2. [Documentation/bpf/bpf-abi-notes.md](Documentation/bpf/bpf-abi-notes.md)
3. [Documentation/bpf/bpf-verifier-notes.md](Documentation/bpf/bpf-verifier-notes.md)
4. [Documentation/bpf/bpf-program-types-notes.md](Documentation/bpf/bpf-program-types-notes.md)
5. [Documentation/bpf/bpf-maps-helpers-kfuncs-notes.md](Documentation/bpf/bpf-maps-helpers-kfuncs-notes.md)
6. [Documentation/bpf/bpf-btf-core-notes.md](Documentation/bpf/bpf-btf-core-notes.md)

### 5.2 源码路线

1. [Documentation/bpf/bpf-verifier-source-map.md](Documentation/bpf/bpf-verifier-source-map.md)
2. [Documentation/bpf/bpf-cfg-notes.md](Documentation/bpf/bpf-cfg-notes.md)
3. [kernel/bpf/cfg.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/cfg.c)
4. [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c)
5. [Documentation/bpf/bpf-verifier-core-notes.md](Documentation/bpf/bpf-verifier-core-notes.md)
6. [Documentation/bpf/bpf-maps-helpers-kfuncs-notes.md](Documentation/bpf/bpf-maps-helpers-kfuncs-notes.md)

## 6. 一句话索引版

如果只记一个入口，就记：

- 总览看 [Documentation/bpf/bpf-architecture-summary.md](Documentation/bpf/bpf-architecture-summary.md)
- 源码对照看 [Documentation/bpf/bpf-verifier-source-map.md](Documentation/bpf/bpf-verifier-source-map.md)
- CFG 细节看 [Documentation/bpf/bpf-cfg-notes.md](Documentation/bpf/bpf-cfg-notes.md)
