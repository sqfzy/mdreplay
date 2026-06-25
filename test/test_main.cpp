// test_main.cpp — 阶段 2 纯逻辑单元测试:scale / clock / merge。
// 极简断言式(无框架,同 cpp 工程 test_core 风格);任一失败返回非零。

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
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
#include "core/signal.hpp"
#include "core/fixed.hpp"
#include "core/merge.hpp"
#include "core/record.hpp"
#include "input/csv.hpp"
#include "input/json.hpp"
#include "input/source.hpp"
#include "output/csv.hpp"
#include "output/json.hpp"
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

// 流式源:skip 在消费(advance)期才归账,断言总数前先消费到耗尽。
void drain(mdreplay::Source& s) { while (s.peek()) s.advance(); }
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
  // 超长整数部分:不得 uint64 回绕静默产错值,须判 ScaleOverflow(不 fits)
  CHECK(!encode_group(std::array<std::string_view, 1>{"99999999999999999999999"}, out).has_value());
  CHECK(decimals_of("1.230") == 2);
  CHECK(decimals_of("5") == 0);
  CHECK(decimals_of("0.001") == 3);
}

static void test_clock() {
  using namespace mdreplay;
  Clock c0(0.0);
  CHECK(c0.offset_ns(123) == 0);  // realtime 0 + 未锚定 → 0

  std::atomic<bool> never{false};
  Clock c(1.0);
  CHECK(!c.anchored());
  c.pace_to(1000, never);  // realtime=1 但首拍只锚定、立即返回
  CHECK(c.anchored());
  CHECK(c.offset_ns(1000) == 0);     // 锚点本身偏移 0
  CHECK(c.offset_ns(2000) == 1000);  // 绝对量:(2000-1000)×1.0
  CHECK(c.offset_ns(5000) == 4000);  // 不累积漂移,仍按绝对差

  Clock h(0.5);
  h.pace_to(0, never);
  CHECK(h.offset_ns(1000) == 500);  // 0.5× 延迟 → 2× 实时
}

// 优雅退出:信号→标志 + pacing 可被 stop 立即打断(否则长间隔下 Ctrl-C 要等满间隔)。
static void test_signal() {
  using namespace mdreplay;
  using namespace std::chrono;

  g_stop_requested.store(false);
  install_signal_handlers();
  std::raise(SIGINT);   // 已装处理器 → 置标志而非终止进程
  CHECK(stop_requested());
  g_stop_requested.store(false);
  std::raise(SIGTERM);
  CHECK(stop_requested());

  // stop=true 时,pace_to 对"很久以后"的事件应立即返回(不睡满)。
  std::atomic<bool> stop{true};
  Clock             c(1.0);
  c.pace_to(0, stop);                       // 锚定
  const auto t0 = steady_clock::now();
  c.pace_to(10'000'000'000, stop);          // 10s 后的事件,但 stop=true → 立即返回
  CHECK(steady_clock::now() - t0 < milliseconds(200));

  g_stop_requested.store(false);            // 复位,免污染后续测试
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
      path = "/s"
    )"));
    CHECK(c.has_value());
    CHECK(c->realtime == 0.5);
    CHECK(c->dir == "d" && c->input_kind == "trade");
    CHECK(c->output.format == "shm" && c->output.path == "/s");
    CHECK(c->start_ns == kNoStart && c->end_ns == kNoEnd);  // 空窗口 → 无界
  }
  // 文件输出 csv:需 path
  {
    const auto c = parse_config(toml::parse("[output]\nformat=\"csv\"\npath=\"out.csv\""));
    CHECK(c.has_value() && c->output.format == "csv" && c->output.path == "out.csv");
  }
  CHECK(!parse_config(toml::parse("[replay]\nrealtime=2.0\n[output]\nformat=\"shm\"\npath=\"/s\"")).has_value());  // 坏 realtime
  CHECK(!parse_config(toml::parse("[output]\nformat=\"xxx\"\npath=\"/s\"")).has_value());        // 未知 output.format
  CHECK(!parse_config(toml::parse("[output]\nformat=\"csv\"")).has_value());                    // csv 缺 path
  CHECK(!parse_config(toml::parse("[output]\nformat=\"shm\"\npath=\"/s\"\n[input]\nkind=\"xxx\"")).has_value());  // 坏 kind
  CHECK(!parse_config(toml::parse("[input]\ndir=\"d\"")).has_value());                          // 无 output
  // datetime 窗口 → 整秒 ns
  {
    const auto c = parse_config(toml::parse(R"(
      [replay]
      start = "2026-06-23 00:00:00"
      [output]
      format = "shm"
      path = "/s"
    )"));
    CHECK(c.has_value());
    CHECK(c->start_ns != kNoStart && c->start_ns > 0 && c->start_ns % 1'000'000'000LL == 0);
  }
  CHECK(!parse_config(toml::parse("[replay]\nstart=\"bad\"\n[output]\nformat=\"shm\"\npath=\"/s\"")).has_value());  // 坏 datetime
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
    r.bid_px[0] = 6884; r.bid_qty[0] = 190667; r.ask_px[0] = 6885; r.ask_qty[0] = 126072;
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
    SkipStats   skips;
    const auto  src = load_csv_source(dir + "/x_SOLUSDT.book.csv", Kind::Book, skips);
    CHECK(src.has_value());
    const Record* r = (*src)->peek();
    CHECK(r && r->ts_ns == 1000 && r->gid == 21 && r->price_scale == 2 && r->depth == 1);
    CHECK(r->bid_px[0] == 6884 && r->ask_px[0] == 6885 && r->bid_qty[0] == 190667 && r->ask_qty[0] == 126072);
    if (src.has_value()) drain(**src);  // 消费余下 → BADSYM 计入 skip
    CHECK(skips.total() == 1 && skips.count(SkipReason::UnknownSymbol) == 1);  // BADSYM 分类正确
    fs::remove_all(dir);
  }
}

