#include "db_server.h"

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "recovery.h"

namespace minidb {

namespace {
std::string Upper(const std::string& s) {
  std::string r = s;
  for (auto& c : r)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return r;
}

std::string EncodeResult(const QueryResult& r) {
  std::string out = "OK\t" + std::to_string(r.rows_affected) + "\t";
  for (size_t i = 0; i < r.column_names.size(); ++i) {
    if (i) out += ",";
    out += r.column_names[i];
  }
  out += "\t";
  for (size_t i = 0; i < r.rows.size(); ++i) {
    if (i) out += "|";
    for (size_t j = 0; j < r.rows[i].size(); ++j) {
      if (j) out += ";";
      out += r.rows[i][j].ToString();
    }
  }
  return out;
}
}  // namespace

DbServer::DbServer(int id, std::string data_dir_, int num_threads,
                   int query_port, int control_port)
    : id(id),
      data_dir(std::move(data_dir_)),
      num_threads(num_threads),
      query_port(query_port),
      control_port(control_port) {
  db_path = data_dir + "/db_file.db";
  wal_path = data_dir + "/log_file.wal";
  lock_path = data_dir + "/primary.lock";

  if (const char* m = std::getenv("MINIDB_REPLICA_MODE"))
    wal_mode = (std::strcmp(m, "flush") != 0);

  for (size_t i = 0; i < connection_pool_size; ++i) {
    primary_conn_pool.push_back(std::make_unique<net::PersistentConnection>());
  }

  for (int attempt = 0; attempt < 50; ++attempt) {
    try {
      BuildEngineStackLocked(/*read_only=*/true);
      return;
    } catch (const std::exception&) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  throw std::runtime_error("DbServer: shared files never appeared at " +
                           db_path);
}

void DbServer::BuildEngineStackLocked(bool read_only) {
  disk = std::make_unique<DiskManager>(db_path, read_only);
  wal = std::make_unique<WALManager>(wal_path, read_only);
  if (!read_only) {
    RecoveryManager(disk.get(), wal.get()).Recover();
  }
  pool = std::make_unique<BufferPool>(128, disk.get(), wal.get(), 8);
  {
    std::lock_guard<std::mutex> l(wal_index_mutex);
    wal_index.clear();
  }
  if (wal_mode && read_only) {
    std::vector<long> offsets;
    long end = 0;
    auto recs = wal->ReadAll(0, &end, &offsets);
    IndexWALRecords(recs, offsets);
    pool->SetPageProvider(
        [this](PageId pid, Page* page) { return WALPageProvider(pid, page); });
  } else {
    {
      std::lock_guard<std::mutex> lock(pending_flush_mutex);
      pending_flush_pages.reserve(1024);
    }
    pool->SetPreDiskReadHook(
        [this](PageId page_id) { PreDiskReadHook(page_id); });
    pool->SetPostDiskReadHook(
        [this](PageId page_id, LSN lsn) { PostDiskReadHook(page_id, lsn); });
  }
  txn_mgr = std::make_unique<TransactionManager>(wal.get());
  engine = std::make_unique<Engine>(disk.get(), pool.get(), wal.get());
}

void DbServer::BecomePrimary(
    uint32_t epoch_,
    std::vector<std::pair<std::string, int>> replica_control_addrs_) {
  std::unique_lock<std::shared_mutex> lock(engine_mutex);

  int lock_fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
  if (lock_fd < 0) {
    throw std::runtime_error("BecomePrimary: could not open lock file " +
                             lock_path);
  }
  if (::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(lock_fd);
    throw std::runtime_error(
        "BecomePrimary: could not acquire the primary lock -- another process "
        "still holds it and genuinely believes it is primary");
  }
  primary_lock_fd = lock_fd;
  current_primary_control_port = -1;
  BuildEngineStackLocked(false);
  {
    std::lock_guard<std::mutex> lock(pending_flush_mutex);
    pending_flush_pages.clear();
  }
  is_primary.store(true);
  epoch.store(epoch_);
  replica_control_addrs = std::move(replica_control_addrs_);
  replica_conns.clear();
  for (const auto& [host, port] : replica_control_addrs) {
    auto conn = std::make_unique<net::PersistentConnection>();
    conn->SetTarget(host, port);
    replica_conns.push_back(std::move(conn));
  }
}

void DbServer::DoNotifyRound() {
  std::vector<net::PersistentConnection*> conns;
  conns.reserve(replica_conns.size());
  for (auto& c : replica_conns) conns.push_back(c.get());
  std::vector<std::string> replies = net::PersistentConnection::SendAndRecvAll(
      conns, "NOTIFY " + std::to_string(epoch.load()));
  for (size_t i = 0; i < replies.size(); ++i) {
    if (replies[i].rfind("ACK", 0) != 0) {
      auto& [host, port] = replica_control_addrs[i];
      throw std::runtime_error("replication failed: replica at " + host + ":" +
                               std::to_string(port) + " did not ack (" +
                               replies[i] + ")");
    }
  }
}
void DbServer::NotifyReplicasAndWait() {
  if (replica_control_addrs.empty()) return;
  notify_calls.fetch_add(1);
  std::unique_lock<std::mutex> lk(notify_mu);
  const uint64_t need = notify_rounds_started + 1;
  while (true) {
    if (notify_rounds_completed >= need) {
      const NotifyErr& e = notify_errors[need % notify_err_ring];
      if (e.round == need && !e.msg.empty()) throw std::runtime_error(e.msg);
      return;
    }
    if (!notify_round_in_flight) {
      notify_round_in_flight = true;
      const uint64_t my_round = ++notify_rounds_started;
      lk.unlock();
      std::string err;
      const auto t0 = std::chrono::steady_clock::now();
      try {
        DoNotifyRound();

      } catch (const std::exception& ex) {
        err = ex.what();
        if (err.empty()) err = "replication failed";
      }
      const auto dt = std::chrono::steady_clock::now() - t0;
      lk.lock();
      notify_errors[my_round % notify_err_ring] = NotifyErr{my_round, err};
      notify_rounds_completed = my_round;
      notify_round_in_flight = false;
      notify_rounds.fetch_add(1);
      notify_busy_ns.fetch_add(static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count()));
      notify_cv.notify_all();
      continue;  // re-check: this round is >= need, so it returns above
    }
    notify_cv.wait(lk);
  }
}

void DbServer::RequestPrimaryFlushPage(PageId page_id) {
  if (current_primary_control_port < 0) return;
  flush_requests.fetch_add(1);
  size_t idx = next_primary_conn.fetch_add(1) % primary_conn_pool.size();
  std::string resp = primary_conn_pool[idx]->SendAndRecv(
      "FLUSHPAGE " + std::to_string(page_id));
  if (resp.rfind("ACK", 0) != 0) {
    throw std::runtime_error(
        "RequestPrimaryFlushPage: primary did not ack for page " +
        std::to_string(page_id) + " (" + resp + ")");
  }
}

void DbServer::SyncFromSharedWal() {
  long new_offset;
  std::vector<long> new_offsets;
  auto new_records =
      wal->ReadAll(last_synced_offset.load(), &new_offset, &new_offsets);
  if (new_records.empty()) {
    last_synced_offset = new_offset;
    return;
  }

  std::unordered_set<PageId> touched_pages;
  bool catalog_touched = false;
  LSN batch_max_lsn = 0;
  for (auto& r : new_records) {
    if (r.lsn > batch_max_lsn) batch_max_lsn = r.lsn;
    if (r.type == WALRecordType::COMMIT) {
      txn_mgr->ObserveCommit(r.txn_id);
    } else if (r.type == WALRecordType::UPDATE) {
      touched_pages.insert(r.page_id);
      if (r.page_id == Catalog::catalogPageId) catalog_touched = true;
    }
  }

  if (wal_mode) {
    IndexWALRecords(new_records, new_offsets);
  } else {
    std::lock_guard<std::mutex> lock(pending_flush_mutex);
    for (PageId pid : touched_pages) pending_flush_pages.insert(pid);
  }
  for (PageId pid : touched_pages) {
    pool->InvalidatePage(pid);
  }

  if (catalog_touched) engine->GetCatalog().ReloadFromDisk();

  if (batch_max_lsn > sync_max_lsn.load()) sync_max_lsn.store(batch_max_lsn);
  last_synced_offset = new_offset;
}

void DbServer::PreDiskReadHook(PageId page_id) {
  if (wal_mode || is_primary.load()) return;
  bool needs_flush;
  {
    std::lock_guard<std::mutex> lock(pending_flush_mutex);
    needs_flush = pending_flush_pages.erase(page_id) > 0;
  }
  if (needs_flush) RequestPrimaryFlushPage(page_id);
}

void DbServer::PostDiskReadHook(PageId page_id, LSN page_lsn) {
  if (is_primary.load() || current_primary_control_port < 0) return;
  if (page_lsn <= std::max(sync_max_lsn.load(), mark_max_lsn.load())) return;
  std::lock_guard<std::mutex> lock(mark_mutex);
  LSN horizon = std::max(sync_max_lsn.load(), mark_max_lsn.load());
  if (page_lsn <= horizon) return;
  long start = std::max(mark_offset, last_synced_offset.load());
  long end = start;
  auto records = wal->ReadAll(start, &end);
  LSN max_lsn = horizon;
  {
    std::lock_guard<std::mutex> plock(pending_flush_mutex);
    for (auto& r : records) {
      if (r.lsn > max_lsn) max_lsn = r.lsn;
      if (r.type == WALRecordType::UPDATE && r.page_id != page_id)
        pending_flush_pages.insert(r.page_id);
    }
  }
  mark_offset = end;
  mark_max_lsn.store(max_lsn);
}

void DbServer::IndexWALRecords(const std::vector<WALRecord>& records,
                               const std::vector<long>& offsets) {
  std::lock_guard<std::mutex> lock(wal_index_mutex);
  for (size_t i = 0; i < records.size(); ++i) {
    const WALRecord& r = records[i];
    if (r.type != WALRecordType::UPDATE) continue;
    if (r.offset != 0 || r.after_image.size() != Page::UsableSize()) {
      throw std::runtime_error(
          "WAL replay requires every UPDATE to be a full-page image "
          "(page " +
          std::to_string(r.page_id) +
          " is not); use "
          "MINIDB_REPLICA_MODE=flush or change the writer");
    }
    auto it = wal_index.find(r.page_id);
    if (it == wal_index.end() || r.lsn >= it->second.lsn)
      wal_index[r.page_id] = WALLoc{offsets[i], r.lsn};
  }
}

bool DbServer::WALPageProvider(PageId page_id, Page* page) {
  WALLoc loc;
  {
    std::lock_guard<std::mutex> lock(wal_index_mutex);
    auto it = wal_index.find(page_id);
    if (it == wal_index.end()) {
      disk_page_reads.fetch_add(1);
      return false;
    }
    loc = it->second;
  }
  WALRecord rec;
  if (!wal->ReadRecordAt(loc.offset, &rec) ||
      rec.type != WALRecordType::UPDATE || rec.page_id != page_id ||
      rec.lsn != loc.lsn || rec.after_image.size() != Page::UsableSize()) {
    throw std::runtime_error("WALPageProvider: WAL record for page " +
                             std::to_string(page_id) + " at offset " +
                             std::to_string(loc.offset) +
                             " is missing or does not match the index");
  }
  std::memset(page->GetData(), 0, Page::HeaderSize());
  std::memcpy(page->GetData() + Page::HeaderSize(), rec.after_image.data(),
              Page::UsableSize());
  page->SetLSN(rec.lsn);
  wal_page_reads.fetch_add(1);
  return true;
}

void DbServer::ServeWithEpoll(
    int listen_fd, int threads,
    std::function<std::string(const std::string&)> handler) {
  int ep = ::epoll_create1(0);
  auto arm = [ep](int fd, bool add) {
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLONESHOT | EPOLLRDHUP;
    ev.data.fd = fd;
    ::epoll_ctl(ep, add ? EPOLL_CTL_ADD : EPOLL_CTL_MOD, fd, &ev);
  };
  arm(listen_fd, true);
  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([=] {
      epoll_event ev;
      while (true) {
        int n = ::epoll_wait(ep, &ev, 1, -1);
        if (n <= 0) continue;
        int fd = ev.data.fd;
        if (fd == listen_fd) {
          int c = net::AcceptConnection(listen_fd);
          if (c >= 0) arm(c, true);
          arm(listen_fd, false);
          continue;
        }
        std::string line;
        if (!net::RecvLine(fd, &line)) {
          ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
          net::CloseSocket(fd);
          continue;
        }
        std::string resp;
        try {
          resp = handler(line);
        } catch (const std::exception& e) {
          resp = std::string("ERR ") + e.what();
        }
        if (!net::SendLine(fd, resp)) {
          ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
          net::CloseSocket(fd);
          continue;
        }
        arm(fd, false);
      }
    });
  }
  for (auto& w : workers) w.join();
}

