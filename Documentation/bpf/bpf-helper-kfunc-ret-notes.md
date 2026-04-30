# helper 与 kfunc 返回值如何写回 R0

这份文档继续沿 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c) 往下看，但只盯住一个问题：helper 和 kfunc 调用之后，verifier 是如何重置 caller-saved 寄存器，并把返回值写回 R0 的。

这是理解 verifier 调用语义的关键一环，因为一次调用之后，程序状态不是“调用过一个函数”这么简单，而是：

- R1-R5 之类 caller-saved 寄存器失效
- R0 被重新赋予新的抽象类型
- 某些返回值会带上 `id`
- 某些返回值会带上 `ref_obj_id`
- 某些返回值会带上 BTF、map、mem size 等对象语义

## 1. 先抓总规律

不管是 helper 还是 kfunc，verifier 都把“调用返回”建模成一次明确的状态转换：

1. 先检查参数是否合法
2. 清掉 caller-saved 寄存器的旧状态
3. 按返回原型给 R0 写入新的类型语义
4. 如有需要，再补 `id`、`ref_obj_id`、`btf_id`、`mem_size` 等附加元数据

所以“函数返回值”在 verifier 里并不是单独一列信息，而是整份寄存器状态的一次重写。

## 2. helper 调用后为什么先重置 R1-R5

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L10542) 一带的 `check_helper_call()` 里，可以看到 helper 参数检查结束后，先会做一轮 caller-saved 寄存器重置。

这一步对应的就是 BPF ABI：

- R0-R5 是 caller-saved
- helper 返回后，调用者不能继续依赖这些寄存器里旧有语义

因此 verifier 会主动把这些寄存器标成未初始化或重置状态，然后只把新的返回语义重新写进 R0。

## 3. helper 返回值写回 R0 的主轴在哪里

helper 返回值写回的主轴就在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L10552) 之后这段 `switch (base_type(ret_type))`。

这段逻辑可以理解成：

- helper prototype 说返回什么
- verifier 就把 R0 改写成对应抽象类型

它不是在“保存运行时值”，而是在“写入 verifier 对后续程序点的认知”。

## 4. `RET_INTEGER` 为什么只是标成未知标量

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L10559) 的 `RET_INTEGER` 分支里，verifier 做的不是写一个具体数字，而是：

- 把 R0 设成 `SCALAR_VALUE`
- 具体值未知

这说明 verifier 默认不会假设 helper 返回某个常量，除非另有专门的范围细化逻辑。

所以 helper 返回整数的第一层语义通常只是：

- 这是个合法可读的标量
- 但它的值范围仍需后续约束进一步收窄

## 5. `RET_PTR_TO_MAP_VALUE` 为什么要写入 map 元数据

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L10566) 的 `RET_PTR_TO_MAP_VALUE` 分支里，除了把 R0 设成 `PTR_TO_MAP_VALUE | ret_flag`，还会额外写入：

- `map_ptr`
- `map_uid`

原因很直接：后面一旦程序对这个返回值做 load/store，verifier 还得知道：

- 它来自哪个 map
- 对应 value_size 是多少
- 有没有 record/BTF 字段约束

所以 map lookup 返回值不是“普通指针”，而是一个带 map 身份的对象指针。

## 6. 为什么 `PTR_MAYBE_NULL` 和 `id` 经常一起出现

helper 返回值如果可能为 NULL，verifier 往往还会给 R0 分配一个新的 `id`。例如在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L10688) 之后，可以看到：

- 如果返回类型可能为 NULL
- 就给 R0 分配新的 `id`

这样做是为了后续控制流中的判空收窄。

换句话说：

- `PTR_MAYBE_NULL` 描述“这个指针可空”
- `id` 让 verifier 能把这次返回值在不同分支里作为同一个对象关系来追踪

## 7. `RET_PTR_TO_MEM` 和 `RET_PTR_TO_BTF_ID` 的区别

helper 返回“某块内存”与返回“某个类型化对象指针”在 verifier 里是两种不同语义。

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L10606) 的 `RET_PTR_TO_MEM`，R0 会被写成：

- `PTR_TO_MEM`
- 外加 `mem_size`

而在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L10648) 的 `RET_PTR_TO_BTF_ID`，R0 则会被写成：

- `PTR_TO_BTF_ID`
- `btf`
- `btf_id`

这说明 verifier 会区分：

- 返回的是一段可访问内存
- 还是返回的是一个带 BTF 类型身份的对象

