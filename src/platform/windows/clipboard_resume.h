/**
 * @file src/platform/windows/clipboard_resume.h
 * @brief Keep acknowledged clipboard files available to a reconnecting paired client.
 */
#pragma once

#include "clipboard.h"
#include "clipboard_paste_target.h"
#include "clipboard_upload.h"

#include <mutex>

namespace platf::clipboard_upload {
  /** @brief Per-process cache of pinned files. Stream tickets are ordered at negotiation, before any file requests. */
  class resume_cache_t {
  public:
    using clock = std::chrono::steady_clock;  ///< Monotonic clock used for idle expiry.
    static constexpr auto lifetime = file_resume_lifetime;  ///< Maximum idle retention time.

    /** @brief Remove expired files even if the client never returns. */
    void expire(clock::time_point now) {
      std::lock_guard lock(mutex);
      expire_locked(now);
    }

    /** @brief Bind a paste gesture once to its foreground target before accepting content or a filename. */
    clipboard_upload_result_t capture_target(std::string_view cert, LUID logon, std::uint64_t stream, std::string_view task, clock::time_point now, const std::function<bool()> &active, const std::function<int(paste_target_t &)> &capture, std::string_view previous = {}) {
      if (cert.empty() || !stream || !active()) {
        return {403};
      }
      if (!valid_task(task) || (!previous.empty() && (!valid_task(previous) || previous == task))) {
        return {400};
      }
      auto target = std::make_shared<paste_target_t>();
      bool probe_at_capacity = false;
      {
        std::lock_guard lock(mutex);
        expire_locked(now);
        auto found = find_record(cert, logon, task);
        if (found != records.end()) {
          if (stream < found->stream) {
            return {403};
          }
          if (found->capturing) {
            return {409, {}, {}, {}, "busy"};
          }
          if (found->stream != stream) {
            found->lease.clear();
            found->stream = stream;
          }
          found->touched = now;
          return {204, {}, {}, {}, {}, found->task};
        }
        auto prior = previous.empty() ? records.end() : find_record(cert, logon, previous);
        if (!make_room(true, prior == records.end() ? nullptr : &*prior)) {
          // At capacity, a reconnect still needs one short probe to compare its folder.
          if (prior == records.end() || prior->stream >= stream || pending_count() != max_records) {
            return {429};
          }
          probe_at_capacity = true;
        }
        auto &record = records.emplace_back();
        record.cert = cert;
        record.logon = logon;
        record.stream = stream;
        record.task = task;
        record.touched = now;
        record.target = target;
        record.capturing = true;
      }
      // Explorer may need to answer a COM call. Keep unrelated chunks and cancellations running.
      int status = 500;
      try {
        status = capture(*target);
      } catch (const std::exception &) {
        // Release the reserved slot even if the shell or a filesystem operation failed.
      }
      const auto identity = status == 200 ? folder_identity(*target) : std::nullopt;
      std::lock_guard lock(mutex);
      auto found = find_record(cert, logon, task);
      if (found == records.end() || found->target != target) {
        return {404};
      }
      if (!active()) {
        status = 403;
      }
      if (status != 200) {
        records.erase(found);
        return {status == 204 ? 422 : status};
      }
      if (!previous.empty()) {
        auto prior = find_record(cert, logon, previous);
        if (prior != records.end() && !prior->capturing && prior->stream < stream && identity && prior->folder == identity && CompareStringOrdinal(target->directory.c_str(), -1, prior->folder_path.c_str(), -1, TRUE) == CSTR_EQUAL) {
          prior->gesture = task;
          prior->stream = stream;
          prior->lease.clear();
          prior->touched = now;
          clipboard_upload_result_t chosen {204, {}, {}, {}, {}, prior->task};
          records.erase(found);
          return chosen;
        }
      }
      if (probe_at_capacity && pending_count() > max_records) {
        records.erase(found);
        return {429};
      }
      found->folder = identity;
      found->folder_path = target->directory;
      found->capturing = false;
      return {204, {}, {}, {}, {}, found->task};
    }

