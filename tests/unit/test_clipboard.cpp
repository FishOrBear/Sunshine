/**
 * @file tests/unit/test_clipboard.cpp
 * @brief Exercise per-session clipboard negotiation and Windows clipboard message validation.
 */
// test includes
#include "../tests_common.h"

// local includes
#include "src/clipboard.h"

#ifdef _WIN32
  // standard includes
  #include <fstream>
  #include <thread>

  // platform includes
  #include "src/platform/windows/clipboard.h"
  #include "src/platform/windows/clipboard_download.h"
  #include "src/platform/windows/clipboard_resume.h"
  #include "src/platform/windows/clipboard_upload.h"
  #include "src/platform/windows/clipboard_win32.h"
  #include "src/platform/windows/file_drag.h"
  #include "src/platform/windows/file_sessions.h"
  #include "src/platform/windows/interactive_desktop.h"
  #include "src/utility.h"

#endif

TEST(ClipboardGrantTest, RequiresMutualConsentAndValidCapability) {
  const std::string token(64, 'a');
  const auto text = clipboard::negotiate({true, true}, {true, false}, token);
  EXPECT_TRUE(text.matches(token));
  EXPECT_FALSE(text.matches(std::string(64, 'b')));
  EXPECT_TRUE(text.formats.text);
  EXPECT_FALSE(text.formats.files);
  const auto files = clipboard::negotiate({false, true}, {true, true}, token);
  EXPECT_FALSE(files.formats.text);
  EXPECT_TRUE(files.formats.files);
  EXPECT_FALSE(clipboard::negotiate({true, false}, {false, true}, token).formats.matches_any());
  for (const auto &invalid : {std::string {}, std::string(63, '0'), std::string(64, '!')}) {
    const auto grant = clipboard::negotiate({true, true}, {true, true}, invalid);
    EXPECT_FALSE(grant.matches(invalid));
    EXPECT_FALSE(grant.formats.matches_any());
  }
}

TEST(ClipboardGrantTest, ImageSyncRequiresPermissionOnBothPeers) {
  const std::string token(64, 'a');
  clipboard::formats_t images;
  images.images = true;
  const clipboard::formats_t other {true, true};
  const auto granted = clipboard::negotiate(images, images, token);
  EXPECT_TRUE(granted.matches(token));
  EXPECT_TRUE(granted.formats.images);
  EXPECT_FALSE(granted.formats.files);
  EXPECT_FALSE(granted.formats.text);
  EXPECT_FALSE(clipboard::negotiate(images, other, token).matches(token));
  EXPECT_FALSE(clipboard::negotiate(other, images, token).matches(token));
  EXPECT_FALSE(clipboard::negotiate(images, images, "").formats.matches_any());
}

TEST(ClipboardGrantTest, FileBrowserRequiresSeparateBilateralPermission) {
  const std::string token(64, 'd');
  const clipboard::formats_t browser {false, false, 0, true};
  const clipboard::formats_t copied {false, true};
  EXPECT_TRUE(clipboard::negotiate(browser, browser, token).formats.browser);
  EXPECT_FALSE(clipboard::negotiate(browser, browser, token).formats.matches_any());
  EXPECT_FALSE(clipboard::negotiate(copied, browser, token).formats.browser);
  EXPECT_FALSE(clipboard::negotiate(browser, copied, token).formats.browser);
  EXPECT_FALSE(clipboard::negotiate(browser, browser, "bad").formats.browser);
}

TEST(ClipboardGrantTest, DisclosesOnlyNewCopiesAfterSessionStartup) {
  clipboard::formats_t formats {true, true, 1234};
  EXPECT_FALSE(formats.should_read(1234, 0));
  EXPECT_FALSE(formats.should_read(4321, std::nullopt));
  EXPECT_FALSE(formats.should_read(4321, 4321));
  EXPECT_TRUE(formats.should_read(4321, 1234));
}

#ifdef _WIN32
TEST(ClipboardDesktopTest, RestoresInheritedDesktopAfterShellAccess) {
  if (!GetShellWindow()) {
    GTEST_SKIP() << "Requires an interactive Windows shell";
  }
  const auto name = L"SunshineClipboardTest-" + std::to_wstring(GetCurrentProcessId());
  const auto isolated = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
  ASSERT_NE(isolated, nullptr);
  std::thread worker([&] {
    const auto original = GetThreadDesktop(GetCurrentThreadId());
    if (!SetThreadDesktop(isolated)) {
      ADD_FAILURE() << "Cannot attach test worker to isolated desktop";
      return;
    }
    EXPECT_EQ(GetShellWindow(), nullptr);
    {
      platf::desktop::shell_impersonation_t user;
      EXPECT_TRUE(user.begin());
      EXPECT_NE(GetShellWindow(), nullptr);
      EXPECT_NE(user.token, nullptr);
    }
    EXPECT_EQ(GetThreadDesktop(GetCurrentThreadId()), isolated);
    EXPECT_EQ(GetShellWindow(), nullptr);
    HANDLE token {};
    EXPECT_FALSE(OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token));
    EXPECT_EQ(GetLastError(), ERROR_NO_TOKEN);
    if (token) {
      CloseHandle(token);
    }
    EXPECT_TRUE(SetThreadDesktop(original));
  });
  worker.join();
  EXPECT_TRUE(CloseDesktop(isolated));
}

TEST(ClipboardDesktopTest, KeepsExistingShellDesktop) {
  if (!GetShellWindow()) {
    GTEST_SKIP() << "Requires an interactive Windows shell";
  }
  std::thread worker([] {
    const auto original = GetThreadDesktop(GetCurrentThreadId());
    {
      platf::desktop::shell_impersonation_t user;
      EXPECT_TRUE(user.begin());
      EXPECT_EQ(user.shell_desktop, nullptr);
      EXPECT_EQ(GetThreadDesktop(GetCurrentThreadId()), original);
    }
    EXPECT_EQ(GetThreadDesktop(GetCurrentThreadId()), original);
  });
  worker.join();
}

TEST(ClipboardDesktopTest, RefusesDesktopSwitchWhenThreadOwnsAWindow) {
  const auto name = L"SunshineClipboardBusyTest-" + std::to_wstring(GetCurrentProcessId());
  const auto isolated = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
  ASSERT_NE(isolated, nullptr);
  std::thread worker([&] {
    const auto original = GetThreadDesktop(GetCurrentThreadId());
    if (!SetThreadDesktop(isolated)) {
      ADD_FAILURE() << "Cannot attach test worker to isolated desktop";
      return;
    }
    const auto window = CreateWindowW(L"STATIC", L"", 0, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr);
    EXPECT_NE(window, nullptr);
    if (window) {
      {
        platf::desktop::shell_impersonation_t user;
        EXPECT_FALSE(user.begin());
        EXPECT_FALSE(user.active);
        EXPECT_EQ(user.token, nullptr);
        EXPECT_EQ(GetThreadDesktop(GetCurrentThreadId()), isolated);
      }
      EXPECT_TRUE(DestroyWindow(window));
    }
    EXPECT_TRUE(SetThreadDesktop(original));
  });
  worker.join();
  EXPECT_TRUE(CloseDesktop(isolated));
}

TEST(ClipboardGrantTest, FirstPollPreservesCopyMadeAfterStreamStartup) {
  const std::uint32_t current = platf::clipboard_sequence();
  const clipboard::formats_t formats {true, false, current - 1};
  std::string payload;
  std::uint32_t reported {};
  EXPECT_EQ(platf::exchange_clipboard(formats, false, std::nullopt, {}, payload, reported), 204);
  EXPECT_TRUE(payload.empty());
  EXPECT_EQ(reported, formats.initial_sequence);
  EXPECT_TRUE(formats.should_read(current, reported));
}

TEST(ClipboardCodecTest, RejectsUnsafeNamesAndMalformedFrames) {
  using namespace win_clipboard;
  for (const auto *name : {L"", L"..", L"../copy", L"dir\\copy", L"C:copy", L"nul.txt", L"copy.", L"com1 ", L"my|file", L"AUX"}) {
    EXPECT_FALSE(detail::valid_name(name));
  }
  EXPECT_TRUE(detail::valid_name(L"folder list.txt"));
  EXPECT_TRUE(detail::valid_name(L"conventional.jpg"));
  std::string encoded;
  detail::append_size(encoded, 3);
  encoded += "abc";
  std::string_view valid(encoded), extracted;
  EXPECT_TRUE(detail::take_field(valid, extracted));
  EXPECT_EQ(extracted, "abc");
  EXPECT_TRUE(valid.empty());
  std::string_view missing(encoded.data(), encoded.size() - 1);
  EXPECT_FALSE(detail::take_field(missing, extracted));
  std::string_view short_header(encoded.data(), 2);
  EXPECT_FALSE(detail::take_field(short_header, extracted));
}