static void test_file_io() {
  using namespace mdreplay;
  namespace fs = std::filesystem;
  const std::string dir = "test_tmp_io";
  fs::create_directories(dir);

  Record b;
  b.kind = Kind::Book; b.ts_ns = 1000; b.gid = 21; b.price_scale = 2; b.qty_scale = 2;
  b.bid_px[0] = 6884; b.bid_qty[0] = 190667; b.ask_px[0] = 6885; b.ask_qty[0] = 126072;
  Record t;
  t.kind = Kind::Trade; t.ts_ns = 2000; t.gid = 21; t.side = 1; t.price_scale = 2; t.qty_scale = 2;
  t.px = 6884; t.qty = 2902;

  // CSV 文件输出 → 读回(book)。sink 作用域结束即 flush+close,再读回。
  {
    const std::string p = dir + "/x.csv";
    { const auto sink = CsvSink::open(p, Kind::Book, 1);
      CHECK(sink.has_value() && (*sink)->write(b).has_value()); }
    SkipStats   sk;
    const auto  src = load_csv_source(p, Kind::Book, sk);
    CHECK(src.has_value() && sk.total() == 0);
    const Record* r = src.has_value() ? (*src)->peek() : nullptr;
    CHECK(r && r->ts_ns == 1000 && r->gid == 21 && r->price_scale == 2);
    CHECK(r && r->bid_px[0] == 6884 && r->ask_px[0] == 6885 && r->bid_qty[0] == 190667 && r->ask_qty[0] == 126072);
  }
  // JSON 文件输出 → 读回(book)
  {
    const std::string p = dir + "/x.book.json";
    { const auto sink = JsonSink::open(p, Kind::Book);
      CHECK(sink.has_value() && (*sink)->write(b).has_value()); }
    SkipStats   sk;
    const auto  src = load_json_source(p, Kind::Book, sk);
    CHECK(src.has_value() && sk.total() == 0);
    const Record* r = src.has_value() ? (*src)->peek() : nullptr;
    CHECK(r && r->ts_ns == 1000 && r->gid == 21 && r->bid_px[0] == 6884 && r->ask_px[0] == 6885);
  }
  // JSON 文件输出 → 读回(trade)
  {
    const std::string p = dir + "/x.trade.json";
    { const auto sink = JsonSink::open(p, Kind::Trade);
      CHECK(sink.has_value() && (*sink)->write(t).has_value()); }
    SkipStats   sk;
    const auto  src = load_json_source(p, Kind::Trade, sk);
    CHECK(src.has_value() && sk.total() == 0);
    const Record* r = src.has_value() ? (*src)->peek() : nullptr;
    CHECK(r && r->ts_ns == 2000 && r->gid == 21 && r->side == 1 && r->px == 6884 && r->qty == 2902);
  }
  fs::remove_all(dir);
}

