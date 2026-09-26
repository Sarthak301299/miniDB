#pragma once

#include <functional>

#include "buffer_pool.h"
#include "heap_file.h"
#include "wal_manager.h"

namespace minidb {
class IVFIndex {
 private:
  BufferPool* pool;
  WALManager* wal;
  uint32_t dim;
  std::vector<std::vector<float>> centroids;
  std::vector<std::vector<PageId>> cluster_pages;
  mutable std::vector<std::mutex> cluster_latches;
  std::function<void(size_t, const std::vector<PageId>&)> on_new_page;

  struct PostingPageHeader {
    PageId next_page = INVALID_PAGE_ID;
    uint16_t count = 0;
  };
  void InitPage(Page* page) const;
  size_t EntrySize() const {
    return dim * sizeof(float) + sizeof(PageId) + sizeof(uint16_t);
  }
  size_t EntriesPerPages() const {
    return (Page::UsableSize() - sizeof(PostingPageHeader)) / EntrySize();
  }
  bool AppendToCluster(size_t cluster_idx, const std::vector<float>& vector,
                       RID rid);
  void RunKMeans(const std::vector<std::pair<std::vector<float>, RID>>& vectors,
                 uint32_t num_clusters);

 public:
  IVFIndex(BufferPool* pool, WALManager* wal, uint32_t dim,
           uint32_t num_clusters,
           const std::vector<std::pair<std::vector<float>, RID>>& vectors);
  IVFIndex(BufferPool* pool, WALManager* wal, uint32_t dim,
           std::vector<std::vector<float>> centroids,
           std::vector<std::vector<PageId>> cluster_pages);
  uint32_t Dim() const { return dim; }
  uint32_t NumClusters() const {
    return static_cast<uint32_t>(centroids.size());
  }
  const std::vector<std::vector<float>>& Centroids() const { return centroids; }
  std::vector<std::vector<PageId>> AllClusterPages() const;
  void SetOnNewPageCallback(
      std::function<void(size_t cluster_idx, const std::vector<PageId>&)> cb) {
    on_new_page = std::move(cb);
  }
  void Insert(const std::vector<float>& vector, RID rid);
  std::vector<RID> Search(const std::vector<float>& query, uint32_t k,
                          uint32_t nprobe) const;
};
}  // namespace minidb