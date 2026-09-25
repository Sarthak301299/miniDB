#include "recovery.h"

namespace minidb {
RecoveryManager::RecoveryManager(DiskManager* disk, WALManager* wal)
    : disk(disk), wal(wal) {}

size_t RecoveryManager::Recover() {
  auto records = wal->ReadAll();
  LSN last_checkpoint_lsn = INVALID_LSN;
  for (auto& r : records) {
    if (r.type == WALRecordType::CHECKPOINT) last_checkpoint_lsn = r.lsn;
    if (r.type == WALRecordType::UPDATE) disk->AdvancePastPage(r.page_id);
  }

  size_t redone = 0;
  for (auto& r : records) {
    if (r.lsn <= last_checkpoint_lsn) continue;
    if (r.type != WALRecordType::UPDATE) continue;

    Page page;
    disk->ReadPage(r.page_id, page.GetData());
    if (page.GetLSN() >= r.lsn) continue;

    std::memcpy(page.GetData() + Page::HeaderSize() + r.offset,
                r.after_image.data(), r.after_image.size());
    if (r.page_id == 0) {
      uint16_t nt;
      std::memcpy(&nt, page.GetData() + Page::HeaderSize() + r.offset,
                  sizeof(nt));
    }
    page.SetLSN(r.lsn);
    disk->WritePage(r.page_id, page.GetData());
    redone++;
  }
  return redone;
}
}  // namespace minidb