#include "storage/sql_table.h"
#include <algorithm>
#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>
#include "catalog/catalog.h"
#include "catalog/catalog_sql_table.h"
#include "catalog/database_handle.h"
#include "transaction/transaction_manager.h"
#include "util/test_harness.h"
#include "util/transaction_test_util.h"
namespace terrier {

struct SqlTableTests : public TerrierTest {
  void SetUp() override { TerrierTest::SetUp(); }

  void TearDown() override { TerrierTest::TearDown(); }

  storage::RecordBufferSegmentPool buffer_pool_{100, 100};
  transaction::TransactionManager txn_manager_ = {&buffer_pool_, true, LOGGING_DISABLED};
};

// NOLINTNEXTLINE
TEST_F(SqlTableTests, DISABLED_SelectInsertTest) {
  catalog::SqlTableRW table(catalog::table_oid_t(2));

  auto txn = txn_manager_.BeginTransaction();
  table.DefineColumn("id", type::TypeId::INTEGER, false, catalog::col_oid_t(0));
  table.DefineColumn("datname", type::TypeId::INTEGER, false, catalog::col_oid_t(1));
  table.Create();
  table.StartRow();
  table.SetIntColInRow(0, 100);
  table.SetIntColInRow(1, 15721);
  table.EndRowAndInsert(txn);

  table.StartRow();
  table.SetIntColInRow(0, 200);
  table.SetIntColInRow(1, 25721);
  table.EndRowAndInsert(txn);

  // This operation is slow, due to how sequential scan is done for a datatable.
  // auto num_rows = table.GetNumRows();
  // EXPECT_EQ(2, num_rows);

  auto row_p = table.FindRow(txn, 0, 100);
  uint32_t id = table.GetIntColInRow(0, row_p);
  EXPECT_EQ(100, id);
  uint32_t datname = table.GetIntColInRow(1, row_p);
  EXPECT_EQ(15721, datname);
  // leaks the row_buffer_

  row_p = table.FindRow(txn, 0, 200);
  id = table.GetIntColInRow(0, row_p);
  EXPECT_EQ(200, id);
  datname = table.GetIntColInRow(1, row_p);
  EXPECT_EQ(25721, datname);
  // leaks the row_buffer_

  txn_manager_.Commit(txn, TestCallbacks::EmptyCallback, nullptr);
  delete txn;
}

/**
 * Insertion test, with content verification using the Value vector calls
 */
// NOLINTNEXTLINE
TEST_F(SqlTableTests, SelectInsertTest1) {
  catalog::SqlTableRW table(catalog::table_oid_t(2));

  auto txn = txn_manager_.BeginTransaction();
  table.DefineColumn("id", type::TypeId::INTEGER, false, catalog::col_oid_t(0));
  table.DefineColumn("c1", type::TypeId::INTEGER, false, catalog::col_oid_t(1));
  table.DefineColumn("c2", type::TypeId::INTEGER, false, catalog::col_oid_t(2));
  table.Create();
  table.StartRow();
  table.SetIntColInRow(0, 100);
  table.SetIntColInRow(1, 15721);
  table.SetIntColInRow(2, 17);
  table.EndRowAndInsert(txn);

  table.StartRow();
  table.SetIntColInRow(0, 200);
  table.SetIntColInRow(1, 25721);
  table.SetIntColInRow(2, 27);
  table.EndRowAndInsert(txn);

  // search for a single column
  std::vector<type::Value> search_vec;
  search_vec.emplace_back(type::ValueFactory::GetIntegerValue(100));

  // search for a value in column 0
  auto row_p = table.FindRow(txn, search_vec);
  EXPECT_EQ(3, row_p.size());
  EXPECT_EQ(100, row_p[0].GetIntValue());
  EXPECT_EQ(15721, row_p[1].GetIntValue());
  EXPECT_EQ(17, row_p[2].GetIntValue());

  // add a value for column 1 and search again
  search_vec.emplace_back(type::ValueFactory::GetIntegerValue(15721));
  row_p = table.FindRow(txn, search_vec);
  EXPECT_EQ(3, row_p.size());
  EXPECT_EQ(100, row_p[0].GetIntValue());
  EXPECT_EQ(15721, row_p[1].GetIntValue());
  EXPECT_EQ(17, row_p[2].GetIntValue());

  // now search for a non-existent value in column 2.
  // This is slow.
  // search_vec.emplace_back(type::ValueFactory::GetIntegerValue(19));
  // row_p = table.FindRow(txn, search_vec);
  // EXPECT_EQ(0, row_p.size());

  // search for second item
  search_vec.clear();
  search_vec.emplace_back(type::ValueFactory::GetIntegerValue(200));
  row_p = table.FindRow(txn, search_vec);
  EXPECT_EQ(3, row_p.size());
  EXPECT_EQ(200, row_p[0].GetIntValue());
  EXPECT_EQ(25721, row_p[1].GetIntValue());
  EXPECT_EQ(27, row_p[2].GetIntValue());

  txn_manager_.Commit(txn, TestCallbacks::EmptyCallback, nullptr);
  delete txn;
}

// NOLINTNEXTLINE
TEST_F(SqlTableTests, VarlenInsertTest) {
  catalog::SqlTableRW table(catalog::table_oid_t(2));
  auto txn = txn_manager_.BeginTransaction();

  table.DefineColumn("id", type::TypeId::INTEGER, false, catalog::col_oid_t(0));
  table.DefineColumn("datname", type::TypeId::VARCHAR, false, catalog::col_oid_t(1));
  table.Create();

  table.StartRow();
  table.SetIntColInRow(0, 100);
  table.SetVarcharColInRow(1, "name");
  table.EndRowAndInsert(txn);

  std::vector<type::Value> search_vec;
  search_vec.emplace_back(type::ValueFactory::GetIntegerValue(100));

  auto row_p = table.FindRow(txn, search_vec);
  EXPECT_EQ(100, row_p[0].GetIntValue());
  EXPECT_STREQ("name", row_p[1].GetStringValue());

  txn_manager_.Commit(txn, TestCallbacks::EmptyCallback, nullptr);
  delete txn;
}

}  // namespace terrier