TEST(ClipboardCodecTest, RejectsDisallowedOrMalformedWritesBeforeOpeningClipboard) {
  DWORD sequence {};
  EXPECT_EQ(win_clipboard::write(true, true, "Iinvalid", sequence, {}), 403);
  EXPECT_EQ(win_clipboard::write(false, false, "Iinvalid", sequence, {}, {}, {}, true), 400);
  EXPECT_EQ(win_clipboard::write(false, true, "Tprivate", sequence, {}), 403);
  EXPECT_EQ(win_clipboard::write(true, false, "Fprivate", sequence, {}), 403);
  EXPECT_EQ(win_clipboard::write(true, true, "Tcopied", sequence, {}, std::nullopt, []() {
              return false;
            }),
            403);
  EXPECT_EQ(win_clipboard::write(true, true, "?", sequence, {}), 400);
  EXPECT_EQ(win_clipboard::write(true, true, {}, sequence, {}), 400);
  EXPECT_EQ(win_clipboard::write(true, true, "F", sequence, {}), 400);
  EXPECT_EQ(win_clipboard::write(true, true, std::string("Ta\0b", 4), sequence, {}), 400);
  EXPECT_EQ(win_clipboard::write(true, true, std::string("T\xff", 2), sequence, {}), 400);
  EXPECT_EQ(win_clipboard::write(true, true, std::string(win_clipboard::max_text + 2, 'T'), sequence, {}), 413);
  EXPECT_EQ(win_clipboard::write(true, true, std::string(win_clipboard::max_body + 1, 'F'), sequence, {}), 413);
}

TEST(ClipboardCodecTest, PngRoundTripPreservesBgraAndAlpha) {
  clipboard_image::snapshot source;
  source.width = 2;
  source.height = 1;
  source.stride = 8;
  source.alpha = true;
  source.pixels = {1, 2, 3, 64, 4, 5, 6, 255};
  std::string packet;
  ASSERT_EQ(clipboard_image::encode(source, packet), 200);
  ASSERT_EQ(packet.front(), 'I');
  HGLOBAL decoded {};
  ASSERT_EQ(clipboard_image::decode(std::string_view(packet).substr(1), decoded), 200);
  {
    clipboard_image::locked_memory pixels(decoded);
    ASSERT_NE(pixels.bytes, nullptr);
    const auto *header = static_cast<const BITMAPV5HEADER *>(pixels.bytes);
    EXPECT_EQ(header->bV5Width, 2);
    EXPECT_EQ(header->bV5Height, -1);
    EXPECT_EQ(std::memcmp(header + 1, source.pixels.data(), source.pixels.size()), 0);
  }
  GlobalFree(decoded);
}

TEST(FileSessionTest, BindRenewExpireAndCloseWithoutStreaming) {
  file_sessions::registry_t sessions;
  const std::string token(64, 'a');
  const auto now = file_sessions::registry_t::clock::now();
  const auto timeout = file_sessions::registry_t::idle_timeout;
  EXPECT_EQ(sessions.open("a", "short", now), 400);
  EXPECT_EQ(sessions.open("", "", now), 400);
  ASSERT_EQ(sessions.open("a", token, now), 204);
  auto first = sessions.find("a", token, now);
  ASSERT_TRUE(first);
  EXPECT_FALSE(sessions.find("b", token, now));
  sessions.close("b", token);
  EXPECT_EQ(sessions.open("a", token, now + timeout / 2), 204);
  EXPECT_EQ(sessions.find("a", token, now + timeout), first);
  std::weak_ptr<platf::file_browser_session_t> released = first;
  first.reset();
  sessions.expire(now + timeout * 2);
  EXPECT_TRUE(released.expired());
  EXPECT_FALSE(sessions.find("a", token, now + timeout * 2));
  EXPECT_EQ(sessions.open("a", token, now + timeout * 2), 204);
  sessions.close("a", token);
  sessions.close("a", token);
  EXPECT_FALSE(sessions.find("a", token, now + timeout * 2));
}

TEST(FileSessionTest, IndependentClipboardTaskSurvivesStreamAndCannotBeReplaced) {
  file_sessions::basic_registry_t<int> tasks;
  auto stream = std::make_shared<int>(42);
  const std::string task(64, 'd');
  ASSERT_EQ(tasks.insert("peer", task, [&] {
    return stream;
  },
                         false),
            204);
  stream.reset();
  EXPECT_FALSE(tasks.find("other peer", task));
  EXPECT_EQ(tasks.insert("peer", task, [] {
    return std::make_shared<int>(0);
  },
                         false),
            409);
  ASSERT_TRUE(tasks.find("peer", task));
  EXPECT_EQ(*tasks.find("peer", task), 42);
  tasks.revoke("peer");
  EXPECT_FALSE(tasks.find("peer", task));
}

TEST(FileSessionTest, LimitsEachPairingAndTotalWithoutEvictingAnActiveTransfer) {
  file_sessions::registry_t sessions;
  for (int i = 0; i < 8; ++i) {
    for (int n = 0; n < 4; ++n) {
      ASSERT_EQ(sessions.open(std::to_string(i), std::string(64, 'a' + n)), 204);
    }
    EXPECT_EQ(sessions.open(std::to_string(i), std::string(64, 'e')), 429);
  }
  const std::string token(64, 'a');
  EXPECT_EQ(sessions.open("another", token), 429);
  EXPECT_TRUE(sessions.find("0", token));
  sessions.revoke("0");
  EXPECT_FALSE(sessions.find("0", token));
  EXPECT_TRUE(sessions.find("1", token));
  EXPECT_EQ(sessions.open("another", token), 204);
  sessions.revoke();
  EXPECT_FALSE(sessions.find("1", token));
}

TEST(FileBrowserTest, RequiresValidCapabilityAndWriteBounds) {
  auto session = platf::make_file_browser_session();
  std::string out;
  const auto denied = []() {
    return false;
  };
  EXPECT_EQ(platf::exchange_files(*session, "list", "", "", 0, 0, {}, {}, out, denied), 403);
  EXPECT_EQ(platf::exchange_files(*session, "write", "", "bad", 0, 0, {}, {}, out, denied), 400);
  EXPECT_EQ(platf::exchange_files(*session, "read", "", "", 10, 9, {}, {}, out, denied), 400);
  EXPECT_EQ(platf::exchange_files(*session, "read", "", "", 0, UINT64_MAX, {}, {}, out, denied), 400);
}

TEST(FileDragTest, UsesTheCapturedMonitorAndRejectsLetterboxBars) {
  const platf::file_drag::display_t display {1920, 50, 1600, 1200, 1920, 1080};
  const POINT origin {-1920, -100};
  EXPECT_FALSE(display.point(0, 100, origin));
  EXPECT_FALSE(display.point(65535, 400, origin));
  EXPECT_FALSE(display.point(32768, 65536, origin));
  EXPECT_FALSE(platf::file_drag::display_t {}.point(0, 0, origin));
  const auto center = display.point(32768, 32768, origin);
  ASSERT_TRUE(center);
  EXPECT_EQ(center->x, 800);
  EXPECT_EQ(center->y, 550);
}

TEST(FileDragTest, DescribesFileAndFolderSelectionsWithoutReadingFileContents) {
  namespace fs = std::filesystem;
  std::string output;
  GUID guid {};
  ASSERT_EQ(CoCreateGuid(&guid), S_OK);
  wchar_t name[40] {};
  ASSERT_TRUE(StringFromGUID2(guid, name, 40));
  const auto folder = fs::temp_directory_path() / name;
  ASSERT_TRUE(fs::create_directory(folder));
  const auto file = folder / L"sample.bin";
  ASSERT_TRUE(std::ofstream(file) << "drag");
  auto cleanup = util::fail_guard([&] {
    fs::remove(file);
    fs::remove(folder);
  });
  EXPECT_EQ(platf::file_drag::describe({}, true, output), 200);
  EXPECT_TRUE(nlohmann::json::parse(output)["entries"].empty());
  ASSERT_EQ(platf::file_drag::describe({file, folder}, true, output), 200);
  const auto entries = nlohmann::json::parse(output)["entries"];
  ASSERT_EQ(entries.size(), 2);
  EXPECT_EQ(entries[0]["size"], "4");
  EXPECT_EQ(entries[0]["directory"], false);
  EXPECT_EQ(entries[1]["directory"], true);
  ASSERT_EQ(platf::file_drag::describe({folder}, false, output), 200);
  EXPECT_EQ(nlohmann::json::parse(output)["directory"], win_clipboard::detail::to_utf8(folder.generic_wstring()));
  EXPECT_EQ(platf::file_drag::describe({file}, false, output), 403);
  EXPECT_EQ(platf::file_drag::describe({folder, folder}, false, output), 403);
  EXPECT_EQ(platf::file_drag::describe({folder.root_path()}, true, output), 403);
  EXPECT_EQ(platf::file_drag::describe({file.wstring() + L".missing"}, true, output), 409);
}

