# `mm/zsmalloc.c`：Linux 压缩对象分配器研究报告

> 研究时间：2026-04-30 | 所属领域：Linux 内核内存管理 | 研究对象类型：内核机制 / 特种分配器实现文件

## 一、一句话定义

`mm/zsmalloc.c` 实现的是 Linux 的 **zsmalloc 分配器**：一个专门给 `zram`、`zswap` 这类“压缩后页数据存储池”使用的特种内存分配器。它不负责压缩算法本身，而是负责把**大小不一、最多不超过 `PAGE_SIZE` 的压缩对象**尽量低碎片地塞进内存里，并允许后续再整理、搬迁、回收。

如果只用一句更直白的话来形容它：  
**它不是在分配普通内核对象，而是在维护一座可重排的压缩字节仓库。**

## 二、纵向分析：这套机制为什么会出现，又是怎么长成今天这个样子的

### 1. 它的起点不是“造一个更快的 slab”，而是“压缩页存储这件事和普通 slab 根本不是一个问题”

内核文档把这个动机讲得很清楚：`zsmalloc` 是为 `zram` 设计出来的（`Documentation/mm/zsmalloc.rst:5-31`）。当你要存的是“压缩后的页”时，会遇到一个非常具体的问题：

- 对象大小波动很大，可能只有几百字节，也可能接近 4K；
- 系统往往恰恰处在内存紧张场景下，不能依赖高阶页分配；
- 如果只是拿 4K 单页来装对象，那 `PAGE_SIZE/2` 以上的对象会造成非常严重的内部碎片。

这正是文档里点名 predecessor `xvmalloc` 的痛点：对象一旦接近半页甚至更大，就很容易“一页只装一个对象”，压缩省下来的空间又被碎片吃回去。

`zsmalloc` 的回答不是去改 slab/slub，而是换了问题表述方式：**既然对象大小最多不超过 `PAGE_SIZE`，那就专门围绕“<= PAGE_SIZE 的压缩对象”造一套分配模型。**

### 2. 从设计目标上看，它从一开始就和普通 slab 有两条根本分叉

把 2014 年那笔把它正式移入 `mm/` 的提交翻出来看，提交说明其实已经把差异说得很透（`bcf1647d0899 zsmalloc: move it under mm`）：

1. **zsmalloc 不要求高阶页分配**。  
   它只分配多个 order-0 页，再把它们“缝”成一个逻辑整体。

2. **zsmalloc 允许对象跨页边界**。  
   这点非常关键，因为它直接把 `PAGE_SIZE/2 ~ PAGE_SIZE` 区间对象的碎片问题打掉了一大截。

这两条一旦成立，后面的设计几乎是顺理成章的：

- 一个对象可能分布在两个物理页上；
- 所以不能对外返回“可稳定解引用的指针”；
- 于是分配结果必须变成一个 **handle**；
- 真正读写对象内容时，再通过 handle 去映射或复制。

也就是说，`zsmalloc` 最核心的历史决策，不是“按 class 管对象”，而是：  
**放弃“返回稳定指针”这条 slab 世界的默认前提。**

### 3. `zspage` 是这套机制的灵魂：不是高阶页，却要表现得像一个更大的页容器

文档和代码都反复强调 `zspage` 的概念（`Documentation/mm/zsmalloc.rst:13-17`，`mm/zsmalloc.c:896-972`）。

`zspage` 不是真正的高阶页，而是：

- 由若干个 order-0 page 组成；
- 这些 page 用 `zpdesc->next` 串起来；
- 对象可以从一个 page 尾部延续到下一个 page 开头。

这有点像拿很多小集装箱拼成一个可连续装货的平台。它没有高阶页分配对物理连续性的要求，但它在“对象摆放效果”上又尽量逼近一个更大的连续空间。

这一步，决定了 `zsmalloc` 的一切后续复杂性：

