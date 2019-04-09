#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "catalog/catalog.h"
#include "catalog/catalog_defs.h"
#include "storage/sql_table.h"
#include "transaction/transaction_context.h"

namespace terrier::catalog {

class Catalog;
struct SchemaCol;

/**
 * An attribute handle provides accessors to the pg_attribute catalog.
 * Each database has it's own pg_attribute catalog.
 *
 * Following description verbatim from the Postgres documentation:
 * The catalog pg_attribute stores information about table columns.
 * There will be exactly one pg_attribute row for every column in every
 * table in the database. (There will also be attribute entries for indexes,
 * and indeed all objects that have pg_class entries.)
 *
 * The term attribute is equivalent to column and is used for historical
 * reasons.
 */

class AttributeHandle {
 public:
  /**
   * A attribute entry represent a row in pg_attribute catalog.
   */
  class AttributeEntry {
   public:
    /**
     * Constructs a attribute entry.
     * @param oid the col_oid of the attribute
     * @param entry: the row as a vector of values
     */
    AttributeEntry(col_oid_t oid, std::vector<type::TransientValue> &&entry) : oid_(oid), entry_(std::move(entry)) {}

    /**
     * Get the value for a given column
     * @param col_num the column index
     * @return the value of the column
     */
    const type::TransientValue &GetColumn(int32_t col_num) { return entry_[col_num]; }

    /**
     * Return the col_oid of the attribute
     * @return col_oid of the attribute
     */
    col_oid_t GetAttributeOid() { return oid_; }

   private:
    col_oid_t oid_;
    std::vector<type::TransientValue> entry_;
  };

  /**
   * Construct an attribute handle
   * @param catalog catalog ptr
   * @param pg_attribute a pointer to pg_attribute sql table rw helper instance
   */
  explicit AttributeHandle(Catalog *catalog, std::shared_ptr<catalog::SqlTableRW> pg_attribute)
      : pg_attribute_hrw_(std::move(pg_attribute)) {}

  /**
   * Construct an attribute handle. It keeps a pointer to the pg_attribute sql table.
   * @param table a pointer to SqlTableRW
   * @param pg_attribute a pointer to pg_attribute sql table rw helper instance
   */
  // TODO(pakhtar): deprecate
  explicit AttributeHandle(SqlTableRW *table, std::shared_ptr<catalog::SqlTableRW> pg_attribute)
      : table_(table), pg_attribute_hrw_(std::move(pg_attribute)) {}

  /**
   * Convert a attribute string to its oid representation
   * @param name the attribute
   * @param txn the transaction context
   * @return the col oid
   */
  col_oid_t NameToOid(transaction::TransactionContext *txn, const std::string &name);

  /**
   * Get an attribute entry.
   * @param txn transaction (required)
   * @param table_oid, an attribute for this table
   * @param col_oid, attribute for col_oid of table_oid
   * @return a shared pointer to Attribute entry; NULL if the attribute doesn't exist
   */
  std::shared_ptr<AttributeEntry> GetAttributeEntry(transaction::TransactionContext *txn, table_oid_t table_oid,
                                                   col_oid_t col_oid);

  /**
   * Get an attribute entry.
   * @param txn transaction (required)
   * @param table_oid, an attribute for this table
   * @param name, attribute for column name of table_oid
   * @return a shared pointer to Attribute entry;
   */
  std::shared_ptr<AttributeEntry> GetAttributeEntry(transaction::TransactionContext *txn, table_oid_t table_oid,
      const std::string &name);

  /**
   * Create the storage table
   */
  static std::shared_ptr<catalog::SqlTableRW> Create(transaction::TransactionContext *txn, Catalog *catalog,
                                                     db_oid_t db_oid, const std::string &name);

  // start Debug methods
  /**
   * Dump the contents of the table
   * @param txn
   */
  void Dump(transaction::TransactionContext *txn) {
    auto limit = static_cast<int32_t>(AttributeHandle::schema_cols_.size());
    pg_attribute_hrw_->Dump(txn, limit);
  }
  // end Debug methods

  /** Used schema columns */
  static const std::vector<SchemaCol> schema_cols_;
  /** Unused schema columns */
  static const std::vector<SchemaCol> unused_schema_cols_;

 private:
  // Catalog *catalog_;
  SqlTableRW *table_;
  std::shared_ptr<catalog::SqlTableRW> pg_attribute_hrw_;
};

}  // namespace terrier::catalog