void DbServer::Run() {
  control_listen_fd = net::CreateListenSocket(control_port);
  query_listen_fd = net::CreateListenSocket(query_port);
  std::thread control([this] {
    ServeWithEpoll(
        control_listen_fd, numControlThreads,
        [this](const std::string& l) { return ProcessControlMessage(l); });
  });
  control.detach();
  ServeWithEpoll(query_listen_fd, num_threads,
                 [this](const std::string& l) { return ProcessQuery(l); });
}

std::string DbServer::RunAutocommit(const std::string& sql) {
  std::string upper = Upper(sql);
  bool touches_wal = upper.rfind("INSERT", 0) == 0 ||
                     upper.rfind("DELETE", 0) == 0 ||
                     upper.rfind("CREATE", 0) == 0;

  if (touches_wal) {
    if (!is_primary.load()) {
      return "ERR this server is not the primary -- writes must be sent to "
             "the "
             "primary";
    }
    std::shared_lock<std::shared_mutex> lock(engine_mutex);
    auto txn = txn_mgr->Begin();
    QueryResult result;
    try {
      result = engine->Execute(sql, txn.get(), *txn_mgr);
    } catch (const std::exception& e) {
      txn_mgr->Abort(txn.get());
      return std::string("ERR ") + e.what();
    }
    txn_mgr->Commit(txn.get());
    try {
      NotifyReplicasAndWait();
    } catch (const std::exception& e) {
      return std::string("ERR ") + e.what() +
             " (committed locally, NOT durable)";
    }
    return "OK\t" + std::to_string(result.rows_affected) + "\t\t";
  }
  std::shared_lock<std::shared_mutex> lock(engine_mutex);
  try {
    QueryResult result = engine->Execute(sql, nullptr, *txn_mgr);
    return EncodeResult(result);
  } catch (const std::exception& e) {
    return std::string("ERR ") + e.what();
  }
}

