/**
 * @file tests/unit/test_file_transfer.cpp
 * @brief Exercise Windows file transfer recovery and content verification on real files.
 */
// test includes
#include "../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <fstream>

  // local includes
  #include "src/platform/windows/clipboard.h"
  #include "src/platform/windows/file_transfer.h"

  // Win32 IOCTL definitions require the Windows types supplied by file_transfer.h.
  #include <winioctl.h>

namespace {
  namespace files = platf::file_transfer;
  namespace fs = std::filesystem;
  constexpr auto abc_sha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  constexpr auto empty_sha = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

  /** @brief Isolate each test under a randomly named directory and authorize a mock desktop user. */
  struct transfer_test_t {
    fs::path directory = fs::temp_directory_path() / ("sunshine-transfer-" + platf::new_file_lease());
    fs::path destination = directory / L"result.bin";
    files::resume_store_t store;
    std::string task = platf::new_file_lease();
    std::string cert = "one", lease, output;
    LUID logon {1, 0};
    std::uint64_t session = 1;
    std::optional<std::uint64_t> ack;
    bool active = true;
    bool can_start_transfer = true;
    std::string replace;

    transfer_test_t() {
      fs::create_directory(directory);
    }

    ~transfer_test_t() {
      store.expire(files::resume_store_t::clock::time_point::max());
      std::error_code ec;
      fs::remove_all(directory, ec);
    }

    int call(std::string_view op, std::uint64_t offset = 0, std::string_view bytes = {}, std::string_view sha = abc_sha, std::uint64_t size = 3) {
      output.clear();
      ack.reset();
      const bool bind = op == "prepare-upload" || op == "resume" || op == "discard";
      const int status = store.exchange(op, cert, logon, session, destination, bind ? task : lease, sha, offset, size, bytes, output, ack, [&] {
        return active;
      },
                                        op == "prepare-upload" || op == "resume" || op == "commit" ? replace : "",
                                        can_start_transfer);
      if (op == "resume" && status == 200) {
        lease = nlohmann::json::parse(output)["id"].get<std::string>();
      }
      return status;
    }
  };
}  // namespace

TEST(StandaloneFileTransferTest, RotatesLeaseAndResumesAcceptedBytesAndCommittedReceipt) {
  transfer_test_t t;
  ASSERT_EQ(t.call("resume"), 200);
  ASSERT_EQ(t.call("chunk", 0, "ab"), 204);
  ASSERT_EQ(t.ack, 2);
  EXPECT_EQ(t.call("chunk", 0, "ab"), 409);
  EXPECT_EQ(t.ack, 2);
  EXPECT_FALSE(fs::exists(t.destination));
  const auto old = t.lease;
  ++t.session;
  ASSERT_EQ(t.call("resume"), 200);
  EXPECT_EQ(nlohmann::json::parse(t.output)["offset"], "2");
  EXPECT_NE(old, t.lease);
  std::string current = t.lease;
  t.lease = old;
  EXPECT_EQ(t.call("chunk", 2, "c"), 404);
  t.lease = current;
  EXPECT_EQ(t.call("commit"), 409);
  ASSERT_EQ(t.call("chunk", 2, "c"), 204);
  ASSERT_EQ(t.call("commit"), 204);
  EXPECT_EQ(t.call("commit"), 204);
  ++t.session;
  ASSERT_EQ(t.call("resume"), 200);
  EXPECT_EQ(nlohmann::json::parse(t.output)["complete"], true);
  const auto stamp = fs::last_write_time(t.destination);
  std::ofstream(t.destination, std::ios::binary) << "xyz";
  fs::last_write_time(t.destination, stamp);
  EXPECT_EQ(t.call("resume"), 409);
  EXPECT_EQ(t.call("discard"), 204);
  EXPECT_TRUE(fs::exists(t.destination));
}

TEST(StandaloneFileTransferTest, EnforcesDigestGrantOwnershipCancellationAndExpiry) {
  transfer_test_t t;
  ASSERT_EQ(t.call("resume"), 200);
  EXPECT_EQ(t.call("resume", 0, {}, empty_sha), 409);
  t.cert = "two";
  EXPECT_EQ(t.call("chunk", 0, "a"), 404);
  t.cert = "one";
  ++t.logon.LowPart;
  EXPECT_EQ(t.call("chunk", 0, "a"), 404);
  --t.logon.LowPart;
  ++t.session;
  EXPECT_EQ(t.call("resume"), 200);
  --t.session;
  EXPECT_EQ(t.call("resume"), 403);
  EXPECT_EQ(t.call("discard"), 403);
  ++t.session;
  t.active = false;
  EXPECT_EQ(t.call("chunk", 0, "abc"), 403);
  t.active = true;
  EXPECT_EQ(t.call("chunk", 0, "abd"), 204);
  EXPECT_EQ(t.call("commit"), 422);
  EXPECT_TRUE(fs::is_empty(t.directory));
  EXPECT_EQ(t.call("resume"), 200);
  EXPECT_EQ(t.call("discard"), 204);
  EXPECT_EQ(t.call("discard"), 204);
  EXPECT_TRUE(fs::is_empty(t.directory));
  EXPECT_EQ(t.call("resume"), 200);
  t.store.expire(files::resume_store_t::clock::now() + platf::file_resume_lifetime);
  EXPECT_TRUE(fs::is_empty(t.directory));
  EXPECT_EQ(t.call("chunk", 0, "abc"), 404);
}

