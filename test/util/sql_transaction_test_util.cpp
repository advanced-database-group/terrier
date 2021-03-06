#include "util/sql_transaction_test_util.h"
#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>
#include "common/allocator.h"
#include "transaction/transaction_util.h"

namespace terrier {
SqlRandomWorkloadTransaction::SqlRandomWorkloadTransaction(SqlLargeTransactionTestObject *test_object)
    : test_object_(test_object),
      txn_(test_object->txn_manager_.BeginTransaction()),
      aborted_(false),
      start_time_(txn_->StartTime()),
      commit_time_(UINT64_MAX),
      buffer_(test_object->bookkeeping_
                  ? nullptr
                  : common::AllocationUtil::AllocateAligned(test_object->row_initializer_.ProjectedRowSize())) {}

SqlRandomWorkloadTransaction::~SqlRandomWorkloadTransaction() {
  if (!test_object_->gc_on_) delete txn_;
  if (!test_object_->bookkeeping_) delete[] buffer_;
  for (auto &entry : updates_) delete[] reinterpret_cast<byte *>(entry.second);
  for (auto &entry : selects_) delete[] reinterpret_cast<byte *>(entry.second);
}

template <class Random>
void SqlRandomWorkloadTransaction::RandomUpdate(Random *generator) {
  if (aborted_) return;
  storage::TupleSlot updated =
      RandomTestUtil::UniformRandomElement(test_object_->last_checked_version_, generator)->first;
  if (test_object_->bookkeeping_) {
    auto it = updates_.find(updated);
    // don't double update if checking for correctness, as it is complicated to keep track of on snapshots,
    // and not very helpful in finding bugs anyways
    if (it != updates_.end()) return;
  }

  std::vector<storage::col_id_t> update_col_ids =
      StorageTestUtil::ProjectionListRandomColumns(test_object_->layout_, generator);
  storage::ProjectedRowInitializer initializer =
      storage::ProjectedRowInitializer::Create(test_object_->layout_, update_col_ids);
  auto *update_buffer =
      test_object_->bookkeeping_ ? common::AllocationUtil::AllocateAligned(initializer.ProjectedRowSize()) : buffer_;
  storage::ProjectedRow *update = initializer.InitializeRow(update_buffer);

  StorageTestUtil::PopulateRandomRow(update, test_object_->layout_, 0.0, generator);

  if (test_object_->bookkeeping_) updates_[updated] = update;

  // TODO(Tianyu): Hardly efficient, but will do for testing.
  if (test_object_->wal_on_ || test_object_->bookkeeping_) {
    auto *record = txn_->StageWrite(test_object_->table_, updated, initializer);
    std::memcpy(reinterpret_cast<void *>(record->Delta()), update, update->Size());
  }
  auto result = test_object_->table_->Update(txn_, updated, *update);
  aborted_ = !result;
}

template <class Random>
void SqlRandomWorkloadTransaction::RandomSelect(Random *generator) {
  if (aborted_) return;
  storage::TupleSlot selected =
      RandomTestUtil::UniformRandomElement(test_object_->last_checked_version_, generator)->first;
  auto *select_buffer = test_object_->bookkeeping_
                            ? common::AllocationUtil::AllocateAligned(test_object_->row_initializer_.ProjectedRowSize())
                            : buffer_;
  storage::ProjectedRow *select = test_object_->row_initializer_.InitializeRow(select_buffer);
  test_object_->table_->Select(txn_, selected, select);
  if (test_object_->bookkeeping_) {
    auto updated = updates_.find(selected);
    // Only track reads whose value depend on the snapshot
    if (updated == updates_.end())
      selects_.emplace_back(selected, select);
    else
      delete[] select_buffer;
  }
}

void SqlRandomWorkloadTransaction::Finish() {
  if (aborted_)
    test_object_->txn_manager_.Abort(txn_);
  else
    commit_time_ = test_object_->txn_manager_.Commit(txn_, SqlTestCallbacks::EmptyCallback, nullptr);
}

SqlLargeTransactionTestObject::SqlLargeTransactionTestObject(
    uint16_t max_columns, uint32_t initial_table_size, uint32_t txn_length, std::vector<double> update_select_ratio,
    storage::BlockStore *block_store, storage::RecordBufferSegmentPool *buffer_pool,
    std::default_random_engine *generator, bool gc_on, bool bookkeeping, storage::LogManager *log_manager,
    bool varlen_allowed)
    : txn_length_(txn_length),
      update_select_ratio_(std::move(update_select_ratio)),
      generator_(generator),
      schema_(StorageTestUtil::GenerateRandomSchema(max_columns, generator_, varlen_allowed)),
      sql_table_(block_store, schema_, catalog::table_oid_t(0)),
      table_(sql_table_.get_data_table()),
      layout_(sql_table_.get_layout()),
      txn_manager_(buffer_pool, gc_on, log_manager),
      gc_on_(gc_on),
      wal_on_(log_manager != LOGGING_DISABLED),
      bookkeeping_(bookkeeping) {
  // Bootstrap the table to have the specified number of tuples
  PopulateInitialTable(initial_table_size, generator_);
}

SqlLargeTransactionTestObject::~SqlLargeTransactionTestObject() {
  if (!gc_on_) delete initial_txn_;
  if (bookkeeping_) {
    for (auto &tuple : last_checked_version_) delete[] reinterpret_cast<byte *>(tuple.second);
  }
}

// Caller is responsible for freeing the returned results if bookkeeping is on.
SqlSimulationResult SqlLargeTransactionTestObject::SimulateOltp(uint32_t num_transactions,
                                                                uint32_t num_concurrent_txns) {
  std::vector<SqlRandomWorkloadTransaction *> txns;
  std::function<void(uint32_t)> workload;
  std::atomic<uint32_t> txns_run = 0;
  if (gc_on_ && !bookkeeping_) {
    // Then there is no need to keep track of RandomWorkloadTransaction objects
    workload = [&](uint32_t) {
      for (uint32_t txn_id = txns_run++; txn_id < num_transactions; txn_id = txns_run++) {
        SqlRandomWorkloadTransaction txn(this);
        SimulateOneTransaction(&txn, txn_id);
      }
    };
  } else {
    txns.resize(num_transactions);
    // Either for correctness checking, or to cleanup memory afterwards, we need to retain these
    // test objects
    workload = [&](uint32_t) {
      for (uint32_t txn_id = txns_run++; txn_id < num_transactions; txn_id = txns_run++) {
        txns[txn_id] = new SqlRandomWorkloadTransaction(this);
        SimulateOneTransaction(txns[txn_id], txn_id);
        //
        if (gc_on_ && !bookkeeping_) delete txns[txn_id];
      }
    };
  }
  common::WorkerPool thread_pool(num_concurrent_txns, {});
  MultiThreadTestUtil::RunThreadsUntilFinish(&thread_pool, num_concurrent_txns, workload);

  if (!bookkeeping_) {
    // We only need to deallocate, and return, if gc is on, this loop is a no-op
    for (SqlRandomWorkloadTransaction *txn : txns) delete txn;
    // This result is meaningless if bookkeeping is not turned on.
    return {{}, {}};
  }
  // filter out aborted transactions
  std::vector<SqlRandomWorkloadTransaction *> committed, aborted;
  for (SqlRandomWorkloadTransaction *txn : txns) (txn->aborted_ ? aborted : committed).push_back(txn);

  // Sort according to commit timestamp (Although we probably already are? Never hurts to be sure)
  std::sort(committed.begin(), committed.end(), [](SqlRandomWorkloadTransaction *a, SqlRandomWorkloadTransaction *b) {
    return transaction::TransactionUtil::NewerThan(b->commit_time_, a->commit_time_);
  });
  return {committed, aborted};
}

void SqlLargeTransactionTestObject::CheckReadsCorrect(std::vector<SqlRandomWorkloadTransaction *> *commits) {
  TERRIER_ASSERT(bookkeeping_, "Cannot check for correctness with bookkeeping off");
  // If nothing commits, then all our reads are vacuously correct
  if (commits->empty()) return;
  VersionedSnapshots snapshots = ReconstructVersionedTable(commits);
  // make sure table_version is updated
  transaction::timestamp_t latest_version = commits->at(commits->size() - 1)->commit_time_;
  // Only need to check that reads make sense?
  for (SqlRandomWorkloadTransaction *txn : *commits) CheckTransactionReadCorrect(txn, snapshots);

  // clean up memory, update the kept version to be the latest.
  for (auto &snapshot : snapshots) {
    if (snapshot.first == latest_version) {
      UpdateLastCheckedVersion(snapshot.second);
    } else {
      for (auto &entry : snapshot.second) delete[] reinterpret_cast<byte *>(entry.second);
    }
  }
}

void SqlLargeTransactionTestObject::SimulateOneTransaction(terrier::SqlRandomWorkloadTransaction *txn,
                                                           uint32_t txn_id) {
  std::default_random_engine thread_generator(txn_id);

  auto update = [&] { txn->RandomUpdate(&thread_generator); };
  auto select = [&] { txn->RandomSelect(&thread_generator); };
  RandomTestUtil::InvokeWorkloadWithDistribution({update, select}, update_select_ratio_, &thread_generator,
                                                 txn_length_);
  txn->Finish();
}

template <class Random>
void SqlLargeTransactionTestObject::PopulateInitialTable(uint32_t num_tuples, Random *generator) {
  initial_txn_ = txn_manager_.BeginTransaction();
  byte *redo_buffer = nullptr;
  if (!bookkeeping_) {
    // If no bookkeeping is required we can reuse the same buffer over and over again.
    redo_buffer = common::AllocationUtil::AllocateAligned(row_initializer_.ProjectedRowSize());
    row_initializer_.InitializeRow(redo_buffer);
  }
  for (uint32_t i = 0; i < num_tuples; i++) {
    // Otherwise we will need to get a new redo buffer each insert so we log the values inserted.
    if (bookkeeping_) redo_buffer = common::AllocationUtil::AllocateAligned(row_initializer_.ProjectedRowSize());
    // reinitialize every time only if bookkeeping;
    storage::ProjectedRow *redo = bookkeeping_ ? row_initializer_.InitializeRow(redo_buffer)
                                               : reinterpret_cast<storage::ProjectedRow *>(redo_buffer);
    StorageTestUtil::PopulateRandomRow(redo, layout_, 0.0, generator);
    storage::TupleSlot inserted = table_->Insert(initial_txn_, *redo);
    // TODO(Tianyu): Hardly efficient, but will do for testing.
    if (wal_on_ || bookkeeping_) {
      auto *record = initial_txn_->StageWrite(table_, inserted, row_initializer_);
      std::memcpy(reinterpret_cast<void *>(record->Delta()), redo, redo->Size());
    }
    last_checked_version_.emplace_back(inserted, bookkeeping_ ? redo : nullptr);
  }
  txn_manager_.Commit(initial_txn_, SqlTestCallbacks::EmptyCallback, nullptr);
  // cleanup if not keeping track of all the inserts.
  if (!bookkeeping_) delete[] redo_buffer;
}

storage::ProjectedRow *SqlLargeTransactionTestObject::CopyTuple(storage::ProjectedRow *other) {
  auto *copy = common::AllocationUtil::AllocateAligned(other->Size());
  std::memcpy(copy, other, other->Size());
  return reinterpret_cast<storage::ProjectedRow *>(copy);
}

void SqlLargeTransactionTestObject::UpdateSnapshot(SqlRandomWorkloadTransaction *txn, TableSnapshot *curr,
                                                   const TableSnapshot &before) {
  for (auto &entry : before) curr->emplace(entry.first, CopyTuple(entry.second));
  for (auto &update : txn->updates_) {
    // TODO(Tianyu): Can be smarter about copies
    storage::ProjectedRow *new_version = (*curr)[update.first];
    storage::StorageUtil::ApplyDelta(layout_, *update.second, new_version);
  }
}

VersionedSnapshots SqlLargeTransactionTestObject::ReconstructVersionedTable(
    std::vector<SqlRandomWorkloadTransaction *> *txns) {
  VersionedSnapshots result;
  // empty starting version
  TableSnapshot *prev = &(result.emplace(transaction::timestamp_t(0), TableSnapshot()).first->second);
  // populate with initial image of the table
  for (auto &entry : last_checked_version_) (*prev)[entry.first] = CopyTuple(entry.second);

  for (SqlRandomWorkloadTransaction *txn : *txns) {
    auto ret = result.emplace(txn->commit_time_, TableSnapshot());
    UpdateSnapshot(txn, &(ret.first->second), *prev);
    prev = &(ret.first->second);
  }
  return result;
}

void SqlLargeTransactionTestObject::CheckTransactionReadCorrect(SqlRandomWorkloadTransaction *txn,
                                                                const VersionedSnapshots &snapshots) {
  transaction::timestamp_t start_time = txn->start_time_;
  // this version is the most recent future update
  auto ret = snapshots.upper_bound(start_time);
  // Go to the version visible to this txn
  --ret;
  transaction::timestamp_t version_timestamp = ret->first;
  const TableSnapshot &before_snapshot = ret->second;
  EXPECT_TRUE(transaction::TransactionUtil::NewerThan(start_time, version_timestamp));
  for (auto &entry : txn->selects_) {
    auto it = before_snapshot.find(entry.first);
    EXPECT_TRUE(StorageTestUtil::ProjectionListEqualShallow(layout_, entry.second, it->second));
  }
}

void SqlLargeTransactionTestObject::UpdateLastCheckedVersion(const TableSnapshot &snapshot) {
  for (auto &entry : last_checked_version_) {
    delete[] reinterpret_cast<byte *>(entry.second);
    entry.second = snapshot.find(entry.first)->second;
  }
}

SqlLargeTransactionTestObject SqlLargeTransactionTestObject::Builder::build() {
  return {builder_max_columns_, builder_initial_table_size_, builder_txn_length_, builder_update_select_ratio_,
          builder_block_store_, builder_buffer_pool_,        builder_generator_,  builder_gc_on_,
          builder_bookkeeping_, builder_log_manager_,        varlen_allowed_};
}

}  // namespace terrier
