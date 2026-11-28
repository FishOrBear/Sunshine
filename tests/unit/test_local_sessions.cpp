/**
 * @file tests/unit/test_local_sessions.cpp
 * @brief Regression tests for host-side device tracking and the desktop status pipe.
 */
#ifdef _WIN32
  // test includes
  #include "../tests_common.h"

  // standard includes
  #include <array>
  #include <atomic>
  #include <chrono>
  #include <thread>

  // lib includes
  #include <nlohmann/json.hpp>
  #include <windows.h>

  // local includes
  #include "src/platform/windows/local_sessions.h"

using namespace std::chrono_literals;

/** @brief Packet loss preserves a visit's recovery token. Explicit stops also reject handshakes already in flight. */
TEST(LocalSessionsTest, RecoveryFollowsThePairedDeviceAndHostDecision) {
  platf::local_sessions::roster_t roster;
  int stops = 0;
  auto stop = [&] {
    ++stops;
  };
  EXPECT_TRUE(roster.recovery_token("", true).empty());
  EXPECT_TRUE(roster.recovery_token("a").empty());
  const auto a = roster.recovery_token("a", true);
  const auto b = roster.recovery_token("b", true);
  ASSERT_FALSE(a.empty());
  ASSERT_NE(a, b);
  roster.add(1, "a", "Desk", false, stop, false, a);
  roster.remove(1);
  EXPECT_EQ(roster.recovery_token("a"), a);
  roster.add(2, "a", "Desk", false, stop, false, a);
  const auto id = nlohmann::json::parse(roster.reply("list")).at("devices").at(0).at("id").get<std::string>();
  roster.reply("disconnect " + id);
  EXPECT_EQ(stops, 1);
  EXPECT_TRUE(roster.recovery_token("a").empty());
  EXPECT_EQ(roster.recovery_token("b"), b);
  roster.add(3, "a", "Late", false, stop, false, a);
  EXPECT_EQ(stops, 2);
  const auto fresh = roster.recovery_token("a", true);
  ASSERT_NE(fresh, a);
  roster.add(4, "a", "Desk", false, stop, false, fresh);
  EXPECT_EQ(stops, 2);
  roster.revoke_recovery("b");
  EXPECT_TRUE(roster.recovery_token("b").empty());
  roster.reply("disconnect-all");
  EXPECT_TRUE(roster.recovery_token("a").empty());
  EXPECT_EQ(stops, 3);
  roster.recovery_token("a", true);
  roster.revoke_recovery();
  EXPECT_TRUE(roster.recovery_token("a").empty());
}

/** @brief Paired clients can recognize the host only in the Windows session that owns the event. */
TEST(LocalSessionsTest, AudioHostIdNamesALiveSessionLocalEvent) {
  const auto id = platf::local_sessions::audio_host_id();
  ASSERT_EQ(id.size(), 32U);
  EXPECT_EQ(platf::local_sessions::audio_host_id(), id);
  const auto event = OpenEventA(SYNCHRONIZE, FALSE, ("Local\\Sunshine.AudioHost." + id).c_str());
  ASSERT_NE(event, nullptr);
  CloseHandle(event);
}