TEST(StandaloneFileTransferTest, PublishesZeroBytesAndPreservesExistingDestinations) {
  transfer_test_t t;
  ASSERT_EQ(t.call("resume", 0, {}, empty_sha, 0), 200);
  EXPECT_EQ(t.call("commit"), 204);
  ASSERT_TRUE(fs::exists(t.destination));
  EXPECT_EQ(fs::file_size(t.destination), 0);
  t.task = platf::new_file_lease();
  EXPECT_EQ(t.call("resume"), 409);
  t.destination = t.directory / "conflict";
  ASSERT_EQ(t.call("resume"), 200);
  ASSERT_EQ(t.call("chunk", 0, "abc"), 204);
  std::ofstream(t.destination) << "original";
  EXPECT_EQ(t.call("commit"), 409);
  std::string contents;
  std::ifstream(t.destination) >> contents;
  EXPECT_EQ(contents, "original");
  t.destination = t.directory / "large";
  ASSERT_EQ(t.call("resume", 0, {}, abc_sha, 1ULL << 36), 200);
}

TEST(StandaloneFileTransferTest, ReplacesOnlyTheSelectedVersionAtCommitAndResumesItsReceipt) {
  transfer_test_t t;
  ASSERT_TRUE(std::ofstream(t.destination) << "original");
  t.replace = files::replacement_version(t.destination);
  ASSERT_FALSE(t.replace.empty());
  ASSERT_EQ(t.call("resume"), 200);
  ASSERT_EQ(t.call("chunk", 0, "ab"), 204);
  EXPECT_EQ(fs::file_size(t.destination), 8);
  ++t.session;
  ASSERT_EQ(t.call("resume"), 200);
  ASSERT_EQ(t.call("chunk", 2, "c"), 204);
  const auto version = std::exchange(t.replace, "");
  EXPECT_EQ(t.call("commit"), 409);
  t.replace = version;
  ASSERT_TRUE(std::ofstream(t.destination) << "edited");
  EXPECT_EQ(t.call("commit"), 409);
  EXPECT_EQ(fs::file_size(t.destination), 6);
  EXPECT_EQ(t.call("resume"), 409);
  t.replace = files::replacement_version(t.destination);
  ASSERT_EQ(t.call("resume"), 200);
  ASSERT_EQ(t.call("chunk", 0, "abc"), 204);
  ASSERT_EQ(t.call("commit"), 204);
  ASSERT_EQ(t.call("resume"), 200);
  EXPECT_EQ(nlohmann::json::parse(t.output)["complete"], true);
  std::string content;
  std::ifstream(t.destination) >> content;
  EXPECT_EQ(content, "abc");
}

TEST(StandaloneFileTransferTest, PollsContentHashAndDetectsChangesWithIdenticalSizeAndTimestamp) {
  transfer_test_t t;
  constexpr auto size = 8 * files::chunk_size + 1;
  std::string contents(size, 'a');
  std::ofstream(t.destination, std::ios::binary).write(contents.data(), contents.size());
  const auto stamp = fs::last_write_time(t.destination);
  std::map<std::string, files::identity_job_t> jobs;
  auto identify = [&] {
    return files::source_identity(jobs, t.task, t.destination, size, {}, t.output, [] {
      return true;
    });
  };
  ASSERT_EQ(identify(), 202);
  EXPECT_EQ(nlohmann::json::parse(t.output)["offset"], "8388608");
  EXPECT_FALSE(std::ofstream(t.destination, std::ios::binary).is_open());
  ASSERT_EQ(identify(), 200);
  const auto first = nlohmann::json::parse(t.output)["sha256"];
  platf::file_digest_t expected;
  ASSERT_TRUE(expected.start());
  ASSERT_TRUE(expected.update(contents));
  EXPECT_EQ(first, expected.finish());
  contents.back() = 'b';
  std::ofstream(t.destination, std::ios::binary).write(contents.data(), contents.size());
  fs::last_write_time(t.destination, stamp);
  ASSERT_EQ(identify(), 202);
  ASSERT_EQ(identify(), 200);
  EXPECT_NE(first, nlohmann::json::parse(t.output)["sha256"]);
}

