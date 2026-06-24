// test_main.cpp — 阶段 2 纯逻辑单元测试:scale / clock / merge。
// 极简断言式(无框架,同 cpp 工程 test_core 风格);任一失败返回非零。

#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

#include "clock.hpp"
#include "config.hpp"
#include "merge.hpp"
#include "output/scale.hpp"
#include "record.hpp"

static int g_fail = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                \
      ++g_fail;                                                                  \
    }                                                                            \
  } while (0)

// ── 测试用源:从预置 ts 序列产出 Record ──────────────────────────────────────
namespace {
struct VectorSource : mdreplay::Source {
  std::vector<mdreplay::Record> rows;
  std::size_t                   i = 0;
  const mdreplay::Record*       peek() override { return i < rows.size() ? &rows[i] : nullptr; }
  void                          advance() override { ++i; }
};

std::unique_ptr<mdreplay::Source> make_src(std::vector<std::int64_t> ts, std::uint16_t gid) {
  auto s = std::make_unique<VectorSource>();
  for (const auto t : ts) {
    mdreplay::Record r;
    r.ts_ns = t;
    r.gid   = gid;
    s->rows.push_back(r);
  }
  return s;
}
}  // namespace

static void test_scale() {
  using namespace mdreplay;
  CHECK(to_scaled(68.84, 2).value() == 6884);            // happy
  CHECK(to_scaled(0.0, 2).value() == 0);                 // 零
  CHECK(to_scaled(static_cast<double>(kU32Max), 0).value() == kU32Max);  // u32 边界
  CHECK(!to_scaled(4294967296.0, 0).has_value());        // 越界 +1
  CHECK(!to_scaled(5e7, 2).has_value());                 // 5e7×100=5e9 > u32
  CHECK(!to_scaled(-1.0, 2).has_value());                // 负
  CHECK(!to_scaled(std::numeric_limits<double>::infinity(), 2).has_value());  // inf
}

static void test_clock() {
  using namespace mdreplay;
  Clock c0(0.0);
  CHECK(c0.offset_ns(123) == 0);  // realtime 0 + 未锚定 → 0

  Clock c(1.0);
  CHECK(!c.anchored());
  c.pace_to(1000);  // realtime=1 但首拍只锚定、立即返回
  CHECK(c.anchored());
  CHECK(c.offset_ns(1000) == 0);     // 锚点本身偏移 0
  CHECK(c.offset_ns(2000) == 1000);  // 绝对量:(2000-1000)×1.0
  CHECK(c.offset_ns(5000) == 4000);  // 不累积漂移,仍按绝对差

  Clock h(0.5);
  h.pace_to(0);
  CHECK(h.offset_ns(1000) == 500);  // 0.5× 延迟 → 2× 实时
}

static void test_merge() {
  using namespace mdreplay;
  // 两源交错 → 全局有序
  {
    std::vector<std::unique_ptr<Source>> s;
    s.push_back(make_src({100, 103, 105}, 1));
    s.push_back(make_src({101, 102, 106}, 2));
    Merger m(std::move(s), kNoStart, kNoEnd);
    std::vector<std::int64_t> out;
    while (auto r = m.next()) out.push_back(r->ts_ns);
    CHECK((out == std::vector<std::int64_t>{100, 101, 102, 103, 105, 106}));
  }
  // 同 ts → 源序 tiebreak(src0 先于 src1)
  {
    std::vector<std::unique_ptr<Source>> s;
    s.push_back(make_src({100, 200}, 10));  // src0
    s.push_back(make_src({100, 200}, 20));  // src1
    Merger m(std::move(s), kNoStart, kNoEnd);
    std::vector<std::uint16_t> gids;
    while (auto r = m.next()) gids.push_back(r->gid);
    CHECK((gids == std::vector<std::uint16_t>{10, 20, 10, 20}));
  }
  // 时间窗闭区间 [200,300]
  {
    std::vector<std::unique_ptr<Source>> s;
    s.push_back(make_src({100, 200, 300, 400}, 1));
    Merger m(std::move(s), 200, 300);
    std::vector<std::int64_t> w;
    while (auto r = m.next()) w.push_back(r->ts_ns);
    CHECK((w == std::vector<std::int64_t>{200, 300}));
  }
  // 窗口空集(start 高于全部)
  {
    std::vector<std::unique_ptr<Source>> s;
    s.push_back(make_src({100, 200}, 1));
    Merger m(std::move(s), 500, kNoEnd);
    CHECK(!m.next().has_value());
  }
}

static void test_config() {
  using namespace mdreplay;
  // happy:realtime / dir / outputs 解析
  {
    const auto c = parse_config(toml::parse(R"(
      [input]
      dir = "d"
      [replay]
      realtime = 0.5
      [[output]]
      format = "book"
      shm = "/s"
      create = true
    )"));
    CHECK(c.has_value());
    CHECK(c->realtime == 0.5);
    CHECK(c->dir == "d");
    CHECK(c->outputs.size() == 1);
    CHECK(c->start_ns == kNoStart && c->end_ns == kNoEnd);  // 空窗口 → 无界
  }
  // 坏 realtime(>1)→ ConfigInvalid
  CHECK(!parse_config(toml::parse(
            "[replay]\nrealtime=2.0\n[[output]]\nformat=\"book\"\nshm=\"/s\"")).has_value());
  // 未知 format → ConfigInvalid
  CHECK(!parse_config(toml::parse("[[output]]\nformat=\"xxx\"\nshm=\"/s\"")).has_value());
  // 空 outputs → ConfigInvalid
  CHECK(!parse_config(toml::parse("[input]\ndir=\"d\"")).has_value());
  // datetime 窗口 → 整秒 ns(不硬编码具体 epoch,验证落到 ns 量级)
  {
    const auto c = parse_config(toml::parse(R"(
      [replay]
      start = "2026-06-23 00:00:00"
      [[output]]
      format = "trade"
      shm = "/s"
    )"));
    CHECK(c.has_value());
    CHECK(c->start_ns != kNoStart && c->start_ns > 0);
    CHECK(c->start_ns % 1'000'000'000LL == 0);  // 整秒 → ns
  }
  // 坏 datetime → ConfigInvalid
  CHECK(!parse_config(toml::parse("[replay]\nstart=\"not-a-date\"\n[[output]]\nformat=\"book\"\nshm=\"/s\"")).has_value());
}

int main() {
  test_scale();
  test_clock();
  test_merge();
  test_config();
  if (g_fail == 0) std::printf("all tests passed\n");
  else std::printf("%d checks FAILED\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
