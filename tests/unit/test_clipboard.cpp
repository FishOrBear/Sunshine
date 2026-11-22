/**
 * @file tests/unit/test_clipboard.cpp
 * @brief Exercise per-session clipboard negotiation and Windows clipboard message validation.
 */
// test includes
#include "../tests_common.h"

// local includes
#include "src/clipboard.h"

#ifdef _WIN32
  // platform includes
  #include "src/platform/windows/clipboard.h"
  #include "src/platform/windows/clipboard_resume.h"
  #include "src/platform/windows/clipboard_upload.h"
  #include "src/platform/windows/clipboard_win32.h"
  #include "src/platform/windows/file_sessions.h"

  #include <thread>
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

TEST(FileBrowserTest, CopiesInChunksAndCleansAbandonedUploads) {
  namespace fs = std::filesystem;
  auto session = platf::make_file_browser_session();
  std::string out;
  const std::string id(32, 'e');
  const auto active = []() {
    return true;
  };
  const auto op = [&](std::string_view action, const fs::path &path, std::uint64_t offset = 0, std::uint64_t size = 0, std::string_view body = {}) {
    out.clear();
    return platf::exchange_files(*session, action, win_clipboard::detail::to_utf8(path.generic_wstring()), id, offset, size, {}, body, out, active);
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
  EXPECT_EQ(manifest(R"([{"name":"a","size":17179869184},{"name":"b","size":1}])", entries, total), 413);
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
    std::string lease;

    platf::clipboard_upload_result_t send(std::string_view op, std::string_view bytes = {}, std::uint64_t offset = 0, std::optional<DWORD> since = {}) {
      const auto id = op == "resume" || op == "discard" ? std::string_view {} : std::string_view(lease);
      return cache.exchange(root, cert, logon, stream, op, id, 0, offset, since, bytes, [&] {
        return active;
      });
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
  t.resume(0);
  EXPECT_EQ(t.send("chunk", "a").status, 204);
  auto huge = nlohmann::json::parse(resume_manifest);
  huge[0]["size"] = platf::clipboard_upload::batch_limit;
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