std::string DbServer::ProcessQuery(const std::string& line) {
  if (line.rfind("QUERY ", 0) == 0) {
    return RunAutocommit(line.substr(6));
  }
  return "ERR unrecognized command on the query port (control messages go to "
         "the control port)";
}

void DbServer::HandleControlConnection(int fd) {
  while (true) {
    std::string line;
    if (!net::RecvLine(fd, &line)) break;
    std::string response;
    try {
      response = ProcessControlMessage(line);
    } catch (const std::exception& e) {
      response = std::string("ERR ") + e.what();
    }
    if (!net::SendLine(fd, response)) break;
  }
  net::CloseSocket(fd);
}

std::string DbServer::ProcessControlMessage(const std::string& line) {
  if (line == "PING") return "PONG";

  if (line == "STATS") {
    size_t indexed;
    {
      std::lock_guard<std::mutex> l(wal_index_mutex);
      indexed = wal_index.size();
    }
    return std::string("OK mode=") + (wal_mode ? "wal" : "flush") +
           " wal_reads=" + std::to_string(wal_page_reads.load()) +
           " disk_reads=" + std::to_string(disk_page_reads.load()) +
           " flush_requests=" + std::to_string(flush_requests.load()) +
           " indexed_pages=" + std::to_string(indexed) +
           " notify_calls=" + std::to_string(notify_calls.load()) +
           " notify_rounds=" + std::to_string(notify_rounds.load()) +
           " notify_busy_ms=" + std::to_string(notify_busy_ns.load() / 1000000);
  }

  if (line == "STATUS") {
    return "STATUS " + std::string(is_primary.load() ? "PRIMARY" : "REPLICA") +
           " " + std::to_string(epoch.load());
  }

  if (line.rfind("PROMOTE ", 0) == 0) {
    std::istringstream iss(line.substr(8));
    uint32_t epoch;
    std::string peer_list;
    iss >> epoch >> peer_list;
    std::vector<std::pair<std::string, int>> peers;
    std::stringstream ss(peer_list);
    std::string item;
    while (std::getline(ss, item, ',')) {
      if (item.empty()) continue;
      auto colon = item.find(':');
      peers.push_back(
          {item.substr(0, colon), std::stoi(item.substr(colon + 1))});
    }
    BecomePrimary(epoch, std::move(peers));
    return "ACK";
  }

  if (line.rfind("SETPRIMARY ", 0) == 0) {
    std::string addr = line.substr(11);
    auto colon = addr.find(':');
    current_primary_host = addr.substr(0, colon);
    current_primary_control_port = std::stoi(addr.substr(colon + 1));
    for (auto& conn : primary_conn_pool) {
      conn->SetTarget(current_primary_host, current_primary_control_port);
    }
    return "ACK";
  }

  if (line.rfind("NOTIFY ", 0) == 0) {
    uint32_t incoming_epoch = static_cast<uint32_t>(std::stoul(line.substr(7)));
    if (incoming_epoch < epoch.load()) {
      return "REJECT stale epoch " + std::to_string(incoming_epoch) + " < " +
             std::to_string(epoch.load());
    }
    epoch.store(incoming_epoch);
    std::shared_lock<std::shared_mutex> lock(engine_mutex);
    SyncFromSharedWal();
    return "ACK";
  }

  if (line.rfind("FLUSHPAGE ", 0) == 0) {
    if (!is_primary.load()) return "ERR not primary";
    PageId pid = std::stoll(line.substr(10));
    std::shared_lock<std::shared_mutex> lock(engine_mutex);
    pool->FlushPage(pid);
    return "ACK";
  }

  return "ERR unrecognized control message";
}

}  // namespace minidb