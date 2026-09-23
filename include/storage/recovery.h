#pragma once
#include "disk_manager.h"
#include "wal_manager.h"

namespace minidb {
class RecoveryManager {
 private:
  DiskManager* disk;
  WALManager* wal;

 public:
  RecoveryManager(DiskManager* disk, WALManager* wal);
  size_t Recover();
};
}  // namespace minidb