#include <buffer_pool.h>
#include <disk_manager.h>
#include <recovery.h>
#include <wal_manager.h>

#include <algorithm>
#include <iostream>
#include <random>

using namespace minidb;

namespace {
constexpr const char* db_file = "mini.db";
constexpr const char* log_file = "mini.wal";
void CleanFiles() {
  std::remove(db_file);
  std::remove(log_file);
}
}  // namespace

// Simulating a scenario where data is written to WAL but not to disk and is
// cleanly recovered. Manager objects are created in a scope and are deleted at
// the end of the scope without flushing to disk. The recovery manager then
// recovers unflushed transactions in the WAL.

int main() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<int> distr(0, 15);
  CleanFiles();
  std::cout << "===Before Crash===\n";
  std::vector<std::string> dumps;
  std::vector<PageId> page_ids;
  std::vector<uint32_t> page_offsets(16);
  for (int i = 0; i < 16; i++)
    dumps.push_back("This is test string " + std::to_string(i));

  {
    DiskManager disk(db_file);
    WALManager wal(log_file);
    BufferPool pool(16, &disk, &wal);

    LSN max = INVALID_LSN;
    for (int i = 0; i < 16; i++) {
      PageId page_id;
      Page* page = pool.NewPage(&page_id);
      page_ids.push_back(page_id);
      std::cout << "Allocated page " << page_id << "\n";
      WALRecord rec;
      rec.type = WALRecordType::UPDATE;
      rec.page_id = page_id;
      // Pick a random offset
      rec.offset = distr(gen) * 32;
      page_offsets[page_id] = rec.offset;
      rec.after_image.assign(dumps[i].begin(), dumps[i].end());
      LSN lsn = wal.Append(rec);
      std::memcpy(page->GetData() + Page::HeaderSize() + rec.offset,
                  dumps[i].data(), dumps[i].size());
      page->SetLSN(lsn);
      max = std::max(max, lsn);
    }
    wal.Flush(max);
    for (const auto& page_id : page_ids) {
      pool.UnpinPage(page_id, true);
    }
    std::cout << "WAL record durable at LSN " << max
              << ". All Pages are dirty in memory but NOT written to disk.\n";
    // Write a random set of pages to list
    int count = distr(gen);
    std::vector<int> numbers(16);
    std::iota(numbers.begin(), numbers.end(), 0);
    std::shuffle(numbers.begin(), numbers.end(), gen);
    for (int i = 0; i < count; i++) {
      int idx = numbers[i];
      pool.FlushPage(page_ids[idx]);
      std::printf("Flushed %d out of %d pages with page_id %ld\n", i + 1, count,
                  page_ids[idx]);
    }

    std::cout << "Simulating a crash now (process state discarded, no clean "
                 "shutdown)...\n\n";
  }  // The in-memory dumps which have not been flushed are lost at this point
     // and need recovery from WAL.
  std::cout << "===After Restart===\n";

  {
    DiskManager disk(db_file);
    WALManager wal(log_file);

    for (const auto& page_id : page_ids) {
      Page page;
      disk.ReadPage(page_id, page.GetData());
      std::string before(page.GetData() + Page::HeaderSize() +
                         page_offsets[page_id]);
      std::printf("Found dump of page_id %ld as %s in disk\n", page_id,
                  before.c_str());
    }
    RecoveryManager recovery(&disk, &wal);
    size_t redone = recovery.Recover();
    std::cout << "Recovery redid " << redone << " update(s) from the WAL.\n";
    BufferPool pool(16, &disk, &wal);

    for (const auto& page_id : page_ids) {
      Page* page = pool.FetchPage(page_id);
      std::string after(page->GetData() + Page::HeaderSize() +
                        page_offsets[page_id]);
      pool.UnpinPage(page_id, false);
      std::printf("Updated page with page_id %ld as %s after recovery\n",
                  page_id, after.c_str());
    }
  }
  CleanFiles();
}