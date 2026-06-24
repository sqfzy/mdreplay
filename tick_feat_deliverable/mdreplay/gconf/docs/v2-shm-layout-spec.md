# gconf v2 SHM 布局规约

> 状态:**已实现**——本仓应用代码(三件套 + bridge)的唯一布局;`gconf` 公共库 v1/v2 契约定义并存
> (见 [`v2-migration.md`](v2-migration.md))。规则后括注其范式来源。结构体字段为**示意**,确切布局以
> 代码中的 `static_assert(sizeof==64)` 等编译期断言为准(`gconf/include/gconf/shm/v2/`)。§9 的开放决策
> 均已按推荐默认落地。

## 0. 目标 / 非目标

- **目标**:三条流按各自语义重构(market=latest-wins,events=广播,req=可靠);消息格式统一信封 +
  版本化;内存序做对(修 v1 #10);布局自校验、可演进。
- **非目标**:不改 WS/业务逻辑;不引入跨进程指针/容器;不为假想需求加字段(YAGNI)。

## 1. 布局铁律(每条都用 static_assert / 段头强制)

| # | 规则 | 来源范式 |
|---|------|---------|
| R1 | **每段一个版本化头**(magic + layout_version + entry_size + capacity + kind),attach 时逐字段比对、不符拒启动 | SBE message header、Aeron log-buffer 元数据 |
| R2 | **零裸指针、零 std:: 容器**:只 POD + 定长数组 + 索引/偏移(SHM 各进程映射地址不同) | Boost.Interprocess `offset_ptr` |
| R3 | `static_assert(is_trivially_copyable)`、`sizeof(Entry)==64`、`alignof==64`、关键字段 `offsetof` 锁定 | Drepper、SBE 固定偏移 |
| R4 | **entry = 1 cache line(64B)**;热点原子各占一行,producer head 与 consumer tail **不同行**(防 false sharing) | Drepper、LMAX Disruptor padding、Folly |
| R5 | **capacity 为 2 的幂**,用 `& MASK` 取下标 | Disruptor、DPDK rte_ring |
| R6 | **内存序契约写进头注**,seqlock 用**唯一**正确实现(§4) | Linux `memory-barriers.txt` / `seqlock.h`、McKenney |
| R7 | 段整体 `alignas(4096)`(页对齐) | 通用 mmap 实践 |
| R8 | **enum 显式底层类型 + 固定值**(`enum class : uint8_t`) | SBE |

## 2. 公共段头(64B,独占一行)

```cpp
enum class SegKind : uint16_t { Board = 1, BcastRing = 2, SpscQueue = 3 };

struct alignas(64) SegHeader {
  uint32_t magic;          // 0x47434632 = "GCF2"
  uint16_t layout_version; // = 2
  SegKind  kind;
  uint32_t entry_size;     // sizeof(Entry)
  uint32_t capacity;       // 环/队列:2 的幂;board:N_GLOBAL_SYMBOL_IDS
  uint64_t schema_hash;    // 字段布局的编译期 FNV(强校验,§9-4)
  uint64_t created_ns;     // shm_init 建段时刻(诊断)
  uint8_t  _rsvd[32];
};
static_assert(sizeof(SegHeader) == 64);
```
**attach 校验**:`magic / layout_version / kind / entry_size / capacity` 与本进程编译期常量逐一比对,
任一不符 → 拒启动(把 v1 仅比 `sizeof(Seg)` 升级为结构化自校验)。`schema_hash` 见 §9-4。

## 3. 公共信息封(seqlock 类 entry 前缀,16B)

```cpp
struct Envelope {                 // board / bcast_ring 的 entry 前缀
  std::atomic<uint64_t> seq;      // seqlock: (slot<<1)|writing;0=从未写;天然单调 → 消费者查空洞
  int64_t exch_ns;                // 来源(交易所)时间戳,全精度 ns(废弃 v1 的 frac 增量压缩)
};
```
- **时间戳全精度 int64**:丢掉 v1 `*_frac`(uint32 增量 >4.29s 溢出的过早优化)。延迟统计用消费侧
  `recv_ns - exch_ns` 自算,或在 entry 里放具名 `int32 *_lat_ns`(有符号、可 clamp)。
- **版本只在段头**,不进每条 entry(省热路径字节)。
- **stream seq 复用 seqlock 的 slot**,不另设字段。
- **SPSC 队列 entry 无 `Envelope.seq`**:其同步靠 head/tail 的 release/acquire(§5.3),不需 per-entry seqlock。

## 4. 内存序 / seqlock(唯一正确实现,修 v1 #10)

```cpp
// ── writer(in-place 覆盖:board / bcast ring)──
seq.store(s | 1, relaxed);                     // 奇:写入中
std::atomic_thread_fence(release);             // 前沿 StoreStore(v1 缺这条 → aarch64 撕裂)
... payload 普通写 ...
seq.store(s + 2, release);                     // 偶:可读(后沿 release,payload 先于此可见)

// ── reader ──
s1 = seq.load(acquire); if (s1 & 1) retry;
... payload 普通读 ...
std::atomic_thread_fence(acquire);             // 后沿:payload 读不越过第二次 seq load
s2 = seq.load(acquire);  ok = (s1 == s2);
```
- **board / bcast_ring 共用这一份**(头注写明 producer/consumer 各自 fence 责任)。
- **SPSC** 不用 seqlock:producer 写 payload → `head.store(release)`;consumer `head.load(acquire)` → 读 payload。

## 5. 三种段(kind)

### 5.1 Board(market,latest-wins)
- 布局:`[SegHeader][BoardSlot[N_GLOBAL_SYMBOL_IDS]]`,按 GID 索引,每槽独立 seqlock 覆盖最新值。
- 语义:**丢旧的本来就对**(只要最新 BBO);消费者用 `update_id` 跳变感知"跳过了 N 个中间 tick"(对 BBO 交易无害)。
- 只相对 v1 改:seqlock fence(§4)+ 信封统一 + 时间戳全精度。
```cpp
struct alignas(64) BoardSlot {           // 字段示意,sizeof 锁 64
  Envelope env;                          // seq + exch_ns
  uint64_t update_id;                    // venue update id
  uint32_t bid_px, bid_qty, ask_px, ask_qty;   // ×10^scale
  int32_t  kernel_lat_ns, recv_lat_ns, write_lat_ns;   // 相对 exch_ns 的有符号延迟
  uint16_t global_symbol_id;                          // GID(board N_GLOBAL_SYMBOL_IDS 索引)
  uint8_t  price_scale, qty_scale, path_idx;
  uint8_t  _rsvd[/* 补到 64 */];
};
static_assert(sizeof(BoardSlot) == 64);
```

### 5.2 BcastRing(events:ack / fill / lifecycle)
- 布局:`[SegHeader][ProducerCursor 64B][Entry[capacity]]`。**消费者 tail 在 SHM 外**(广播,多消费者,
  不背压——producer 是 venue RX,不能阻塞)。
- producer:`slot = head.fetch_add(1)` 抢格 → seqlock 写。
- **消费者契约(强制)**:`head - tail > capacity` ⇒ **被绕圈、丢了 `head-tail-capacity` 条** → **告警 + 计数 + `tail = head - capacity`**;否则 seqlock 未就绪 ⇒ **等**(别 advance tail)。**不丢成交靠 resync(D1),不是无限加大环。**(Vyukov per-cell seq 模式 / Disruptor)
- 格式改进:**统一 OrderEvent 与 PrivateEvent 为一种 `EventEntry`**,用 `ev_kind` 区分(同布局,两个段)。
```cpp
struct alignas(64) ProducerCursor { std::atomic<uint64_t> head; uint8_t _pad[56]; };
enum class EvKind : uint8_t { Ack=1, Fill=2, Lifecycle=3, Count };
struct alignas(64) EventEntry {          // 字段示意
  Envelope env;
  uint64_t client_order_id;
  uint32_t filled_qty, avg_px, last_px, last_qty;
  uint16_t local_symbol_id; EvKind ev_kind; uint8_t status;   // local_symbol_id = subset27 LID(非 GID)
  uint32_t err_code; uint8_t resp_type; uint8_t path_idx;   // err_code u32:容 retCode>u16
  uint8_t  price_scale, qty_scale; uint8_t _rsvd[/* 补到 64 */];
};
static_assert(sizeof(EventEntry) == 64);
```

### 5.3 SpscQueue(req:命令,可靠)
- 布局:`[SegHeader][ProducerCursor 64B][ConsumerCursor 64B][ReqFrame[capacity]]`。
  **consumer tail 发布回 SHM**(独占一行)→ producer 能看见满 → **背压**。
- **per-venue 各一条 SPSC**(bybit / binance 分开),取代 v1"共享 MPMC + 按 ex/ac 过滤"。
- producer(策略)入队前:`if (head - tail >= capacity)` ⇒ **满**:按 §9-2 处理(推荐:**立即本地 REJECTED**,
  不阻塞热路径);**绝不静默覆盖** → 根除"静默丢单"。
- 同步:producer 写 payload → `head.store(release)`;consumer `head.load(acquire)` 读 → 处理 → `tail.store(release)`。(io_uring SQ/CQ 范式)
```cpp
struct alignas(64) ConsumerCursor { std::atomic<uint64_t> tail; uint8_t _pad[56]; };
struct alignas(64) ReqFrame {            // 无 atomic seq:head/tail 定序
  int64_t  enq_ns; uint64_t client_order_id;
  uint8_t  type; uint8_t target_path; uint16_t local_symbol_id;
  /* union { place{px_1e8,qty_1e8,mode,...}; cancel{...} } */
  uint8_t  _rsvd[/* 补到 64 */];
};
static_assert(sizeof(ReqFrame) == 64);
```

## 6. 生产者 / 消费者职责契约

- **board**:writer 守 §4 写序;reader 守 §4 读序 + 校验 `s1==s2` 重试。
- **bcast_ring**:producer fetch_add + seqlock;**consumer 必须做 §5.2 的 lapped 检测**(否则 B3 静默丢)。
- **spsc_queue**:producer **必须**先查满再写(背压);consumer 处理后**必须**发布 tail(否则 producer 永远看满)。
- 所有 attach 方:**必须**做 §2 段头校验。

## 7. 版本化与迁移(已完成)

- **段名带版本**:`/shm_*_v2_book` / `_v2_order_evt` / `_v2_priv_evt` / `_v2_req`。
- **迁移路径**(历史):迁移期先以 `[shm].layout = v1|v2` 开关让 v2 与 v1 并存、逐流验证
  (market board → events → req);验证通过后,**应用代码彻底切到 v2**——不再创建 v1 段、不再用
  v1 类型/开关,v2 成为应用代码唯一布局(`gconf` 公共库仍保留 v1 契约定义)。完整迁移说明见
  [`v2-migration.md`](v2-migration.md)。
- **bridge 已同步**:跨机中继的快照(统一 `EventSnap` + `BoardSnap`)与 wire 跟随 v2 段。
- 段头 `SegHeader` 自校验 + 编译期 `schema_hash` 让未来的字段演进可被 attach 侧立即发现。

## 8. 编译期不变量(实现时落成 static_assert)

- `sizeof(SegHeader)==64`;各 `sizeof(Entry)==64`、`alignof==64`。
- `is_trivially_copyable_v<Entry>`、`is_standard_layout_v<Entry>`。
- 关键字段 `offsetof` 锁定(信封 seq @0、exch_ns @8…)。
- `(capacity & (capacity-1))==0`(2 的幂)。
- `ProducerCursor` / `ConsumerCursor` 各独占 64B(`sizeof==64`)。

## 9. 决策(均已落地)

1. **events 消费者数**:**多** → 用 BcastRing(策略 + 落盘 + 风控 + 监控 + bridge 中继)。
2. **req 满语义**:**立即本地 REJECTED,不阻塞**(`try_push` 返回 false,不覆盖;HFT 不在热路径 block)。
3. **段命名**:**新段名带 `_v2`**(沿用至今;v1 移除后仍保留 `_v2` 以免再次大改名)。
4. **schema_hash 强校验**:**带,attach 时不符先告警**(`seg_validate`;防字段错位)。
5. **首个迁移的流**:market board 先行(已完成,后续 events / req 跟进)。

---
*范式来源:SBE(版本头/兼容)、Aeron(段元数据)、LMAX Disruptor(single-writer/padding)、Vyukov 1024cores
MPMC(per-cell seq)、io_uring(SPSC head/tail 背压)、Linux seqlock + memory-barriers.txt(内存序)、
Drepper《What Every Programmer Should Know About Memory》(cache line)、Boost.Interprocess(offset_ptr)。*
