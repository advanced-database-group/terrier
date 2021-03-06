#pragma once
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "catalog/schema.h"
#include "common/strong_typedef.h"
#include "gtest/gtest.h"
#include "storage/index/compact_ints_key.h"
#include "storage/index/index_defs.h"
#include "storage/sql_table.h"
#include "storage/storage_defs.h"
#include "storage/storage_util.h"
#include "storage/tuple_access_strategy.h"
#include "storage/undo_record.h"
#include "transaction/transaction_manager.h"
#include "type/type_id.h"
#include "util/multithread_test_util.h"
#include "util/random_test_util.h"

namespace terrier {
struct StorageTestUtil {
  StorageTestUtil() = delete;
  static void EmptyCallback(void * /*unused*/) {}

#define TO_INT(p) reinterpret_cast<uintptr_t>(p)
  /**
   * Check if memory address represented by val in [lower, upper)
   * @tparam A type of ptr
   * @tparam B type of ptr
   * @tparam C type of ptr
   * @param val value to check
   * @param lower lower bound
   * @param upper upper bound
   */
  template <typename A, typename B, typename C>
  static void CheckInBounds(A *const val, B *const lower, C *const upper) {
    EXPECT_GE(TO_INT(val), TO_INT(lower));
    EXPECT_LT(TO_INT(val), TO_INT(upper));
  }

  /**
   * Check if memory address represented by val not in [lower, upper)
   * @tparam A type of ptr
   * @tparam B type of ptr
   * @tparam C type of ptr
   * @param val value to check
   * @param lower lower bound
   * @param upper upper bound
   */
  template <typename A, typename B, typename C>
  static void CheckNotInBounds(A *const val, B *const lower, C *const upper) {
    EXPECT_TRUE(TO_INT(val) < TO_INT(lower) || TO_INT(val) >= TO_INT(upper));
  }

  template <typename A>
  static void CheckAlignment(A *const val, const uint32_t word_size) {
    EXPECT_EQ(0, TO_INT(val) % word_size);
  }
#undef TO_INT
  /**
   * @tparam A type of ptr
   * @param ptr ptr to start from
   * @param bytes bytes to advance
   * @return  pointer that is the specified amount of bytes ahead of the given
   */
  template <typename A>
  static A *IncrementByBytes(A *const ptr, const uint64_t bytes) {
    return reinterpret_cast<A *>(reinterpret_cast<byte *>(ptr) + bytes);
  }

  // Returns a random layout that is guaranteed to be valid.
  template <typename Random>
  static storage::BlockLayout RandomLayoutNoVarlen(const uint16_t max_cols, Random *const generator) {
    return RandomLayout(max_cols, generator, false);
  }

  template <typename Random>
  static storage::BlockLayout RandomLayoutWithVarlens(const uint16_t max_cols, Random *const generator) {
    return RandomLayout(max_cols, generator, true);
  }

  // Fill the given location with the specified amount of random bytes, using the
  // given generator as a source of randomness.
  template <typename Random>
  static void FillWithRandomBytes(const uint32_t num_bytes, byte *const out, Random *const generator) {
    std::uniform_int_distribution<uint8_t> dist(0, UINT8_MAX);
    for (uint32_t i = 0; i < num_bytes; i++) out[i] = static_cast<byte>(dist(*generator));
  }

  template <typename Random>
  static void PopulateRandomRow(storage::ProjectedRow *const row, const storage::BlockLayout &layout,
                                const double null_bias, Random *const generator) {
    std::bernoulli_distribution coin(1 - null_bias);
    // TODO(Tianyu): I don't think this matters as a tunable thing?
    // Make sure we have a mix of inlined and non-inlined values
    std::uniform_int_distribution<uint32_t> varlen_size(1, 2 * storage::VarlenEntry::InlineThreshold());
    // For every column in the project list, populate its attribute with random bytes or set to null based on coin flip
    for (uint16_t projection_list_idx = 0; projection_list_idx < row->NumColumns(); projection_list_idx++) {
      storage::col_id_t col = row->ColumnIds()[projection_list_idx];

      if (coin(*generator)) {
        if (layout.IsVarlen(col)) {
          uint32_t size = varlen_size(*generator);
          if (size > storage::VarlenEntry::InlineThreshold()) {
            byte *varlen = common::AllocationUtil::AllocateAligned(size);
            FillWithRandomBytes(size, varlen, generator);
            // varlen entries always start off not inlined
            *reinterpret_cast<storage::VarlenEntry *>(row->AccessForceNotNull(projection_list_idx)) =
                storage::VarlenEntry::Create(varlen, size, true);
          } else {
            byte buf[storage::VarlenEntry::InlineThreshold()];
            FillWithRandomBytes(size, buf, generator);
            *reinterpret_cast<storage::VarlenEntry *>(row->AccessForceNotNull(projection_list_idx)) =
                storage::VarlenEntry::CreateInline(buf, size);
          }
        } else {
          FillWithRandomBytes(layout.AttrSize(col), row->AccessForceNotNull(projection_list_idx), generator);
        }
      } else {
        row->SetNull(projection_list_idx);
      }
    }
  }

