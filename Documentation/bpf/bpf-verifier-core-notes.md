# kernel/bpf/verifier.c 三条主线

这份文档专门顺着 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c) 往下走三条最核心的主线：

- 寄存器状态是怎么建模的
- helper 参数检查是怎么接进 verifier 的
- 引用对象是怎么获取、传播和释放的

目标不是穷举所有细节，而是把这三个主题从“源码里的一大片逻辑”压缩成能抓住主结构的阅读框架。

## 1. 先抓总思路

如果把 `verifier.c` 的第二遍状态模拟压成一句话，可以这样记：

> verifier 在每个程序点上维护一份“寄存器 + 栈 + 引用对象”的抽象状态，并根据指令、helper/kfunc 原型和条件分支不断更新这份状态，直到能证明所有路径都安全。

这也是为什么阅读 `verifier.c` 时，最值得优先抓住的不是单条指令逻辑，而是“状态是如何被表示和推进的”。

## 2. 第一条主线：寄存器状态建模

### 2.1 寄存器里存的不是单纯数值

从 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L1757) 一带开始看，可以非常清楚地感觉到：verifier 对寄存器的理解不是“一个 64-bit 值”，而是一组状态属性的组合。

它至少关心：

- `type`
- `id`
- `ref_obj_id`
- `var_off`
- 有符号/无符号上下界
- 32 位和 64 位子范围

这意味着一个寄存器在 verifier 看来同时有：

- 类型语义
- 区间语义
- 位级不确定性语义
- 生命周期语义

### 2.2 `__mark_reg_known()` 一类函数在做什么

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L1768) 的 `__mark_reg_known()`、[kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L1798) 的 `__mark_reg_known_zero()`、[kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L1904) 的 `__mark_reg_unbounded()` 这些函数里，可以看到 verifier 经常在做三类操作：

- 把寄存器收窄成已知常量
- 把寄存器清成已知零
- 把寄存器重新放宽成不确定范围

这正好对应抽象解释里的常见动作：

- 精化状态
- 重置状态
- 在保守前提下扩大不确定性

### 2.3 `var_off` 和范围为什么要一起看

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L1945) 到 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L2350) 这一带，代码大量在做 bounds 更新和同步。

这里最重要的理解是：

- `var_off` 更像位级不确定性描述
- `umin/umax`、`smin/smax` 更像区间描述

两者不是重复信息，而是互补信息。

verifier 会不断尝试：

- 用 tnum 约束范围
- 用范围反过来收窄 tnum
- 在 32/64 位视角之间推导边界

所以寄存器状态不是一层，而是“类型 + tnum + bounds”叠起来的多层抽象。

### 2.4 `mark_ptr_not_null_reg()` 为什么重要

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L1834) 的 `mark_ptr_not_null_reg()`，可以看到判空之后并不是简单把一个布尔条件记下来，而是直接改变寄存器类型。

例如：

- `PTR_TO_MAP_VALUE_OR_NULL` 可以收窄成 `PTR_TO_MAP_VALUE`
- 某些 map 类型还会进一步转换成更具体的对象指针类型

这正是文档里“NULL check 会改变状态”的源码落点。

## 3. 第二条主线：helper 参数检查

### 3.1 helper 检查不是在 call 那一刻才突然发生

阅读 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L115) 开头那段注释时，最容易忽略的一点是：helper 参数检查已经和寄存器状态模型深度耦合了。

因为 helper prototype 告诉 verifier：

- 某个参数应当是什么类型
- 某个返回值应当是什么类型
- 某些内存区域需要满足什么初始化条件

于是 helper 调用就不只是“函数调用”，而是一次状态转换规则。

### 3.2 `check_reg_arg()` 在 helper 检查里扮演什么角色

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L3448) 的 `__check_reg_arg()` 和 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L3486) 的 `check_reg_arg()`，可以看到 verifier 先在更基础层面检查：

- 某个寄存器是否可读
- 某个目标寄存器是否可写
- frame pointer 是否被非法写入
- 当前指令对寄存器宽度是 32 位还是 64 位语义

这是一层“寄存器操作是否合法”的通用门槛。

它本身不等于完整 helper 参数检查，但它是后者成立的基础。

### 3.3 helper 检查真正依赖什么

helper 检查真正依赖的是三样东西：

- 当前寄存器状态
- helper prototype 约束
- 某些对象和内存的附加规则

例如对 map lookup：

- R1 必须是 map 指针
- R2 必须是指向 key 的有效栈区域
- key 区域大小和初始化状态都要满足约束
- 返回值会把 R0 设成 `PTR_TO_MAP_VALUE_OR_NULL`

因此 helper 检查其实是“按 prototype 驱动的状态验证与状态迁移”。

### 3.4 `caller_saved` 数组为什么值得注意

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L1752) 可以看到 `caller_saved`。