- 需要 handle；
- 需要对象位置编码；
- 需要映射 API；
- 需要 compaction 时能改写对象真实位置；
- 需要 migration 时能替换组成 zspage 的单个子页。

换句话说，`zspage` 解决了碎片问题，也制造了后面所有元数据和并发控制问题。这个交换是值得的，因为没有它，`zsmalloc` 就失去存在意义了。

### 4. 它后来真正长大的方向，不是“分配更快”，而是“生命周期更稳，能和 MM 其他机制协作”

最近一段时间的历史，能看出 `zsmalloc` 的演化重点已经不再是早期那种“基本结构成型”的阶段，而是在补细部边界：

- `dc2e4982cb01`：引入基于 SG list 的读取 API；
- `19c4707b535a`：简化 read begin/end 逻辑；
- `07864f1a57fb`：移除旧 object mapping API 和 per-CPU map areas；
- `e27af3f9360e`：让 zspage reader-lock 可睡眠；
- `5ec3583309ef`：让 `PageZsmalloc()` 在页释放回 buddy 前都保持 sticky；
- `dc711106a0bc`：zspage migration 锁冲突时返回 `-EBUSY` 而不是 `-EAGAIN`；
- `4fb61d95ad21`：`zs_page_migrate()` 里补 KMSAN metadata 的复制。

这组变化很说明问题。`zsmalloc` 不再只是一个“内部自洽的小 allocator”，它已经要和：

- `page migration`
- `compaction`
- `KMSAN`
- `zswap` 的 SG 解压路径

这些外围机制长期协作。

所以今天的 `mm/zsmalloc.c`，已经不只是“能分配、能释放”，而是一块带有**重排、迁移、统计、shrinker 接口、并发锁模型**的子系统代码。

### 5. 当前最有张力的历史遗留问题：它已经接入 page migration，但生命周期边界还没完全收口

你 IDE 里选中的 TODO 在 `zs_page_migrate()` 开头（`mm/zsmalloc.c:1702-1706`）。这段 TODO 非常重要，它说的是：

> `zs_page_isolate()` 成功之后，页锁会临时释放；这期间没有什么能阻止整个 zspage 被销毁。应该重构成：这种页在 un-isolated 前，销毁要被推迟。

这不是一句普通注释，它暴露的是整套机制今天最紧绷的地方：

- `zsmalloc` 已经把自己的页面接入了通用 `movable_ops` 迁移框架；
- 但它的真实资源管理单位其实是 `zspage`，不是单页；
- 当 MM 想迁一个“子页”时，`zsmalloc` 必须证明整个所属 `zspage` 的生命周期仍然安全。

这和你之前研究的 `mm/migrate.c` 可以完全对上：**通用迁移框架已经接住了 `zsmalloc`，但抽象边界仍然在被现实摩擦。**

## 三、横向分析：把 `zsmalloc` 放在当前 MM 体系里看，它到底扮演什么角色

### 1. 它不是压缩器，而是压缩后数据的存储层

这个区分非常重要，因为第一次看代码的人很容易把 `zram` / `zswap` 和 `zsmalloc` 混在一起。

实际上分工很清楚：

- **`zram` / `zswap` 负责压缩与解压**
- **`zsmalloc` 负责存放压缩结果**

比如：

- `zram_write_page()` 先调用压缩器得到 `comp_len`，然后 `zs_malloc()` 分配，再 `zs_obj_write()` 写入压缩字节（`drivers/block/zram/zram_drv.c:2277-2319`）；
- `zswap_store` 路径也是先压缩，再把结果塞进 `zs_pool`（`mm/zswap.c:899-907`）；
- `zswap_decompress()` 再通过 `zs_obj_read_sg_begin()` 拿到 SG list，交给解压器（`mm/zswap.c:924-958`）。

所以 `zsmalloc` 更像：

**一个为“压缩页字节流”优化过的对象仓库**。

