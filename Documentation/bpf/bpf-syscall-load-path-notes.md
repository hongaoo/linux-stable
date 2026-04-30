# BPF syscall、load 与 attach 入口说明

这份文档把用户态进入内核 BPF 子系统的总入口串起来，重点围绕 [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c) 回答这些问题：

- `bpf()` syscall 在内核里是怎么分发的
- map create 和 prog load 分别在什么入口里处理
- program type / attach type 的检查在什么时候发生
- 为什么说 syscall.c 是“用户态控制面进入内核 BPF 的大门”

## 1. 先抓总入口

最核心的总入口是：

- [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c#L6359)

也就是：

- `SYSCALL_DEFINE3(bpf, ...)`

如果要理解“用户态是如何进入内核 BPF 子系统”的第一步，这里就是总闸门。

## 2. `bpf()` syscall 解决的不是单一动作，而是一整组命令分发

`bpf()` 并不是只服务 program load。它本质上是一个多命令 syscall：

- map create
- map lookup/update/delete
- prog load
- obj pin/get
- link create/update/detach
- 以及其他 BPF 对象相关操作

所以从结构上看，`syscall.c` 不是“程序装载文件”，而是“BPF 用户态控制面协议入口”。

## 3. dispatch 这一层在源码里怎么体现

从 grep 到的结果可以看到，在 [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c#L6378) 一带，`bpf()` syscall 会根据不同 `cmd` 分发到对应处理路径，例如：

- `BPF_MAP_CREATE`
- `BPF_MAP_UPDATE_ELEM`
- `BPF_PROG_LOAD`
- `BPF_LINK_CREATE`

这说明 syscall 层的第一职责就是：

- 验证用户态传入 attr 基本格式
- 根据命令类型把请求送到对应处理函数

也就是说，`syscall.c` 是“命令分发层 + 对象创建入口层”。

## 4. map create 在哪里进入内核

`BPF_MAP_CREATE` 的直接核心入口之一是：

- [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c#L1362)

也就是 `map_create()`。

从这段逻辑可以清楚看到 map create 的几层结构：

- attr 基础字段检查
- flags/token 处理
- NUMA / map type / map ops 选择
- capability/token 权限检查
- `ops->map_alloc()` 真正创建 map
- BTF 关联与 map_check_btf
- LSM/security 钩子
- 分配 map id
- 最后创建用户态 fd

这说明 map create 不是“分配一个对象”这么简单，而是一次：

- 类型检查
- 权限检查
- 安全检查
- 对象初始化
- ID/FD 发布

的完整过程。

## 5. `bpf_map_types[]` 为什么重要

在 [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c#L58) 的 `bpf_map_types[]`，可以把它看成 map type 到 map ops 的总表。

这意味着 map create 的关键一步其实是：

- 根据 `attr->map_type` 找到对应 `map_ops`

后面的 `map_alloc_check()`、`map_alloc()`、`map_mem_usage()` 等能力，都是围绕这个 ops 分发表展开的。

所以 syscall 层不是亲自实现所有 map，而是在“类型分发后委托给具体 map 实现”。

## 6. prog load 为什么不只是把字节码塞进内核

虽然这次没有把 `BPF_PROG_LOAD` 整段完整展开，但从 [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c#L2626) 到 [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c#L2774) 一带，可以很清楚看出 prog load 至少会涉及：

- expected attach type 修正
- program type 与 attach type 匹配检查
- capability 检查
- 后续 verifier / BTF / attach 相关准备

也就是说，prog load 的 syscall 入口不是只管 copy 字节码，而是在进入 verifier 之前就先建立程序所属语义环境。

## 7. `bpf_prog_load_fixup_attach_type()` 在解决什么问题

在 [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c#L2626) 的 `bpf_prog_load_fixup_attach_type()`，内核会对某些 program type 的 `expected_attach_type` 做兼容性修正。

这一步的重要性在于：

- 用户态传进来的 attach type 不一定总是完整显式指定
- 某些历史兼容路径要求内核补全默认 attach type

这说明 syscall 层不仅在做验证，也在做“用户态语义归一化”。

## 8. `bpf_prog_load_check_attach()` 为什么重要

在 [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c#L2646) 的 `bpf_prog_load_check_attach()`，可以看到一件很关键的事：

- program type 和 expected_attach_type 的对应关系，是在 syscall/load 入口就开始检查的

这意味着 program type 语义并不是只在 attach 那一步才生效，而是 program load 阶段就开始被验证。

例如：

- `BPF_PROG_TYPE_CGROUP_SOCK` 允许哪些 attach type
- `BPF_PROG_TYPE_CGROUP_SKB` 允许哪些 attach type
- `BPF_PROG_TYPE_SK_LOOKUP` / `BPF_PROG_TYPE_NETFILTER` 允许哪些 attach type

这一步本质上是在回答：

- 这段程序的声明语义和内核允许的挂载语义是否一致

## 9. 为什么 capability 检查在 syscall 层就很重

从 `map_create()` 和 `is_net_admin_prog_type()` 一类逻辑可以看到，权限检查在 syscall 层就已经非常重。

原因很直接：

- BPF 对象能否被创建
- 哪些 map type 可以被谁创建
- 哪些 program type 需要 `CAP_BPF` / `CAP_NET_ADMIN`

这些都必须在对象真正进入内核执行体系之前就卡住。

所以 syscall 层不仅是协议入口，也是权限边界的一部分。

## 10. token 为什么会出现在 map create 里

在 `map_create()` 里可以看到 token 相关逻辑，例如：

- `BPF_F_TOKEN_FD`
- `bpf_token_get_from_fd()`
- `bpf_token_allow_cmd()`
- `bpf_token_allow_map_type()`

这说明当前内核里的 BPF 权限模型已经不只是简单 capability，而是在向更细粒度的授权模型发展。

也就是说，syscall 层除了做传统权限检查，还逐步承担“按命令/对象类型粒度授权”的职责。

## 11. 为什么说 syscall.c 是“用户态控制面的大门”

因为用户态和内核 BPF 子系统之间，很多关键动作都要经过这里：

- 创建 map
- 装载 program
- 生成 fd
- 绑定 link
- pin/get 对象

这和 verifier 的角色完全不同：

- verifier 证明程序安全
- syscall.c 则负责把用户态请求变成受控内核对象生命周期操作

所以如果你把 BPF 看成一个平台，那么 `syscall.c` 更像控制面入口，而 verifier 更像安全分析引擎。

## 12. syscall 层和 libbpf 的关系

从用户视角，开发者大多数时候是通过 libbpf 工作；但从内核视角，libbpf 只是更高层的用户态包装。

真正进入内核时，还是要落到：

- `bpf()` syscall
- `union bpf_attr`
- 各种 `BPF_*` command

所以可以把两者关系压成这样：

- libbpf 组织对象、补充元数据、处理重定位和调用顺序
- syscall.c 接收最终请求并创建/校验/发布内核对象

## 13. 推荐阅读顺序

如果想从 syscall 入口建立地图，建议这样读：

1. [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c#L6359) 的 `SYSCALL_DEFINE3(bpf, ...)`
2. 看命令分发里 `BPF_MAP_CREATE`、`BPF_PROG_LOAD`、`BPF_LINK_CREATE` 这些分支
3. 再看 [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c#L1362) 的 `map_create()`
4. 再看 [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c#L2626) 和 [kernel/bpf/syscall.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/syscall.c#L2646) 一带的 prog load attach 检查
5. 最后回看 [Documentation/bpf/libbpf/libbpf_overview.rst](Documentation/bpf/libbpf/libbpf_overview.rst) 和 [Documentation/bpf/bpf-doc-source-overview.md](Documentation/bpf/bpf-doc-source-overview.md)

## 14. 一句话压缩版

如果把 syscall/load 入口压成一句话，可以这样记：

> `kernel/bpf/syscall.c` 是用户态控制面进入 BPF 子系统的大门，它负责把 `bpf()` syscall 的多种命令分发成具体对象操作，并在 map create、prog load、attach/link 创建等路径上完成类型检查、权限检查、对象初始化和语义约束校验。

如果想继续把 `BPF_PROG_LOAD` 单独展开到“plain prog allocation、attach_btf/dst_prog 绑定、security hook、`bpf_check()` 进入 verifier、成功后如何分配 ID 和 fd”，可以继续看补充文档 [Documentation/bpf/bpf-prog-load-path-notes.md](Documentation/bpf/bpf-prog-load-path-notes.md)。
