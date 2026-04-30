# `mm/migrate.c`：Linux 页迁移机制研究报告

> 研究时间：2026-04-29 | 所属领域：Linux 内核内存管理 | 研究对象类型：内核机制 / 核心实现文件

## 一、一句话定义

`mm/migrate.c` 是 Linux 内核里负责**把一个 folio/page 从旧物理页安全迁到新物理页**的总控引擎。它不决定“为什么迁”，但它决定“既然要迁，怎样迁才不会把页表、rmap、mapping、LRU、memcg 和大页状态弄乱”。

这句话听起来有点工程味。换成人话，它像一个搬家公司调度中心。搬家不是难点，真正难的是：搬的途中门牌号不能丢，快递地址不能错，电梯要留人看着，搬一半不能让住户突然推门进来，搬失败了还得把所有家具原样放回去。

`mm/migrate.c` 做的就是这件事。

## 二、纵向分析：这套机制是怎么长成今天这个样子的

### 1. 起点不是“优化”，而是“必须能挪”

这个文件的文件头已经把来历点明了：页迁移最早是在 **memory hotplug** 的语境里发展出来的。内存要热插拔、要下线，内核就得有一种办法，把落在某段物理内存上的页挪走，而不是要求整台机器停下来（`mm/migrate.c:3-14`）。

从 git 历史看，`mm/migrate.c` 作为独立文件是在 2006 年被抽出来的。`b20a35035f98` 这笔提交的标题就叫 **“page migration reorg”**，提交说明写得很直白：把原先散在 `vmscan.c`、`mempolicy.c`、`fs/buffer.c` 里的页迁移逻辑集中起来，单独形成 `mm/migrate.c`。这不是一次小整理，而是一次架构判断：**页迁移不是某个子系统的附属能力，它值得一套公共核心。**

这一刀切出来之后，后面的演化路径就很清楚了：凡是需要“把页挪个地方”的场景，都开始往这套公共引擎上挂。

### 2. 真正让它站住脚的，不是 copy，而是 migration entry

页迁移如果只是“分配新页、拷贝旧页、换个指针”，那它根本活不到今天。因为真实世界里页不是静止的：CPU 还在访问，页表还在指向它，文件映射还在共享它，rmap 还在追踪它，回收线程和 fault 线程可能正好撞上。

所以这套机制早期最关键的补强，是 **migration entry**。在历史提交里你能看到类似 “Swapless page migration: add R/W migration entries” 和 “use migration entries for file pages” 这样的演化痕迹；在今天的实现里，这套思想落在几组核心函数上：

- `try_to_migrate()`：把现有映射换成 migration entries；
- `migration_entry_wait()`：并发访问撞上迁移条目时等待或重试；
- `remove_migration_ptes()`：迁移完成后，把 migration entries 恢复成真正映射（`mm/migrate.c:455-474`、`481-507`）。

这一步很像给旧住址门口贴了一张“正在搬家，请稍后”的告示。重要的不是告示本身，而是它把“搬运中”的临界区从一团混乱，变成了一种被整个 MM 子系统理解的状态。

### 3. 从 NUMA / mempolicy 扩到更广的内存管理世界

页迁移一旦成了公共能力，它的调用场景就开始扩散。

用户态显式搬页这条线，最后落成了 `move_pages()` syscall。今天 `mm/migrate.c` 里还能直接看到完整入口：`kernel_move_pages()`、`do_pages_move()`、`do_move_pages_to_node()`，最终都收敛到 `migrate_pages()`（`mm/migrate.c:2239-2625`）。

另一条线是内核自己因为策略需要而搬页。比方说：

- **compaction**：为了拼出更高阶连续物理内存，把零散页挪开（`mm/compaction.c:2664-2678`）；
- **memory hotplug**：为了让某段内存下线，先把页迁走（`mm/memory_hotplug.c:1840-1875`）；
- **mempolicy / mbind**：按内存策略重排物理落点（`mm/mempolicy.c:1318-1327`）；
- **automatic NUMA balancing**：fault 之后发现 folio 放错 NUMA 节点，就尝试异步迁回去（`mm/memory.c:6185-6204`、`mm/huge_memory.c:2298-2315`）；
- **demotion**：把冷页从快内存降到慢内存（`mm/vmscan.c:1019-1035`）；
- **DAMON**：基于访问特征做页面搬运（`mm/damon/ops-common.c:322-325`）。