    /** @brief Apply one operation after the caller validates this stream and its interactive desktop. */
    clipboard_upload_result_t exchange(const fs::path &root, std::string_view cert, LUID logon, std::uint64_t stream, std::string_view op, std::string_view id, std::uint32_t file, std::uint64_t offset, std::optional<std::uint32_t> since, std::string_view input, const std::function<bool()> &active, std::string_view task = {}) {
      if (cert.empty() || !stream) {
        return {403};
      }
      std::lock_guard lock(mutex);
      if (!active()) {
        return {403};
      }
      const auto now = clock::now();
      expire_locked(now);
      auto found = find_record(cert, logon, task);
      if (!task.empty()) {
        if (!valid_task(task) || since || op == "commit") {
          return {400};
        }
        if (op == "discard" && input.empty() && id.empty()) {
          if (found != records.end()) {
            if (stream < found->stream) {
              return {403};
            }
            records.erase(found);
          }
          return {204};
        }
        if (op == "discard") {
          return {400};
        }
        if (found == records.end()) {
          return {404};
        }
        if (found->capturing) {
          return {409, {}, {}, {}, "busy"};
        }
      }
      if (op == "resume" || op == "discard") {
        return id.empty() ? bind(root, cert, logon, stream, op == "discard", input, found, now) : clipboard_upload_result_t {400};
      }
      if (found == records.end() || !found->batch || found->lease != id || found->stream != stream || id.empty()) {
        return {404};
      }
      auto &batch = *found->batch;
      if (op == "abort" && input.empty()) {
        records.erase(found);
        return {204};
      }
      if (op == "chunk" && !batch.published) {
        const auto status = batch.append(file, offset, input);
        if (status == 204) {
          found->touched = now;
          return {204, {}, batch.entries[file].received};
        }
        if (status == 409) {
          return {409, {}, batch.entries[file].received, {}, "offset"};
        }
        if (status == 422 || status >= 500) {
          // The acknowledged prefix is no longer trustworthy after a failed write or hash.
          records.erase(found);
        }
        return {status};
      }
      if (op == "deliver" && !task.empty() && input.empty()) {
        if (!batch.published) {
          const auto status = batch.deliver(found->target->directory, active);
          if (status != 204) {
            return {status};
          }
          found->reserved = 0;
          found->target.reset();
        }
        found->touched = now;
        return {204};
      }
      if (op != "commit" || !since || !input.empty()) {
        return {400};
      }
      DWORD revision = GetClipboardSequenceNumber();
      if (batch.published) {
        if (revision != found->revision) {
          return {409, {}, {}, revision, "sequence"};
        }
        return {204, {}, {}, revision};
      }
      const auto status = batch.commit(since, revision, active);
      if (status == 204) {
        found->touched = now;
        found->revision = revision;
        found->reserved = 0;
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

  private:
    /** @brief Verified identity, lease and pinned file state of one retained batch. */
    struct record_t {
      std::string cert;  ///< Authenticated client certificate.
      LUID logon {};  ///< Local Windows authentication session.
      std::unique_ptr<batch_t> batch;  ///< Manifest and verified file contents.
      std::uint64_t reserved {};  ///< Bytes promised to this unfinished batch.
      std::uint64_t stream {};  ///< Most recent stream that acquired the lease.
      std::string lease;  ///< Random identifier rotated by every resume.
      clock::time_point touched;  ///< Last successful contact for expiry.
      DWORD revision {};  ///< Clipboard revision of a committed receipt.
      std::string task;  ///< Client gesture ID; empty for legacy clipboard publication.
      std::string gesture;  ///< Most recent reconnect gesture that selected this task.
      fs::path folder_path;  ///< Original local folder location retained in the delivery receipt.
      std::optional<std::array<DWORD, 3>> folder;  ///< Original folder volume serial and file index.
      std::shared_ptr<paste_target_t> target;  ///< Pins the destination until direct delivery or cancellation.
      bool capturing {};  ///< Explorer is resolving the gesture outside the cache lock.
    };

    static constexpr size_t max_records = 8;
    static constexpr size_t max_receipts = 64;
    std::mutex mutex;
    std::vector<record_t> records;

    static bool valid_task(std::string_view task) {
      return task.size() == 32 && task.find_first_not_of("0123456789abcdef") == std::string_view::npos;
    }

    /** @brief A gesture is private to the paired client and interactive Windows logon. */
    std::vector<record_t>::iterator find_record(std::string_view cert, LUID logon, std::string_view task) {
      return std::find_if(records.begin(), records.end(), [&](const auto &record) {
        return record.cert == cert && record.logon.HighPart == logon.HighPart && record.logon.LowPart == logon.LowPart &&
               (record.task == task || (!task.empty() && record.gesture == task));
      });
    }

    /** @brief Identify the pinned directory object, including a drive root with no ancestor handles. */
    static std::optional<std::array<DWORD, 3>> folder_identity(const paste_target_t &target) {
      file_handle root;
      if (target.pins.empty()) {
        const auto raw = CreateFileW(target.directory.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (raw == INVALID_HANDLE_VALUE) {
          return {};
        }
        root.reset(raw);
      }
      BY_HANDLE_FILE_INFORMATION info {};
      if (!GetFileInformationByHandle(root ? root.get() : target.pins.back().get(), &info) || (!info.nFileIndexHigh && !info.nFileIndexLow)) {
        return {};
      }
      return std::array {info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow};
    }

    /** @brief Bound pinned uploads separately from lightweight delivery receipts. */
    bool make_room(bool creating = true, const record_t *preserve = nullptr) {
      const auto pending = pending_count();
      if (pending >= max_records) {
        return false;
      }
      if (!creating || records.size() - pending < max_receipts) {
        return true;
      }
      auto receipt = records.end();
      for (auto it = records.begin(); it != records.end(); ++it) {
        if (&*it != preserve && it->batch && it->batch->published && (receipt == records.end() || it->touched < receipt->touched)) {
          receipt = it;
        }
      }
      records.erase(receipt);
      return true;
    }

    /** @brief Count tasks that still hold a target or staging handles. */
    size_t pending_count() const {
      return std::count_if(records.begin(), records.end(), [](const auto &record) {
        return !record.batch || !record.batch->published;
      });
    }

    /** @brief Bind a new stream lease to a verified manifest or discard its unfinished files. */
    clipboard_upload_result_t bind(const fs::path &root, std::string_view cert, LUID logon, std::uint64_t stream, bool discard, std::string_view input, std::vector<record_t>::iterator found, clock::time_point now) {
      auto batch = std::make_unique<batch_t>();
      std::uint64_t total {};
      const auto status = manifest(input, batch->entries, total, true);
      if (status != 200) {
        return {status};
      }
      if (found != records.end() && stream < found->stream) {
        // A request from the retired connection cannot reclaim or discard its successor's batch.
        return {403};
      }
      const bool direct = found != records.end() && !found->task.empty();
      const bool matches = found != records.end() && found->batch && std::ranges::equal(batch->entries, found->batch->entries, [](const auto &a, const auto &b) {
                             return a.name == b.name && a.size == b.size && a.sha256 == b.sha256;
                           });
      if (discard) {
        if (matches) {
          records.erase(found);
        }
        return {204};
      }
      auto lease = new_file_lease();
      if (lease.empty()) {
        return {500};
      }
      if (direct && found->batch && !matches) {
        return {409, {}, {}, {}, "manifest"};
      }
      if (!matches || (!direct && found->batch->published && GetClipboardSequenceNumber() != found->revision)) {
        std::uint64_t reservation = total;
        for (auto it = records.begin(); it != records.end(); ++it) {
          if (it != found) {
            reservation += it->reserved;
          }
        }
        if (reservation > batch_limit) {
          return {507};
        }
        const bool create = found == records.end();
        if ((create || (found->batch && found->batch->published)) && !make_room(create)) {
          return {507};
        }
        if (!direct) {
          remove_abandoned_directories(root);
        }
        const auto created = direct ? batch->stage_for_delivery(found->target->directory, total) : batch->stage(root, total);
        if (created != 200) {
          return {created};
        }
        if (create) {
          records.emplace_back();
          found = std::prev(records.end());
        }
        found->cert = cert;
        found->logon = logon;
        found->batch = std::move(batch);
        found->reserved = total;
      }
      found->stream = stream;
      found->lease = std::move(lease);
      found->touched = now;
      nlohmann::json offsets = nlohmann::json::array();
      for (const auto &entry : found->batch->entries) {
        offsets.push_back(entry.received);
      }
      nlohmann::json response {{"id", found->lease}, {"offsets", std::move(offsets)}};
      if (direct) {
        response["committed"] = found->batch->published;
      }
      return {200, response.dump()};
    }

    /** @brief Drop file handles whose last successful transfer predates the retention window. */
    void expire_locked(clock::time_point now) {
      std::erase_if(records, [&](const auto &record) {
        return now - record.touched >= lifetime;
      });
    }

    /** @brief Remove empty GUID directories left after Windows closed the interrupted process's delete-pending files. */
    static void remove_abandoned_directories(const fs::path &root) {
      if (!win_clipboard::detail::local_file_path(root)) {
        return;
      }
      const auto cutoff = fs::file_time_type::clock::now() - lifetime;
      std::error_code ec;
      for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const auto &path = it->path();
        if (!win_clipboard::detail::batch_name(path.filename().native())) {
          continue;
        }
        const auto attrs = GetFileAttributesW(path.c_str());
        std::error_code err;
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT) && fs::last_write_time(path, err) < cutoff && !err) {
          RemoveDirectoryW(path.c_str());
        }
      }
    }
  };
}  // namespace platf::clipboard_upload