TEST(FileDragTest, RequiresLiveStreamGeometryAndAValidPoint) {
  auto session = platf::make_file_browser_session();
  std::string out;
  auto query = [&](std::optional<std::pair<unsigned, unsigned>> point, bool grant = true) {
    return platf::exchange_files(*session, "drag-source", {}, {}, 0, 0, {}, {}, out, [grant] {
      return grant;
    },
                                 {},
                                 {},
                                 nullptr,
                                 false,
                                 point);
  };
  EXPECT_EQ(query(std::pair {0U, 0U}, false), 403);
  if (query({}) == 403) {
    GTEST_SKIP() << "This test needs an unlocked interactive Windows desktop";
  }
  EXPECT_EQ(query({}), 400);
  EXPECT_EQ(query(std::pair {65536U, 0U}), 400);
  EXPECT_EQ(query(std::pair {0U, 0U}), 403);
  platf::set_file_drag_display(*session, 0, 0, 800, 600, 1920, 1080);
  EXPECT_EQ(query(std::pair {0U, 0U}), 200);
  EXPECT_TRUE(nlohmann::json::parse(out)["entries"].empty());
}

TEST(FileBrowserTest, CopiesInChunksAndCleansAbandonedUploads) {
  namespace fs = std::filesystem;
  auto session = platf::make_file_browser_session();
  std::string out;
  const std::string id(32, 'e');
  const auto active = []() {
    return true;
  };
  const auto op = [&](std::string_view action, const fs::path &path, std::uint64_t offset = 0, std::uint64_t size = 0, std::string_view body = {}, bool allow = false, std::string_view replace = {}, std::string_view filter = {}) {
    out.clear();
    return platf::exchange_files(*session, action, win_clipboard::detail::to_utf8(path.generic_wstring()), id, offset, size, {}, body, out, active, {}, {}, nullptr, allow, {}, replace, filter);
  };
  const auto drives = op("list", {});
  if (drives == 403) {
    GTEST_SKIP() << "This test needs an unlocked interactive Windows desktop";
  }
  ASSERT_EQ(drives, 200);
  EXPECT_NE(out.find("entries"), std::string::npos);
  ASSERT_EQ(op("places", {}), 200);
  const auto places = nlohmann::json::parse(out).at("entries");
  ASSERT_TRUE(places.is_array());
  ASSERT_LE(places.size(), 32);
  EXPECT_FALSE(places.empty());
  for (const auto &place : places) {
    EXPECT_TRUE(place.at("name").is_string());
    EXPECT_TRUE(place.at("kind").is_string());
    EXPECT_TRUE(fs::path(win_clipboard::detail::from_utf8(place.at("path").get<std::string>())).is_absolute());
  }
  GUID guid {};
  ASSERT_EQ(CoCreateGuid(&guid), S_OK);
  wchar_t unique[40] {};
  ASSERT_TRUE(StringFromGUID2(guid, unique, 40));
  const auto dir = fs::temp_directory_path() / unique;
  ASSERT_EQ(op("mkdir", dir), 204);

  struct cleanup_t {
    fs::path directory;

    ~cleanup_t() {
      std::error_code ec;
      fs::remove_all(directory, ec);
    }
  } cleanup {dir};

  EXPECT_EQ(op("mkdir", dir), 409);
  const auto target = dir / L"copied + file.txt";
  EXPECT_EQ(op("write", target, 0, 5, "ab"), 204);
  EXPECT_FALSE(fs::exists(target));
  EXPECT_EQ(op("write", target, 1, 5, "cd"), 409);
  EXPECT_EQ(op("write", target, 2, 5, "cde"), 204);
  EXPECT_EQ(op("read", target, 0, 5), 200);
  EXPECT_EQ(out, "abcde");
  EXPECT_EQ(op("read", target, 2, 5), 200);
  EXPECT_EQ(out, "cde");
  EXPECT_EQ(op("read", target, 0, 4), 409);
  EXPECT_EQ(op("write", target, 0, 0), 409);
  EXPECT_EQ(op("list", dir), 200);
  EXPECT_NE(out.find("copied + file.txt"), std::string::npos);
  EXPECT_FALSE(nlohmann::json::parse(out).at("entries").at(0).contains("replaceVersion"));
  EXPECT_EQ(op("list", dir, 0, 0, {}, true, {}, "../other"), 400);
  ASSERT_EQ(op("list", dir, 0, 0, {}, true, {}, "absent"), 200);
  EXPECT_TRUE(nlohmann::json::parse(out).at("entries").empty());
  ASSERT_EQ(op("list", dir, 0, 0, {}, true, {}, "copied + file.txt"), 200);
  const auto version = nlohmann::json::parse(out).at("entries").at(0).at("replaceVersion").get<std::string>();
  EXPECT_EQ(op("write", target, 0, 2, "x", false, version), 403);
  ASSERT_EQ(op("write", target, 0, 2, "x", true, version), 204);
  EXPECT_EQ(op("write", target, 1, 2, "y"), 409);
  EXPECT_EQ(op("write", target, 1, 2, "y", false, version), 403);
  EXPECT_EQ(op("read", target, 0, 5), 200);
  EXPECT_EQ(out, "abcde");
  ASSERT_EQ(op("write", target, 1, 2, "y", true, version), 204);
  EXPECT_EQ(op("read", target, 0, 2), 200);
  EXPECT_EQ(out, "xy");
  EXPECT_EQ(op("write", dir / "second", 0, 2, "x"), 204);
  EXPECT_EQ(op("abort", {}), 204);
  EXPECT_EQ(op("write", dir / "second", 1, 2, "y"), 409);
  EXPECT_FALSE(fs::exists(dir / "second"));
  EXPECT_EQ(op("write", dir / "empty", 0, 0), 204);
  EXPECT_EQ(op("read", dir / "empty", 0, 0), 200);
  EXPECT_TRUE(out.empty());
  EXPECT_EQ(op("write", dir / "third", 0, 2, "x"), 204);
  session.reset();
  EXPECT_EQ(std::distance(fs::directory_iterator(dir), fs::directory_iterator()), 2);
}

