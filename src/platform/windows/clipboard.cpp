/**
 * @file src/platform/windows/clipboard.cpp
 * @brief Access the interactive clipboard and file system with the desktop user's privileges.
 */
#include "clipboard.h"

#include "clipboard_download.h"
#include "clipboard_resume.h"
#include "clipboard_upload.h"
#include "clipboard_win32.h"
#include "interactive_desktop.h"

#include <atomic>
#include <mutex>

namespace platf {

  /** @brief Pending and most recently committed batches belong only to this stream. */
  struct clipboard_upload_session_t: std::enable_shared_from_this<clipboard_upload_session_t> {
    clipboard_download_t download;  ///< Read-only clipboard snapshot for this stream.
    std::string cert;  ///< Verified TLS identity that negotiated this stream.
    std::uint64_t serial {};  ///< Issuance order of streams competing for the same upload.
    bool resumable {};  ///< Current batch uses the reconnect cache.
    std::mutex mutex;  ///< Serializes begin, append, commit and cancellation.
    std::unique_ptr<clipboard_upload::batch_t> pending;  ///< Partial files removed when the stream is destroyed.
    LUID logon {};  ///< Interactive logon that owns the pinned files.
    std::string last_id;  ///< Support retries when the commit acknowledgement was lost.
    DWORD last_revision {};  ///< Revision that still identifies the last published batch.
  };

  static clipboard_upload::resume_cache_t &resume_cache() {
    static clipboard_upload::resume_cache_t cache;
    return cache;
  }

  static std::mutex download_sessions_mutex;
  static std::vector<std::weak_ptr<clipboard_upload_session_t>> download_sessions;

  void expire_clipboard_uploads() {
    const auto now = std::chrono::steady_clock::now();
    resume_cache().expire(now);
    std::lock_guard sessions_lock(download_sessions_mutex);
    std::erase_if(download_sessions, [&](const auto &weak) {
      const auto session = weak.lock();
      if (!session) {
        return true;
      }
      auto &snapshot = session->download;
      std::unique_lock snapshot_lock(snapshot.mutex, std::try_to_lock);
      if (snapshot_lock && !snapshot.files.empty() && now - snapshot.last_read > std::chrono::minutes(2)) {
        snapshot.files.clear();
      }
      return false;
    });
  }

  std::shared_ptr<clipboard_upload_session_t> make_clipboard_upload_session(std::string cert) {
    static std::atomic_uint64_t serial {};
    auto session = std::make_shared<clipboard_upload_session_t>();
    session->cert = std::move(cert);
    session->serial = ++serial;
    std::lock_guard lock(download_sessions_mutex);
    download_sessions.emplace_back(session);
    return session;
  }

