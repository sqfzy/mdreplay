# gconf — 跨进程 SHM 只读契约

gconf 是面向**所有消费者**(三件套 / bridge / 策略 / 异构语言绑定)的只读 SHM ABI 契约。
本文讲**为什么它这么严**;布局 / 演进轴细节见 [`shm/README.md`](shm/README.md),v2 版本规约见
[`v2-shm-layout-spec.md`](../../docs/v2-shm-layout-spec.md)。

> 改契约前先读完这页。它是给谁、为什么这么克制,决定了你能怎么改。

## 根原则:契约 = 关于字节的永久承诺

producer 与 consumer **独立编译、独立部署、独立崩溃重启**,运行时**无法协商**——它们唯一的共识渠道
就是这片**字节 + 段头**:没有共享运行时、没有握手电话、没有第二次确认。下面每条原则,都是从
"只能靠字节对齐认知"这一点推出来的。

## 设计原则

**1. 单一事实来源(契约即真理)**
字节的定义只有一处,两端都从同一个头**派生**,绝不手抄第二份布局。这是校验能成立的前提——两端
`schema_hash` 一致,是因为同一个 `constexpr`。红线"不把别的 venue 名塞进 gconf"也属此条:一旦有
第二处定义,校验只是在比对两个谎言。

**2. 纯被动数据:零行为、零依赖、零裸指针**
契约只描述**字节 + 最小访问原语**,不含任何策略(不打日志、不退出、不下单、不选 logger)。整库零三方
依赖(只标准库)。"可移植"是**后果**,"契约不替任何消费者做行为决策"才是**原因**——所以带 spdlog 的
`seg_validate` 在应用层(`common/util.hpp`),契约只出纯 `seg_check() → SegError`。SHM 各进程映射地址
不同,故用**偏移不用指针**。

**3. 总是校验(编译期 + attach 期双兜底)**
"无法运行时协商"在两个时刻兜底:**编译期** `static_assert`(`sizeof`/`alignof`/`offsetof`/`is_trivially_copyable`/
`is_always_lock_free`/GID 锚点/枚举 `Count`);**attach 期** 段头自描述(`magic`/`layout_version`/`entry_size`/
`capacity`/`schema_hash`,经 `seg_check` 逐字段比对)。文件夹不是版本机制,**运行时段头**才是。

**4. 布局确定性:绝不让编译器替你决定字节**
定宽类型、显式 `alignas`、显式 `_pad`(对齐填充)/`_rsvd`(预留增长位)、定字节序。这是校验的孪生:
校验在运行时**查**,确定性在设计时**让字节有个稳定的东西可查**。靠手算 padding 凑 size 而不锁 `offsetof`,
就是把布局交给编译器——违背此条。

**5. 内存序 / 并发协议属于契约本身**
跨进程 lock-free 同步是 SHM 的全部意义,也最容易悄悄错。**谁是单写者、seqlock 前后沿 fence、
release/acquire 责任**必须由契约规定并写进头注(见 `shm/v2/seqlock.h`),不能指望消费者猜对——
弱序生产主机(aarch64)上一条缺失的 acquire fence 就会撕裂 BBO。

**6. 检测而不决策;永不静默丢 / 永不静默截断**
契约负责把异常**变成可见的值**(`seg_check → SegError`、`BcastRing::drain` 返回 lapped 计数、
`SpscQueue::try_push` 返回 `false`、`err_code` 加宽到不截断),但**不替应用决定怎么办**。这同时解释了
"为什么 `seg_validate` 搬去应用层"和"为什么 req 队列要背压而非覆盖"——同一原则的两面。

**7. 最小且稳定的表面(YAGNI 是硬约束)**
跨进程结构体里**每个字段都是永久的**。不为假想需求加字段;看着像契约却没接线的死代码要删。表面越小,
要永久兼容的东西越少。扩展靠**预留位 + append-only**,不靠投机加字段。

**8. 受治理的演进(兼容 / 可移植 / 扩展优先)**
两条正交的轴(详见 [`shm/README.md`](shm/README.md)):**传输/布局**走硬断 + 并存(`shm/vN/` 子目录),
**语义/领域 ABI**(`domain/`、命令帧)走 append-only(只追加、永不重编号)。配 `_rsvd` 预留位作同版本
内增长、`schema_hash` 探测字段漂移、小端集群不变量(`static_assert(endian::native==little)` + `BadMagic`
运行期兜底)。

**9. 性能语义即布局决策(HFT 特有)**
cache line 对齐、head/tail 分行防 false sharing、64B entry、2 的幂 capacity,以及**每条流选 latest-wins
还是可靠背压**——都是写进布局的**契约级**决定,不是某个 bin 的优化。换布局会改这些,故归契约管。

---

**一句话**:gconf 克制(纯数据、零依赖、最小表面)是为了能**严**(总是校验、内存序写死、永不静默丢),
而严是因为两个进程**只能靠字节互信**。改它之前,先确认你的改动落在哪条轴、有没有破坏 append-only、
有没有对应的 `static_assert`/`schema_hash` 兜底。
