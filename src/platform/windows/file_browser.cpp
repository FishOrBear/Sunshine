/**
 * @file src/platform/windows/file_browser.cpp
 * @brief List and copy files on local Windows volumes for an authorized streaming client.
 */
#include "clipboard.h"
#include "clipboard_win32.h"
#include "file_transfer.h"
#include "interactive_desktop.h"
#include "src/utility.h"

#include <atomic>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>

namespace platf {
  namespace fs = std::filesystem;

  namespace {
    using namespace win_clipboard::detail;
    using namespace file_transfer;

    /** @brief Encode canonical Windows paths with forward separators for both peers. */
    std::string wire_path(const fs::path &path) {
      return to_utf8(path.generic_wstring());
    }

    /** @brief Verify that the current desktop user can actually list a local directory. */
    bool can_list_place(const fs::path &dir) {
      std::vector<file_handle> pins;
      if (pin_directory(dir, pins) != 200) {
        return false;
      }
      const auto handle = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (handle == INVALID_HANDLE_VALUE) {
        return false;
      }
      file_handle directory(handle);
      return true;
    }

    /** @brief Append local drives as browser entries or as labeled sidebar locations. */
    int append_drives(nlohmann::json &entries, bool places) {
      const auto drives = GetLogicalDrives();
      if (!drives) {
        return file_error();
      }
      for (unsigned i = 0; i < 26; ++i) {
        if (!(drives & (1U << i))) {
          continue;
        }
        std::wstring root = L"A:\\";
        root[0] += i;
        if (!local_volume(root)) {
          continue;
        }
        auto path = wire_path(root);
        if (places) {
          wchar_t volume[MAX_PATH + 1] {};
          if (!can_list_place(root) || !GetVolumeInformationW(root.c_str(), volume, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0)) {
            continue;
          }
          auto label = to_utf8(volume);
          auto name = label.empty() ? path.substr(0, 2) : label + " (" + path.substr(0, 2) + ")";
          entries.push_back({{"kind", "drive"}, {"path", path}, {"name", name}});
          continue;
        }
        entries.push_back({{"name", path}, {"path", path}, {"directory", true}, {"size", "0"}, {"modified", "0"}});
      }
      return 200;
    }

    /** @brief List accessible native user folders and volumes on the interactive desktop. */
    int list_places(HANDLE user_token, std::string &output) {
      // Removable slots without media must not open an insert-disk prompt on the host.
      const auto mode = GetThreadErrorMode();
      SetThreadErrorMode(mode | SEM_FAILCRITICALERRORS, nullptr);
      auto restore_mode = util::fail_guard([mode] {
        SetThreadErrorMode(mode, nullptr);
      });
      auto entries = nlohmann::json::array();
      const auto result = append_drives(entries, true);
      if (result != 200) {
        return result;
      }
      const std::pair<const char *, const KNOWNFOLDERID *> folders[] = {
        {"documents", &FOLDERID_Documents},
        {"desktop", &FOLDERID_Desktop},
        {"pictures", &FOLDERID_Pictures},
        {"videos", &FOLDERID_Videos},
        {"music", &FOLDERID_Music},
        {"downloads", &FOLDERID_Downloads}
      };
      for (const auto &[kind, id] : folders) {
        PWSTR path {};
        const auto hr = SHGetKnownFolderPath(*id, KF_FLAG_DONT_VERIFY, user_token, &path);
        std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> release(path, &CoTaskMemFree);
        if (SUCCEEDED(hr) && path && can_list_place(path)) {
          entries.push_back({{"kind", kind}, {"path", wire_path(path)}, {"name", ""}});
        }
      }
      output = nlohmann::json({{"entries", std::move(entries)}}).dump();
      return 200;
    }