你会发现，这里面的“为什么搬”完全不一样。有的是为了性能，有的是为了拓扑调整，有的是为了策略执行，有的是为了硬件容错。但它们最终都共用同一个事实：**页一旦真的要搬，就必须穿过同一片一致性雷区。**

### 4. folio、THP、hugetlb 把这套机制逼得更像“事务引擎”

如果说早年的页迁移更像“单页搬运”，那今天的 `mm/migrate.c` 已经明显长成了一个带回滚、重试、分支处理的事务系统。

最能说明这一点的是主路径：

- `migrate_pages()`：总体入口（`mm/migrate.c:2091-2185`）
- `migrate_pages_batch()`：批量异步迁移（`mm/migrate.c:1802-2011`）
- `migrate_pages_sync()`：先异步批量试一轮，再对失败页做同步兜底（`mm/migrate.c:2014-2063`）
- `migrate_folio_unmap()`：分配目标 folio、拿锁、塞 migration entries、记录旧状态（`mm/migrate.c:1205-1352`）
- `migrate_folio_move()`：真正提交迁移，成功则切映射，失败则回滚（`mm/migrate.c:1355-1450`）

尤其是 `migrate_folio_unmap()` / `migrate_folio_move()` 这一对，已经非常像数据库里的两阶段提交：

1. **预备阶段**：拿到 `src` / `dst`，锁住，记录 `PAGE_WAS_MAPPED` / `PAGE_WAS_MLOCKED` / `anon_vma`，把旧映射替换成 migration entries。
2. **提交阶段**：调用真正的迁移实现，成功后把页表切到新页；失败则通过 `migrate_folio_undo_src()` 和 `migrate_folio_undo_dst()` 完整撤销。

这也是为什么文件里会有那么多 `-EAGAIN`、`-ENOMEM`、永久失败、可重试失败、拆分后重试的分支。它已经不是“搬一次看看”，而是“**在复杂并发环境里，尽可能多地把可搬的搬掉，把不能搬的分类记账，并且不破坏系统状态**”。

### 5. 新问题不是“还能不能迁”，而是“还能不能继续共用同一套抽象”

最近一两年的提交，读起来会有一种很鲜明的味道：大方向没有变，难点转向了细部结构和抽象边界。

比如：

- `53050890802e`：在 `folio_migrate_mapping()` 里防止 memcg 生命周期出问题；
- `a2e0c0668a34` / `3bac01168982`：处理 deferred split queue 和 large folio 迁移期间的竞态；
- 2025 年一串提交把 `movable_ops` 页的处理从普通 folio 路径里继续拆出来。

这背后的信号很明确：**页迁移这件事已经不是“有没有”问题，而是“在 folio 化、memcg 重构、大页普及、特殊页类型增多之后，这套通用内核还能不能保持清晰边界”问题。**

这一点在你 IDE 里选中的两个 TODO 上都能看出来：

- `mm/migrate.c:116-117`：这些 `movable_ops` 页未来不想再被当作 folio 处理；
- `mm/zsmalloc.c:1702-1706`：zsmalloc 迁移时，隔离后的页在 page lock 暂时释放期间仍可能被销毁，说明这条支线的生命周期语义还没有完全收口。

这些 TODO 不是小修小补，它们是在提醒开发者：**最初那套统一抽象很好用，但边界已经开始被现实挤压。**

## 三、横向分析：今天的 `mm/migrate.c` 在整个 MM 体系里占什么位置

如果把当前时间切开来看，`mm/migrate.c` 并不直接和“回收、压缩、策略、热插拔”竞争。它更像一个共享底盘。真正有意思的，是把它和周边几套机制放在一起看。

### 1. 它和 compaction 的关系：一个找空间，一个做搬运