这和 ABI 文档能直接对上：

- R0-R5 会在调用边界上被当作 caller-saved 处理

这意味着 helper 检查不仅要看参数类型，还要把“调用之后哪些寄存器还能继续信赖”纳入状态更新。

所以 helper 调用在 verifier 里总是同时触发：

- 参数合法性检查
- 返回值类型写回
- caller-saved 寄存器失效或重置

## 4. 第三条主线：引用对象跟踪

### 4.1 引用跟踪解决什么问题

一旦 helper 或 kfunc 返回的是带生命周期约束的对象，例如引用计数对象，仅仅把它当普通指针是不够的。

verifier 还必须保证：

- 引用不会泄漏
- 引用会在所有路径上正确释放
- 不会出现释放后继续使用
- 某些锁/IRQ 状态不会乱序恢复

这就是引用跟踪这条主线存在的原因。

### 4.2 `acquire_reference()` 和 `release_reference()` 是主轴

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L1389) 到 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L1488) 这段，可以看到引用状态管理的核心骨架：

- `acquire_reference_state()`
- `acquire_reference()`
- `release_reference_state()`
- 以及各种 lock/irq 相关的 acquire/release 变体

核心逻辑很清楚：

- 新获取一个需要跟踪的对象时，分配一条 reference state
- 赋予唯一 id
- 挂到当前 verifier state 的 `refs` 数组里
- 在释放时从状态里移除

这说明 verifier 并不是只把“引用”记在寄存器上，而是把它提升成了 verifier state 的一部分。

### 4.3 为什么 `id` 和 `ref_obj_id` 要分开看

寄存器层有 `id`，引用跟踪里又有 `ref_obj_id`，这两者很容易混。

更实用的理解是：

- `id` 更多用于区分某类对象/指针来源关系
- `ref_obj_id` 更明确服务于“这是一个需要 release 的受跟踪引用对象”

所以引用跟踪并不是单靠一般寄存器 id 完成的，而是有自己更专门的状态字段和引用表。

### 4.4 为什么 lock/irq 也进入了同一套框架

在同一段 acquire/release 逻辑里，还能看到：

- lock state
- irq state

这说明 verifier 对“必须成对获取/释放”的资源建模，其实在走统一思路：

- 获取时入状态栈/表
- 释放时按规则出状态
- 顺序不对或路径不闭合就报错

也就是说，引用对象跟踪本质上是 verifier 生命周期模型的一部分，而不是只服务少数 helper。

## 5. 三条主线是怎么汇合的

把这三条线并起来看，会更清楚 `verifier.c` 的结构：

1. 寄存器状态提供基本抽象：类型、范围、对象属性
2. helper/kfunc 检查读取这些状态，并按原型约束决定能否调用
3. 某些调用又会进一步创建或释放需要跨路径跟踪的引用对象

所以它们不是三块互不相干的逻辑，而是一条连续的数据流：

- 指令更新寄存器状态
- 调用消费和改写寄存器状态
- 生命周期对象进入 verifier state 并要求后续路径闭合

## 6. 读 verifier.c 时建议的顺序

如果要沿这三条主线往下读，推荐顺序是：

1. 先重读 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L53) 开头总注释
2. 再看 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L1757) 一带的寄存器状态更新函数
3. 再看 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L3448) 一带的 `__check_reg_arg()` / `check_reg_arg()`
4. 再看 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L1389) 一带的 acquire/release reference 逻辑
5. 最后把这些和 [Documentation/bpf/bpf-maps-helpers-kfuncs-notes.md](Documentation/bpf/bpf-maps-helpers-kfuncs-notes.md) 一起看，会更容易把 helper / kfunc / 引用对象串起来

## 7. 一句话压缩版

如果把 `verifier.c` 的第二遍核心压成一句话，可以这样记：

> `kernel/bpf/verifier.c` 的主体逻辑就是维护一份带类型、范围和生命周期信息的抽象程序状态，并用它去验证寄存器操作、helper/kfunc 调用以及引用对象的获取与释放在所有路径上都成立。

如果想继续沿这条线下钻到“指针类型为什么不是普通整数、map value 访问为什么要同时检查范围/BTF field/kptr/uptr”，可以继续看补充文档 [Documentation/bpf/bpf-pointer-map-access-notes.md](Documentation/bpf/bpf-pointer-map-access-notes.md)。

如果想继续沿 helper/kfunc 调用路径往下看“返回值是如何写回 R0、什么时候变成 PTR_TO_MAP_VALUE / PTR_TO_BTF_ID / PTR_TO_MEM、什么时候附带 ref_obj_id 和 id”，可以继续看补充文档 [Documentation/bpf/bpf-helper-kfunc-ret-notes.md](Documentation/bpf/bpf-helper-kfunc-ret-notes.md)。