// CSV 解析委托 csv-parser 后的新能力:带引号/转义字段正确解析(旧手写 split 会把引号留在值里 →
// fixed.hpp 拒为非数字 → 整行静默跳)。同时验证列数不符的 ragged 行仍计数跳过。
static void test_csv_quoting() {
  using namespace mdreplay;
  namespace fs = std::filesystem;
  const std::string dir = "test_tmp_quote";
  fs::create_directories(dir);
  const std::string p = dir + "/q.trade.csv";
  {
    std::ofstream f(p);
    f << "ts,symbol,side,px,qty\n";
    f << "100,\"SOLUSDT\",0,\"68.84\",\"1906.67\"\n";  // 全字段带引号
    f << "200,SOLUSDT,1,69.00,5\n";                     // 普通行
    f << "300,SOLUSDT,1,69\n";                          // 列数不足 → 跳
  }
  SkipStats   sk;
  const auto  src = load_csv_source(p, Kind::Trade, sk);
  CHECK(src.has_value());
  if (src.has_value()) {
    const Record* r0 = (*src)->peek();
    CHECK(r0 && r0->ts_ns == 100 && r0->gid == 21 && r0->side == 0);
    CHECK(r0 && r0->px == 6884 && r0->price_scale == 2 && r0->qty == 190667 && r0->qty_scale == 2);
    (*src)->advance();
    const Record* r1 = (*src)->peek();
    CHECK(r1 && r1->ts_ns == 200 && r1->side == 1 && r1->px == 69 && r1->price_scale == 0);
    (*src)->advance();
    CHECK((*src)->peek() == nullptr);  // 第 3 行跳过后耗尽
  }
  CHECK(sk.total() == 1 && sk.count(SkipReason::Malformed) == 1);  // 消费完:仅第 3 行(列数不符)被跳
  fs::remove_all(dir);
}

// M1:跳过路径分原因归类(bad_ts / bad_field=side / bad_number),与 UnknownSymbol(test_e2e)、
// Malformed(test_csv_quoting)合起来覆盖全部 SkipReason 的可达分支。
static void test_skip_reasons() {
  using namespace mdreplay;
  namespace fs = std::filesystem;
  const std::string dir = "test_tmp_skip";
  fs::create_directories(dir);
  const std::string p = dir + "/s.trade.csv";
  {
    std::ofstream f(p);
    f << "ts,symbol,side,px,qty\n";
    f << "100,SOLUSDT,0,68.84,5\n";    // 好行
    f << "abc,SOLUSDT,0,68.84,5\n";    // ts 非整数 → bad_ts
    f << "200,SOLUSDT,7,68.84,5\n";    // side 非 0/1 → bad_field
    f << "300,SOLUSDT,0,6x.8,5\n";     // px 非数字 → bad_number
  }
  SkipStats   sk;
  const auto  src = load_csv_source(p, Kind::Trade, sk);
  CHECK(src.has_value());
  const Record* r = src.has_value() ? (*src)->peek() : nullptr;
  CHECK(r && r->ts_ns == 100);  // 唯一好行留下(首条)
  if (src.has_value()) drain(**src);  // 消费余下 3 坏行 → 计入 skip
  CHECK(sk.total() == 3);
  CHECK(sk.count(SkipReason::BadTimestamp) == 1);
  CHECK(sk.count(SkipReason::BadField) == 1);
  CHECK(sk.count(SkipReason::BadNumber) == 1);
  fs::remove_all(dir);
}