`mm/compaction.c` 负责的是“在哪里挑出可迁页、在哪里找空闲目标页”。真正执行迁移时，它调用 `migrate_pages()`，并传入 `compaction_alloc` / `compaction_free` 这组回调（`mm/compaction.c:2663-2668`）。

所以 compaction 更像前线指挥员，`mm/migrate.c` 更像工兵部队。前者决定“这片地方该整理了”，后者决定“整理的时候别把桥炸了”。

这个分工非常重要。因为 compaction 可以有自己的扫描策略、skip hint、scanner meeting 之类的 heuristics，但它不需要重新发明一套页表切换和回滚机制。

### 2. 它和 mempolicy / `move_pages()` 的关系：一个负责意图，一个负责兑现

`mempolicy.c` 和 `move_pages()` syscall 描述的是**用户或策略想把页放到哪里**。真正把页拿出来、隔离、迁移、失败后放回去，还是交给 `migrate_pages()`（`mm/mempolicy.c:1318-1323`，`mm/migrate.c:2239-2625`）。

这条线很像“控制面”和“数据面”的关系。控制面说“这批页应该去 node X”，数据面负责真正执行。

好处是显而易见的：无论是用户显式搬，还是策略暗中搬，最后都共享同一套失败语义和成功语义。坏处也很明显：一旦公共引擎行为变化，影响面会非常大。

### 3. 它和 automatic NUMA balancing 的关系：这套引擎被压缩到了最轻量的形态

`migrate_misplaced_folio_prepare()` 和 `migrate_misplaced_folio()` 是一个非常有代表性的切片（`mm/migrate.c:2676-2766`）。

这里的迁移，不追求“必须成功”，追求的是“值不值得试一把”。所以你会看到一些很克制的约束：

- 共享可执行文件页尽量不迁，避免把共享库搬得满天飞（`mm/migrate.c:2682-2692`）；
- dirty file folio 不迁，因为异步模式下很多文件系统不愿意配合（`2694-2700`）；
- 目标 node 水位不够就直接放弃，甚至先唤醒 kswapd（`2703-2724`）；
- 真正迁移用的是 `MIGRATE_ASYNC`（`2751-2753`）。

换句话说，NUMA balancing 把页迁移当成“轻量级校正器”，而不是“必须完成的任务”。这和 memory hotplug 那种“这批页不走，内存就下不了线”完全是两种气质。

### 4. 它和 demotion / DAMON 的关系：迁移不再只是在 NUMA 节点之间横跳

`vmscan.c` 里的 demotion 路径说明一件事：页迁移已经不只是“把页放回更近的 CPU 节点”，还承担了**快慢内存分层**里的层级流动（`mm/vmscan.c:1019-1035`）。

同样，DAMON 也通过 `migrate_pages()` 做页面重排（`mm/damon/ops-common.c:322-325`）。这说明 `mm/migrate.c` 的抽象已经足够稳，以至于新的内存策略不用自己造轮子，只要给它提供目标分配规则就能接入。

这类场景和早期 page migration 相比，语义更“政策化”了：迁移不是为了某个具体页，而是为了整体冷热分布。

### 5. 它和 `movable_ops` 页的关系：统一框架的上限，也暴露了统一框架的代价

`movable_operations` 这组接口定义在 `include/linux/migrate.h:16-49`。它允许一些并不归普通 LRU 路径管理的页，也接入页迁移框架。

目前至少两类对象用了它：

- balloon/offline pages：`balloon_mops` 注册到 `PGTY_offline`（`mm/balloon.c:332-341`）；
- zsmalloc pages：注册到 `PGTY_zsmalloc`，并通过自己的 `isolate_page` / `migrate_page` / `putback_page` 处理真实迁移（`mm/zsmalloc.c:1676-1709`）。

这条线把 `mm/migrate.c` 的通用性推到了一个很高的位置：它不只会迁匿名页和页缓存，连“页的真正拥有者是驱动或特殊分配器”的对象也能迁。

