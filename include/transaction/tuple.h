#pragma once
#include <cstdint>

namespace minidb {
using TxnId = uint64_t;
constexpr TxnId INVALID_TXN_ID = 0;

struct TupleHeader {
  TxnId xmin = INVALID_TXN_ID;
  TxnId xmax = INVALID_TXN_ID;
  uint32_t data_len = 0;
};
}  // namespace minidb