它对上层暴露的是“分配一个 slot、写进去、以后再读出来”，不是“给你一个长期有效地址自己玩”。

### 2. 它和 slab/slub 的差别，不是实现细节差一点，而是哲学不同

slab/slub 假设的是：

- 分配器返回一个可直接访问的对象地址；
- 对象不会在分配器内部被悄悄搬走；
- 一个 slab page 内部的对象布局是本地的、相对静态的。

而 `zsmalloc` 假设的是相反的一套世界：

- 对象可能跨页；
- 对象的真实物理位置以后可以变；
- 外部只能拿 handle，不能拿稳定地址；
- allocator 自己要负责后续 compact / migrate。

这意味着 `zsmalloc` 根本不是 slab 的一个“变种”，它更像是：

**在页级内存管理和对象级存储之间，自己搭了一层 handle-based indirection。**

### 3. 它和 `zram` 的关系最紧密，因为 `zram` 的工作负载几乎是它的理想输入

`zram` 的典型负载是：

- 每个逻辑页压缩后得到一个 `comp_len`；
- 如果压缩后仍然很大，直接当 incompressible page 存；
- 否则交给 `zsmalloc` 存起来；
- 读回时再解压。

这个模式和 `zsmalloc` 的能力高度吻合：

- 对象上限不超过 `PAGE_SIZE`；
- 大量中小对象；
- 偶尔也有接近整页的大对象；
- 非常在意内存占用效率；
- 系统往往处在内存紧张场景。

所以 `zsmalloc` 文档里那句“it fulfills the allocation needs for zram perfectly” 并不夸张。它本质上就是为这类工作负载量身定做的。

### 4. 它和 `zswap` 的关系稍微间接一点，但更能体现它的新接口价值

`zswap` 不是块设备缓存，而是 swap 子系统前面的一个压缩缓存层。它更强调：

- 快速存入压缩页；
- 以后 fault 时尽量高效读出再解压；
- 必要时还能 writeback 回真正 swap device。

这就是为什么当前 `zswap` 会用到 `zs_obj_read_sg_begin()` / `zs_obj_read_sg_end()` 这组 API：  
`zsmalloc` 不再要求“先 map 出一段线性地址”，它可以直接给你一段 1~2 个 SG 项的输入描述（`mm/zswap.c:927-953`）。

这很关键，因为它说明 `zsmalloc` 的接口已经在朝“更贴合压缩/解压流水线”的方向长，而不是只满足早期 zram 的需求。

### 5. 它和 page migration / compaction 的关系，说明它已经半只脚迈进了通用 MM 框架

`Documentation/mm/page_migration.rst` 明确把 zsmalloc pages 列进了 `movable_ops` 页面类型（`Documentation/mm/page_migration.rst:149-165`）。

在实现里：

- `zs_init()` 里通过 `set_movable_ops(&zsmalloc_mops, PGTY_zsmalloc)` 注册（`mm/zsmalloc.c:2238-2253`）；
- `zsmalloc_mops` 提供了 isolate / migrate / putback 回调（`1796-1800`）。

这说明 `zsmalloc` 已经不只是一个“用户自己负责整理的 allocator”，它必须和整个 MM 的 page migration 规则兼容。

这是一种升级，也是一种负担。升级在于它能更好地配合 compaction；负担在于它现在必须回答那些普通 allocator 不需要回答的问题：  
**迁一个组成 zspage 的单页时，整个 zspage 的一致性和生命周期怎么办？**

## 四、当前实现拆解：顺着代码看，这个 allocator 是怎么工作的

### 1. 元数据模型：`zpdesc`、`zspage`、`size_class`

这份代码读起来最容易乱的地方，是它不像 slab 那样有一种非常单纯的层次，而是三层元数据交织：

#### `zpdesc`

`zpdesc` 是覆盖在 `struct page` 上的描述符（`mm/zpdesc.h:14-63`）。它借用了 page 的若干字段：