但代价也摆在那儿。`movable_ops` 页并不天然适合 folio 语义，这就是为什么 `mm/migrate.c` 和 `mm/zsmalloc.c` 里都留着 TODO。**统一抽象把更多东西纳进来了，也把更多边界问题一并纳进来了。**

## 四、当前实现拆解：如果顺着代码读，最该抓住哪条主线

### 1. 入口：`migrate_pages()`

`migrate_pages()` 是今天的总入口（`mm/migrate.c:2091-2185`）。它做三件事：

- 先单独处理 hugetlb；
- 把普通 folio 按批次切出来；
- 根据模式走异步批处理或同步兜底。

它返回的不是“迁了多少”，而是**还有多少 folio 没迁成**，或者直接返回错误码。这个接口设计很内核：它更关心“还有多少事情没做完”，因为调用方通常还要决定 putback、重试还是放弃。

### 2. 第一阶段：`migrate_folio_unmap()`

这是整个文件最值得精读的函数（`mm/migrate.c:1205-1352`）。

它的动作顺序非常讲究：

1. 分配 `dst`；
2. 锁住 `src`，必要时按模式决定要不要等；
3. 处理 writeback；
4. 对匿名页抓 `anon_vma` 引用；
5. 锁住 `dst`；
6. 如果是 `movable_ops` 页，记录状态后直接返回给后续专门路径；
7. 如果 `src` 还被映射，调用 `try_to_migrate()` 建 migration entries；
8. 记录旧状态，准备提交或回滚。

整套动作的目标不是搬内容，而是**冻结可见世界**。在这一步结束之前，系统还不应该把“新家地址”暴露出去。

### 3. 第二阶段：`migrate_folio_move()`

`migrate_folio_move()` 接过已经准备好的 `src/dst`，进入真正的提交阶段（`mm/migrate.c:1355-1450`）。

如果是 `movable_ops` 页，就调 `migrate_movable_ops_page()`；如果是普通 folio，就走 `move_to_new_folio()`：

- 没有 mapping 的 folio 直接按普通页迁；
- 有 `a_ops->migrate_folio` 的，就尊重 mapping 所属者；
- 都没有时用 fallback 路径（`mm/migrate.c:1092-1129`）。

迁移成功之后，代码会：

- 让 `dst` 回到 LRU；
- 必要时恢复 mlock 语义；
- 调 `remove_migration_ptes(src, dst, 0)`，把页表里的“迁移中”状态切成真实新映射；
- 释放旧页。

如果失败，回滚就顺着另一条非常清晰的对称路径走：

- `migrate_folio_undo_src()`：把旧页恢复；
- `migrate_folio_undo_dst()`：把新页回收。

这份代码的美感，恰恰在这种对称性里。

### 4. 为什么会有那么多 retry / split / batch 逻辑

因为真实世界里很多失败并不是“永远不行”，而是“这次时机不好”。

在 `migrate_pages_batch()` 里可以看到几类非常典型的处理：

- `-EAGAIN`：保留在列表里，下轮再试；
- `-ENOMEM`：内存紧张时，先把已 unmap 的部分尽快收尾，然后退出；
- large folio / THP 迁不动时，尝试 split 成小页再迁；
- 对同步迁移，先用 `MIGRATE_ASYNC` 批量扫一遍，再把剩下难啃的单独同步处理（`mm/migrate.c:1802-2063`）。

它不是在追求“逻辑漂亮”，而是在追求“别把能成功的那部分也拖死”。

## 五、横纵交汇洞察

### 1. 历史决定了它今天为什么像一台“事务机器”

`mm/migrate.c` 之所以不是一个简单工具函数，而是一整套重试、回滚、记录旧状态、分类失败的系统，根子在它的出身：它从一开始就不是为“偶尔搬一页”设计的，而是为 **memory hotplug、NUMA、策略迁移** 这类会频繁撞上并发状态的场景设计的。

也就是说，今天它所有“看起来有点重”的代码，大多不是过度设计，而是历史压力留下来的沉积层。

### 2. 它最厉害的地方，是把“迁移原因”跟“迁移执行”硬生生拆开了

这套设计最成功的一点，是让 compaction、mempolicy、NUMA balancing、demotion、DAMON、memory hotplug 这些完全不同的上层理由，共用一套执行内核。

