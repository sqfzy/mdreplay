#pragma once
// shm.hpp — 输出去向之一:gconf v2 共享内存段。
//   ShmSegment  POSIX shm_open/ftruncate/mmap 的 RAII 封装 + 建段写头 / 连段自校验。
//   BookSink    Book Record → Board.slot[gid].write(BBO, latest-wins;单写直接写)。
//   TradeSink   Trade Record → TradeRing.publish(无损广播,fetch_add + seqlock)。
//
// create=true:O_TRUNC 清零重建(幂等可复现)→ seg_init 写段头(zeroed 内存即各段初始态)。
// create=false:attach → seg_check;BadMagic/BadVersion/AbiMismatch → SegMismatch,SchemaDrift 容忍。

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include <gconf/shm/v2/booktick_board.h>
#include <gconf/shm/v2/seg_header.h>
#include <gconf/shm/v2/trade.h>  // 临时:gconf v1.2.2 无 market trade 段,暂用旧 vendored TradeRing

#include "core/error.hpp"
#include "core/record.hpp"
#include "output/sink.hpp"

namespace mdreplay {

class ShmSegment {
public:
  ShmSegment() = default;
  ShmSegment(const ShmSegment&) = delete;
  ShmSegment& operator=(const ShmSegment&) = delete;
  ShmSegment(ShmSegment&& o) noexcept { swap(o); }
  ShmSegment& operator=(ShmSegment&& o) noexcept {
    if (this != &o) { close_(); swap(o); }
    return *this;
  }
  ~ShmSegment() { close_(); }

  [[nodiscard]] void* base() const noexcept { return base_; }

  static Result<ShmSegment> open(const std::string& name, std::size_t bytes,
                                 gconf::shm::v2::SegKind kind, std::uint32_t entry_size,
                                 std::uint32_t capacity, std::uint64_t schema_hash, bool create) {
    namespace v2 = gconf::shm::v2;
    const int flags = create ? (O_CREAT | O_RDWR | O_TRUNC) : O_RDWR;
    const int fd = ::shm_open(name.c_str(), flags, 0660);
    if (fd < 0) return std::unexpected(Error::ShmOpen);
    if (create && ::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
      ::close(fd);
      return std::unexpected(Error::ShmOpen);
    }
    void* base = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
      ::close(fd);
      return std::unexpected(Error::ShmOpen);
    }

    auto* hdr = static_cast<v2::SegHeader*>(base);
    if (create) {
      v2::seg_init(*hdr, kind, entry_size, capacity, schema_hash, 0);
    } else {
      const v2::SegError e = v2::seg_check(*hdr, kind, entry_size, capacity, schema_hash);
      if (e != v2::SegError::Ok && e != v2::SegError::SchemaDrift) {
        ::munmap(base, bytes);
        ::close(fd);
        return std::unexpected(Error::SegMismatch);
      }
    }

    ShmSegment s;
    s.base_  = base;
    s.bytes_ = bytes;
    s.fd_    = fd;
    return s;
  }

private:
  void close_() noexcept {
    if (base_) ::munmap(base_, bytes_);
    if (fd_ >= 0) ::close(fd_);
    base_  = nullptr;
    fd_    = -1;
    bytes_ = 0;
  }
  void swap(ShmSegment& o) noexcept {
    std::swap(base_, o.base_);
    std::swap(bytes_, o.bytes_);
    std::swap(fd_, o.fd_);
  }

  void*       base_{nullptr};
  std::size_t bytes_{0};
  int         fd_{-1};
};

// Book Record → BookTickBoard.slot[lid] 写 BBO(最优档)。gconf v1.2.2 行情段只有单档 book tick,
// 输入若多档则只取 L0(截断)。先 claim_if_newer(update_id 真值)赢得本槽再 write(单写者天然赢;
// 若遇非递增的陈旧 update_id 则跳过,符合"只接受更新的盘口版本"语义)。延迟字段回放无 → 0。
class BookSink : public Sink {
public:
  explicit BookSink(gconf::shm::v2::BookTickBoard* board) : board_(board) {}
  Result<void> write(const Record& r) override {
    std::uint64_t replaced_old = 0;
    if (board_->claim_if_newer(r.gid, r.update_id, replaced_old))  // r.gid 即 LID
      board_->slot[r.gid].write(r.ts_ns, r.update_id, r.bid_px[0], r.bid_qty[0], r.ask_px[0],
                                r.ask_qty[0], 0, 0, 0,  // kernel/shm/E 延迟:回放无
                                r.gid, r.price_scale, r.qty_scale, 0 /*path_idx*/);
    return {};
  }

private:
  gconf::shm::v2::BookTickBoard* board_;
};

class TradeSink : public Sink {
public:
  explicit TradeSink(gconf::shm::v2::TradeRing* ring) : ring_(ring) {}
  Result<void> write(const Record& r) override {
    gconf::shm::v2::TradePayload p;
    p.exch_ns          = r.ts_ns;
    p.px               = r.px;
    p.qty              = r.qty;
    p.global_symbol_id = r.gid;
    p.side             = r.side;
    p.price_scale      = r.price_scale;
    p.qty_scale        = r.qty_scale;
    ring_->publish(p);
    ++published_;
    return {};
  }

  // 广播环不背压(铁律 3):发布数超过环容量即绕圈覆盖。收尾时告知绕了几圈 + 无损跟上之道。
  void on_finish() override {
    constexpr std::uint64_t cap = gconf::shm::v2::TRADE_RING_CAP;
    if (published_ > cap)
      spdlog::warn("trade 共发布 {} 条 > 环容量 {} → 广播环绕圈 ~{} 次;realtime=0 全速灌时消费者只拿"
                   "最近一圈(要无损跟上,用 realtime=1.0 真盘节奏)", published_, cap, published_ / cap);
  }

private:
  gconf::shm::v2::TradeRing* ring_;
  std::uint64_t              published_{0};
};

}  // namespace mdreplay