  static std::vector<storage::col_id_t> ProjectionListAllColumns(const storage::BlockLayout &layout) {
    std::vector<storage::col_id_t> col_ids(layout.NumColumns() - NUM_RESERVED_COLUMNS);
    // Add all of the column ids from the layout to the projection list
    // 0 is version vector so we skip it
    for (uint16_t col = NUM_RESERVED_COLUMNS; col < layout.NumColumns(); col++) {
      col_ids[col - NUM_RESERVED_COLUMNS] = storage::col_id_t(col);
    }
    return col_ids;
  }

  template <typename Random>
  static std::vector<storage::col_id_t> ProjectionListRandomColumns(const storage::BlockLayout &layout,
                                                                    Random *const generator) {
    // randomly select a number of columns for this delta to contain. Must be at least 1, but shouldn't be num_cols
    // since we exclude the version vector column
    uint16_t num_cols = std::uniform_int_distribution<uint16_t>(
        1, static_cast<uint16_t>(layout.NumColumns() - NUM_RESERVED_COLUMNS))(*generator);

    std::vector<storage::col_id_t> col_ids;
    // Add all of the column ids from the layout to the projection list
    // 0 is version vector so we skip it
    for (uint16_t col = NUM_RESERVED_COLUMNS; col < layout.NumColumns(); col++) col_ids.emplace_back(col);

    // permute the column ids for our random delta
    std::shuffle(col_ids.begin(), col_ids.end(), *generator);

    // truncate the projection list
    col_ids.resize(num_cols);

    return col_ids;
  }

  template <class Random>
  static std::unordered_map<storage::TupleSlot, storage::ProjectedRow *> PopulateBlockRandomly(
      const storage::BlockLayout &layout, storage::RawBlock *block, double empty_ratio, Random *const generator) {
    std::unordered_map<storage::TupleSlot, storage::ProjectedRow *> result;
    std::bernoulli_distribution coin(empty_ratio);
    // TODO(Tianyu): Do we ever want to tune this for tests?
    const double null_ratio = 0.1;
    storage::TupleAccessStrategy accessor(layout);  // Have to construct one since we don't have access to data table
    auto initializer =
        storage::ProjectedRowInitializer::Create(layout, StorageTestUtil::ProjectionListAllColumns(layout));
    for (uint32_t i = 0; i < layout.NumSlots(); i++) {
      storage::TupleSlot slot;
      bool ret UNUSED_ATTRIBUTE = accessor.Allocate(block, &slot);
      TERRIER_ASSERT(ret && slot == storage::TupleSlot(block, i),
                     "slot allocation should happen sequentially and succeed");
      if (coin(*generator)) {
        // slot will be marked empty
        accessor.Deallocate(slot);
        continue;
      }
      auto *redo_buffer = common::AllocationUtil::AllocateAligned(initializer.ProjectedRowSize());
      storage::ProjectedRow *redo = initializer.InitializeRow(redo_buffer);
      StorageTestUtil::PopulateRandomRow(redo, layout, null_ratio, generator);
      result[slot] = redo;
      // Copy without transactions to simulate a version-free block
      accessor.SetNotNull(slot, VERSION_POINTER_COLUMN_ID);
      for (uint16_t j = 0; j < redo->NumColumns(); j++)
        storage::StorageUtil::CopyAttrFromProjection(accessor, slot, *redo, j);
    }
    TERRIER_ASSERT(block->insert_head_ == layout.NumSlots(), "The block should be considered full at this point");
    return result;
  }

  template <class Random>
  static storage::ProjectedRowInitializer RandomInitializer(const storage::BlockLayout &layout, Random *generator) {
    return {layout, ProjectionListRandomColumns(layout, generator)};
  }

