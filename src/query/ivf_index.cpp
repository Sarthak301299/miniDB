#include "ivf_index.h"

#include <algorithm>
#include <numeric>
#include <random>

#include "value.h"

namespace minidb {

void IVFIndex::InitPage(Page* page) const {
  PostingPageHeader hdr;
  std::memcpy(page->GetData() + Page::HeaderSize(), &hdr, sizeof(hdr));
}

bool IVFIndex::AppendToCluster(size_t cluster_idx,
                               const std::vector<float>& vector, RID rid) {
  bool allocated_new = false;
  if (cluster_pages[cluster_idx].empty()) {
    PageId page_id;
    Page* page = pool->NewPage(&page_id);
    InitPage(page);
    pool->UnpinPage(page_id, true);
    cluster_pages[cluster_idx].push_back(page_id);
    allocated_new = true;
  }

  PageId tail_pid = cluster_pages[cluster_idx].back();
  Page* page = pool->FetchPage(tail_pid);
  char* base = page->GetData() + Page::HeaderSize();
  PostingPageHeader hdr;
  std::memcpy(&hdr, base, sizeof(hdr));
  if (hdr.count >= EntriesPerPages()) {
    PageId new_page_id;
    Page* new_page = pool->NewPage(&new_page_id);
    InitPage(new_page);
    pool->UnpinPage(new_page_id, true);
    cluster_pages[cluster_idx].push_back(new_page_id);
    hdr.next_page = new_page_id;
    std::memcpy(base, &hdr, sizeof(hdr));

    WALRecord record;
    record.type = WALRecordType::UPDATE;
    record.page_id = tail_pid;
    record.offset = 0;
    record.txn_id = INVALID_TXN_ID;
    record.after_image.assign(base, base + Page::UsableSize());
    LSN lsn = wal->Append(record);
    page->SetLSN(lsn);
    pool->UnpinPage(tail_pid, true);

    cluster_pages[cluster_idx].push_back(new_page_id);
    allocated_new = true;
    tail_pid = new_page_id;
    page = pool->FetchPage(tail_pid);
    base = page->GetData() + Page::HeaderSize();
    std::memcpy(&hdr, base, sizeof(hdr));
  }
  size_t entry_off = sizeof(PostingPageHeader) + hdr.count * EntrySize();
  char* entry_ptr = base + entry_off;
  std::memcpy(entry_ptr, vector.data(), dim * sizeof(float));
  std::memcpy(entry_ptr + dim * sizeof(float), &rid.page_id, sizeof(PageId));
  std::memcpy(entry_ptr + dim * sizeof(float) + sizeof(PageId), &rid.slot,
              sizeof(uint16_t));
  hdr.count++;
  std::memcpy(base, &hdr, sizeof(hdr));
  WALRecord record;
  record.type = WALRecordType::UPDATE;
  record.page_id = tail_pid;
  record.offset = 0;
  record.txn_id = INVALID_TXN_ID;
  record.after_image.assign(base, base + Page::UsableSize());
  LSN lsn = wal->Append(record);
  page->SetLSN(lsn);
  pool->UnpinPage(tail_pid, true);
  return allocated_new;
}

void IVFIndex::RunKMeans(
    const std::vector<std::pair<std::vector<float>, RID>>& vectors,
    uint32_t num_clusters) {
  if (vectors.empty())
    throw std::runtime_error("IVFIndex: cannot build from zero vectors");
  if (vectors.size() < num_clusters)
    throw std::runtime_error(
        "IVFIndex: need at least as many vectors as clusters (" +
        std::to_string(vectors.size()) + " < " + std::to_string(num_clusters) +
        ")");
  std::vector<size_t> idx(vectors.size());
  std::iota(idx.begin(), idx.end(), 0);
  std::mt19937 rng(42);
  std::shuffle(idx.begin(), idx.end(), rng);

  centroids.clear();
  for (uint32_t c = 0; c < num_clusters; ++c) {
    centroids.push_back(vectors[idx[c]].first);
  }

  constexpr int max_iterations = 15;
  std::vector<uint32_t> assignment(vectors.size(), 0);
  for (int iter = 0; iter < max_iterations; ++iter) {
    bool changed = false;
    for (size_t i = 0; i < vectors.size(); ++i) {
      uint32_t best = 0;
      float best_dist = SquaredL2Distance(vectors[i].first, centroids[0]);
      for (uint32_t c = 1; c < num_clusters; ++c) {
        float d = SquaredL2Distance(vectors[i].first, centroids[c]);
        if (d < best_dist) {
          best_dist = d;
          best = c;
        }
      }
      if (iter == 0 || assignment[i] != best) changed = true;
      assignment[i] = best;
    }
    if (!changed && iter > 0) break;

    std::vector<std::vector<float>> sums(num_clusters,
                                         std::vector<float>(dim, 0.0f));
    std::vector<uint32_t> counts(num_clusters, 0);
    for (size_t i = 0; i < vectors.size(); ++i) {
      uint32_t c = assignment[i];
      counts[c]++;
      for (uint32_t d = 0; d < dim; ++d) sums[c][d] += vectors[i].first[d];
    }
    for (uint32_t c = 0; c < num_clusters; ++c) {
      if (counts[c] == 0) continue;
      for (uint32_t d = 0; d < dim; ++d)
        centroids[c][d] = sums[c][d] / static_cast<float>(counts[c]);
    }
  }
}

IVFIndex::IVFIndex(
    BufferPool* pool, WALManager* wal, uint32_t dim, uint32_t num_clusters,
    const std::vector<std::pair<std::vector<float>, RID>>& vectors)
    : pool(pool), wal(wal), dim(dim), cluster_latches(num_clusters) {
  RunKMeans(vectors, num_clusters);
  cluster_pages.resize(num_clusters);
  for (const auto& [vec, rid] : vectors) {
    uint32_t best = 0;
    float best_dist = SquaredL2Distance(vec, centroids[0]);
    for (uint32_t c = 1; c < num_clusters; ++c) {
      float d = SquaredL2Distance(vec, centroids[c]);
      if (d < best_dist) {
        best_dist = d;
        best = c;
      }
    }
    AppendToCluster(best, vec, rid);
  }
}

IVFIndex::IVFIndex(BufferPool* pool, WALManager* wal, uint32_t dim,
                   std::vector<std::vector<float>> centroids_,
                   std::vector<std::vector<PageId>> cluster_pages)
    : pool(pool),
      wal(wal),
      dim(dim),
      centroids(std::move(centroids_)),
      cluster_pages(std::move(cluster_pages)),
      cluster_latches(centroids.size()) {}

std::vector<std::vector<PageId>> IVFIndex::AllClusterPages() const {
  std::vector<std::vector<PageId>> out(centroids.size());
  for (size_t c = 0; c < centroids.size(); ++c) {
    std::lock_guard<std::mutex> lock(cluster_latches[c]);
    out[c] = cluster_pages[c];
  }
  return out;
}

void IVFIndex::Insert(const std::vector<float>& vector, RID rid) {
  if (vector.size() != dim)
    throw std::runtime_error("IVFIndex:Insert: Expected dimension " +
                             std::to_string(dim) + ", got " +
                             std::to_string(vector.size()));
  uint32_t best = 0;
  float best_dist = SquaredL2Distance(vector, centroids[0]);
  for (uint32_t c = 1; c < centroids.size(); ++c) {
    float d = SquaredL2Distance(vector, centroids[c]);
    if (d < best_dist) {
      best_dist = d;
      best = c;
    }
  }
  bool allocated_new = false;
  std::vector<PageId> pages_copy;
  {
    std::lock_guard<std::mutex> lock(cluster_latches[best]);
    allocated_new = AppendToCluster(best, vector, rid);
    if (allocated_new) pages_copy = cluster_pages[best];
  }
  if (allocated_new && on_new_page) on_new_page(best, pages_copy);
}

std::vector<RID> IVFIndex::Search(const std::vector<float>& query, uint32_t k,
                                  uint32_t nprobe) const {
  if (query.size() != dim)
    throw std::runtime_error("IVFIndex:Search: Expected dimension " +
                             std::to_string(dim) + ", got " +
                             std::to_string(query.size()));

  nprobe = std::min<uint32_t>(nprobe, static_cast<uint32_t>(centroids.size()));
  std::vector<std::pair<float, uint32_t>> centroid_dists;
  for (uint32_t c = 0; c < centroids.size(); ++c) {
    centroid_dists.push_back({SquaredL2Distance(query, centroids[c]), c});
  }
  std::partial_sort(centroid_dists.begin(), centroid_dists.begin() + nprobe,
                    centroid_dists.end());
  std::vector<std::pair<float, RID>> candidates;
  for (uint32_t p = 0; p < nprobe; ++p) {
    uint32_t cluster_idx = centroid_dists[p].second;
    std::lock_guard<std::mutex> lock(cluster_latches[cluster_idx]);
    PageId page_id = cluster_pages[cluster_idx].empty()
                         ? INVALID_PAGE_ID
                         : cluster_pages[cluster_idx].front();
    while (page_id != INVALID_PAGE_ID) {
      Page* page = pool->FetchPage(page_id);
      const char* base = page->GetData() + Page::HeaderSize();
      PostingPageHeader hdr;
      std::memcpy(&hdr, base, sizeof(hdr));
      for (uint16_t e = 0; e < hdr.count; ++e) {
        size_t offset = sizeof(PostingPageHeader) + e * EntrySize();
        std::vector<float> v(dim);
        RID rid;
        std::memcpy(v.data(), base + offset, sizeof(float) * dim);
        std::memcpy(&rid.page_id, base + offset + sizeof(float) * dim,
                    sizeof(PageId));
        std::memcpy(&rid.slot,
                    base + offset + sizeof(float) * dim + sizeof(PageId),
                    sizeof(uint16_t));
        candidates.push_back({SquaredL2Distance(query, v), rid});
      }
      pool->UnpinPage(page_id, false);
      page_id = hdr.next_page;
    }
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const std::pair<float, RID>& a, const std::pair<float, RID>& b) {
              return a.first < b.first;
            });
  std::vector<RID> results;
  for (size_t i = 0; i < candidates.size() && i < k; ++i) {
    results.push_back(candidates[i].second);
  }
  return results;
}
}  // namespace minidb