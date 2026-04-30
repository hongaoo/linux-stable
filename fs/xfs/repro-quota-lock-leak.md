# xfs quota 锁未释放（修改前）复现手册

本文用于复现 `fs/xfs/scrub/quota.c` 在修改前的早退锁未释放风险，并给出修复后对照验证方法。

## 1. 静态复现（推荐，稳定）

目标：复现 Coccinelle `preceding lock` 告警。

### 1.1 切到“修改前”版本

> 下面命令仅回退单文件，便于快速对比。

```bash
cd /home/hongao/github-workspace/linux-stable
git switch -c repro/quota-lock-leak || git switch repro/quota-lock-leak

# 回退到修复前版本（如果上一提交就是修复）
git checkout HEAD~1 -- fs/xfs/scrub/quota.c

# 确认是“早退直接 return”形态
sed -n '165,190p' fs/xfs/scrub/quota.c
# 期望看到：
# if (!xchk_fblock_process_error(...))
#     return error;
```

### 1.2 运行锁规则

```bash
make coccicheck MODE=report M=fs/xfs/scrub \
  COCCI=$PWD/scripts/coccinelle/locks/mini_lock.cocci \
  DEBUG_FILE="cocci-xfs-scrub-minilock-repro.log"
```

### 1.3 期望结果

输出中出现（行号可能有微小偏移）：

```text
./scrub/quota.c:175:... preceding lock on line 160
```

---

## 2. 修复后对照验证

目标：验证修复后不再命中该锁告警。

### 2.1 恢复修复版本

```bash
# 如果本分支已有修复，可直接恢复工作区版本
# （或者切回包含修复的分支）
git restore fs/xfs/scrub/quota.c
```

### 2.2 再跑同一规则

```bash
make coccicheck MODE=report M=fs/xfs/scrub \
  COCCI=$PWD/scripts/coccinelle/locks/mini_lock.cocci \
  DEBUG_FILE="cocci-xfs-scrub-minilock-fixed.log"
```

### 2.3 期望结果

- 不再出现 `scrub/quota.c` 的 `preceding lock on line 160`。

---

## 3. 运行时复现思路（进阶）

静态告警已经足以证明路径风险；若要运行时观察，需要触发 `xchk_quota_item_bmap()` 错误路径。

### 3.1 内核建议配置

- `CONFIG_XFS_ONLINE_SCRUB=y`
- `CONFIG_LOCKDEP=y`
- `CONFIG_PROVE_LOCKING=y`
- `CONFIG_DEBUG_MUTEXES=y`
- 可选：`CONFIG_KASAN` / `CONFIG_KCSAN`

### 3.2 测试盘与文件系统

```bash
# 示例：使用 loopback 文件
truncate -s 2G /tmp/xfs-quota.img
mkfs.xfs -f /tmp/xfs-quota.img
sudo mount -o loop,uquota,gquota /tmp/xfs-quota.img /mnt
```

### 3.3 故障注入（具体步骤）

下面给出两套可执行方案：

- **方案 A（推荐）**：`dm-error` 精确打到 quota inode 的数据块（最稳定）
- **方案 B**：`dm-flakey` 周期性制造 I/O 失败（更接近“间歇失败”）

> ⚠️ 以下操作会制造 I/O 错误，请仅在测试镜像/测试机执行。

#### 3.3.A `dm-error` 精确命中 quota inode 读路径

1. 准备 loop 设备并创建 baseline 映射：

```bash
cd /home/hongao/github-workspace/linux-stable
sudo umount /mnt 2>/dev/null || true

LOOP=$(sudo losetup -f --show /tmp/xfs-quota.img)
TOTAL=$(sudo blockdev --getsz "$LOOP")

# 基础映射（全盘 linear）
echo "0 $TOTAL linear $LOOP 0" | sudo dmsetup create xfsbase

# 在 dm 设备上建文件系统并挂载（避免后续底层设备切换）
sudo mkfs.xfs -f /dev/mapper/xfsbase
sudo mount -o uquota,gquota /dev/mapper/xfsbase /mnt
```

2. 找出 quota inode 号（以 user quota 为例）：

```bash
UQINO=$(sudo xfs_db -r -c 'sb 0' -c 'p uquotino' /dev/mapper/xfsbase | awk '/uquotino/ {print $3}')
echo "uquotino=$UQINO"
```

3. 找出该 inode 的一个映射块（`bmap` 输出第一段即可）：

```bash
sudo xfs_db -r -c "inode $UQINO" -c 'bmap' /dev/mapper/xfsbase
```

从输出中记录：

- `startblock`（文件系统块号，记为 `FSB`）
- `blockcount`（长度，记为 `FBLEN`）

4. 把文件系统块号换算为 512B 扇区：

```bash
BS=$(sudo xfs_db -r -c 'sb 0' -c 'p blocksize' /dev/mapper/xfsbase | awk '/blocksize/ {print $3}')
SECT_PER_FSB=$((BS / 512))

# 按你在上一步看到的值填写
FSB=<startblock>
FBLEN=<blockcount>

ERR_START=$((FSB * SECT_PER_FSB))
ERR_LEN=$((FBLEN * SECT_PER_FSB))
echo "ERR_START=$ERR_START ERR_LEN=$ERR_LEN"
```

5. 将 `xfsbase` 在线切换成“中间一段 error”映射：

```bash
sudo dmsetup suspend xfsbase
{
  echo "0 $ERR_START linear $LOOP 0"
  echo "$ERR_START $ERR_LEN error"
  echo "$((ERR_START + ERR_LEN)) $((TOTAL - ERR_START - ERR_LEN)) linear $LOOP $((ERR_START + ERR_LEN))"
} | sudo dmsetup load xfsbase
sudo dmsetup resume xfsbase
```

6. 触发 scrub：

```bash
sudo xfs_scrub -n /mnt
```

7. 观察：

- `dmesg -T | tail -n 200`
- lockdep/挂起迹象（修改前更容易触发）

8. 清理恢复：

```bash
sudo umount /mnt || true
sudo dmsetup remove xfsbase || true
sudo losetup -d "$LOOP" || true
```

#### 3.3.B `dm-flakey` 周期性故障（间歇失败）

如果你更希望“间歇失败”而不是固定坏区：

```bash
sudo umount /mnt 2>/dev/null || true
LOOP=$(sudo losetup -f --show /tmp/xfs-quota.img)
TOTAL=$(sudo blockdev --getsz "$LOOP")

# 20 秒正常 + 10 秒失败循环（down 期间该 target I/O 失败）
echo "0 $TOTAL flakey $LOOP 0 20 10" | sudo dmsetup create xfsflakey

sudo mount -o uquota,gquota /dev/mapper/xfsflakey /mnt
sudo xfs_scrub -n /mnt
```

观察同上；结束后清理：

```bash
sudo umount /mnt || true
sudo dmsetup remove xfsflakey || true
sudo losetup -d "$LOOP" || true
```

### 3.4 观察点

- 修复前：可能出现 lockdep 警告或后续路径在 `q_qlock` 等待（触发依赖具体时机）。
- 修复后：相同注错场景下，不应再出现该早退锁不平衡现象。

---

## 4. 最小结论

- **静态复现是最可靠证据链**：修复前命中 `preceding lock`，修复后消失。
- 运行时复现可作为补充验证，但依赖注错时机与内核调试配置。