  // Returns true iff the underlying varlen is bit-wise identical. Compressions schemes and other metadata are ignored.
  static bool VarlenEntryEqualDeep(const storage::VarlenEntry &one, const storage::VarlenEntry &other) {
    if (one.Size() != other.Size()) return false;
    return memcmp(one.Content(), other.Content(), one.Size()) == 0;
  }

  template <class RowType1, class RowType2>
  static bool ProjectionListEqualDeep(const storage::BlockLayout &layout, const RowType1 *const one,
                                      const RowType2 *const other) {
    if (one->NumColumns() != other->NumColumns()) return false;
    for (uint16_t projection_list_index = 0; projection_list_index < one->NumColumns(); projection_list_index++) {
      // Check that the two point at the same column
      storage::col_id_t one_id = one->ColumnIds()[projection_list_index];
      storage::col_id_t other_id = other->ColumnIds()[projection_list_index];
      if (one_id != other_id) return false;

      // Check that the two have the same content bit-wise
      uint8_t attr_size = layout.AttrSize(one_id);
      const byte *one_content = one->AccessWithNullCheck(projection_list_index);
      const byte *other_content = other->AccessWithNullCheck(projection_list_index);
      // Either both are null or neither is null.

      if (one_content == nullptr || other_content == nullptr) {
        if (one_content == other_content) continue;
        return false;
      }

      if (layout.IsVarlen(one_id)) {
        // Need to follow pointers and throw away metadata and padding for equality comparison
        auto &one_entry = *reinterpret_cast<const storage::VarlenEntry *>(one_content),
             &other_entry = *reinterpret_cast<const storage::VarlenEntry *>(other_content);
        if (one_entry.Size() != other_entry.Size()) return false;
        if (memcmp(one_entry.Content(), other_entry.Content(), one_entry.Size()) != 0) return false;
      } else if (memcmp(one_content, other_content, attr_size) != 0) {
        // Otherwise, they should be bit-wise identical.
        return false;
      }
    }
    return true;
  }

  template <class RowType1, class RowType2>
  static bool ProjectionListEqualShallow(const storage::BlockLayout &layout, const RowType1 *const one,
                                         const RowType2 *const other) {
    EXPECT_EQ(one->NumColumns(), other->NumColumns());
    if (one->NumColumns() != other->NumColumns()) return false;
    for (uint16_t projection_list_index = 0; projection_list_index < one->NumColumns(); projection_list_index++) {
      // Check that the two point at the same column
      storage::col_id_t one_id = one->ColumnIds()[projection_list_index];
      storage::col_id_t other_id = other->ColumnIds()[projection_list_index];
      EXPECT_EQ(one_id, other_id);
      if (one_id != other_id) return false;

      // Check that the two have the same content bit-wise
      uint8_t attr_size = layout.AttrSize(one_id);
      const byte *one_content = one->AccessWithNullCheck(projection_list_index);
      const byte *other_content = other->AccessWithNullCheck(projection_list_index);
      // Either both are null or neither is null.
      if (one_content == nullptr || other_content == nullptr) {
        EXPECT_EQ(one_content, other_content);
        if (one_content == other_content) continue;
        return false;
      }
      // Otherwise, they should be bit-wise identical.
      if (memcmp(one_content, other_content, attr_size) != 0) return false;
    }
    return true;
  }

  template <class RowType>
  static std::string PrintRow(const RowType &row, const storage::BlockLayout &layout, bool varlen_pointer = true) {
    std::ostringstream os;
    os << "num_cols: " << row.NumColumns() << std::endl;
    for (uint16_t i = 0; i < row.NumColumns(); i++) {
      storage::col_id_t col_id = row.ColumnIds()[i];
      const byte *attr = row.AccessWithNullCheck(i);
      if (attr == nullptr) {
        os << "col_id: " << !col_id << " is NULL" << std::endl;
        continue;
      }

      if (layout.IsVarlen(col_id)) {
        auto *entry = reinterpret_cast<const storage::VarlenEntry *>(attr);
        os << "col_id: " << !col_id;
        os << " is varlen";
        if (varlen_pointer) {
          os << ", ptr " << entry->Content();
        }
        os << ", size " << entry->Size();
        os << ", reclaimable " << entry->NeedReclaim();
        os << ", content ";
        for (uint8_t pos = 0; pos < entry->Size(); pos++) {
          os << std::setfill('0') << std::setw(2) << std::hex << +static_cast<uint8_t>(entry->Content()[pos]);
        }
        os << std::endl;
      } else {
        os << "col_id: " << !col_id;
        os << " is ";
        for (uint8_t pos = 0; pos < layout.AttrSize(col_id); pos++) {
          os << std::setfill('0') << std::setw(2) << std::hex << +static_cast<uint8_t>(attr[pos]);
        }
        os << std::endl;
      }
    }
    return os.str();
  }

