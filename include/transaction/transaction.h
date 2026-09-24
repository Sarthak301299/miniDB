#pragma once
#include "tuple.h"

namespace minidb {
struct Transaction {
  TxnId id = INVALID_TXN_ID;
  enum class State { ACTIVE, COMMITTED, ABORTED };
  State state = State::ACTIVE;
};
}  // namespace minidb