// 五档:depth=5 解析全 5 档、csv 往返无损;且只 1/5 两种——depth=5 喂 1 档文件须 CsvSchema 失败(不截断)。
static void test_book_depth5() {
  using namespace mdreplay;
  namespace fs = std::filesystem;
  const std::string dir = "test_tmp_d5";
  fs::create_directories(dir);
  const std::string p = dir + "/d5.book.csv";
  {
    std::ofstream o(p);
    o << "ts,symbol,bid_px,bid_qty,ask_px,ask_qty,bid_px_1,bid_qty_1,ask_px_1,ask_qty_1,"
         "bid_px_2,bid_qty_2,ask_px_2,ask_qty_2,bid_px_3,bid_qty_3,ask_px_3,ask_qty_3,"
         "bid_px_4,bid_qty_4,ask_px_4,ask_qty_4\n";
    o << "1000,SOLUSDT,68.84,10,68.85,20,68.83,11,68.86,21,68.82,12,68.87,22,"
         "68.81,13,68.88,23,68.80,14,68.89,24\n";
  }
  // 5 档表头 → 自动识别 depth=5,逐档正确
  {
    SkipStats  sk;
    const auto src = load_csv_source(p, Kind::Book, sk);
    CHECK(src.has_value() && sk.total() == 0);
    const Record* r = src.has_value() ? (*src)->peek() : nullptr;
    CHECK(r && r->depth == 5 && r->price_scale == 2 && r->qty_scale == 0);
    CHECK(r && r->bid_px[0] == 6884 && r->ask_px[0] == 6885);   // 最优档
    CHECK(r && r->bid_px[4] == 6880 && r->ask_px[4] == 6889);   // 第 5 档
    CHECK(r && r->bid_qty[4] == 14 && r->ask_qty[4] == 24);
  }
  // 1 档表头 → 自动识别 depth=1(正常,非报错)
  {
    const std::string p1 = dir + "/bbo.book.csv";
    { std::ofstream o(p1); o << "ts,symbol,bid_px,bid_qty,ask_px,ask_qty\n1,SOLUSDT,1,1,1,1\n"; }
    SkipStats  sk;
    const auto src = load_csv_source(p1, Kind::Book, sk);
    CHECK(src.has_value() && sk.total() == 0);
    const Record* r = src.has_value() ? (*src)->peek() : nullptr;
    CHECK(r && r->depth == 1 && r->bid_px[0] == 1);
  }
  // 残缺档(只到 _2,缺 _3/_4)→ 既非 1 也非 5 → 整文件 CsvSchema 拒绝(不截断)
  {
    const std::string p2 = dir + "/partial.book.csv";
    { std::ofstream o(p2);
      o << "ts,symbol,bid_px,bid_qty,ask_px,ask_qty,bid_px_1,bid_qty_1,ask_px_1,ask_qty_1,"
           "bid_px_2,bid_qty_2,ask_px_2,ask_qty_2\n1,SOLUSDT,1,1,1,1,1,1,1,1,1,1,1,1\n"; }
    SkipStats  sk;
    const auto src = load_csv_source(p2, Kind::Book, sk);
    CHECK(!src.has_value());  // 部分档 → 拒绝
  }
  // csv 5 档往返:写出(显式 5 档表头)再读回(自动识别 5),逐档一致
  {
    Record b;
    b.kind = Kind::Book; b.ts_ns = 7; b.gid = 21; b.price_scale = 2; b.qty_scale = 0; b.depth = 5;
    for (std::size_t k = 0; k < 5; ++k) {
      b.bid_px[k] = 6884 - k; b.ask_px[k] = 6885 + k; b.bid_qty[k] = 10 + k; b.ask_qty[k] = 20 + k;
    }
    const std::string rp = dir + "/rt.book.csv";
    { const auto sink = CsvSink::open(rp, Kind::Book, 5);
      CHECK(sink.has_value() && (*sink)->write(b).has_value()); }
    SkipStats  sk;
    const auto src = load_csv_source(rp, Kind::Book, sk);
    CHECK(src.has_value() && sk.total() == 0);
    const Record* r = src.has_value() ? (*src)->peek() : nullptr;
    CHECK(r && r->depth == 5 && r->bid_px[3] == 6881 && r->ask_qty[2] == 22);
  }
  fs::remove_all(dir);
}