TEST(FileBrowserTest, RequiresExplicitDeletePermissionAndAnUnchangedRegularFile) {
  namespace fs = std::filesystem;
  auto session = platf::make_file_browser_session();
  std::string out;
  const auto active = [] {
    return true;
  };
  const auto exchange = [&](std::string_view op, const fs::path &path, std::uint64_t size, std::optional<std::int64_t> stamp, bool allow, const std::function<bool()> &grant) {
    return platf::exchange_files(*session, op, win_clipboard::detail::to_utf8(path.generic_wstring()), {}, 0, size, stamp, {}, out, grant, {}, {}, nullptr, allow);
  };
  for (const bool allow : {false, true}) {
    ASSERT_EQ(exchange("capabilities", {}, 0, {}, allow, active), 200);
    EXPECT_EQ(nlohmann::json::parse(out).at("deleteFiles"), allow);
    EXPECT_EQ(nlohmann::json::parse(out).at("deleteDirectories"), allow);
  }
  EXPECT_EQ(exchange("delete", {}, 0, {}, false, active), 403);
  if (exchange("list", {}, 0, {}, true, active) == 403) {
    GTEST_SKIP() << "This test needs an unlocked interactive Windows desktop";
  }
  GUID guid {};
  ASSERT_EQ(CoCreateGuid(&guid), S_OK);
  wchar_t unique[40] {};
  ASSERT_TRUE(StringFromGUID2(guid, unique, 40));
  const auto dir = fs::temp_directory_path() / unique;
  ASSERT_TRUE(fs::create_directory(dir));
  const auto file = dir / L"delete me.txt";
  ASSERT_EQ(exchange("folder-path", dir, 0, {}, false, active), 200);
  EXPECT_NE(out.find(win_clipboard::detail::to_utf8(dir.filename().native())), std::string::npos);
  auto cleanup = util::fail_guard([&] {
    std::error_code ignored;
    fs::remove(file, ignored);
    fs::remove(dir, ignored);
  });
  ASSERT_TRUE(std::ofstream(file) << "hello");
  ASSERT_EQ(exchange("list", dir, 0, {}, true, active), 200);
  const auto stamp = std::stoll(nlohmann::json::parse(out).at("entries").at(0).at("modified").get<std::string>());
  EXPECT_EQ(exchange("delete", file, 5, {}, true, active), 400);
  EXPECT_EQ(exchange("delete", file, 4, stamp, true, active), 409);
  EXPECT_EQ(exchange("delete", file, 5, stamp + 1, true, active), 409);
  EXPECT_EQ(exchange("delete", dir, 0, stamp, true, active), 403);
  int checks = 0;
  EXPECT_EQ(exchange("delete", file, 5, stamp, true, [&] {
              return ++checks == 1;
            }),
            403);
  ASSERT_TRUE(fs::exists(file));
  EXPECT_EQ(exchange("delete", file, 5, stamp, true, active), 204);
  EXPECT_FALSE(fs::exists(file));
  EXPECT_EQ(exchange("delete", file, 5, stamp, true, active), 404);
  const auto tree = dir / L"tree";
  ASSERT_TRUE(fs::create_directories(tree / L"empty"));
  ASSERT_TRUE(std::ofstream(tree / L"child") << "contents");
  ASSERT_EQ(exchange("list", dir, 0, {}, true, active), 200);
  const auto folder_stamp = std::stoll(nlohmann::json::parse(out).at("entries").at(0).at("modified").get<std::string>());
  EXPECT_EQ(exchange("rmdir", tree, 0, folder_stamp, false, active), 403);
  EXPECT_EQ(exchange("rmdir", tree, 0, {}, true, active), 400);
  EXPECT_EQ(exchange("rmdir", tree, 0, folder_stamp + 1, true, active), 409);
  EXPECT_TRUE(fs::exists(tree / L"child"));
  EXPECT_EQ(exchange("rmdir", tree, 0, folder_stamp, true, active), 204);
  EXPECT_FALSE(fs::exists(tree));
  EXPECT_TRUE(fs::exists(dir));
}

TEST(ClipboardBatchTest, RejectsInvalidManifestsAndUses64BitLengths) {
  using namespace platf::clipboard_upload;
  for (const auto &body : {"[]", "{}", "[null]", R"([{"name":"../bad","size":5}])", R"([{"name":"A","size":5},{"name":"a","size":5}])", R"([{"name":"ok","size":-1}])", R"([{"name":"ok","size":1.5}])"}) {
    std::vector<entry_t> entries;
    std::uint64_t total {};
    EXPECT_EQ(manifest(body, entries, total), 400);
  }
  std::vector<entry_t> entries;
  std::uint64_t total {};
  EXPECT_EQ(manifest(R"([{"name":"large.bin","size":4294967297}])", entries, total), 200);
  EXPECT_EQ(total, 4294967297ULL);
  entries.clear();
  total = 0;
  EXPECT_EQ(manifest(R"([{"name":"a","size":9007199254740993},{"name":"b","size":17179869185}])", entries, total), 200);
  EXPECT_EQ(total, 9007199254740993ULL + 17179869185ULL);
  entries.clear();
  total = 0;
  EXPECT_EQ(manifest(R"([{"name":"a","size":9223372036854775807},{"name":"b","size":9223372036854775807},{"name":"c","size":2}])", entries, total), 413);
}

TEST(ClipboardBatchTest, StagesFolderSelectionsAndRollsBackOnlyThePrivateTree) {
  namespace fs = std::filesystem;
  const auto root = fs::temp_directory_path() / platf::new_file_lease();
  auto cleanup = util::fail_guard([&] {
    std::error_code ec;
    fs::remove_all(root, ec);
  });
  constexpr auto manifest = R"([{"name":"photos/","size":0},{"name":"photos/empty/","size":0},{"name":"photos/one","size":3},{"name":"loose","size":0}])";
  {
    platf::clipboard_upload::batch_t batch;
    ASSERT_EQ(batch.begin(root, manifest), 200);
    EXPECT_TRUE(fs::is_directory(batch.directory / "photos/empty"));
    EXPECT_EQ(batch.append(0, 0, "x"), 413);
    EXPECT_EQ(batch.append(2, 0, "abc"), 204);
    DWORD revision {};
    EXPECT_EQ(batch.commit({}, revision, [] {
      return false;
    }),
              403);
  }
  EXPECT_TRUE(fs::is_empty(root));
  for (const auto body : {R"([{"name":"a/","size":1}])", R"([{"name":"a/b","size":1}])", R"([{"name":"a/","size":0},{"name":"a/../escape","size":1}])", R"([{"name":"a/","size":0},{"name":"A","size":0}])"}) {
    platf::clipboard_upload::batch_t invalid;
    EXPECT_EQ(invalid.begin(root, body), 400);
    EXPECT_TRUE(fs::is_empty(root));
  }
}

  #ifdef GTEST_HAS_DEATH_TEST