TEST(StandaloneFileTransferTest, KeepsOnlyTheReservedUploadWhenNewTransfersAreDenied) {
  transfer_test_t t;
  ASSERT_EQ(t.call("prepare-upload", 0, {}, {}), 204);
  t.can_start_transfer = false;
  const auto destination = t.destination;
  t.destination = t.directory / "another.bin";
  EXPECT_EQ(t.call("resume"), 409);
  t.destination = destination;
  EXPECT_EQ(t.call("resume", 0, {}, abc_sha, 2), 409);
  t.replace = "other-version";
  EXPECT_EQ(t.call("resume"), 409);
  t.replace.clear();
  ++t.session;
  EXPECT_EQ(t.call("prepare-upload", 0, {}, {}), 403);
  EXPECT_EQ(t.call("resume"), 403);
  EXPECT_EQ(t.call("discard", 0, {}, {}), 403);
  --t.session;
  ASSERT_EQ(t.call("prepare-upload", 0, {}, {}), 204);
  ASSERT_EQ(t.call("resume"), 200);
  EXPECT_EQ(t.call("resume", 0, {}, empty_sha), 409);
  ASSERT_EQ(t.call("chunk", 0, "ab"), 204);
  EXPECT_EQ(t.call("discard", 0, {}, {}), 204);
  ASSERT_EQ(t.call("chunk", 2, "c"), 204);
  ASSERT_EQ(t.call("commit"), 204);
  ASSERT_EQ(t.call("commit"), 204);
  std::string content;
  std::ifstream(t.destination) >> content;
  EXPECT_EQ(content, "abc");
  t.task = platf::new_file_lease();
  EXPECT_EQ(t.call("prepare-upload", 0, {}, {}), 403);
  EXPECT_EQ(t.call("resume"), 403);
  t.can_start_transfer = true;
  t.destination = t.directory / "cancelled.bin";
  ASSERT_EQ(t.call("prepare-upload", 0, {}, {}), 204);
  t.can_start_transfer = false;
  ASSERT_EQ(t.call("discard", 0, {}, {}), 204);
  EXPECT_EQ(t.call("resume"), 403);
  EXPECT_EQ(std::distance(fs::directory_iterator(t.directory), fs::directory_iterator()), 1);
}

TEST(StandaloneFileTransferTest, HashesAndReadsOnlyThePinnedFileWhenNewTransfersAreDenied) {
  transfer_test_t t;
  const std::string contents(8 * files::chunk_size + 1, 'b');
  ASSERT_TRUE(std::ofstream(t.destination, std::ios::binary).write(contents.data(), contents.size()));
  std::map<std::string, files::identity_job_t> jobs;
  const auto identify = [&](const std::string &id, bool can_start_transfer) {
    return files::source_identity(jobs, id, t.destination, contents.size(), {}, t.output, [] {
      return true;
    },
                                  true,
                                  can_start_transfer);
  };
  const auto read = [&](const std::string &id, const fs::path &path, std::uint64_t offset, std::optional<std::int64_t> stamp = {}) {
    return files::read_pinned_source(jobs, id, path, offset, contents.size(), stamp, t.output);
  };
  EXPECT_EQ(identify(t.task, false), 403);
  ASSERT_EQ(identify(t.task, true), 202);
  EXPECT_EQ(read(t.task, t.destination, 0), 409);
  ASSERT_EQ(identify(t.task, false), 200);
  const auto identity = t.output;
  ASSERT_EQ(identify(t.task, false), 200);
  EXPECT_EQ(t.output, identity);
  platf::file_digest_t hash;
  ASSERT_TRUE(hash.start());
  ASSERT_TRUE(hash.update(contents));
  EXPECT_EQ(nlohmann::json::parse(identity).at("sha256"), hash.finish());
  EXPECT_FALSE(std::ofstream(t.destination).is_open());
  EXPECT_EQ(read(platf::new_file_lease(), t.destination, 0), 404);
  EXPECT_EQ(read(t.task, t.directory / "other.bin", 0), 409);
  EXPECT_EQ(read(t.task, t.destination, 0, 1), 409);
  EXPECT_EQ(read(t.task, t.destination, contents.size() + 1), 400);
  ASSERT_EQ(read(t.task, t.destination, contents.size() - 1), 200);
  EXPECT_EQ(t.output, "b");
  ASSERT_EQ(read(t.task, t.destination, 0), 200);
  EXPECT_EQ(t.output, contents.substr(0, files::chunk_size));
  jobs.at(t.task).touched -= platf::file_resume_lifetime;
  EXPECT_EQ(read(t.task, t.destination, 0), 404);
  EXPECT_EQ(identify(t.task, false), 403);
  EXPECT_TRUE(std::ofstream(t.destination).is_open());
}

