# gconf — 跨进程 SHM 只读契约

gconf 是面向**所有消费者**(三件套 / bridge / 策略 / 异构语言绑定)的只读 SHM ABI 契约。
组织原则:**按"耦合对象 + 变更方式"分层,而不是按一个全局版本号**。契约里有**两条正交的演进轴**——
分清它们,就知道每个头该放哪、该怎么改。

## 两条演进轴

| 轴 | 包含 | 怎么变 | 体现为 |
|---|------|--------|--------|
| **A 传输 / 布局** | 字节如何排布 + 同步(seqlock、段头、环机制) | **硬断 + 并存**:新旧段是形状不同的内存,迁移期同时活、双写灰度 | `shm/vN/` 子目录 + `gconf::shm::vN` 命名空间 |
| **B 语义 / 领域 ABI** | enum 值、symbol 身份(GID/LID)、命令帧字段含义 | **append-only**:只在尾部追加、永不重编号;新旧靠兼容规则互通 | `domain/`(中立命名空间)+ `static_assert` 锚点 |

**为什么 B 不进 `vN/` 文件夹**:轴 A 必须并存(v1/v2 段同时存在),所以要按版本分目录;轴 B 走
append-only 单一演进 schema(像 SBE / protobuf),并存拷贝是错的——它的"版本"活在
`static_assert` 锚点(`BTCUSDT_GLOBAL_SYMBOL_ID==8`、各 enum 的 `Count` 终结项)+ 段头 `schema_hash` 里,不在目录名里。

> **"版本中立" ≠ "永不变"**:`domain/` 与命令帧会变,只是走 append-only 这条更慢、独立的轴。

## 目录

```
gconf/include/gconf/
  domain/            轴 B:语义 ABI(append-only;靠锚点 + schema_hash 治理)
    enums.h            ExType / AcType / OrderMode / OrderStatus / …(各带 Count 终结项)
    symbols.h          GID(全集,含 BTC/ETH)+ subset27 LID + Pack8 match(gconf::sym)
  shm/
    req_frame.h        PrivateReqFrame(策略→order 命令帧;v1/v2 共享,自带 schema)  gconf::priv
    README.md          (本文)
    v1/                轴 A:v1 布局(冻结)                                         gconf::shm::v1
      board · order_event · private_event · req · names
    v2/                轴 A:v2 布局(当前)                                         gconf::shm::v2
      seg_header.h       自描述段头 + schema_fnv + seg_check()→SegError(纯,零三方依赖)
      seqlock.h          正确前后沿 fence 的 seqlock
      board · event · req · names
```

## 可移植性铁律

- **整个 gconf 零三方依赖**:全部头只依赖标准库(`<cstdint>/<atomic>/<cstddef>/<bit>`…),**不引入
  spdlog 等任何日志/框架**。attach 校验出 `seg_check() → SegError`(纯函数,不打印);**怎么打日志是
  消费者(应用层)的事**——本仓的带日志 bool 包装在 `common/util.hpp`(`venue::seg_validate`),
  别的消费者用自己的 logger 包一层即可。契约不替任何特定消费者(包括本仓)做日志选型。
- **小端集群**:契约用本机序整数,假设全集群小端(x86-64 dev + aarch64-LE prod)。`seg_header.h` 有
  `static_assert(endian::native==little)` 编译期兜底;跨字节序对端写出的 magic 会被 `seg_check` 报
  `BadMagic`(运行期兜底)。跨字节序的 SHM / bridge 传输**不支持**。
- **布局自校验**:每段头带 `magic + layout_version + entry_size + capacity + schema_hash`;attach 时
  `seg_check` 逐字段比对。这让"文件夹"不再是版本的执行机制——**运行时段头**才是。关键字段 `offsetof` +
  `sizeof`/`alignof`/`is_trivially_copyable`/`is_always_lock_free` 全程 `static_assert` 锁死。

## 改契约前必读

- **改轴 A(布局)**:开新 `vN/` 子目录 + `gconf::shm::vN`,不动旧版;迁移期双写并存(像 v1→v2)。
- **改轴 B(domain / 命令帧)**:**只在尾部追加,永不重编号 / 不改既有字段含义**;新增 enum 项放
  `Count` 之前;改了帧布局就改 `schema_fnv` 描述串(让 attach 侧能发现漂移)。破坏 append-only =
  所有已 mmap 数据语义平移 = 跨版本静默错乱。
