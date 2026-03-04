/**
 * @file mvcc_read_overhead.cc
 *
 * @brief Micro-benchmark: MVCC read-path overhead
 *
 * Measures, in ns/lookup, how much overhead MVCC adds to reads that are
 * *always* guaranteed to see the latest version (i.e. no version-chain
 * traversal is ever required).
 *
 * Two runs are executed back-to-back over the same pre-loaded B-Tree:
 *
 *   Run A – Baseline (READ_UNCOMMITTED, FLAGS_txn_mvcc = false)
 *     BTree::LookUp skips every MVCC check.  This is the raw cost of
 *     traversing the B-Tree and reading a leaf key.
 *
 *   Run B – MVCC happy path (SERIALIZABLE, FLAGS_txn_mvcc = true)
 *     The reader's start_ts is always >= every tuple's commit_ts, so the
 *     "key found and visible" branch is taken on every lookup.  No
 *     version-chain traversal happens.  The extra cost over Run A captures:
 *       - GetTimestamp() on the leaf slot
 *       - TryLockShared() -> read_set_ insertion
 *       - UpdateTupleReadTS() -> SetTupleTimestamp() on the read_set_ entry
 *       - ValidateReadSet() -> OCC validation + read_set_ clear at commit
 *
 *     NOTE: SERIALIZABLE is used (rather than SNAPSHOT_ISOLATION) because
 *     LeanStore::CommitTransaction() calls ValidateReadSet(), which is the
 *     only code path that clears the thread-local read_set_.  Under
 *     SNAPSHOT_ISOLATION the read_set_ would never be cleared, accumulating
 *     dangling LOCKABLE_TUPLE_STACK pointers and eventually hanging inside
 *     the hash map.  Both isolation levels exercise the identical MVCC
 *     read hot-path when start_ts >= all tuple_ts.
 *
 * Usage:
 *   ./MicroMVCCReadOverhead [--record_count=N] [--warmup_iters=N]
 *                           [--bench_iters=N]
 *
 *   measuring the full version-chain path requires AppendVersion() to be
 *   wired up in LockManager::ReleaseAllLocks(), which is marked TODO in
 *   transaction/mvcc/lock_manager.cc.  Once that lands, Run C can be added
 *   by starting the reader *before* committing the writer so that
 *   tuple_ts > txn.start_ts for every key.
 */

#include "leanstore/leanstore.h"
#include "leanstore/config.h"
#include "storage/btree/tree.h"
#include "transaction/transaction.h"