TEST(StandaloneFileTransferTest, StartsUploadsAndDownloadsInANewLockedFileSession) {
  transfer_test_t t;
  auto session = platf::make_file_browser_session();
  const auto op = [&](std::string_view action, const fs::path &path, const std::string &id, std::string_view sha = {}, std::string_view body = {}) {
    t.output.clear();
    return platf::exchange_files(*session, action, win_clipboard::detail::to_utf8(path.generic_wstring()), id, 0, 3, {}, body, t.output, [&] {
      return t.active;
    },
                                 t.cert,
                                 sha,
                                 nullptr,
                                 true);
  };
  ASSERT_EQ(op("capabilities", {}, {}), 200);
  ASSERT_TRUE(nlohmann::json::parse(t.output).at("backgroundTransfers").get<bool>());
  const auto status = op("list", t.directory, {});
  if (status == 403) {
    GTEST_SKIP() << "A signed-in Windows shell is required to test file access";
  }
  ASSERT_EQ(status, 200);
  session = platf::make_file_browser_session();
  platf::test_support::lock_file_session(*session);
  ASSERT_EQ(op("places", {}, {}), 200);
  ASSERT_EQ(op("list", t.directory, {}), 200);
  ASSERT_EQ(op("mkdir", t.directory / "folder", {}), 204);
  ASSERT_EQ(op("folder-path", t.directory / "folder", {}), 200);
  const auto source = t.directory / "source";
  ASSERT_TRUE(std::ofstream(source) << "abc");
  const auto read_id = platf::new_file_lease();
  ASSERT_EQ(op("open-read", source, read_id), 200);
  ASSERT_EQ(op("prepare-upload", t.destination, t.task), 204);
  for (auto action : {"drag-source", "drop-target", "delete", "rmdir"}) {
    EXPECT_EQ(op(action, source, read_id), 403) << action;
  }
  const auto other_id = platf::new_file_lease();
  EXPECT_EQ(op("read-pinned", source, other_id), 404);
  EXPECT_EQ(op("read-pinned", t.destination, read_id), 409);
  t.active = false;
  EXPECT_EQ(op("read-pinned", source, read_id), 403);
  EXPECT_EQ(op("resume", t.destination, t.task, abc_sha), 403);
  t.active = true;
  ASSERT_EQ(op("read-pinned", source, read_id), 200);
  EXPECT_EQ(t.output, "abc");
  ASSERT_EQ(op("resume", t.destination, t.task, abc_sha), 200);
  const auto lease = nlohmann::json::parse(t.output).at("id").get<std::string>();
  EXPECT_EQ(op("chunk", t.directory / "other", lease, {}, "abc"), 404);
  ASSERT_EQ(op("chunk", t.destination, lease, {}, "abc"), 204);
  ASSERT_EQ(op("commit", t.destination, lease), 204);
  ASSERT_EQ(op("abort", {}, read_id), 204);
  EXPECT_EQ(op("read-pinned", source, read_id), 404);
  EXPECT_TRUE(std::ofstream(source).is_open());
  std::string content;
  std::ifstream(t.destination) >> content;
  EXPECT_EQ(content, "abc");
}

TEST(StandaloneFileTransferTest, ReadsPastFourGiBWithoutTruncatingOffsets) {
  transfer_test_t t;
  constexpr auto offset = (1ULL << 32) + 17;
  {
    const auto raw = CreateFileW(t.destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(raw, INVALID_HANDLE_VALUE);
    files::file_handle file(raw);
    DWORD count {};
    if (!DeviceIoControl(raw, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &count, nullptr)) {
      GTEST_SKIP() << "The test volume does not support sparse files.";
    }
    LARGE_INTEGER position;
    position.QuadPart = offset;
    ASSERT_TRUE(SetFilePointerEx(raw, position, nullptr, FILE_BEGIN));
    ASSERT_TRUE(WriteFile(raw, "end", 3, &count, nullptr));
    ASSERT_EQ(count, 3);
  }
  ASSERT_EQ(files::read_file(t.destination, offset, offset + 3, {}, t.output), 200);
  EXPECT_EQ(t.output, "end");
  EXPECT_EQ(files::read_file(t.destination, offset + 4, offset + 3, {}, t.output), 400);
  EXPECT_EQ(files::read_file(t.destination, offset, offset + 2, {}, t.output), 409);
}
#endif