// 五档 shm:DepthBookSink 写 DepthBoard → 读回逐档;BookSink 从 5 档 Record 只写最优档(L0)。
static void test_depth_shm() {
  using namespace mdreplay;
  namespace v2 = gconf::shm::v2;
  Record r;
  r.kind = Kind::Book; r.ts_ns = 3000; r.gid = 21; r.price_scale = 2; r.qty_scale = 0; r.depth = 5;
  for (std::size_t k = 0; k < 5; ++k) {
    r.bid_px[k] = 6884 - k; r.bid_qty[k] = 10 + k; r.ask_px[k] = 6885 + k; r.ask_qty[k] = 20 + k;
  }
  // DepthBoard:全 5 档逐档读回
  {
    auto          board = std::make_unique<v2::DepthBoard>();
    DepthBookSink sink(board.get());
    CHECK(sink.write(r).has_value());
    v2::DepthSlot out;
    CHECK(board->slot[21].read(out));
    CHECK(out.exch_ns == 3000 && out.depth == 5 && out.price_scale == 2);
    CHECK(out.bid_px[0] == 6884 && out.ask_px[0] == 6885);
    CHECK(out.bid_px[4] == 6880 && out.ask_qty[4] == 24);
  }
  // BBO Board:5 档 Record 喂 BookSink 只落最优档(L0),不报错、不串档
  {
    auto     board = std::make_unique<v2::Board>();
    BookSink sink(board.get());
    CHECK(sink.write(r).has_value());
    v2::BoardSlot out;
    CHECK(board->slot[21].read(out));
    CHECK(out.exch_ns == 3000 && out.bid_px == 6884 && out.ask_px == 6885);
  }
}

// json 五档:写出再读回(自动识别 5),逐档一致;并验 json 逐行可不同档(1 档行 + 5 档行混排)。
static void test_json_depth5() {
  using namespace mdreplay;
  namespace fs = std::filesystem;
  const std::string dir = "test_tmp_jd5";
  fs::create_directories(dir);
  // 往返:5 档 Record → JsonSink → 读回
  {
    Record b;
    b.kind = Kind::Book; b.ts_ns = 9; b.gid = 21; b.price_scale = 2; b.qty_scale = 1; b.depth = 5;
    for (std::size_t k = 0; k < 5; ++k) {
      b.bid_px[k] = 6884 - k; b.ask_px[k] = 6885 + k; b.bid_qty[k] = 100 + k; b.ask_qty[k] = 200 + k;
    }
    const std::string p = dir + "/x.book.json";
    { const auto sink = JsonSink::open(p, Kind::Book);
      CHECK(sink.has_value() && (*sink)->write(b).has_value()); }
    SkipStats  sk;
    const auto src = load_json_source(p, Kind::Book, sk);
    CHECK(src.has_value() && sk.total() == 0);
    const Record* r = src.has_value() ? (*src)->peek() : nullptr;
    CHECK(r && r->depth == 5 && r->bid_px[4] == 6880 && r->ask_qty[3] == 203);
  }
  // 逐行混档:第 1 行 5 档、第 2 行 1 档 → 各按自身档数解析(json 允许行间不同)
  {
    const std::string p = dir + "/mix.book.json";
    { std::ofstream o(p);
      o << R"({"ts":1,"symbol":"SOLUSDT","bid_px":"1","bid_qty":"1","ask_px":"1","ask_qty":"1",)"
           R"("bid_px_1":"1","bid_qty_1":"1","ask_px_1":"1","ask_qty_1":"1",)"
           R"("bid_px_2":"1","bid_qty_2":"1","ask_px_2":"1","ask_qty_2":"1",)"
           R"("bid_px_3":"1","bid_qty_3":"1","ask_px_3":"1","ask_qty_3":"1",)"
           R"("bid_px_4":"1","bid_qty_4":"1","ask_px_4":"1","ask_qty_4":"1"})" << "\n";
      o << R"({"ts":2,"symbol":"SOLUSDT","bid_px":"2","bid_qty":"2","ask_px":"2","ask_qty":"2"})" << "\n"; }
    SkipStats  sk;
    const auto src = load_json_source(p, Kind::Book, sk);
    CHECK(src.has_value() && sk.total() == 0);
    const Record* r0 = src.has_value() ? (*src)->peek() : nullptr;
    CHECK(r0 && r0->ts_ns == 1 && r0->depth == 5);
    if (src.has_value()) (*src)->advance();
    const Record* r1 = src.has_value() ? (*src)->peek() : nullptr;
    CHECK(r1 && r1->ts_ns == 2 && r1->depth == 1);
  }
  fs::remove_all(dir);
}

