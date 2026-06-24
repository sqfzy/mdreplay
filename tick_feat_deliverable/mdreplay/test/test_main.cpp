// test_main.cpp — 阶段 2 纯逻辑单元测试:scale / clock / merge。
// 极简断言式(无框架,同 cpp 工程 test_core 风格);任一失败返回非零。

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gconf/shm/v2/board.h>
#include <gconf/shm/v2/trade.h>

#include "core/clock.hpp"
#include "core/config.hpp"
#include "core/fixed.hpp"
#include "core/merge.hpp"
#include "core/record.hpp"
#include "input/csv.hpp"
#include "input/source.hpp"
#include "output/shm.hpp"

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

static void test_fixed() {
  using namespace mdreplay;
  std::uint32_t out[4];
  // 价对:公共 scale = 组内最大小数位
  {
    const std::string_view in[] = {"68.8", "68.85"};
    const auto sc = encode_group(in, out);
    CHECK(sc.has_value() && *sc == 2);
    CHECK(out[0] == 6880 && out[1] == 6885);  // 68.8→6880@s2, 68.85→6885
  }
  // 去尾零不计入精度
  {
    const std::string_view in[] = {"68.8400"};
    const auto sc = encode_group(in, out);
    CHECK(sc.has_value() && *sc == 2 && out[0] == 6884);
  }
  // 整数 → scale 0
  {
    const std::string_view in[] = {"69"};
    const auto sc = encode_group(in, out);
    CHECK(sc.has_value() && *sc == 0 && out[0] == 69);
  }
  // 小价高精度
  {
    const std::string_view in[] = {"0.00012345"};
    const auto sc = encode_group(in, out);
    CHECK(sc.has_value() && *sc == 8 && out[0] == 12345);
  }
  // 溢出自动降 scale:68000.12345678 在 s8 为 6.8e12>u32 → 降到 s4(6.8e8<u32)
  {
    const std::string_view in[] = {"68000.12345678"};
    const auto sc = encode_group(in, out);
    CHECK(sc.has_value() && *sc == 4 && out[0] == 680001234);
  }
  CHECK(!encode_group(std::array<std::string_view, 1>{"6a.5"}, out).has_value());  // 非法
  CHECK(!encode_group(std::array<std::string_view, 1>{"-1.0"}, out).has_value());  // 负
  CHECK(decimals_of("1.230") == 2);
  CHECK(decimals_of("5") == 0);
  CHECK(decimals_of("0.001") == 3);
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
  // happy:input.kind / realtime / dir / output 解析
  {
    const auto c = parse_config(toml::parse(R"(
      [input]
      dir = "d"
      kind = "trade"
      [replay]
      realtime = 0.5
      [output]
      format = "shm"
      shm = "/s"
    )"));
    CHECK(c.has_value());
    CHECK(c->realtime == 0.5);
    CHECK(c->dir == "d" && c->input_kind == "trade");
    CHECK(c->output.format == "shm" && c->output.shm == "/s");
    CHECK(c->start_ns == kNoStart && c->end_ns == kNoEnd);  // 空窗口 → 无界
  }
  // 文件输出 csv:需 path
  {
    const auto c = parse_config(toml::parse("[output]\nformat=\"csv\"\npath=\"out.csv\""));
    CHECK(c.has_value() && c->output.format == "csv" && c->output.path == "out.csv");
  }
  CHECK(!parse_config(toml::parse("[replay]\nrealtime=2.0\n[output]\nformat=\"shm\"\nshm=\"/s\"")).has_value());  // 坏 realtime
  CHECK(!parse_config(toml::parse("[output]\nformat=\"xxx\"\nshm=\"/s\"")).has_value());        // 未知 output.format
  CHECK(!parse_config(toml::parse("[output]\nformat=\"csv\"")).has_value());                    // csv 缺 path
  CHECK(!parse_config(toml::parse("[output]\nformat=\"shm\"\nshm=\"/s\"\n[input]\nkind=\"xxx\"")).has_value());  // 坏 kind
  CHECK(!parse_config(toml::parse("[input]\ndir=\"d\"")).has_value());                          // 无 output
  // datetime 窗口 → 整秒 ns
  {
    const auto c = parse_config(toml::parse(R"(
      [replay]
      start = "2026-06-23 00:00:00"
      [output]
      format = "shm"
      shm = "/s"
    )"));
    CHECK(c.has_value());
    CHECK(c->start_ns != kNoStart && c->start_ns > 0 && c->start_ns % 1'000'000'000LL == 0);
  }
  CHECK(!parse_config(toml::parse("[replay]\nstart=\"bad\"\n[output]\nformat=\"shm\"\nshm=\"/s\"")).has_value());  // 坏 datetime
}

static void test_e2e() {
  using namespace mdreplay;
  namespace v2 = gconf::shm::v2;

  // book sink → Board 读回
  {
    auto     board = std::make_unique<v2::Board>();
    BookSink sink(board.get());
    Record   r;
    r.kind = Kind::Book; r.ts_ns = 1000; r.gid = 21;  // SOLUSDT
    r.price_scale = 2; r.qty_scale = 2;
    r.bid_px = 6884; r.bid_qty = 190667; r.ask_px = 6885; r.ask_qty = 126072;
    CHECK(sink.write(r).has_value());
    v2::BoardSlot out;
    CHECK(board->slot[21].read(out));
    CHECK(out.exch_ns == 1000 && out.bid_px == 6884 && out.ask_px == 6885 && out.price_scale == 2);
  }
  // trade sink → Ring drain
  {
    auto      ring = std::make_unique<v2::TradeRing>();
    TradeSink sink(ring.get());
    Record    r;
    r.kind = Kind::Trade; r.ts_ns = 2000; r.gid = 21; r.side = 1;
    r.price_scale = 2; r.qty_scale = 2; r.px = 6884; r.qty = 2902;
    CHECK(sink.write(r).has_value());
    std::uint64_t tail = 0;
    int           n = 0;
    std::int64_t  ts = 0;
    std::uint32_t px = 0;
    std::uint8_t  side = 9;
    ring->drain(tail, [&](const v2::TradePayload& p) { ++n; ts = p.exch_ns; px = p.px; side = p.side; });
    CHECK(n == 1 && ts == 2000 && px == 6884 && side == 1);
  }
  // csv 解析(临时文件):编码正确 + 未知符号跳过
  {
    namespace fs = std::filesystem;
    const std::string dir = "test_tmp";
    fs::create_directories(dir);
    {
      std::ofstream o(dir + "/x_SOLUSDT.book.csv");
      o << "ts,symbol,bid_px,bid_qty,ask_px,ask_qty\n";
      o << "1000,SOLUSDT,68.84,1906.67,68.85,1260.72\n";
      o << "2000,BADSYM,1,2,3,4\n";  // 未知符号 → 跳
    }
    std::size_t skipped = 0;
    const auto  src = load_csv_source(dir + "/x_SOLUSDT.book.csv", Kind::Book, skipped);
    CHECK(src.has_value());
    CHECK(skipped == 1);
    const Record* r = (*src)->peek();
    CHECK(r && r->ts_ns == 1000 && r->gid == 21 && r->price_scale == 2);
    CHECK(r->bid_px == 6884 && r->ask_px == 6885 && r->bid_qty == 190667 && r->ask_qty == 126072);
    fs::remove_all(dir);
  }
}

int main() {
  test_fixed();
  test_clock();
  test_merge();
  test_config();
  test_e2e();
  if (g_fail == 0) std::printf("all tests passed\n");
  else std::printf("%d checks FAILED\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
