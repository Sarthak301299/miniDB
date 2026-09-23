#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace minidb {
using PageId = int64_t;
using LSN = uint64_t;

constexpr size_t PAGE_SIZE = 8192;
constexpr PageId INVALID_PAGE_ID = -1;
constexpr LSN INVALID_LSN = 0;

struct PageHeader {
  LSN page_lsn = INVALID_LSN;
};

class Page {
 private:
  std::array<char, PAGE_SIZE> data;

 public:
  Page() { data.fill(0); }

  char* GetData() { return data.data(); }

  const char* GetData() const { return data.data(); }

  LSN GetLSN() const {
    PageHeader h;
    std::memcpy(&h, data.data(), sizeof(h));
    return h.page_lsn;
  }

  void SetLSN(LSN lsn) {
    PageHeader h;
    std::memcpy(&h, data.data(), sizeof(h));
    h.page_lsn = lsn;
    std::memcpy(data.data(), &h, sizeof(h));
  }

  static constexpr size_t HeaderSize() { return sizeof(PageHeader); }

  static constexpr size_t UsableSize() {
    return PAGE_SIZE - sizeof(PageHeader);
  }
};
}  // namespace minidb