这很像内核世界里常见的高明设计：不是把所有场景写成 if/else，而是把“策略”和“提交机制”拆开。上层决定意图，下层守住一致性。

`mm/migrate.c` 能活这么久，靠的不是它知道很多策略，而是它几乎只专注一件事：**页真的要动的时候，怎样别出事故。**

### 3. 它今天的优势，恰恰也在变成今天的包袱

统一引擎带来的好处非常大，但包袱也越来越明显：

- 普通 LRU folio、hugetlb、THP、movable_ops、device/private 页并不是同一种东西；
- folio 化在简化普通页路径的同时，也让特殊页类型越来越显得“勉强共存”；
- memcg、deferred split、large folio、NUMA tiering 的新语义，不停往这台老引擎上加新齿轮。

这就是为什么现在的 TODO 多半不是“补功能”，而是“拆边界”。我判断未来几年的迁移代码，重点不会是再加多少新 reason，而是把某些特殊页类型进一步从公共路径中剥离，或者至少把它们的生命周期语义和普通 folio 切干净。

### 4. 如果只记一个结论，我会记这个

`mm/migrate.c` 不是“页搬家代码”，而是 Linux MM 里一台**负责提交页位置变更的事务引擎**。

它的真正价值不在搬，而在于：  
**当很多不同子系统都想改同一页的物理归属时，它提供了一种系统还能继续保持一致的改法。**

## 六、信息来源

### 代码与头文件

1. `mm/migrate.c`，重点函数：`migrate_pages()`、`migrate_pages_batch()`、`migrate_pages_sync()`、`migrate_folio_unmap()`、`migrate_folio_move()`、`move_to_new_folio()`、`remove_migration_ptes()`、`migrate_misplaced_folio_prepare()`、`migrate_misplaced_folio()`
2. `include/linux/migrate.h`，`movable_operations` 接口与导出入口
3. `include/linux/migrate_mode.h`，`MIGRATE_ASYNC` / `MIGRATE_SYNC_LIGHT` / `MIGRATE_SYNC` 与 `MR_*` 原因枚举
4. `mm/compaction.c`，`migrate_pages()` 在 compaction 中的调用
5. `mm/memory_hotplug.c`，热插拔场景中的迁移调用
6. `mm/mempolicy.c`，策略迁移与 `mbind` 相关调用
7. `mm/memory.c`、`mm/huge_memory.c`，automatic NUMA balancing 中的 misplaced folio 迁移
8. `mm/vmscan.c`，demotion 场景中的迁移调用
9. `mm/damon/ops-common.c`，DAMON 调用迁移引擎
10. `mm/balloon.c`、`drivers/virtio/virtio_balloon.c`，`movable_ops` 在 balloon 页面上的接入
11. `mm/zsmalloc.c`，`movable_ops` 在 zsmalloc 页面上的接入与相关 TODO

### git 历史

1. `b20a35035f98` — `[PATCH] page migration reorg`：`mm/migrate.c` 作为独立文件的起点
2. `0697212a411c` — `Swapless page migration: add R/W migration entries`
3. `04e62a29bf15` — `More page migration: use migration entries for file pages`
4. `742755a1d8ce` — `page migration: sys_move_pages(): support moving of individual pages`
5. `53050890802e` — `mm: migrate: prevent memory cgroup release in folio_migrate_mapping()`
6. `a2e0c0668a34` — `mm: migrate: requeue destination folio on deferred split queue`
7. `3bac01168982` — `mm: fix deferred split queue races during migration`

### 访问时间

- 本地仓库与 git 历史访问时间：2026-04-29

## 七、方法论说明

本文采用横纵分析法：纵向沿 git 历史和机制演化追踪 `mm/migrate.c` 如何从 memory hotplug 背景中的功能代码，长成今天的公共迁移引擎；横向则把它放回当前 Linux MM 体系，与 compaction、mempolicy、NUMA balancing、demotion、DAMON、`movable_ops` 等场景一起观察，判断它真正的系统角色。