  template <class RowType>
  static std::string PrintRowWithSchema(const RowType &row, const catalog::Schema &schema,
                                        const storage::ProjectionMap &map, bool varlen_pointer = true) {
    std::ostringstream os;
    auto &columns = schema.GetColumns();
    os << "num_cols: " << columns.size() << std::endl;
    for (uint16_t i = 0; i < columns.size(); i++) {
      const storage::col_id_t col_id = row.ColumnIds()[i];
      const byte *attr = row.AccessWithNullCheck(map.at(columns[i].GetOid()));
      if (attr == nullptr) {
        os << "col_id: " << !col_id << " is NULL" << std::endl;
        continue;
      }

      if (columns[i].IsVarlen()) {
        auto *entry = reinterpret_cast<const storage::VarlenEntry *>(attr);
        os << "col_id: " << !col_id;
        os << " is varlen";
        if (varlen_pointer) {
          os << ", ptr " << entry->Content();
        }
        os << ", size " << entry->Size();
        os << ", reclaimable " << entry->NeedReclaim();
        os << ", content ";
        for (uint8_t pos = 0; pos < entry->Size(); pos++) {
          os << std::setfill('0') << std::setw(2) << std::hex << +static_cast<uint8_t>(entry->Content()[pos]);
        }
        os << std::endl;
      } else {
        os << "col_id: " << !col_id;
        os << " is ";
        for (uint8_t pos = 0; pos < columns[i].GetAttrSize(); pos++) {
          os << std::setfill('0') << std::setw(2) << std::hex << +static_cast<uint8_t>(attr[pos]);
        }
        os << std::endl;
      }
    }
    return os.str();
  }

  // print all rows in a SqlTable, put print outputs into a vector.
  static void PrintAllRows(transaction::TransactionContext *txn, storage::SqlTable *table,
                           std::vector<std::string> *set) {
    const catalog::Schema schema = table->GetSchema();
    std::vector<catalog::col_oid_t> all_col;
    for (auto &column : schema.GetColumns()) {
      all_col.emplace_back(column.GetOid());
    }
    uint32_t max_tuples = 100;

    auto column_initializer_pair = table->InitializerForProjectedColumns(all_col, max_tuples);
    auto *scan_buffer = common::AllocationUtil::AllocateAligned(column_initializer_pair.first.ProjectedColumnsSize());
    storage::ProjectedColumns *columns = column_initializer_pair.first.Initialize(scan_buffer);
    auto &proj_map = column_initializer_pair.second;

    auto it = table->begin();
    auto end = table->end();
    while (it != end) {
      table->Scan(txn, &it, columns);
      uint32_t num_tuples = columns->NumTuples();
      for (uint32_t off = 0; off < num_tuples; off++) {
        storage::ProjectedColumns::RowView row = columns->InterpretAsRow(off);
        set->push_back(PrintRowWithSchema(row, table->GetSchema(), proj_map, false));
      }
    }
    delete[] scan_buffer;
  }

  // Write the given tuple (projected row) into a block using the given access strategy,
  // at the specified offset
  static void InsertTuple(const storage::ProjectedRow &tuple, const storage::TupleAccessStrategy &tested,
                          const storage::BlockLayout &layout, const storage::TupleSlot slot) {
    // Skip the version vector for tuples
    for (uint16_t projection_list_index = 0; projection_list_index < tuple.NumColumns(); projection_list_index++) {
      storage::col_id_t col_id(static_cast<uint16_t>(projection_list_index + NUM_RESERVED_COLUMNS));
      const byte *val_ptr = tuple.AccessWithNullCheck(projection_list_index);
      if (val_ptr == nullptr)
        tested.SetNull(slot, storage::col_id_t(projection_list_index));
      else
        std::memcpy(tested.AccessForceNotNull(slot, col_id), val_ptr, layout.AttrSize(col_id));
    }
  }