## 8. `RET_PTR_TO_MEM_OR_BTF_ID` 为什么重要

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L10610) 一带的 `RET_PTR_TO_MEM_OR_BTF_ID`，可以看到 verifier 会先看返回 BTF 类型，判断它最终更像：

- 非结构体类型对应的一段 memory
- 还是结构体类型对应的 `PTR_TO_BTF_ID`

这很关键，因为它说明 helper 返回值并不总是“一开始就被静态枚举死了”，有时还要结合 BTF 类型分辨最终该落在哪类寄存器语义上。

## 9. `ref_obj_id` 什么时候写回 R0

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L10696) 到 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L10711) 一带，可以看到 helper 返回之后，R0 有时还会被赋予：

- `dynptr_id`
- `ref_obj_id`

特别是 acquire / ptr cast / dynptr ref 这类 helper，R0 不只是“有一个类型”，而且还背着生命周期语义。

这意味着后续 release 检查并不只看寄存器类型，还会看它携带的引用对象身份。

## 10. `do_refine_retval_range()` 在补什么

helper 主返回类型写回之后，还会走 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L9942) 的 `do_refine_retval_range()`。

这一步的意义不是重写主类型，而是给某些返回值补更细的范围信息。

因此 R0 的最终状态不是“写一次就结束”，而是：

- 先写入主类型
- 再按具体 helper 语义补范围/约束

## 11. kfunc 返回值写回和 helper 有什么共同点

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L12974) 的 `check_kfunc_call()` 里，整体模式和 helper 很像：

- 先检查参数
- 清 caller-saved
- 再根据返回原型改写 R0

共同点是：

- 都把调用返回建模成寄存器状态重写
- 都可能给 R0 分配 `id`
- 都可能给 R0 附上 `ref_obj_id`

不同点是 kfunc 更深地依赖 BTF 原型，所以返回值判定更强烈地由 BTF 类型驱动。

## 12. kfunc 为什么更像“按 BTF 原型解释返回值”

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L13188) 之后，`check_kfunc_call()` 会先看返回类型 `t`，然后区分：

- scalar
- pointer
- void

如果是 pointer，还会继续判断：

- 指向 `void`
- 指向非 struct 类型
- 指向 struct 类型

然后再决定 R0 最终是：

- `SCALAR_VALUE`
- `PTR_TO_MEM`
- `PTR_TO_BTF_ID`

所以和 helper 的“枚举 ret_type 驱动”相比，kfunc 更像“BTF 原型解释驱动”。

## 13. kfunc 返回 `PTR_TO_MEM` 时 verifier 还会补什么

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L13250) 一带，如果 kfunc 返回的是非 struct 指针并且能确定大小，R0 会被标成：

- `PTR_TO_MEM`
- `mem_size`

必要时还会补：

- `MEM_RDONLY`
- `MEM_RCU`
- `ref_obj_id`

这说明即便最终同样落成 `PTR_TO_MEM`，kfunc 也可能携带比普通 helper 更复杂的对象上下文。

## 14. kfunc 返回 `PTR_TO_BTF_ID` 时 verifier 在补什么

在 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L13267) 到 [kernel/bpf/verifier.c](/home/hongao/github-workspace/linux-stable/kernel/bpf/verifier.c#L13316) 一带，可以看到 verifier 会继续补：

- `btf`
- `btf_id`
- `PTR_TRUSTED` / `PTR_UNTRUSTED`
- `MEM_RCU`
- `PTR_MAYBE_NULL`
- `id`
- `ref_obj_id`

这说明 kfunc 返回对象指针时，R0 的语义远不止“指向某结构体”，而是把信任级别、RCU 保护、可空性、引用生命周期一起编码进去了。

## 15. acquire 型 kfunc/helper 为什么特别重要

无论 helper 还是 kfunc，只要是 acquire 型调用，就不只是“返回一个对象指针”，而是“返回一个未来必须被释放或转换语义的对象引用”。

这就是为什么在 helper 和 kfunc 路径里都会看到：

- `acquire_reference()`
- `ref_obj_id`
- 对应 release 逻辑

对 verifier 来说，这类返回值真正重要的不是它指向哪里，而是它是否把程序带入了新的生命周期义务。

## 16. 一句话压缩版

如果把这条线压成一句话，可以这样记：

> helper 和 kfunc 调用返回后，verifier 会先清理 caller-saved 状态，再按 helper ret_type 或 kfunc BTF 原型把 R0 重写成新的抽象对象状态，并在需要时附上 `id`、`ref_obj_id`、`btf_id`、`mem_size` 等元数据，让后续路径继续做类型、判空和生命周期验证。