#include "gflags/gflags.h"
#include "share_headers/config.h"
#include "share_headers/time.h"
#include "spdlog/spdlog.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Benchmark flags
// ---------------------------------------------------------------------------
DEFINE_uint64(record_count, 100'000, "Number of records to load");
DEFINE_uint64(warmup_iters, 3, "Number of warm-up passes (not timed)");
DEFINE_uint64(bench_iters, 5, "Number of timed passes");

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {

// RAII helper: creates and pre-allocates a temporary file for FLAGS_db_path.
// LeanStore's BufferManager opens FLAGS_db_path with O_RDWR|O_DIRECT at
// construction time; the file must already exist and be large enough.
struct TempDB {
  static constexpr u64 DB_SIZE = 4UL * leanstore::GB;  // 4 GB virtual space

  std::filesystem::path path;

  TempDB() {
    path = std::filesystem::temp_directory_path() / "mvcc_bench_db.wal";
    std::ofstream(path).close();             // create / truncate
    truncate(path.c_str(), DB_SIZE);         // pre-allocate
    FLAGS_db_path = path.string();           // tell LeanStore where to look
    FLAGS_blob_buffer_pool_gb = 0;
    FLAGS_wal_enable          = false;
    FLAGS_wal_enable_recovery = false;
  }

  ~TempDB() { std::filesystem::remove(path); }
};

static constexpr u64 KEY_SIZE     = 8;
static constexpr u64 PAYLOAD_SIZE = 64;

void EncodeKey(u8 *buf, u64 k) {
  for (int i = 7; i >= 0; --i) { buf[i] = static_cast<u8>(k & 0xFF); k >>= 8; }
}

struct BenchResult { double ns_per_lookup; double mlookups_per_sec; };

// Run a timed read loop over all N keys, inside a single transaction per pass.
// `iso_str` is the isolation level string accepted by LeanStore::StartTransaction
// ("ru" = READ_UNCOMMITTED, "si" = SNAPSHOT_ISOLATION, "ser" = SERIALIZABLE).
BenchResult MeasureRun(const char *label, u64 n_records,
                       leanstore::LeanStore &db,
                       leanstore::storage::BTree &tree,
                       const std::string &iso_str) {
  u8 key_buf[KEY_SIZE];
  u64 total_cycles = 0;

  // Warm-up: verify correctness, prime caches
  for (u64 w = 0; w < FLAGS_warmup_iters; ++w) {
    db.StartTransaction(0, leanstore::transaction::Transaction::Mode::OLTP, iso_str);
    for (u64 k = 0; k < n_records; ++k) {
      EncodeKey(key_buf, k);
      auto ret = tree.LookUp({key_buf, KEY_SIZE}, [](std::span<const u8>) {});
      assert(ret == leanstore::OpResult::OK);
    }
    db.CommitTransaction();
  }

  // Timed passes
  for (u64 it = 0; it < FLAGS_bench_iters; ++it) {
    db.StartTransaction(0, leanstore::transaction::Transaction::Mode::OLTP, iso_str);
    auto t0 = tsctime::ReadTSC();
    for (u64 k = 0; k < n_records; ++k) {
      EncodeKey(key_buf, k);
      tree.LookUp({key_buf, KEY_SIZE}, [](std::span<const u8>) {});
    }
    total_cycles += tsctime::ReadTSC() - t0;
    db.CommitTransaction();
  }

  u64    total_lookups = n_records * FLAGS_bench_iters;
  u64    total_ns      = static_cast<u64>(static_cast<double>(total_cycles) / tsctime::TSC_PER_NS);
  double ns_per_lkp    = static_cast<double>(total_ns) / total_lookups;
  double mlkps         = static_cast<double>(total_lookups) / (static_cast<double>(total_ns) / 1e9) / 1e6;

  std::cout << std::left  << std::setw(42) << label
            << std::right << std::setw(9)  << std::fixed << std::setprecision(2) << ns_per_lkp << " ns/lookup"
            << "   "      << std::setw(7)  << std::setprecision(2) << mlkps << " Mlookups/s\n";

  return {ns_per_lkp, mlkps};
}

}  // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
  gflags::SetUsageMessage("LeanStore MVCC read-overhead micro-benchmark");
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  (void)tsctime::TSC_PER_NS;  // calibrate TSC once
  const u64 N = FLAGS_record_count;

  BenchResult res_a{}, res_b{};

  // -------------------------------------------------------------------------
  // Run A: READ_UNCOMMITTED baseline (FLAGS_txn_mvcc = false)
  // LookUp skips every MVCC branch: no timestamp read, no read_set_ touch.
  // -------------------------------------------------------------------------
  FLAGS_txn_mvcc = false;
  {
    TempDB tmp_db;
    auto db = std::make_unique<leanstore::LeanStore>();

    // RegisterTable internally calls ScheduleSyncJob(0,...) to set up the
    // B-Tree; it must be called from the MAIN THREAD, not from inside another
    // ScheduleSyncJob — that would deadlock on worker 0.
    db->RegisterTable(std::type_index(typeid(char)), 0);
    auto *tree = reinterpret_cast<leanstore::storage::BTree *>(
                   db->RetrieveIndex(std::type_index(typeid(char))));

    db->worker_pool.ScheduleSyncJob(0, [&]() {
      u8 key_buf[KEY_SIZE], val_buf[PAYLOAD_SIZE];
      std::memset(val_buf, 0xAB, PAYLOAD_SIZE);
      db->StartTransaction();
      for (u64 k = 0; k < N; ++k) {
        EncodeKey(key_buf, k);
        assert(tree->Insert({key_buf, KEY_SIZE}, {val_buf, PAYLOAD_SIZE}) == leanstore::OpResult::OK);
      }
      db->CommitTransaction();

      spdlog::info("Run A: loaded {} records (no MVCC). Benchmarking...", N);
      res_a = MeasureRun("Run A  READ_UNCOMMITTED (no MVCC)", N, *db, *tree, "ru");
    });

    db->Shutdown();
  }

  // -------------------------------------------------------------------------
  // Run B: SERIALIZABLE / MVCC happy path (FLAGS_txn_mvcc = true)
  // Load with "si" so every tuple gets a real commit_ts stamped on it.
  // Reader starts after the load commit => start_ts > all tuple_ts =>
  // the happy path ("key visible") branch is always taken inside LookUp.
  // -------------------------------------------------------------------------
  FLAGS_txn_mvcc = true;
  {
    TempDB tmp_db;
    auto db = std::make_unique<leanstore::LeanStore>();

    // Same as Run A: RegisterTable must be called from the main thread.
    db->RegisterTable(std::type_index(typeid(char)), 0);
    auto *tree = reinterpret_cast<leanstore::storage::BTree *>(
                   db->RetrieveIndex(std::type_index(typeid(char))));

    db->worker_pool.ScheduleSyncJob(0, [&]() {
      // Load with "si" so CommitTransaction stamps every tuple with commit_ts
      // via UpdateTimestamp.  Any reader started after this commit will have
      // start_ts > all tuple_ts => MVCC happy path on every LookUp.
      u8 key_buf[KEY_SIZE], val_buf[PAYLOAD_SIZE];
      std::memset(val_buf, 0xAB, PAYLOAD_SIZE);
      for (u64 k = 0; k < N; ++k) {
        db->StartTransaction(0, leanstore::transaction::Transaction::Mode::OLTP, "si");
        EncodeKey(key_buf, k);
        assert(tree->Insert({key_buf, KEY_SIZE}, {val_buf, PAYLOAD_SIZE}) == leanstore::OpResult::OK);
        db->CommitTransaction();
      }
      spdlog::info("Run B: loaded {} records (MVCC). Benchmarking...", N);
      res_b = MeasureRun("Run B  SERIALIZABLE   (MVCC happy path)", N, *db, *tree, "ser");
    });

    db->Shutdown();
  }

  // -------------------------------------------------------------------------
  // Summary
  // -------------------------------------------------------------------------
  double overhead_abs = res_b.ns_per_lookup - res_a.ns_per_lookup;
  double overhead_pct = (res_b.ns_per_lookup / res_a.ns_per_lookup - 1.0) * 100.0;

  std::cout << std::string(70, '-') << "\n";
  std::cout << "MVCC happy-path overhead vs baseline:  "
            << std::fixed << std::setprecision(2)
            << overhead_abs << " ns/lookup  ("
            << overhead_pct << "%)\n\n";

  std::cout << "Note: Run C (forced version-chain traversal) not yet available --\n"
            << "      AppendVersion() is not yet wired in LockManager::ReleaseAllLocks().\n";
  return 0;
}