    /** @brief List directory entries using metadata returned by a single enumeration. */
    int list_files(const fs::path &dir, std::string &output) {
      auto entries = nlohmann::json::array();
      if (dir.empty()) {
        const auto result = append_drives(entries, false);
        if (result != 200) {
          return result;
        }
      } else {
        std::vector<file_handle> pins;
        int status = pin_directory(dir, pins);
        if (status != 200) {
          return status;
        }
        WIN32_FIND_DATAW data {};
        auto find = FindFirstFileW((dir / L"*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) {
          if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            return file_error();
          }
        } else {
          std::unique_ptr<void, decltype(&FindClose)> close_find(find, &FindClose);
          std::size_t total = 0;
          do {
            if (!valid_name(data.cFileName) || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
              continue;
            }
            auto name = to_utf8(data.cFileName);
            if (name.empty()) {
              continue;
            }
            const bool directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const std::uint64_t size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            auto path = wire_path(dir / data.cFileName);
            // Bound enumeration memory, including JSON escaping and per-object overhead.
            total += 256 + 2 * (path.size() + name.size());
            if (total > 12 * 1024 * 1024) {
              return 413;
            }
            entries.push_back({{"name", name}, {"path", path}, {"directory", directory}, {"size", std::to_string(size)}, {"modified", std::to_string(milliseconds(data.ftLastWriteTime))}});
          } while (FindNextFileW(find, &data));
          if (GetLastError() != ERROR_NO_MORE_FILES) {
            return file_error();
          }
        }
      }
      output = nlohmann::json({{"entries", std::move(entries)}}).dump();
      return 200;
    }

    /** @brief Track the destination and the validated sequential prefix of an upload. */
    struct upload_t {
      fs::path destination;  ///< Final name, created only on success.
      std::uint64_t size;  ///< Declared final length.
      std::uint64_t offset;  ///< Bytes accepted in order.
      file_handle file;  ///< Staging file marked for deletion on close.
    };
  }  // namespace

  /** @brief Keep incomplete uploads scoped to one paired stream. */
  struct file_browser_session_t {
    std::uint64_t serial {};  ///< Issue order of authenticated file grants.
    std::map<std::string, file_transfer::identity_job_t> identities;  ///< Incremental source hashes.
    std::mutex mutex;  ///< Serialize writes and cancellations.
    std::optional<LUID> logon;  ///< Desktop logon that owns this connection and its upload handles.
    std::map<std::string, upload_t> uploads;  ///< Pending uploads identified by client nonce.
  };

  namespace {
    std::mutex session_registry_mutex;
    std::vector<std::weak_ptr<file_browser_session_t>> file_sessions;
  }  // namespace

  std::shared_ptr<file_browser_session_t> make_file_browser_session() {
    static std::atomic<std::uint64_t> serial {};
    auto session = std::make_shared<file_browser_session_t>();
    session->serial = ++serial;
    std::lock_guard guard(session_registry_mutex);
    std::erase_if(file_sessions, [](const auto &weak) {
      return weak.expired();
    });
    file_sessions.emplace_back(session);
    return session;
  }

  /** @brief Append one chunk, then publish or discard the staging file when finished. */
  static int write_chunk(file_browser_session_t &session, const fs::path &path, const std::string &id, std::uint64_t offset, std::uint64_t size, std::string_view input, const std::function<bool()> &active) {
    if (input.size() > chunk_size || input.size() > size - offset || (input.empty() && size)) {
      return 400;
    }
    std::lock_guard lock(session.mutex);
    auto found = session.uploads.find(id);
    if (found == session.uploads.end()) {
      if (offset != 0) {
        return 409;
      }
      if (session.uploads.size() >= 32) {
        return 429;
      }
      file_handle file;
      const auto status = create_staging(path, file);
      if (status != 200) {
        return status;
      }
      found = session.uploads.emplace(id, upload_t {path, size, 0, std::move(file)}).first;
    }
    auto &upload = found->second;
    if (upload.size != size || upload.offset != offset || upload.destination != path) {
      return 409;
    }
    DWORD written {};
    int result = 204;
    if (!active()) {
      result = 403;
    } else if (!input.empty() && (!WriteFile(upload.file.get(), input.data(), static_cast<DWORD>(input.size()), &written, nullptr) || written != input.size())) {
      result = file_error();
    } else {
      upload.offset += written;
      if (upload.offset < size) {
        return 204;
      }
      result = active() ? publish_file(upload.file.get(), path) : 403;
    }
    session.uploads.erase(found);
    return result;
  }