  // Check that the written tuple is the same as the expected one. Does not follow varlen pointers to check that the
  // underlying values are the same
  static void CheckTupleEqualShallow(const storage::ProjectedRow &expected, const storage::TupleAccessStrategy &tested,
                                     const storage::BlockLayout &layout, const storage::TupleSlot slot) {
    for (uint16_t col = NUM_RESERVED_COLUMNS; col < layout.NumColumns(); col++) {
      storage::col_id_t col_id(col);
      const byte *val_ptr = expected.AccessWithNullCheck(static_cast<uint16_t>(col - NUM_RESERVED_COLUMNS));
      byte *col_slot = tested.AccessWithNullCheck(slot, col_id);
      if (val_ptr == nullptr) {
        EXPECT_TRUE(col_slot == nullptr);
      } else {
        EXPECT_TRUE(!memcmp(val_ptr, col_slot, layout.AttrSize(col_id)));
      }
    }
  }

  template <class Random>
  static catalog::Schema GenerateRandomSchema(int num_cols, Random *generator, bool varlen_allowed) {
    std::string prefix = "col_";
    std::vector<type::TypeId> types = varlen_allowed
                                          ? std::vector<type::TypeId>{type::TypeId::INTEGER, type::TypeId::VARCHAR}
                                          : std::vector<type::TypeId>{type::TypeId::INTEGER};
    std::vector<catalog::Schema::Column> cols;
    for (int i = 0; i < num_cols; i++) {
      type::TypeId type = *RandomTestUtil::UniformRandomElement(&types, generator);
      switch (type) {
        case type::TypeId::VARCHAR:
        case type::TypeId::VARBINARY:  // varlen entries
          cols.emplace_back(prefix + std::to_string(uint8_t(type)), type, 2 * storage::VarlenEntry::InlineThreshold(),
                            true, catalog::col_oid_t(i));
          break;
        default:
          cols.emplace_back(prefix + std::to_string(uint8_t(type)), type, true, catalog::col_oid_t(i));
          break;
      }
    }
    return catalog::Schema(cols);
  }

  /**
   * Generates a random GenericKey-compatible schema with the given number of columns using the given types.
   */
  template <typename Random>
  static storage::index::IndexKeySchema RandomGenericKeySchema(const uint32_t num_cols,
                                                               const std::vector<type::TypeId> &types,
                                                               Random *generator) {
    uint32_t max_varlen_size = 20;
    TERRIER_ASSERT(num_cols > 0, "Must have at least one column in your key schema.");

    std::vector<catalog::indexkeycol_oid_t> key_oids;
    key_oids.reserve(num_cols);

    for (uint32_t i = 0; i < num_cols; i++) {
      key_oids.emplace_back(i);
    }

    std::shuffle(key_oids.begin(), key_oids.end(), *generator);

    storage::index::IndexKeySchema key_schema;

    for (uint32_t i = 0; i < num_cols; i++) {
      auto key_oid = key_oids[i];
      auto type = *RandomTestUtil::UniformRandomElement(types, generator);
      auto is_nullable = static_cast<bool>(std::uniform_int_distribution(0, 1)(*generator));

      switch (type) {
        case type::TypeId::VARBINARY:
        case type::TypeId::VARCHAR: {
          auto varlen_size = std::uniform_int_distribution(0u, max_varlen_size)(*generator);
          key_schema.emplace_back(key_oid, type, is_nullable, varlen_size);
          break;
        }
        default:
          key_schema.emplace_back(key_oid, type, is_nullable);
          break;
      }
    }

    return key_schema;
  }