- `lru`
- `movable_ops`
- `next`
- `handle`
- `private -> zspage`
- `page_type -> first_obj_offset`

换句话说，`zsmalloc` 并没有完全离开 page 世界，而是把 page 元数据借壳重用。

#### `zspage`

`zspage` 才是逻辑上的存储单元（`mm/zsmalloc.c:261-274`）。它记录：

- 这个逻辑页链属于哪个 class；
- 当前用了多少对象；
- freelist 头在哪；
- 第一张 `zpdesc` 是谁；
- fullness group 是什么；
- 自己的读写锁。

#### `size_class`

`size_class` 则是按对象大小分桶（`160-174`）。每个 class 维护：

- `size`
- `pages_per_zspage`
- `objs_per_zspage`
- `fullness_list[]`

真正巧妙的点在于：**同一个 class 不只是“一个对象大小”，而是一套最合适的 zspage 组织方式。**

### 2. 分配：先找 class，再找可用 zspage，再切 freelist

`zs_malloc()` 是主入口（`1297-1354`）：

1. 检查大小是否合法；
2. 单独分配一个 handle；
3. 把 `ZS_HANDLE_SIZE` 加进请求大小；
4. 选 `size_class`；
5. 在 class 的 fullness list 里找一个还能塞对象的 zspage；
6. 调 `obj_malloc()` 从 zspage freelist 取对象；
7. 记录 handle -> obj 映射；
8. 更新 `inuse`、fullness group、统计；
9. 如果没有可用 zspage，就 `alloc_zspage()` 新建一串页。

这里最值得盯的是 `obj_malloc()`：

- 它先根据 `freeobj` 找到对象槽位；
- 把对象头里的 `handle` 字段写上；
- 再用 `record_obj(handle, obj)` 把真实位置反写到 handle 存储里。

这一进一出就形成了两层间接：

- 对象里记住自己的 handle；
- handle 里记住对象当前真实位置。

这也为后续 compact / migrate 改写对象位置提供了支点。

### 3. 释放：通过 handle 找对象，再把对象挂回 zspage freelist

`zs_free()` 的路径比想象中短（`1384-1417`）：

1. 通过 handle 找到 `obj`；
2. 从 obj 反推出 `zspage` 和 `class`；
3. `obj_free()` 把对象重新挂回 freelist；
4. 更新 `inuse` 和 fullness group；
5. 如果 zspage 变空，尝试释放；
6. 最后释放 handle 自身。

最有意思的是它对竞态的处理。代码里明确写着：

- `pool->lock` 保护 `zs_free()` 与 zpage migration 的竞态；
- `class->lock` 保护同一个 class 下的 alloc/free 以及 fullness list 操作。

也就是说，handle 虽然是稳定的，但“它指向哪个 obj”并不是无条件永远稳定的，所以 free 路径必须先在迁移语义上站稳，再去改内部状态。

### 4. 读写对象：handle-based access，而不是 stable pointer

这组接口是理解 `zsmalloc` 与普通分配器区别的关键：

- `zs_obj_read_begin/end()`
- `zs_obj_read_sg_begin/end()`
- `zs_obj_write()`

它们共同干的一件事是：

1. 先通过 handle 找到当前对象落在哪个 `zspage` / `zpdesc`；
2. 给 `zspage` 加 reader lock，防止 migration 改页位置；
3. 计算对象偏移；
4. 如果对象没跨页，直接 map 那一页；
5. 如果对象跨页，就拆成两段复制或者 SG 描述。

这套 API 设计其实非常克制。它没有企图向上层伪装成“你拿到了一段普通连续内存”，而是老老实实把“对象可能横跨两个 page”这个现实暴露给接口层。

这种诚实，是这份代码整体风格里很可贵的一点。

### 5. fullness group：它不是附属统计，而是 compaction 的策略基础