namespace {
  void check_clipboard_publication() {
    const auto name = L"SunshineClipboardTest-" + std::to_wstring(GetCurrentProcessId());
    const auto station = CreateWindowStationW(name.c_str(), 0, WINSTA_ALL_ACCESS, nullptr);
    ASSERT_TRUE(station && SetProcessWindowStation(station));
    const auto desktop = CreateDesktopW(L"Private", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    ASSERT_TRUE(desktop && SetThreadDesktop(desktop));

    const auto original = GetClipboardSequenceNumber();
    DWORD sequence {};
    int result {};
    {
      win_clipboard::detail::clipboard_lock held(nullptr);
      ASSERT_TRUE(held.opened);
      std::thread writer([&] {
        result = win_clipboard::publish(L"example", false, sequence, original, {});
      });
      writer.join();
    }
    EXPECT_EQ(result, 423);
    EXPECT_EQ(GetClipboardSequenceNumber(), original);
    EXPECT_EQ(win_clipboard::publish(L"example", false, sequence, original + 1, {}), 409);
    EXPECT_EQ(sequence, original);
    EXPECT_EQ(win_clipboard::publish(L"example", false, sequence, original, {}), 204);
    EXPECT_NE(sequence, original);
  }

  void check_clipboard_download() {
    const auto name = L"SunshineDownloadTest-" + std::to_wstring(GetCurrentProcessId());
    const auto station = CreateWindowStationW(name.c_str(), 0, WINSTA_ALL_ACCESS, nullptr);
    ASSERT_TRUE(station && SetProcessWindowStation(station));
    const auto desktop = CreateDesktopW(L"Private", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    ASSERT_TRUE(desktop && SetThreadDesktop(desktop));
    const auto directory = std::filesystem::temp_directory_path() / name;
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    const auto path = directory / L"part.bin";
    const std::string prefix(platf::file_transfer::chunk_size, 'a');
    ASSERT_TRUE(std::ofstream(path, std::ios::binary) << prefix << "tail");
    const auto second = directory / L"second.bin";
    ASSERT_TRUE(std::ofstream(second, std::ios::binary) << "next");
    auto selection = path.native();
    selection.push_back(L'\0');
    selection += second.native();
    selection.push_back(L'\0');
    const DWORD original = GetClipboardSequenceNumber();
    DWORD sequence {};
    ASSERT_EQ(win_clipboard::publish(selection, true, sequence, original, {}), 204);
    std::string output;
    platf::clipboard_download_t snapshot;
    const LUID owner {3, 7};
    ASSERT_EQ(snapshot.begin(original, original, sequence, owner, output), 200);
    ASSERT_FALSE(output.empty());
    EXPECT_EQ(output.front(), 'M');
    const auto manifest = nlohmann::json::parse(output.substr(1));
    EXPECT_EQ(manifest.at("files").at(0).at("name"), "part.bin");
    EXPECT_EQ(manifest.at("files").at(0).at("size"), std::to_string(prefix.size() + 4));
    platf::clipboard_download_request_t request {"chunk", snapshot.lease, 0, 0};
    ASSERT_EQ(snapshot.read(request, sequence, sequence, owner, output), 200);
    EXPECT_EQ(output, prefix);
    request.offset = prefix.size();
    ASSERT_EQ(snapshot.read(request, sequence, sequence, owner, output), 200);
    EXPECT_EQ(output, "tail");
    EXPECT_EQ(snapshot.read(request, sequence, sequence + 1, owner, output), 409);
    EXPECT_EQ(snapshot.read(request, sequence, sequence, owner, output), 409);
    ASSERT_EQ(snapshot.begin(original, original, sequence, owner, output, true), 200);
    EXPECT_TRUE(nlohmann::json::parse(output.substr(1)).at("metadataOnly").get<bool>());
    EXPECT_TRUE(snapshot.files.empty());
    const auto moved = directory / L"original.bin";
    std::filesystem::rename(path, moved);
    platf::clipboard_download_t task;
    EXPECT_NE(snapshot.reopen(snapshot.lease, sequence, owner, task, output), 200);
    ASSERT_TRUE(std::ofstream(path, std::ios::binary) << prefix << "fake");
    EXPECT_EQ(snapshot.reopen(snapshot.lease, sequence, owner, task, output), 409);
    ASSERT_TRUE(std::filesystem::remove(path));
    std::filesystem::rename(moved, path);
    // The clipboard manager can publish another format after the client receives
    // its descriptors. A paste claims the original selection without rereading it.
    DWORD newer {};
    ASSERT_EQ(win_clipboard::publish(L"another format", false, newer, sequence, {}), 204);
    ASSERT_NE(newer, sequence);
    EXPECT_EQ(snapshot.reopen(snapshot.lease + 1, sequence, owner, task, output), 409);
    EXPECT_EQ(snapshot.reopen(snapshot.lease, newer, owner, task, output), 409);
    EXPECT_EQ(snapshot.reopen(snapshot.lease, sequence, LUID {4, 7}, task, output), 409);
    ASSERT_EQ(snapshot.reopen(snapshot.lease, sequence, owner, task, output), 200);
    std::filesystem::rename(path, moved);
    ASSERT_TRUE(std::ofstream(path, std::ios::binary) << prefix << "fake");
    request.lease = task.lease;
    ASSERT_EQ(task.read(request, sequence, {}, owner, output), 200);
    EXPECT_EQ(output, "tail");
    ASSERT_TRUE(DeleteFileW(moved.c_str()));
    request.file = 1;
    request.offset = 0;
    ASSERT_EQ(task.read(request, sequence, {}, owner, output), 200);
    EXPECT_EQ(output, "next");
    request.op = "finish";
    ASSERT_EQ(task.read(request, sequence, {}, owner, output), 204);
    EXPECT_EQ(snapshot.reopen(snapshot.lease, sequence, owner, task, output), 409);
    EXPECT_TRUE(task.files.empty());
    EXPECT_FALSE(std::filesystem::exists(moved));
    EXPECT_TRUE(std::filesystem::remove(second));
    EXPECT_TRUE(std::filesystem::remove(path));
    EXPECT_TRUE(std::filesystem::remove(directory));
  }
}  // namespace

// Death-test subprocesses capture stderr themselves, so use the plain GoogleTest fixture.
GTEST_TEST(ClipboardPublicationDeathTest, DistinguishesBusyClipboardFromChangedRevisionBeforeRetry) {
  // A fresh process and window station keep the user's clipboard untouched.
  ASSERT_EXIT(
    {
      check_clipboard_publication();
      std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
    },
    ::testing::ExitedWithCode(0),
    ""
  );
}

GTEST_TEST(ClipboardDownloadDeathTest, ReadsPinnedChunksUntilClipboardChanges) {
  ASSERT_EXIT(
    {
      check_clipboard_download();
      std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
    },
    ::testing::ExitedWithCode(0),
    ""
  );
}
  #endif

namespace {
  namespace fs = std::filesystem;
  constexpr std::string_view resume_manifest = R"([{"name":"part","size":3,"sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},{"name":"empty","size":0,"sha256":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}])";

  struct resume_test_t {
    platf::clipboard_upload::resume_cache_t cache;
    fs::path root = fs::temp_directory_path() / ("sunshine-resume-test-" + std::to_string(GetCurrentProcessId()));
    std::string cert = "paired-a";
    LUID logon {7, 3};
    std::uint64_t stream = 1;
    bool active = true;
    bool allow_replace = false;
    std::string lease;
    std::string task;

    platf::clipboard_upload_result_t choose(const fs::path &destination, std::string_view previous = {}, std::weak_ptr<platf::clipboard_upload_session_t> preparing_stream = {}) {
      const auto resolve = [&](auto &target) {
        target.directory = destination;
        return platf::file_transfer::pin_directory(destination, target.pins);
      };
      const auto permitted = [&] {
        return active;
      };
      return cache.capture_target(cert, logon, stream, task, decltype(cache)::clock::now(), permitted, resolve, previous, preparing_stream);
    }

    int capture(const fs::path &destination) {
      return choose(destination).status;
    }

    platf::clipboard_upload_result_t send(std::string_view op, std::string_view bytes = {}, std::uint64_t offset = 0, std::optional<DWORD> since = {}) {
      const auto id = op == "resume" || op == "prepare" || op == "discard" ? std::string_view {} : std::string_view(lease);
      return cache.exchange(root, cert, logon, stream, op, id, 0, offset, since, bytes, [&] {
        return active;
      },
                            task,
                            allow_replace);
    }

    void resume(std::uint64_t expected) {
      const auto result = send("resume", resume_manifest);
      ASSERT_EQ(result.status, 200);
      const auto response = nlohmann::json::parse(result.id);
      auto next = response.at("id").get<std::string>();
      EXPECT_EQ(next.size(), 32U);
      EXPECT_NE(next, lease);
      EXPECT_EQ(response.at("offsets"), (nlohmann::json {expected, 0}));
      lease = std::move(next);
    }
  };
}  // namespace

TEST(ClipboardResumeTest, DeliversInterleavedPastesToTheirCapturedFoldersWithoutReplacingExistingFiles) {
  resume_test_t t;
  t.root += "-" + platf::new_file_lease();
  const auto a = t.root / L"a";
  const auto b = t.root / L"b";
  ASSERT_TRUE(fs::create_directories(a));
  ASSERT_TRUE(fs::create_directories(b));
  std::ofstream(a / L"part") << "keep";
  t.task.assign(32, 'a');
  EXPECT_EQ(t.send("resume", resume_manifest).status, 404);
  ASSERT_EQ(t.capture(a), 204);
  ASSERT_EQ(t.capture(b), 204);
  t.resume(0);
  const auto a_lease = t.lease;
  EXPECT_EQ(t.send("chunk", "a").status, 204);
  EXPECT_EQ(t.send("deliver").status, 400);
  t.task.assign(32, 'b');
  ASSERT_EQ(t.capture(b), 204);
  t.resume(0);
  const auto b_lease = t.lease;
  EXPECT_EQ(t.send("chunk", "abc").status, 204);
  t.lease = a_lease;
  EXPECT_EQ(t.send("abort").status, 404);
  t.lease = b_lease;
  EXPECT_EQ(t.send("commit", {}, 0, 0).status, 400);
  EXPECT_EQ(t.send("deliver").status, 204);
  EXPECT_EQ(t.send("deliver").status, 204);
  EXPECT_FALSE(fs::exists(b / L"part (2)"));
  t.task.assign(32, 'a');
  ++t.stream;
  auto changed = nlohmann::json::parse(resume_manifest);
  changed[0]["name"] = "wrong";
  EXPECT_EQ(t.send("resume", changed.dump()).conflict, "manifest");
  t.resume(1);
  EXPECT_EQ(t.send("chunk", "bc", 1).status, 204);
  t.active = false;
  EXPECT_EQ(t.send("deliver").status, 403);
  t.active = true;
  EXPECT_EQ(t.send("deliver").status, 204);
  t.resume(3);
  EXPECT_EQ(t.send("deliver").status, 204);
  EXPECT_FALSE(fs::exists(a / L"part (3)"));
  std::string value;
  EXPECT_TRUE(std::ifstream(a / L"part") >> value);
  EXPECT_EQ(value, "keep");
  EXPECT_TRUE(std::ifstream(a / L"part (2)") >> value);
  EXPECT_EQ(value, "abc");
  EXPECT_TRUE(std::ifstream(b / L"part") >> value);
  EXPECT_EQ(value, "abc");
  EXPECT_EQ(fs::file_size(a / L"empty"), 0U);
  EXPECT_EQ(fs::file_size(b / L"empty"), 0U);
  EXPECT_EQ(fs::remove_all(t.root), 8U);
}

