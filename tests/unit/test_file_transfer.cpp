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
      const bool bind = op == "resume" || op == "discard";
      std::vector<files::file_handle> pins;
      const int status = store.exchange(op, cert, logon, session, destination, bind ? task : lease, sha, offset, size, bytes, pins, output, ack, [&] {
        return active;
      });
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
