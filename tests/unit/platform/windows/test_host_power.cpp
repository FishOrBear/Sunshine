/**
 * @file tests/unit/platform/windows/test_host_power.cpp
 * @brief Verify host power dispatch using in-memory callbacks.
 */
#ifdef _WIN32
  // test includes
  #include "../../../tests_common.h"

  // local includes
  #include "src/platform/windows/host_power.h"

TEST(HostPowerTest, AcceptsOnlyNamedPowerActions) {
  EXPECT_EQ(host_power::restart_requested("restart"), true);
  EXPECT_EQ(host_power::restart_requested("shutdown"), false);
  for (const auto name : {"", "sleep", "shutdown now", "Restart"}) {
    EXPECT_FALSE(host_power::restart_requested(name).has_value());
  }
}

TEST(HostPowerTest, RequiresPrivilegeAndWaitsForReplyBeforeExecutingOnce) {
  host_power::queue_t queue;
  const auto allowed = []() {
    return true;
  };
  EXPECT_EQ(queue.prepare([]() {
                   return host_power::action_t {};
                 },
                          allowed)
              .first,
            503);
  int calls = 0;
  const auto acquire = [&]() {
    return [&]() {
      ++calls;
      return true;
    };
  };
  auto [code, send] = queue.prepare(acquire, allowed);
  ASSERT_EQ(code, 202);
  ASSERT_TRUE(send);
  EXPECT_EQ(queue.prepare(acquire, allowed).first, 409);
  EXPECT_EQ(calls, 0);
  send(true);
  send(true);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(queue.prepare(acquire, allowed).first, 409);
}

TEST(HostPowerTest, FailedOrAbandonedReplyAndRevokedPairingCancelPendingAction) {
  host_power::queue_t queue;
  bool enabled = true;
  const auto allowed = [&]() {
    return enabled;
  };
  int calls = 0;
  const auto acquire = [&]() {
    return [&]() {
      ++calls;
      return false;
    };
  };
  auto [code, send] = queue.prepare(acquire, allowed);
  ASSERT_EQ(code, 202);
  send(false);
  send(true);
  EXPECT_EQ(calls, 0);
  EXPECT_EQ(queue.prepare(acquire, allowed).first, 202);
  auto revoked = queue.prepare(acquire, allowed);
  ASSERT_EQ(revoked.first, 202);
  enabled = false;
  revoked.second(true);
  EXPECT_EQ(calls, 0);
  enabled = true;
  auto final = queue.prepare(acquire, allowed);
  ASSERT_EQ(final.first, 202);
  final.second(true);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(queue.prepare(acquire, allowed).first, 202);
}
#endif