TEST(ClipboardResumeTest, MergesFolderPasteAndReadsItBackAtTheOriginalIndices) {
  resume_test_t t;
  t.root += "-" + platf::new_file_lease();
  auto cleanup = util::fail_guard([&] {
    std::error_code ec;
    fs::remove_all(t.root, ec);
  });
  const auto photos = t.root / L"photos";
  ASSERT_TRUE(fs::create_directories(photos));
  ASSERT_TRUE(std::ofstream(photos / "part") << "old");
  ASSERT_TRUE(std::ofstream(photos / "untouched") << "original");
  t.task.assign(32, 'f');
  ASSERT_EQ(t.capture(t.root), 204);
  auto files = nlohmann::json::parse(resume_manifest);
  files[0]["name"] = "photos/part";
  files[1]["name"] = "photos/nested/zero";
  files.insert(files.begin(), nlohmann::json {{"name", "photos/nested/"}, {"size", 0}});
  files.insert(files.begin(), nlohmann::json {{"name", "photos/"}, {"size", 0}});
  files.push_back({{"name", "photos/empty/"}, {"size", 0}});
  auto result = t.send("prepare", files.dump());
  ASSERT_EQ(result.conflict, "file");
  EXPECT_EQ(nlohmann::json::parse(result.id)["file"], 2);
  files[2]["resolution"] = "keep-both";
  result = t.send("prepare", files.dump());
  ASSERT_EQ(result.status, 200);
  t.lease = nlohmann::json::parse(result.id)["id"].get<std::string>();
  ASSERT_EQ(t.cache.exchange(t.root, t.cert, t.logon, t.stream, "chunk", t.lease, 2, 0, {}, "abc", [] {
                     return true;
                   },
                             t.task)
              .status,
            204);
  ASSERT_EQ(t.send("deliver").status, 204);
  EXPECT_EQ(t.send("deliver").status, 204);
  EXPECT_TRUE(fs::is_empty(photos / "empty"));
  EXPECT_EQ(fs::file_size(photos / "nested/zero"), 0U);
  EXPECT_EQ(fs::file_size(photos / "untouched"), 8U);
  std::string value;
  EXPECT_TRUE(std::ifstream(photos / "part") >> value);
  EXPECT_EQ(value, "old");
  EXPECT_TRUE(std::ifstream(photos / "part (2)") >> value);
  EXPECT_EQ(value, "abc");

  platf::clipboard_download_t offer, task;
  const DWORD revision = GetClipboardSequenceNumber();
  std::string output;
  EXPECT_EQ(offer.pin({photos}, revision, t.logon, output, true, false), 204);
  ASSERT_EQ(offer.pin({photos}, revision, t.logon, output, true, true), 200);
  const auto metadata = nlohmann::json::parse(output.substr(1)).at("files");
  ASSERT_EQ(offer.reopen(offer.lease, revision, t.logon, task, output), 200);
  EXPECT_EQ(nlohmann::json::parse(output.substr(1)).at("files"), metadata);
  EXPECT_EQ(metadata[0]["name"], "photos/");
  auto part = std::find_if(metadata.begin(), metadata.end(), [](const auto &item) {
    return item["name"] == "photos/part (2)";
  });
  ASSERT_NE(part, metadata.end());
  const auto index = static_cast<std::uint32_t>(std::distance(metadata.begin(), part));
  EXPECT_EQ(task.read({"chunk", task.lease, index, 0}, revision, {}, t.logon, output), 200);
  EXPECT_EQ(output, "abc");
  EXPECT_EQ(task.read({"chunk", task.lease, 0, 0}, revision, {}, t.logon, output), 400);
  EXPECT_EQ(task.read({"finish", task.lease}, revision, {}, t.logon, output), 204);
  ASSERT_TRUE(std::ofstream(photos / "part") << "changed");
  EXPECT_EQ(offer.reopen(offer.lease, revision, t.logon, task, output), 409);
}

TEST(ClipboardResumeTest, AsksBeforeStagingAndAppliesEachExplorerChoiceOnlyOnce) {
  for (const std::string_view choice : {"skip", "keep-both", "replace"}) {
    SCOPED_TRACE(choice);
    resume_test_t t;
    t.root += "-" + platf::new_file_lease();
    ASSERT_TRUE(fs::create_directories(t.root));
    const auto path = t.root / L"part";
    std::ofstream(path) << "original";
    std::ofstream(t.root / L"part (2)") << "occupied";
    t.task.assign(32, 'a');
    ASSERT_EQ(t.capture(t.root), 204);
    auto files = nlohmann::json::parse(resume_manifest);
    auto result = t.send("prepare", files.dump());
    ASSERT_EQ(result.conflict, "file");
    auto conflict = nlohmann::json::parse(result.id);
    EXPECT_EQ(conflict.at("reason"), "permission");
    EXPECT_EQ(conflict.at("version"), "");
    EXPECT_EQ(conflict.at("file"), 0);
    EXPECT_EQ(conflict.at("path"), win_clipboard::detail::to_utf8(path.native()));
    EXPECT_EQ(std::distance(fs::directory_iterator(t.root), fs::directory_iterator {}), 2);
    t.allow_replace = true;
    result = t.send("prepare", files.dump());
    ASSERT_EQ(result.status, 409);
    conflict = nlohmann::json::parse(result.id);
    ASSERT_NE(conflict.at("version"), "");
    files[0]["resolution"] = false;
    EXPECT_EQ(t.send("prepare", files.dump()).status, 400);
    files[0]["resolution"] = choice == "replace" ? conflict.at("version").get<std::string>() : std::string(choice);
    result = t.send("prepare", files.dump());
    ASSERT_EQ(result.status, 200);
    auto prepared = nlohmann::json::parse(result.id);
    t.lease = prepared.at("id").get<std::string>();
    if (choice == "skip") {
      EXPECT_EQ(prepared.at("skipped"), nlohmann::json::array({0}));
      EXPECT_EQ(prepared.at("offsets"), nlohmann::json::array({3, 0}));
    } else {
      EXPECT_EQ(prepared.at("offsets"), nlohmann::json::array({0, 0}));
      EXPECT_EQ(t.send("deliver", R"({"file":0,"resolution":"skip"})").status, 400);
      EXPECT_EQ(t.send("chunk", "abc").status, 204);
    }
    EXPECT_EQ(fs::file_size(path), 8U);
    ASSERT_EQ(t.send("deliver").status, 204);
    ++t.stream;
    result = t.send("prepare", resume_manifest);
    ASSERT_EQ(result.status, 200);
    prepared = nlohmann::json::parse(result.id);
    t.lease = prepared.at("id").get<std::string>();
    EXPECT_TRUE(prepared.at("committed").get<bool>());
    EXPECT_EQ(t.send("deliver").status, 204);
    std::string content;
    EXPECT_TRUE(std::ifstream(path) >> content);
    EXPECT_EQ(content, choice == "replace" ? "abc" : "original");
    EXPECT_TRUE(std::ifstream(t.root / L"part (2)") >> content);
    EXPECT_EQ(content, "occupied");
    EXPECT_EQ(fs::exists(t.root / L"part (3)"), choice == "keep-both");
    if (choice == "keep-both") {
      EXPECT_TRUE(std::ifstream(t.root / L"part (3)") >> content);
      EXPECT_EQ(content, "abc");
    }
    EXPECT_FALSE(fs::exists(t.root / L"part (4)"));
    EXPECT_EQ(fs::file_size(t.root / L"empty"), 0U);
    EXPECT_GT(fs::remove_all(t.root), 0U);
  }
}