  /**
   * Generates a random CompactIntsKey-compatible schema.
   */
  template <typename Random>
  static storage::index::IndexKeySchema RandomCompactIntsKeySchema(Random *generator) {
    const uint16_t max_bytes = sizeof(uint64_t) * INTSKEY_MAX_SLOTS;
    const auto key_size = std::uniform_int_distribution(static_cast<uint16_t>(1), max_bytes)(*generator);

    const std::vector<type::TypeId> types{type::TypeId::TINYINT, type::TypeId::SMALLINT, type::TypeId::INTEGER,
                                          type::TypeId::BIGINT};  // has to be sorted in ascending type size order

    const uint16_t max_cols = max_bytes;  // could have up to max_bytes TINYINTs
    std::vector<catalog::indexkeycol_oid_t> key_oids;
    key_oids.reserve(max_cols);

    for (auto i = 0; i < max_cols; i++) {
      key_oids.emplace_back(i);
    }

    std::shuffle(key_oids.begin(), key_oids.end(), *generator);

    storage::index::IndexKeySchema key_schema;

    uint8_t col = 0;

    for (uint16_t bytes_used = 0; bytes_used != key_size;) {
      auto max_offset = static_cast<uint8_t>(types.size() - 1);
      for (const auto &type : types) {
        if (key_size - bytes_used < type::TypeUtil::GetTypeSize(type)) {
          max_offset--;
        }
      }
      const uint8_t type_offset = std::uniform_int_distribution(static_cast<uint8_t>(0), max_offset)(*generator);
      const auto type = types[type_offset];

      key_schema.emplace_back(key_oids[col++], type, false);
      bytes_used = static_cast<uint16_t>(bytes_used + type::TypeUtil::GetTypeSize(type));
    }

    return key_schema;
  }

 private:
  template <typename Random>
  static storage::BlockLayout RandomLayout(const uint16_t max_cols, Random *const generator, bool allow_varlen) {
    TERRIER_ASSERT(max_cols > NUM_RESERVED_COLUMNS, "There should be at least 2 cols (reserved for version).");
    // We probably won't allow tables with fewer than 2 columns
    const uint16_t num_attrs = std::uniform_int_distribution<uint16_t>(NUM_RESERVED_COLUMNS + 1, max_cols)(*generator);
    std::vector<uint8_t> possible_attr_sizes{1, 2, 4, 8}, attr_sizes(num_attrs);
    if (allow_varlen) possible_attr_sizes.push_back(VARLEN_COLUMN);

    for (uint16_t i = 0; i < NUM_RESERVED_COLUMNS; i++) {
      attr_sizes[i] = 8;
    }

    for (uint16_t i = NUM_RESERVED_COLUMNS; i < num_attrs; i++)
      attr_sizes[i] = *RandomTestUtil::UniformRandomElement(&possible_attr_sizes, generator);
    return storage::BlockLayout(attr_sizes);
  }
};

class RandomSqlTableTestObject {
 public:
  RandomSqlTableTestObject() = default;

  /**
   * Generate random columns, and add them sequencially to the internal list.
   * @param num_cols number of columns to add.
   * @param varlen_allowed allow VARCHAR to be used in columns.
   * @param generator of random numbers.
   */
  template <class Random>
  void GenerateRandomColumns(int num_cols, bool varlen_allowed, Random *generator) {
    std::string prefix = "col_";
    std::vector<type::TypeId> types = DataTypeAll(varlen_allowed);
    for (int i = 0; i < num_cols; i++) {
      type::TypeId type = *RandomTestUtil::UniformRandomElement(&types, generator);
      DefineColumn(prefix + std::to_string(uint8_t(type)), type, true, catalog::col_oid_t(i));
    }
  }

  /**
   * Append a column definition to the internal list. The list will be
   * used when creating the SqlTable.
   * @param name of the column
   * @param type of the column
   * @param nullable
   * @param oid for the column
   */
  void DefineColumn(std::string name, type::TypeId type, bool nullable, catalog::col_oid_t oid) {
    switch (type) {
      case type::TypeId::VARCHAR:
      case type::TypeId::VARBINARY:  // varlen entries
        cols_.emplace_back(name, type, 2 * storage::VarlenEntry::InlineThreshold(), nullable, oid);
        break;
      default:
        cols_.emplace_back(name, type, nullable, oid);
        break;
    }
  }

  /**
   * Create the SQL table.
   */
  void Create(catalog::table_oid_t table_oid) {
    schema_ = new catalog::Schema(cols_);
    table_ = new storage::SqlTable(&block_store_, *schema_, table_oid);

    for (const auto &c : cols_) {
      col_oids_.emplace_back(c.GetOid());
    }

    // save information needed for (later) reading and writing
    auto row_pair = table_->InitializerForProjectedRow(col_oids_);
    pri_ = new storage::ProjectedRowInitializer(std::get<0>(row_pair));
    pr_map_ = new storage::ProjectionMap(std::get<1>(row_pair));
  }

