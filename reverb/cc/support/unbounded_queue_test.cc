// Copyright 2019 DeepMind Technologies Limited.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "reverb/cc/support/unbounded_queue.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/synchronization/notification.h"
#include "reverb/cc/platform/thread.h"
#include "reverb/cc/platform/logging.h"

namespace deepmind {
namespace reverb {
namespace internal {
namespace {

TEST(UnboundedQueueTest, PushAndPopAreConsistent) {
  UnboundedQueue<int> q;
  int output;
  for (int i = 0; i < 100; i++) {
    q.Push(i);
    q.Pop(&output);
    EXPECT_EQ(output, i);
  }
}

TEST(UnboundedQueueTest, PopBlocksWhenEmpty) {
  UnboundedQueue<int> q;
  absl::Notification n;
  int output;
  auto t = StartThread("", [&q, &n, &output] {
    REVERB_CHECK(q.Pop(&output));
    n.Notify();
  });
  ASSERT_FALSE(n.HasBeenNotified());
  ASSERT_TRUE(q.Push(1));
  n.WaitForNotification();
  EXPECT_EQ(output, 1);
}

TEST(UnboundedQueueTest, AfterClosePushAndPopReturnFalse) {
  UnboundedQueue<int> q;
  q.Close();
  EXPECT_FALSE(q.Push(1));
  EXPECT_FALSE(q.Pop(nullptr));
}

TEST(UnboundedQueueTest, CloseUnblocksPop) {
  UnboundedQueue<int> q;
  absl::Notification n;
  bool ok;
  auto t = StartThread("", [&q, &n, &ok] {
    int output;
    ok = q.Pop(&output);
    n.Notify();
  });
  ASSERT_FALSE(n.HasBeenNotified());
  q.Close();
  n.WaitForNotification();
  EXPECT_FALSE(ok);
}

TEST(UnboundedQueueTest, SizeReturnsNumberOfElements) {
  UnboundedQueue<int> q;
  EXPECT_EQ(q.size(), 0);

  q.Push(20);
  q.Push(30);
  EXPECT_EQ(q.size(), 2);

  int v;
  ASSERT_TRUE(q.Pop(&v));
  EXPECT_EQ(q.size(), 1);
}

TEST(UnboundedQueueTest, PushFailsAfterSetLastItemPushed) {
  UnboundedQueue<int> q;
  q.SetLastItemPushed();
  EXPECT_FALSE(q.Push(1));
}

TEST(UnboundedQueueTest, ExistingItemsCanBePoppedAfterSetLastItemPushed) {
  UnboundedQueue<int> q;

  q.Push(1);
  q.Push(2);

  q.SetLastItemPushed();

  int v;
  ASSERT_TRUE(q.Pop(&v));
  EXPECT_EQ(v, 1);
  ASSERT_TRUE(q.Pop(&v));
  EXPECT_EQ(v, 2);

  // Queue is now empty and no items can be pushed so it is effectively closed.
  EXPECT_FALSE(q.Pop(&v));
}

TEST(UnboundedQueueTest, BlockingPopReturnsIfSetLastItemPushedCalled) {
  UnboundedQueue<int> q;
  absl::Notification n;
  bool ok;
  auto t = StartThread("", [&q, &n, &ok] {
    int output;
    ok = q.Pop(&output);
    n.Notify();
  });
  ASSERT_FALSE(n.HasBeenNotified());
  q.SetLastItemPushed();
  n.WaitForNotification();
  EXPECT_FALSE(ok);
}

}  // namespace
}  // namespace internal
}  // namespace reverb
}  // namespace deepmind