TEST(ClipboardResumeTest, ReportsAllInitialCollisionsAndRechecksEachApprovedVersion) {
  resume_test_t t;
  t.root += "-" + platf::new_file_lease();
  ASSERT_TRUE(fs::create_directories(t.root / L"empty"));
  std::ofstream(t.root / L"part") << "original";
  t.task.assign(32, 'a');
  ASSERT_EQ(t.capture(t.root), 204);
  t.allow_replace = true;
  auto files = nlohmann::json::parse(resume_manifest);
  auto result = t.send("prepare", files.dump());
  ASSERT_EQ(result.conflict, "file");
  auto response = nlohmann::json::parse(result.id);
  const auto conflicts = response.at("conflicts");
  ASSERT_EQ(conflicts.size(), 2U);
  response.erase("conflicts");
  EXPECT_EQ(response, conflicts[0]);
  EXPECT_EQ(conflicts[0].at("file"), 0);
  EXPECT_NE(conflicts[0].at("version"), "");
  EXPECT_EQ(conflicts[1].at("file"), 1);
  EXPECT_EQ(conflicts[1].at("reason"), "folder");
  EXPECT_EQ(conflicts[1].at("version"), "");
  files[0]["resolution"] = conflicts[0].at("version");
  files[1]["resolution"] = "skip";
  std::ofstream(t.root / L"part") << "updated while choosing";
  result = t.send("prepare", files.dump());
  ASSERT_EQ(result.conflict, "file");
  response = nlohmann::json::parse(result.id);
  EXPECT_FALSE(response.contains("conflicts"));
  EXPECT_EQ(response.at("file"), 0);
  EXPECT_NE(response.at("version"), files[0].at("resolution"));
  files[0]["resolution"] = "keep-both";
  result = t.send("prepare", files.dump());
  ASSERT_EQ(result.status, 200);
  response = nlohmann::json::parse(result.id);
  EXPECT_EQ(response.at("skipped"), nlohmann::json::array({1}));
  t.lease = response.at("id").get<std::string>();
  ASSERT_EQ(t.send("chunk", "abc").status, 204);
  ASSERT_EQ(t.send("deliver").status, 204);
  EXPECT_TRUE(fs::is_directory(t.root / L"empty"));
  EXPECT_EQ(fs::file_size(t.root / L"part"), 22U);
  EXPECT_EQ(fs::file_size(t.root / L"part (2)"), 3U);
  EXPECT_EQ(fs::remove_all(t.root), 4U);
}

TEST(ClipboardResumeTest, RechecksLateAndChangedConflictsBeforePublishingTheRestOfABatch) {
  resume_test_t t;
  t.root += "-" + platf::new_file_lease();
  ASSERT_TRUE(fs::create_directories(t.root));
  t.task.assign(32, 'a');
  ASSERT_EQ(t.capture(t.root), 204);
  t.allow_replace = true;
  const auto prepared = t.send("prepare", resume_manifest);
  ASSERT_EQ(prepared.status, 200);
  t.lease = nlohmann::json::parse(prepared.id).at("id").get<std::string>();
  ASSERT_EQ(t.send("chunk", "abc").status, 204);
  const auto path = t.root / L"empty";
  std::ofstream(path) << "new";
  auto collision = t.send("deliver");
  ASSERT_EQ(collision.conflict, "file");
  auto details = nlohmann::json::parse(collision.id);
  EXPECT_EQ(details.at("file"), 1);
  EXPECT_EQ(fs::file_size(t.root / L"part"), 3U);
  nlohmann::json decision {{"file", 1}, {"resolution", details.at("version")}};
  std::ofstream(path) << "changed";
  collision = t.send("deliver", decision.dump());
  ASSERT_EQ(collision.conflict, "file");
  details = nlohmann::json::parse(collision.id);
  EXPECT_NE(details.at("version"), decision.at("resolution"));
  decision["resolution"] = details.at("version");
  t.allow_replace = false;
  collision = t.send("deliver", decision.dump());
  ASSERT_EQ(collision.conflict, "file");
  EXPECT_EQ(nlohmann::json::parse(collision.id).at("reason"), "permission");
  EXPECT_EQ(fs::file_size(path), 7U);
  ASSERT_TRUE(fs::remove(path));
  ASSERT_TRUE(fs::create_directory(path));
  collision = t.send("deliver");
  ASSERT_EQ(collision.conflict, "file");
  EXPECT_EQ(nlohmann::json::parse(collision.id).at("reason"), "folder");
  EXPECT_EQ(t.send("deliver", R"({"file":9,"resolution":"skip"})").status, 400);
  EXPECT_EQ(t.send("deliver", R"({"file":1,"resolution":"overwrite"})").status, 400);
  decision["resolution"] = "skip";
  ASSERT_EQ(t.send("deliver", decision.dump()).status, 204);
  EXPECT_EQ(t.send("deliver", decision.dump()).status, 204);
  EXPECT_TRUE(fs::is_directory(path));
  EXPECT_FALSE(fs::exists(t.root / L"part (2)"));
  EXPECT_EQ(fs::remove_all(t.root), 3U);
}

TEST(ClipboardResumeTest, RetainsACapturedFolderWhileItsStreamPreparesTheUpload) {
  resume_test_t t;
  t.root += "-" + platf::new_file_lease();
  ASSERT_TRUE(fs::create_directories(t.root));
  auto stream = platf::make_clipboard_upload_session(t.cert);
  t.task.assign(32, 'a');
  ASSERT_EQ(t.choose(t.root, {}, stream).status, 204);
  const auto later = decltype(t.cache)::clock::now() + std::chrono::hours(2);
  t.cache.expire(later);
  t.resume(0);
  t.cache.expire(later);
  EXPECT_EQ(t.send("resume", resume_manifest).status, 404);

  t.task.assign(32, 'b');
  ASSERT_EQ(t.choose(t.root, {}, stream).status, 204);
  stream.reset();
  t.cache.expire(later);
  EXPECT_EQ(t.send("resume", resume_manifest).status, 404);
  EXPECT_TRUE(fs::remove(t.root));
}

TEST(ClipboardResumeTest, CancelsOnlyTheAddressedPasteAndExpiresIdleFolderPins) {
  resume_test_t t;
  t.root += "-" + platf::new_file_lease();
  ASSERT_TRUE(fs::create_directories(t.root));
  t.task.assign(32, 'a');
  ASSERT_EQ(t.capture(t.root), 204);
  t.resume(0);
  ASSERT_EQ(t.send("chunk", "ab").status, 204);
  t.task.assign(32, 'b');
  ASSERT_EQ(t.capture(t.root), 204);
  t.resume(0);
  ASSERT_EQ(t.send("chunk", "a").status, 204);
  ++t.stream;
  t.resume(1);
  --t.stream;
  EXPECT_EQ(t.send("discard").status, 403);
  ++t.stream;
  t.cert = "paired-b";
  EXPECT_EQ(t.send("resume", resume_manifest).status, 404);
  EXPECT_EQ(t.send("discard").status, 204);
  t.cert = "paired-a";
  ++t.logon.LowPart;
  EXPECT_EQ(t.send("resume", resume_manifest).status, 404);
  --t.logon.LowPart;
  EXPECT_EQ(t.send("discard").status, 204);
  EXPECT_EQ(t.send("resume", resume_manifest).status, 404);
  t.task.assign(32, 'a');
  t.resume(2);
  for (char suffix = 'b'; suffix <= 'f'; ++suffix) {
    t.task.assign(32, suffix);
    ASSERT_EQ(t.capture(t.root), 204);
  }
  t.task.assign(32, '1');
  ASSERT_EQ(t.capture(t.root), 204);
  t.task.assign(32, '2');
  ASSERT_EQ(t.capture(t.root), 204);
  t.task.assign(32, '3');
  EXPECT_EQ(t.capture(t.root), 429);
  t.cache.expire(decltype(t.cache)::clock::now() + std::chrono::minutes(31));
  EXPECT_EQ(t.send("resume", resume_manifest).status, 404);
  EXPECT_TRUE(fs::is_empty(t.root));
  EXPECT_TRUE(fs::remove(t.root));
}

TEST(ClipboardResumeTest, ReleasesUnsupportedRevokedAndCancelledTargetCaptures) {
  resume_test_t t;
  using target_t = platf::clipboard_upload::paste_target_t;
  const auto capture = [&](const std::function<int(target_t &)> &resolve) {
    return t.cache.capture_target(t.cert, t.logon, t.stream, t.task, decltype(t.cache)::clock::now(), [&] {
                    return t.active;
                  },
                                  resolve)
      .status;
  };
  auto unsupported = [](target_t &) {
    return 204;
  };
  t.task = "invalid";
  EXPECT_EQ(capture(unsupported), 400);
  t.task.assign(32, 'f');
  EXPECT_EQ(capture(unsupported), 422);
  EXPECT_EQ(t.send("resume", resume_manifest).status, 404);
  EXPECT_EQ(capture([&](target_t &) {
              EXPECT_EQ(t.send("resume", resume_manifest).conflict, "busy");
              EXPECT_EQ(capture(unsupported), 409);
              EXPECT_EQ(t.send("discard").status, 204);
              return 200;
            }),
            404);
  EXPECT_EQ(capture([&](target_t &) {
              t.active = false;
              return 200;
            }),
            403);
  t.active = true;
  EXPECT_EQ(t.send("resume", resume_manifest).status, 404);
}

