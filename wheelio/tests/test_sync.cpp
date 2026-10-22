#include <wheelio/coro.hpp>
#include <wheelio/sync.hpp>

#include <gtest/gtest.h>

using wheelio::Task;
using wheelio::WaitGroup;

namespace {

Task<void> wait_for(WaitGroup &group, bool &finished) {
  co_await group.wait();
  finished = true;
}

} // namespace

TEST(WaitGroup, ReturnsAtOnceWhenNothingIsPending) {
  WaitGroup group;
  bool finished = false;
  Task<void> task = wait_for(group, finished);
  EXPECT_TRUE(finished);
}

TEST(WaitGroup, WaitsForEveryDone) {
  WaitGroup group;
  bool finished = false;

  group.add(3);
  Task<void> task = wait_for(group, finished);
  EXPECT_FALSE(finished);
  group.done();
  group.done();
  EXPECT_FALSE(finished);
  group.done();
  EXPECT_TRUE(finished);
  EXPECT_TRUE(task.done());
}
