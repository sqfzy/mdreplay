#pragma once
// shm_segment.hpp — POSIX shm 段的 RAII 封装:shm_open + ftruncate + mmap,建段写头 / 连段自校验。
//
// create=true:O_TRUNC 清零重建(幂等、可复现)→ seg_init 写段头(zeroed 内存即各段的初始态)。
// create=false:attach 既有段 → seg_check;BadMagic/BadVersion/AbiMismatch → SegMismatch,
//              SchemaDrift 可容忍(字段尺寸都对、仅描述串哈希不同)。

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <gconf/shm/v2/seg_header.h>

#include "error.hpp"

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
    base_ = nullptr;
    fd_   = -1;
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

}  // namespace mdreplay
