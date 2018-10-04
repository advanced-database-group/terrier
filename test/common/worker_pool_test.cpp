#include <atomic>
#include <thread>  // NOLINT
#include <vector>

#include "common/worker_pool.h"
#include "gtest/gtest.h"
#include "util/random_test_util.h"
#include "util/test_thread_pool.h"

namespace terrier {

// Rather minimalistic checks for whether we reuse memory
// NOLINTNEXTLINE
TEST(WorkerPoolTests, BasicTest) {
  common::TaskQueue tasks;
  common::WorkerPool thread_pool("test-pool", 5, tasks);
  thread_pool.Startup();
  std::atomic<int> counter(0);

  int var1 = 1;
  int var2 = 2;
  int var3 = 3;
  int var4 = 4;
  int var5 = 5;
  thread_pool.SubmitTask([&]() {
    var1++;
    counter.fetch_add(1);
  });
  thread_pool.SubmitTask([&]() {
    var2--;
    counter.fetch_add(1);
  });
  thread_pool.SubmitTask([&]() {
    var3 *= var3;
    counter.fetch_add(1);
  });
  thread_pool.SubmitTask([&]() {
    var4 = var4 / var4;
    counter.fetch_add(1);
  });

  thread_pool.SubmitTask([&]() {
    var5 = var5 / var5;
    counter.fetch_add(1);
  });

  // Wait for all the test to finish
  while (counter.load() != 5) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_EQ(2, var1);
  EXPECT_EQ(1, var2);
  EXPECT_EQ(9, var3);
  EXPECT_EQ(1, var4);
  EXPECT_EQ(1, var5);

  thread_pool.Shutdown();
}
}  // namespace terrier