`zsmalloc` 把 zspage 按使用率分成多个 fullness group（`134-141`，`624-695`）。这不是为了 debug 看着好看，而是为了后续 compact 能有策略地选源和目标：

- 低使用率 zspage 更适合当 `src`
- 高使用率 zspage 更适合当 `dst`

它并不追求全局最优，只追求一个足够有效的启发式：  
**让稀疏页往稠密页合并，尽可能腾出整串页来释放。**

## 五、compaction 与 migration：`zsmalloc` 最复杂、也最值钱的两条支线

### 1. 内部 compaction：对象搬家，但 handle 不变

`zs_compact()` / `__zs_compact()` / `migrate_zspage()` 是整份文件的另一条主线（`1519-1977`）。

大致过程是：

1. `zs_can_compact()` 估算某个 class 还能不能挤出整页（`1872-1885`）；
2. 从低 fullness group 里拿一个 `src_zspage`；
3. 从高 fullness group 里拿一个 `dst_zspage`；
4. `migrate_zspage()` 扫描 `src` 里的已分配对象；
5. 每迁一个对象，就：
   - 在 `dst` 上 `obj_malloc()` 分配新槽位；
   - `zs_object_copy()` 复制对象内容；
   - 更新 handle 指向的新 obj；
   - 在 `src` 上 `obj_free()`；
6. 如果 `src` 最终空了，就释放整串页。

这里真正漂亮的点是：

**对象位置可以变，但 handle 不变。**

这就是为什么 `zsmalloc` 能像“磁盘整理碎片”一样整理自己，而不需要让外部持有者配合修改引用。

### 2. page migration：不是迁整个对象池，而是迁 zspage 里的组成页

`zs_page_migrate()` 看起来篇幅不长，但它是全文件最容易出边界问题的地方（`1686-1789`）。

它迁的是：

- 一个 `zspage` 里的某个组成 page / `zpdesc`

不是：

- 整个 zspage 一次性换掉。

流程是：

1. 从 old page 拿到 `zpdesc` 和所属 `zspage`；
2. 通过 `pool->lock`、`class->lock`、`zspage_write_trylock()` 封住并发访问；
3. 复制整页内容到 `newzpdesc`；
4. 扫描旧页上的所有已分配对象；
5. 把每个 handle 中记录的旧对象位置改成新页对应位置；
6. `replace_sub_page()` 把 zspage 链里的 old page 替换成 new page；
7. 重置旧 `zpdesc`。

这一步本质上像在桥上换一块桥板：桥整体还在，但其中一块物理页要被挪走或替换。

### 3. 那个 TODO 真正说明的问题

`zs_page_migrate()` 的 TODO 不是在抱怨实现丑，而是在指出一个尚未被彻底制度化的生命周期问题：

- 页已经 isolated for migration；
- 但隔离成功后，页锁会临时释放；
- 在这段窗口里，如果整个 `zspage` 被销毁，就会和迁移路径打架。

当前实现用很多锁在尽量缩小这个窗口的风险，但它自己也承认：  
**真正稳的做法，应该是把“已隔离页所属 zspage 暂缓销毁”做成明确规则。**

这就是这份代码今天最值得继续盯的点。

## 六、横纵交汇洞察

### 1. `zsmalloc` 最成功的决策，是尽早放弃了“返回稳定指针”这层幻想

很多 allocator 的复杂度来自“想让调用方用起来像普通内存”，然后内部再偷偷维持一堆不普通的现实。

`zsmalloc` 没走这条路。它从很早就承认：

- 对象可能跨页；
- 对象以后可能被搬；
- 内核不能永久映射所有对象；
- 所以上层就别拿稳定地址，拿 handle 吧。

这个决定看上去增加了使用成本，实际上给它换来了今天的全部能力：

- 可 compact；
- 可 migrate；
- 可在低内存下仍以 order-0 页为底层存储；
- 可和 zswap 的 SG 解压路径对接。