  /** @brief Resolve clipboard staging under the impersonated shell owner's local profile. */
  int clipboard_staging_root(HANDLE token, const wchar_t *folder, std::filesystem::path &path) {
    PWSTR app_data {};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, token, &app_data))) {
      return 500;
    }
    auto release = std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)>(app_data, &CoTaskMemFree);
    path = std::filesystem::path(app_data) / L"Sunshine" / folder;
    return 200;
  }

  clipboard_upload_result_t exchange_clipboard_batch(clipboard_upload_session_t &session, std::string_view op, std::string_view id, std::uint32_t file, std::uint64_t offset, std::optional<std::uint32_t> since, std::string_view input, const std::function<bool()> &grant_active, std::string_view task, std::string_view previous) {
    desktop::shell_impersonation_t user;
    if (!user.begin()) {
      return {409, {}, {}, {}, "shell"};
    }
    if (!desktop::on_user_desktop()) {
      return {409, {}, {}, {}, "desktop"};
    }
    TOKEN_STATISTICS stats {};
    DWORD needed {};
    if (!GetTokenInformation(user.token, TokenStatistics, &stats, sizeof(stats), &needed)) {
      return {403};
    }
    if (!task.empty()) {
      const auto permitted = [&] {
        return grant_active() && desktop::on_user_desktop();
      };
      if (op == "target") {
        if (!input.empty() || !id.empty() || since) {
          return {400};
        }
        const auto capture = [&](auto &target) {
          return clipboard_upload::capture_paste_target(user.token, target);
        };
        return resume_cache().capture_target(session.cert, stats.AuthenticationId, session.serial, task, clipboard_upload::resume_cache_t::clock::now(), permitted, capture, previous, session.weak_from_this());
      }
      return resume_cache().exchange({}, session.cert, stats.AuthenticationId, session.serial, op, id, file, offset, since, input, permitted, task);
    }
    std::lock_guard lock(session.mutex);
    if (!grant_active()) {
      return {403};
    }
    if (op == "resume" || op == "discard" || (session.resumable && op != "begin")) {
      std::filesystem::path root;
      if (op == "resume" && clipboard_staging_root(user.token, L"ClipboardUploads", root) != 200) {
        return {500};
      }
      auto result = resume_cache().exchange(root, session.cert, stats.AuthenticationId, session.serial, op, id, file, offset, since, input, grant_active);
      if (op == "resume" && result.status == 200) {
        session.resumable = true;
        session.pending.reset();
      }
      return result;
    }
    if (op == "begin") {
      if (!id.empty()) {
        return {400};
      }
      std::filesystem::path root;
      if (clipboard_staging_root(user.token, L"ClipboardUploads", root) != 200) {
        return {500};
      }
      session.pending.reset();
      auto batch = std::make_unique<clipboard_upload::batch_t>();
      int status = batch->begin(root, input);
      if (status != 200) {
        return {status};
      }
      session.logon = stats.AuthenticationId;
      session.last_id.clear();
      session.pending = std::move(batch);
      session.resumable = false;
      return {200, session.pending->id};
    }

    if (session.logon.HighPart != stats.AuthenticationId.HighPart || session.logon.LowPart != stats.AuthenticationId.LowPart) {
      return {403};
    }
    if (op == "commit" && !id.empty() && id == session.last_id && input.empty()) {
      auto revision = GetClipboardSequenceNumber();
      if (revision != session.last_revision) {
        return {409, {}, {}, revision, "sequence"};
      }
      return {204, {}, {}, revision};
    }
    if (!session.pending || session.pending->id != id) {
      return {404};
    }
    auto &batch = *session.pending;
    if (op == "abort" && input.empty()) {
      session.pending.reset();
      return {204};
    }
    if (op == "chunk") {
      int status = batch.append(file, offset, input);
      if (status != 204) {
        if (status == 409) {
          return {409, {}, batch.entries[file].received, {}, "offset"};
        }
        if (status >= 500) {
          session.pending.reset();
        }
        return {status};
      }
      return {204, {}, batch.entries[file].received};
    }
    if (op != "commit" || !input.empty()) {
      return {400};
    }
    DWORD revision = GetClipboardSequenceNumber();
    int status = batch.commit(since, revision, grant_active);
    if (status == 204) {
      session.last_id = batch.id;
      session.last_revision = revision;
      session.pending.reset();
      return {204, {}, {}, revision};
    }
    if (status == 423) {
      return {409, {}, {}, {}, "busy"};
    }
    if (status == 409) {
      return {409, {}, {}, revision, "sequence"};
    }
    return {status};
  }

  std::uint32_t clipboard_sequence() {
    return GetClipboardSequenceNumber();
  }

  int exchange_clipboard(clipboard::formats_t formats, bool upload, std::optional<std::uint32_t> since, std::string_view input, std::string &output, std::uint32_t &sequence, const std::function<bool()> &grant_active, const clipboard_download_request_t &download) {
    // Report the stream-start revision even when the user copied something before the first poll.
    if (!upload && !since) {
      output.clear();
      sequence = formats.initial_sequence;
      return 204;
    }
    desktop::shell_impersonation_t user;
    if (!user.begin() || !desktop::on_user_desktop()) {
      return 409;
    }
    DWORD current = GetClipboardSequenceNumber();
    sequence = current;
    if (!download.op.empty()) {
      if (upload || !since || !formats.files || !formats.upload_session) {
        return 403;
      }
      TOKEN_STATISTICS stats {};
      DWORD needed {};
      if (!GetTokenInformation(user.token, TokenStatistics, &stats, sizeof(stats), &needed)) {
        return 403;
      }
      auto &snapshot = formats.upload_session->download;
      std::lock_guard lock(snapshot.mutex);
      if (download.op != "1") {
        return snapshot.read(download, *since, GetClipboardSequenceNumber(), stats.AuthenticationId, output);
      }
      auto status = snapshot.begin(*since, formats.initial_sequence, current, stats.AuthenticationId, output);
      sequence = current;
      if (status) {
        return *status;
      }
    }
    if (!upload && !formats.should_read(current, since)) {
      return 204;
    }

    int status {};
    if (upload) {
      std::filesystem::path staging;
      if (formats.files && !input.empty() && input.front() == 'F') {
        if (clipboard_staging_root(user.token, L"Clipboard", staging) != 200) {
          return 500;
        }
      }
      status = win_clipboard::write(formats.text, formats.files, input, current, staging, since, grant_active, formats.images);
    } else {
      const auto inspected = current;
      status = win_clipboard::read(formats.text, formats.files && download.op.empty(), *since, output, current, formats.images);
      if (!download.op.empty() && inspected != current) {
        output.clear();
        status = 409;
      }
      if (status == 200 && current == formats.initial_sequence) {
        output.clear();
        status = 204;
      }
    }
    sequence = current;
    return status;
  }
}  // namespace platf