TEST(ClipboardResumeTest, ReconnectGestureReusesBytesOnlyAtTheSameCapturedDirectory) {
  resume_test_t t;
  t.root += "-" + platf::new_file_lease();
  const auto a = t.root / L"a";
  const auto b = t.root / L"b";
  ASSERT_TRUE(fs::create_directories(a));
  ASSERT_TRUE(fs::create_directories(b));
  const std::string original(32, 'a');
  t.task = original;
  ASSERT_EQ(t.choose(a).task, original);
  t.resume(0);
  ASSERT_EQ(t.send("chunk", "ab").status, 204);

  ++t.stream;
  t.task.assign(32, 'b');
  ASSERT_EQ(t.choose(b, original).task, t.task);
  t.resume(0);
  ASSERT_EQ(t.send("abort").status, 204);
  t.task.assign(32, 'c');
  ASSERT_EQ(t.choose(a, original).task, original);
  ASSERT_EQ(t.choose(b, original).task, original);
  t.task = original;
  t.resume(2);
  ASSERT_EQ(t.send("chunk", "c", 2).status, 204);
  ASSERT_EQ(t.send("deliver").status, 204);

  // A lost final acknowledgement is still recognized after releasing the directory pins.
  ++t.stream;
  t.task.assign(32, 'd');
  ASSERT_EQ(t.choose(a, original).task, original);
  t.task = original;
  t.resume(3);
  ASSERT_EQ(t.send("deliver").status, 204);
  ASSERT_FALSE(fs::exists(a / L"part (2)"));
  ASSERT_TRUE(fs::is_empty(b));
  EXPECT_EQ(fs::remove_all(t.root), 5U);
}

TEST(ClipboardResumeTest, RebindsOnlyMatchingOwnerAndFileRevisionsAndDiscardsOnCancel) {
  resume_test_t t;
  t.resume(0);
  EXPECT_EQ(t.send("chunk", "ab").status, 204);
  const auto old = t.lease;
  t.stream = 2;
  t.resume(2);
  const auto current = t.lease;
  t.lease = old;
  t.stream = 1;
  EXPECT_EQ(t.send("abort").status, 404);
  EXPECT_EQ(t.send("resume", resume_manifest).status, 403);
  EXPECT_EQ(t.send("discard", resume_manifest).status, 403);
  t.stream = 3;
  t.cert = "paired-b";
  t.resume(0);
  EXPECT_EQ(t.send("discard", resume_manifest).status, 204);
  t.cert = "paired-a";
  ++t.logon.LowPart;
  t.resume(0);
  EXPECT_EQ(t.send("abort").status, 204);
  --t.logon.LowPart;
  t.resume(2);
  EXPECT_NE(current, t.lease);
  EXPECT_EQ(t.send("chunk", "a").conflict, "offset");
  EXPECT_EQ(t.send("chunk", "c", 2).status, 204);
  t.active = false;
  EXPECT_EQ(t.send("discard", resume_manifest).status, 403);
  t.active = true;
  EXPECT_EQ(t.send("discard", resume_manifest).status, 204);
  t.resume(0);
  EXPECT_EQ(t.send("chunk", "abc").status, 204);
  auto changed = nlohmann::json::parse(resume_manifest);
  changed[0]["sha256"] = std::string(64, '0');
  auto replacement = t.send("resume", changed.dump());
  ASSERT_EQ(replacement.status, 200);
  t.lease = nlohmann::json::parse(replacement.id).at("id");
  EXPECT_EQ(t.send("chunk", "abc").status, 422);
  EXPECT_EQ(t.send("commit", {}, 0, 0).status, 404);
  EXPECT_TRUE(fs::is_empty(t.root));
  fs::remove(t.root);
}

TEST(ClipboardResumeTest, ExpiresIdleFilesAndEnforcesSharedReservation) {
  resume_test_t t;
  auto huge = nlohmann::json::parse(resume_manifest);
  const auto free = fs::space(t.root.parent_path()).available;
  huge[0]["size"] = free - free / 4;
  ASSERT_EQ(t.send("resume", huge.dump()).status, 200);
  t.cert = "paired-b";
  EXPECT_EQ(t.send("resume", huge.dump()).status, 507);
  t.cache.expire(platf::clipboard_upload::resume_cache_t::clock::now() + std::chrono::minutes(31));
  EXPECT_TRUE(fs::is_empty(t.root));
  t.cert = "paired-a";
  t.resume(0);
  EXPECT_EQ(t.send("abort").status, 204);
  EXPECT_TRUE(fs::is_empty(t.root));
  fs::remove(t.root);
}

  #ifdef GTEST_HAS_DEATH_TEST
GTEST_TEST(ClipboardResumeDeathTest, PublishesVerifiedFilesOnceAcrossLostCommitAcknowledgement) {
  ASSERT_EXIT(
    {
      auto station = CreateWindowStationW((L"SunshineResumeTest" + std::to_wstring(GetCurrentProcessId())).c_str(), 0, WINSTA_ALL_ACCESS, nullptr);
      if (!station || !SetProcessWindowStation(station)) {
        std::_Exit(2);
      }
      auto desk = CreateDesktopW(L"Private", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
      if (!desk || !SetThreadDesktop(desk)) {
        std::_Exit(2);
      }
      resume_test_t t;
      const DWORD original = GetClipboardSequenceNumber();
      t.resume(0);
      EXPECT_EQ(t.send("chunk", "ab").status, 204);
      ++t.stream;
      t.resume(2);
      EXPECT_EQ(t.send("chunk", "c", 2).status, 204);
      EXPECT_EQ(t.send("commit").status, 400);
      EXPECT_EQ(t.send("commit", {}, 0, original + 1).conflict, "sequence");
      const auto published = t.send("commit", {}, 0, original);
      EXPECT_EQ(published.status, 204);
      EXPECT_EQ(published.sequence, GetClipboardSequenceNumber());
      ++t.stream;
      t.resume(3);
      EXPECT_EQ(t.send("commit", {}, 0, original).sequence, published.sequence);
      EXPECT_EQ(GetClipboardSequenceNumber(), published.sequence);
      {
        win_clipboard::detail::clipboard_lock held(nullptr);
        EXPECT_TRUE(held.opened && IsClipboardFormatAvailable(CF_HDROP));
      }
      EXPECT_EQ(t.send("discard", resume_manifest).status, 204);
      for (const auto &dir : fs::directory_iterator(t.root)) {
        fs::remove(dir.path() / L"part");
        fs::remove(dir.path() / L"empty");
        fs::remove(dir.path());
      }
      fs::remove(t.root);
      std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
    },
    ::testing::ExitedWithCode(0),
    ""
  );
}
  #endif

TEST(ClipboardBatchTest, RejectsMissingOrOutOfOrderBytesAndCleansUpRevokedBatches) {
  using namespace platf::clipboard_upload;
  const auto root = fs::temp_directory_path() / ("sunshine-upload-test-" + std::to_string(GetCurrentProcessId()));
  const std::string bytes(chunk_limit, 'z');
  const auto original = GetClipboardSequenceNumber();
  {
    batch_t batch;
    ASSERT_EQ(batch.begin(root, R"([{"name":"large.bin","size":20971521},{"name":"empty","size":0}])"), 200);
    EXPECT_EQ(batch.id.size(), 32U);
    DWORD revision {};
    EXPECT_EQ(batch.append(2, 0, "x"), 400);
    EXPECT_EQ(batch.append(0, 1, "x"), 409);
    EXPECT_EQ(batch.append(1, 0, "x"), 413);
    EXPECT_EQ(batch.commit({}, revision, [] {
      return false;
    }),
              400);
    for (std::uint64_t offset = 0; offset < 5 * chunk_limit; offset += chunk_limit) {
      ASSERT_EQ(batch.append(0, offset, bytes), 204);
      EXPECT_EQ(batch.append(0, offset, bytes), 409);
    }
    ASSERT_EQ(batch.append(0, 5 * chunk_limit, "!"), 204);
    EXPECT_EQ(batch.commit({}, revision, [] {
      return false;
    }),
              403);
    EXPECT_EQ(GetClipboardSequenceNumber(), original);
  }
  EXPECT_TRUE(fs::is_empty(root));
  fs::remove(root);
}
#endif