// 边界:量 ×10^scale 越 u32 → ScaleOverflow 分类;5 档行中任一价非法 → BadNumber 分类(整行跳)。
static void test_overflow_and_badvalue() {
  using namespace mdreplay;
  namespace fs = std::filesystem;
  const std::string dir = "test_tmp_ovf";
  fs::create_directories(dir);
  // trade qty=5e9 > u32(4.29e9),scale 0 仍越界 → ScaleOverflow
  {
    const std::string p = dir + "/o.trade.csv";
    { std::ofstream o(p); o << "ts,symbol,side,px,qty\n1,SOLUSDT,0,68.84,5000000000\n"; }
    SkipStats  sk;
    const auto src = load_csv_source(p, Kind::Trade, sk);
    CHECK(src.has_value() && (*src)->peek() == nullptr);
    CHECK(sk.total() == 1 && sk.count(SkipReason::ScaleOverflow) == 1);
  }
  // 5 档行某档价非数字 → BadNumber(整行跳,不产部分档)
  {
    const std::string p = dir + "/b.book.csv";
    { std::ofstream o(p);
      o << "ts,symbol,bid_px,bid_qty,ask_px,ask_qty,bid_px_1,bid_qty_1,ask_px_1,ask_qty_1,"
           "bid_px_2,bid_qty_2,ask_px_2,ask_qty_2,bid_px_3,bid_qty_3,ask_px_3,ask_qty_3,"
           "bid_px_4,bid_qty_4,ask_px_4,ask_qty_4\n";
      o << "1,SOLUSDT,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,9x,1,1,1\n"; }  // ask_px_4=9x 非法
    SkipStats  sk;
    const auto src = load_csv_source(p, Kind::Book, sk);
    CHECK(src.has_value() && (*src)->peek() == nullptr);
    CHECK(sk.total() == 1 && sk.count(SkipReason::BadNumber) == 1);
  }
  fs::remove_all(dir);
}

// book_depth_of:档数判定唯一真相源,纯逻辑穷举(1 / 5 / 残缺 / 中缺 / >5)。
static void test_book_depth_of() {
  using namespace mdreplay;
  CHECK(book_depth_of([](std::size_t) { return false; }) == std::optional<std::size_t>{1});  // 无高档→1
  CHECK(book_depth_of([](std::size_t k) { return k >= 1 && k <= 4; }) ==
        std::optional<std::size_t>{5});                                                       // _1.._4→5
  CHECK(!book_depth_of([](std::size_t k) { return k == 1 || k == 2; }).has_value());          // 残缺(只到_2)
  CHECK(!book_depth_of([](std::size_t k) { return k == 1 || k == 3 || k == 4; }).has_value()); // 中缺_2
  CHECK(!book_depth_of([](std::size_t k) { return k >= 1 && k <= 5; }).has_value());           // 含_5 >5
}

// 退化文件:只有表头 → 载入成功但空源(不崩);真空文件(无表头)→ CsvSchema。
static void test_degenerate_files() {
  using namespace mdreplay;
  namespace fs = std::filesystem;
  const std::string dir = "test_tmp_degen";
  fs::create_directories(dir);
  // 只有表头,无数据行
  {
    const std::string p = dir + "/h.book.csv";
    { std::ofstream o(p); o << "ts,symbol,bid_px,bid_qty,ask_px,ask_qty\n"; }
    SkipStats  sk;
    const auto src = load_csv_source(p, Kind::Book, sk);
    CHECK(src.has_value() && sk.total() == 0);
    CHECK(src.has_value() && (*src)->peek() == nullptr);  // 空源,不崩
  }
  // 真空文件(无表头)→ CsvSchema
  {
    const std::string p = dir + "/e.book.csv";
    { std::ofstream o(p); }  // 0 字节
    SkipStats  sk;
    const auto src = load_csv_source(p, Kind::Book, sk);
    CHECK(!src.has_value());  // 缺表头
  }
  fs::remove_all(dir);
}

int main() {
  test_fixed();
  test_book_depth_of();
  test_degenerate_files();
  test_clock();
  test_signal();
  test_merge();
  test_config();
  test_e2e();
  test_file_io();
  test_csv_quoting();
  test_skip_reasons();
  test_book_depth5();
  test_depth_shm();
  test_json_depth5();
  test_overflow_and_badvalue();
  if (g_fail == 0) std::printf("all tests passed\n");
  else std::printf("%d checks FAILED\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
