# v1 → v2 SHM 布局迁移说明

> 状态:**已完成**(2026-06)。本仓的**应用代码**(三件套 market/order/private + shm_init +
> bridge + smoke)自此为**纯 v2**,不再有 `[shm].layout` 开关。**`gconf` 是公共契约库,v1/v2 定义并存**
> ——v1 契约(`board.h` / `order_event.h` / `private_event.h` / `names.h` / `private_req.h` 的
> `PrivateReqRing`)保留供其它 v1 消费者,应用代码只是不再 include 它们。布局规约见
> [`v2-shm-layout-spec.md`](v2-shm-layout-spec.md),系统架构见 [`architecture.md`](../../docs/architecture.md)。

## 1. 为什么迁移

v1 的三类 SHM 流共用「一个 MPMC 广播环模板 + 一个 latest-wins 板」,语义被强行拉平,留下三处隐患:

| # | v1 隐患 | v2 修复 |
|---|---------|---------|
| #10 | board/event 的 seqlock 发布缺前沿 `StoreStore`(release)fence → 弱内存序机器(aarch64,生产是 Amazon Linux aarch64 gcc14)上**可撕裂读** | `v2/seqlock.h` 唯一正确实现:奇序 store 后插 `atomic_thread_fence(release)`,偶序 store 用 release |
| B3 | 事件环被绕圈(lapped)时**静默丢事件**,消费者无从察觉 | BcastRing `drain` **强制返回丢失计数**(>0 ⇒ 该从 REST resync) |
| — | req 是 SPMC 广播环,策略写快于 order 消费时**静默丢单**且物理不可恢复(被覆盖的 client_order_id 没了) | SpscQueue per-venue + 背压:满则 `try_push` 返回 false(**不覆盖**)→ 生产者本地 REJECTED |

附带改进:自描述段头(`SegHeader`:magic / layout_version / kind / entry_size / capacity / `schema_hash` /
created_ns)让 attach 侧 `seg_validate` 能立即发现契约不符;全精度 ns 时间戳(int64 + 三个有符号 int32
延迟)取代 v1 的 uint32 frac 增量(>4.29s 溢出的过早优化)。

## 2. v2 按流语义分型(三类段)

| 段类型 | 语义 | 实现 | 范式 |
|--------|------|------|------|
| **Board** | latest-wins(只留最新 BBO) | `Board` / `BoardSlot`(per-GID best_uid CAS 去重 + seqlock 槽) | Linux seqlock |
| **BcastRing** | 事件广播(多生产者抢绝对槽 + 强制 lapped 检测) | `BcastRing<EventEntry>`(`EventRing`) | Vyukov 槽编码 seq + Disruptor |
| **SpscQueue** | 请求(per-venue 可靠 SPSC + 背压) | `SpscQueue<PrivateReqFrame>`(`ReqQueue`) | io_uring SQ/CQ |

头文件:`gconf/include/gconf/shm/v2/`(`seg_header.h` / `seqlock.h` / `board.h` / `event.h` /
`req.h` / `names.h`)。

## 3. v1 → v2 映射

### 段(SHM 对象名)

| 流 | v1 | v2 |
|----|----|----|
| 行情板 | `/shm_*_book_tick` | `/shm_*_v2_book` |
| order ack 环 | `/shm_*_order_event_ring` | `/shm_*_v2_order_evt` |
| private 事件环 | `/shm_*_private_event_ring` | `/shm_*_v2_priv_evt` |
| 策略→order 请求 | `/shm_private_req_ring`(**跨所共享** SPMC) | `/shm_*_v2_req`(**per-venue** SPSC) |

> `*` = `bybit_lin` / `bn_sp`。最大语义变化:req 从一条跨所共享广播环,变成每所一条 SPSC 队列;策略在
> **生产侧**按 `ex`/`ac` 路由到对应所的队列(不再靠消费侧过滤)。

### 类型

| v1 | v2 |
|----|----|
| `ShmSlot` / `BookTickBoard` | `v2::BoardSlot` / `v2::Board` |
| `OrderEventEntry` + `PrivateEventEntry`(两套) | `v2::EventPayload` / `v2::EventEntry`(**统一**;`ev_kind` 区分 ack/fill/lifecycle) |
| `PrivateEventRing<...>`(MPMC 广播模板) | `v2::EventRing`(`BcastRing<EventEntry>`) |
| `PrivateReqRing`(SPMC) | `v2::ReqQueue`(`SpscQueue<PrivateReqFrame>`) |