  template <class Random>
  void InsertRandomRow(transaction::TransactionContext *txn, const double null_bias, Random *generator) {
    std::bernoulli_distribution coin(1 - null_bias);
    std::uniform_int_distribution<uint32_t> varlen_size(1, 2 * storage::VarlenEntry::InlineThreshold());
    std::uniform_int_distribution<uint8_t> char_dist(0, UINT8_MAX);
    std::uniform_int_distribution<int32_t> int_dist(INT32_MIN, INT32_MAX);

    auto insert_buffer = common::AllocationUtil::AllocateAligned(pri_->ProjectedRowSize());
    auto insert = pri_->InitializeRow(insert_buffer);

    for (int i = 0; i < static_cast<int>(cols_.size()); i++) {
      if (coin(*generator)) {  // not null
        uint16_t offset = pr_map_->at(col_oids_[i]);
        insert->SetNotNull(offset);
        auto col = cols_[i];
        byte *col_p = insert->AccessForceNotNull(offset);
        uint32_t size;
        switch (col.GetType()) {
          case type::TypeId::INTEGER:
            (*reinterpret_cast<uint32_t *>(col_p)) = int_dist(*generator);
            break;
          case type::TypeId::VARCHAR:
            size = varlen_size(*generator);
            if (size > storage::VarlenEntry::InlineThreshold()) {
              byte *varlen = common::AllocationUtil::AllocateAligned(size);
              StorageTestUtil::FillWithRandomBytes(size, varlen, generator);
              // varlen entries always start off not inlined
              *reinterpret_cast<storage::VarlenEntry *>(col_p) = storage::VarlenEntry::Create(varlen, size, true);
            } else {
              byte buf[storage::VarlenEntry::InlineThreshold()];
              StorageTestUtil::FillWithRandomBytes(size, buf, generator);
              *reinterpret_cast<storage::VarlenEntry *>(col_p) = storage::VarlenEntry::CreateInline(buf, size);
            }
            break;
          default:
            break;
        }
      } else {  // null
        insert->SetNull(pr_map_->at(col_oids_[i]));
      }
    }

    table_->Insert(txn, *insert);
    delete[] insert_buffer;
  }

  template <class Random>
  void InsertRandomRows(const int num_rows, const double null_bias, Random *generator) {
    auto txn = txn_manager_.BeginTransaction();
    for (int i = 0; i < num_rows; i++) {
      InsertRandomRow(txn, null_bias, generator);
    }
    txn_manager_.Commit(txn, StorageTestUtil::EmptyCallback, nullptr);
    delete txn;
  }

  storage::SqlTable *GetTable() { return table_; }

  transaction::TransactionManager *GetTxnManager() { return &txn_manager_; }

  catalog::Schema *GetSchema() { return schema_; }

  /**
   * Generate a random sqlTable, and return the table with its schema. These 2 objects need to be freed manually.
   * @tparam Random Random generator class
   * @param num_cols number of columns in the table
   * @param varlen_allowed whether the table contains varlen columns
   * @param generator Random generator object
   * @param num_rows number of row in the table
   * @param null_bias bias towards null value.
   * @return a pair of SqlTable and its Schema object.
   */
  template <class Random>
  std::pair<storage::SqlTable *, catalog::Schema *> GenerateAndPopulateRandomTable(int num_cols, bool varlen_allowed,
                                                                                   Random *generator,
                                                                                   const int num_rows,
                                                                                   const double null_bias) {
    cols_.clear();
    col_oids_.clear();
    GenerateRandomColumns(num_cols, varlen_allowed, generator);
    Create(catalog::table_oid_t(table_oid_));
    table_oid_++;
    InsertRandomRows(num_rows, null_bias, generator);

    delete pri_;
    delete pr_map_;

    return {table_, schema_};
  }

 private:
  static std::vector<type::TypeId> DataTypeAll(bool varlen_allowed) {
    if (varlen_allowed) return {type::TypeId::INTEGER, type::TypeId::VARCHAR};
    return {type::TypeId::INTEGER};
  }

  storage::RecordBufferSegmentPool buffer_pool_{10000, 10000};
  transaction::TransactionManager txn_manager_ = {&buffer_pool_, true, LOGGING_DISABLED};

  storage::BlockStore block_store_{10000, 10000};
  storage::SqlTable *table_ = nullptr;
  uint32_t table_oid_ = 1;

  catalog::Schema *schema_ = nullptr;
  std::vector<catalog::Schema::Column> cols_;
  std::vector<catalog::col_oid_t> col_oids_;

  storage::ProjectedRowInitializer *pri_ = nullptr;
  storage::ProjectionMap *pr_map_ = nullptr;
};

}  // namespace terrier