  namespace {
    file_transfer::resume_store_t resumable_files;
  }

  void expire_file_transfers() {
    const auto now = file_transfer::resume_store_t::clock::now();
    resumable_files.expire(now);
    std::vector<std::shared_ptr<file_browser_session_t>> live;
    {
      std::lock_guard guard(session_registry_mutex);
      for (const auto &weak : file_sessions) {
        if (auto session = weak.lock()) {
          live.push_back(std::move(session));
        }
      }
    }
    for (const auto &session : live) {
      std::lock_guard guard(session->mutex);
      std::erase_if(session->identities, [&](const auto &job) {
        return now - job.second.touched >= file_resume_lifetime;
      });
    }
  }

  int exchange_files(file_browser_session_t &session, std::string_view operation, std::string_view name, const std::string &id, std::uint64_t offset, std::uint64_t size, std::optional<std::int64_t> modified, std::string_view input, std::string &output, const std::function<bool()> &active, std::string_view cert, std::string_view digest, std::optional<std::uint64_t> *ack) {
    if ((operation == "write" || operation == "abort" || operation == "resume" || operation == "chunk" || operation == "commit" || operation == "discard" || operation == "identity") && (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos)) {
      return 400;
    }
    if (size > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)()) || offset > size || name.size() > 4 * 32767) {
      return 400;
    }
    if (!active()) {
      return 403;
    }
    if (operation == "capabilities") {
      output = R"({"version":2})";
      return 200;
    }
    if (operation == "abort") {
      std::lock_guard lock(session.mutex);
      session.uploads.erase(id);
      session.identities.erase(id);
      return 204;
    }
    desktop::shell_impersonation_t user;
    if (!user.begin() || !desktop::on_user_desktop()) {
      return 403;
    }
    TOKEN_STATISTICS stats {};
    DWORD needed {};
    if (!GetTokenInformation(user.token, TokenStatistics, &stats, sizeof(stats), &needed)) {
      return 403;
    }
    {
      std::lock_guard lock(session.mutex);
      if (!session.logon) {
        session.logon = stats.AuthenticationId;
      }
      if (session.logon->HighPart != stats.AuthenticationId.HighPart || session.logon->LowPart != stats.AuthenticationId.LowPart) {
        return 403;
      }
    }
    if (operation == "places") {
      return list_places(user.token, output);
    }
    auto wide = from_utf8(name);
    if ((!name.empty() && wide.empty()) || wide.find(L'\0') != std::wstring::npos) {
      return 400;
    }
    std::replace(wide.begin(), wide.end(), L'/', L'\\');
    const fs::path path(wide);
    if (operation == "list") {
      return list_files(path, output);
    }
    if (!valid_name(path.filename().native())) {
      return 400;
    }
    std::vector<file_handle> pins;
    const int result = pin_directory(path.parent_path(), pins);
    if (result != 200) {
      return result;
    }
    if (operation == "resume" || operation == "chunk" || operation == "commit" || operation == "discard") {
      std::optional<std::uint64_t> confirmed;
      const int status = resumable_files.exchange(operation, cert, stats.AuthenticationId, session.serial, path, id, digest, offset, size, input, pins, output, confirmed, active);
      if (ack) {
        *ack = confirmed;
      }
      return status;
    }
    if (operation == "identity") {
      std::lock_guard lock(session.mutex);
      return file_transfer::source_identity(session.identities, id, path, size, modified, output, active);
    }
    if (operation == "read") {
      return read_file(path, offset, size, modified, output);
    }
    if (operation == "write") {
      return write_chunk(session, path, id, offset, size, input, active);
    }
    if (operation == "mkdir") {
      if (!active()) {
        return 403;
      }
      return CreateDirectoryW(path.c_str(), nullptr) ? 204 : file_error();
    }
    return 400;
  }
}  // namespace platf