> `gconf::priv::PrivateReqFrame`(64B 请求帧,含 place/modify/query union)**原样复用**——帧格式已够好,
> v2 只换队列机制。

## 4. 各组件改动

- **gconf**(公共契约库,v1/v2 并存):按版本号组织成对称的三棵子树(见
  [`../gconf/include/gconf/shm/README.md`](../gconf/include/gconf/shm/README.md)):
  - `common/`(版本中立):`req_frame.h`(`PrivateReqFrame`,两版共用)、`symbol_info.h`。
  - `v1/`(冻结,`gconf::shm::v1`):`board.h` / `order_event.h` / `private_event.h` / `req.h`
    (`PrivateReqRing`)/ `names.h` —— 保留供外部 v1 消费者。
  - `v2/`(当前,`gconf::shm::v2`):全套 v2 契约头。

  应用代码只 include `common/` + `v2/`。v1 子树无活跃消费者,但已 `-fsyntax-only` 验证仍可编译。
- **market / order / private / shm_init**(两所):去掉 `[shm].layout` 开关与所有 `if(use_v2){v2}else{v1}`
  分支,v2 无条件执行。`shm_init` 只建 4 个 v2 段/所(board / order_evt / priv_evt / req)并写 `SegHeader`;
  各 bin attach 时 `seg_validate`。binance order 在 v2 drain 中保留 Face-B(`-1003` weight-ban)限频守卫。
- **config**(两所):删 `shm_use_v2` 成员 + `[shm]` 段解析 + root 白名单里的 `"shm"`。
- **bridge**(`br::`):跨机中继迁到 v2——统一 `EventSnap`(order-ack 与 fill/lifecycle 同为 `EventPayload`)
  替代 v1 双快照;board 用 `BoardSlot`;环用 `BcastRing`/`SpscQueue`。Kind 枚举名(`board` /
  `private_event_ring` / `order_event_ring` / `req_ring`)、wire 协议、Go-Back-N ARQ **不变**(布局无关)。
  `bridge_smoke` 回环工具同步迁移。

## 5. 应用代码移除的东西(负向清单)

> 仅指**应用代码**;`gconf` 公共库的 v1 契约定义**保留不删**(见 §4)。

- v1 SHM 段(4 类 × 2 所 + 共享 req ring):`shm_init` 不再创建。
- 各 bin 不再 include v1 gconf 头、不再引用 `PrivateReqRing` / `BookTickBoard` / `*EventEntry` 等 v1 类型。
- `Config::shm_use_v2` + `[shm].layout` toml 键 + smoke 的 `--layout` CLI 开关。
- bin 里所有 v1/v2 双路分支与 v1 类型别名(`AckRing` / `PrivRing` / `shm` 命名空间别名等)。

应用代码净删除约 **470 行**(gconf 的 v1 头未动)。

## 6. 验证

- `xmake build -r`(全量干净重建)绿。
- `xmake test`:**11/11** 单测通过 —— 两所 config/util + bybit ws + binance ed25519 + v2 board/ring
  (`test_by_v2_board` / `test_by_v2_ring`)+ bridge wire/arq/config。
- `shm_init`(两所)实测创建 4 个 v2 段且尺寸正确:board 2176B、evt 65664B、req 65728B
  (= `SegHeader` + 独占 cache-line 游标 + entries)。
- LIVE 端到端 smoke(`*_integration_smoke`,需密钥/网络)已纯 v2:策略桩用统一 `Bus` 抽象走 v2 req 生产 +
  v2 事件消费。

## 7. 给后来者

- 改 v2 段字段:更新对应 `v2/*.h` 的 struct + `static_assert` + `schema_fnv(...)` 字符串(改了布局必须改
  哈希,否则 `seg_validate` 仅告警放行——见 spec §9.4)。
- 加新流/段:仿 `seg_header.h` 的 `SegKind` + `init_v2_header` 模式;attach 侧务必 `seg_validate`。
- 段命名仍带 `_v2` 后缀(v1 已无)——保留以免将来再次大改名;不是"还有 v1"的意思。