### 2. 它今天的优势，全都源于 `zspage`

今天 `zsmalloc` 的几个核心优势——

- 不依赖高阶页；
- 对 `PAGE_SIZE/2 ~ PAGE_SIZE` 对象碎片更小；
- 可以做对象重排；

都能追溯到最早那个决策：**多个小页缝成 zspage，对象允许跨页。**

这是一种非常典型的内核式取舍：  
前期多承担元数据和复杂性，换运行期更稳定的资源利用率。

### 3. 它今天的主要包袱，也来自同一个根

`zspage` 让对象布局灵活，也带来了现在最难的两类问题：

1. **并发访问控制更难**  
   因为对象不是静态待在某一页某一偏移，compact/migrate 都可能改它位置。

2. **生命周期边界更难**  
   因为真正的资源单元是 zspage，但 migration 框架很多时候看到的是单页。

所以可以说，`zsmalloc` 今天最值得继续优化的地方，不是“再省 1% 内存”，而是把 **zspage 级生命周期** 和 **单页级 migration 协议** 之间的边界处理得更干净。

### 4. 如果只记一个判断，我会记这个

`mm/zsmalloc.c` 本质上不是一个普通内存分配器，而是 Linux MM 里一座：

**以 handle 暴露对象、以 zspage 组织底层页、以 compaction/migration 维持空间利用率的压缩对象仓库。**

它最核心的价值不在“分配成功”本身，而在：

**在低内存、对象大小剧烈波动、对象位置允许重排的前提下，仍然把存储和并发语义维持在一个可控范围里。**

## 七、信息来源

### 代码与头文件

1. `mm/zsmalloc.c`，重点函数：`zs_create_pool()`、`zs_malloc()`、`zs_free()`、`zs_obj_read_begin/end()`、`zs_obj_read_sg_begin/end()`、`zs_obj_write()`、`migrate_zspage()`、`zs_page_migrate()`、`zs_compact()`
2. `include/linux/zsmalloc.h`，对外接口定义
3. `mm/zpdesc.h`，`zpdesc` 与 `struct page`/folio 的关系
4. `drivers/block/zram/zram_drv.c`，`zram` 如何使用 `zs_malloc()` / `zs_obj_write()` / 读回路径
5. `mm/zswap.c`，`zswap` 如何使用 `zs_malloc()` 与 SG-read API
6. `Documentation/mm/zsmalloc.rst`，设计动机、内部机制、统计说明
7. `Documentation/admin-guide/mm/zswap.rst`，`zswap` 对 `zsmalloc` 的依赖说明
8. `Documentation/mm/page_migration.rst`，`movable_ops` 页面迁移框架对 zsmalloc 页的说明

### git 历史

1. `bcf1647d0899` — `zsmalloc: move it under mm`
2. `07864f1a57fb` — `mm: zsmalloc: remove object mapping APIs and per-CPU map areas`
3. `dc2e4982cb01` — `zsmalloc: introduce SG-list based object read API`
4. `e27af3f9360e` — `zsmalloc: sleepable zspage reader-lock`
5. `5ec3583309ef` — `mm/zsmalloc: make PageZsmalloc() sticky until the page is freed`
6. `dc711106a0bc` — `zsmalloc: return -EBUSY for zspage migration lock contention`
7. `4fb61d95ad21` — `mm/zsmalloc: copy KMSAN metadata in zs_page_migrate()`

### 访问时间

- 本地仓库与 git 历史访问时间：2026-04-30

## 八、方法论说明

本文采用横纵分析法：纵向沿 `zsmalloc` 的设计动机、文件历史与接口演化，追踪它如何从“为 zram 定制的压缩页分配器”成长为一个带 compaction/migration 语义的特种 allocator；横向则把它放回当前 Linux MM 体系，与 slab/slub、zram、zswap、page migration 等模块并置，判断它今天真正承担的系统角色。
