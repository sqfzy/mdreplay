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

#include <gconf/shm/v2/board.h>
#include <gconf/shm/v2/depth_board.h>
#include <gconf/shm/v2/seg_header.h>
#include <gconf/shm/v2/trade.h>

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

class BookSink : public Sink {
public:
  explicit BookSink(gconf::shm::v2::Board* board) : board_(board) {}
  Result<void> write(const Record& r) override {
    board_->slot[r.gid].write(r.ts_ns, static_cast<std::uint64_t>(r.ts_ns),       // update_id = ts(单调)
                              r.bid_px[0], r.bid_qty[0], r.ask_px[0], r.ask_qty[0],  // BBO = 最优档
                              0, 0, 0,                                              // 延迟:回放无
                              r.gid, r.price_scale, r.qty_scale, 0 /*path_idx*/);
    return {};
  }

private:
  gconf::shm::v2::Board* board_;
};

// 五档 book → DepthBoard.slot[gid](latest-wins;Record 的 5 档定点数组直写)。
class DepthBookSink : public Sink {
public:
  explicit DepthBookSink(gconf::shm::v2::DepthBoard* board) : board_(board) {}
  Result<void> write(const Record& r) override {
    board_->slot[r.gid].write(r.ts_ns, static_cast<std::uint64_t>(r.ts_ns), r.bid_px.data(),
                              r.bid_qty.data(), r.ask_px.data(), r.ask_qty.data(), r.gid,
                              r.price_scale, r.qty_scale, r.depth);
    return {};
  }

private:
  gconf::shm::v2::DepthBoard* board_;
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
    return {};
  }

private:
  gconf::shm::v2::TradeRing* ring_;
};

}  // namespace mdreplay