/** @brief A device retains one visit across displays and stale controls cannot affect a reconnect. */
TEST(LocalSessionsTest, GroupsDisplaysAndTargetsOnlyTheSelectedVisit) {
  platf::local_sessions::roster_t roster;
  int primary = 0, companion = 0, other = 0;
  roster.add(1, "cert-a", "Same name", true, [&] {
    ++primary;
  });
  roster.add(2, "cert-a", "Same name", false, [&] {
    ++companion;
  },
             true);
  roster.add(3, "cert-b", "Same name", true, [&] {
    ++other;
  });
  const auto first = nlohmann::json::parse(roster.reply("list"));
  ASSERT_TRUE(first.at("ok"));
  ASSERT_EQ(first.at("devices").size(), 2);
  const auto device = first.at("devices").at(0);
  const auto drop = "disconnect " + device.at("id").get<std::string>();
  EXPECT_EQ(device.at("since"), first.at("since"));
  EXPECT_EQ(device.at("name"), "Same name");
  EXPECT_FALSE(device.at("viewOnly"));
  EXPECT_TRUE(first.at("devices").at(1).at("viewOnly"));
  EXPECT_TRUE(nlohmann::json::parse(roster.reply(drop)).at("ok"));
  EXPECT_EQ(primary, 1);
  EXPECT_EQ(companion, 1);
  EXPECT_EQ(other, 0);
  roster.remove(1);
  const auto closing = nlohmann::json::parse(roster.reply("list")).at("devices").at(0);
  EXPECT_EQ(closing.at("id"), device.at("id"));
  EXPECT_EQ(closing.at("since"), device.at("since"));
  EXPECT_TRUE(closing.at("closing"));
  roster.remove(2);
  roster.add(4, "cert-a", "Same name", false, [&] {
    ++primary;
  });
  EXPECT_FALSE(nlohmann::json::parse(roster.reply(drop)).at("ok"));
  EXPECT_FALSE(nlohmann::json::parse(roster.reply("disconnect ")).at("ok"));
  EXPECT_TRUE(nlohmann::json::parse(roster.reply("disconnect-all")).at("ok"));
  EXPECT_EQ(primary, 2);
  EXPECT_EQ(companion, 1);
  EXPECT_EQ(other, 1);
  roster.remove(3);
  roster.remove(4);
  const auto idle = nlohmann::json::parse(roster.reply("list"));
  EXPECT_TRUE(idle.at("devices").empty());
  EXPECT_EQ(idle.at("since"), 0);
}

/** @brief A display handshake cannot restore a disconnected visit or prevent a deliberate reconnect. */
TEST(LocalSessionsTest, RejectsLateCompanionsAndAllowsFreshPrimaryConnections) {
  platf::local_sessions::roster_t roster;
  int stops = 0;
  auto stop = [&] {
    ++stops;
  };
  roster.add(1, "cert", "Desk", false, stop, true);
  EXPECT_TRUE(nlohmann::json::parse(roster.reply("list")).at("devices").empty());
  roster.add(2, "cert", "Desk", false, stop);
  const auto old_id = nlohmann::json::parse(roster.reply("disconnect-all")).at("devices").at(0).at("id").get<std::string>();
  roster.reply("disconnect-all");
  EXPECT_EQ(stops, 2);
  roster.add(3, "cert", "Desk", false, stop, true);
  EXPECT_EQ(stops, 3);
  roster.add(4, "cert", "Desk", false, stop);
  roster.remove(2);
  roster.remove(3);
  const auto fresh = nlohmann::json::parse(roster.reply("disconnect " + old_id));
  ASSERT_EQ(fresh.at("devices").size(), 1);
  EXPECT_FALSE(fresh.at("ok"));
  EXPECT_FALSE(fresh.at("devices").at(0).at("closing"));
  EXPECT_EQ(stops, 3);
}

/** @brief The local pipe exchanges real requests and shuts down even with an idle client. */
TEST(LocalSessionsTest, ServesSnapshotsAndDisconnectsOverTheLocalPipe) {
  platf::local_sessions::roster_t roster;
  std::atomic_int stopped {};
  roster.add(10, "certificate", "Desk", false, [&] {
    ++stopped;
  });
  const auto name = platf::local_sessions::pipe_name() + L".test." + std::to_wstring(GetCurrentProcessId());
  std::jthread server {platf::local_sessions::serve, std::ref(roster), name};
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!WaitNamedPipeW(name.c_str(), 100) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  std::array<char, 1024> response {};
  DWORD size {};
  std::string request = "list";
  ASSERT_TRUE(CallNamedPipeW(name.c_str(), request.data(), request.size(), response.data(), response.size(), &size, 1000));
  const auto list = nlohmann::json::parse(response.begin(), response.begin() + size);
  ASSERT_EQ(list.at("devices").size(), 1);
  EXPECT_EQ(list.at("devices").at(0).at("name"), "Desk");
  request = "disconnect " + list.at("devices").at(0).at("id").get<std::string>();
  ASSERT_TRUE(CallNamedPipeW(name.c_str(), request.data(), request.size(), response.data(), response.size(), &size, 1000));
  EXPECT_TRUE(nlohmann::json::parse(response.begin(), response.begin() + size).at("ok"));
  EXPECT_EQ(stopped.load(), 1);
  ASSERT_TRUE(WaitNamedPipeW(name.c_str(), 1000));
  const auto idle = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr);
  ASSERT_NE(idle, INVALID_HANDLE_VALUE);
  server.request_stop();
  server.join();
  CloseHandle(idle);
}
#endif
