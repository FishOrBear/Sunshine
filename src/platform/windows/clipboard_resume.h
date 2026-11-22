/**
 * @file src/platform/windows/clipboard_resume.h
 * @brief Keep acknowledged clipboard files available to a reconnecting paired client.
 */
#pragma once

#include "clipboard.h"
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

    /** @brief Apply one operation after the caller validates this stream and its interactive desktop. */
    clipboard_upload_result_t exchange(const fs::path &root, std::string_view cert, LUID logon, std::uint64_t stream, std::string_view op, std::string_view id, std::uint32_t file, std::uint64_t offset, std::optional<std::uint32_t> since, std::string_view input, const std::function<bool()> &active) {
      if (cert.empty() || !stream) {
        return {403};
      }
      std::lock_guard lock(mutex);
      if (!active()) {
        return {403};
      }
      const auto now = clock::now();
      expire_locked(now);
      auto found = std::find_if(records.begin(), records.end(), [&](const auto &r) {
        return r.cert == cert && r.logon.HighPart == logon.HighPart && r.logon.LowPart == logon.LowPart;
      });
      if (op == "resume" || op == "discard") {
        return id.empty() ? bind(root, cert, logon, stream, op == "discard", input, found, now) : clipboard_upload_result_t {400};
      }
      if (found == records.end() || found->lease != id || found->stream != stream || id.empty()) {
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
    };

    static constexpr size_t max_records = 8;
    std::mutex mutex;
    std::vector<record_t> records;

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
      const bool matches = found != records.end() && std::ranges::equal(batch->entries, found->batch->entries, [](const auto &a, const auto &b) {
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
      if (!matches || (found->batch->published && GetClipboardSequenceNumber() != found->revision)) {
        std::uint64_t reservation = total;
        for (auto it = records.begin(); it != records.end(); ++it) {
          if (it != found) {
            reservation += it->reserved;
          }
        }
        if (reservation > batch_limit) {
          return {507};
        }
        if (found == records.end() && records.size() >= max_records) {
          // A delivered receipt may be dropped before its TTL to make room for an unfinished paste.
          auto receipt = std::find_if(records.begin(), records.end(), [](const auto &r) {
            return r.batch->published;
          });
          if (receipt == records.end()) {
            return {507};
          }
          records.erase(receipt);
          found = records.end();
        }
        remove_abandoned_directories(root);
        const auto created = batch->stage(root, total);
        if (created != 200) {
          return {created};
        }
        if (found == records.end()) {
          records.emplace_back();
          found = std::prev(records.end());
        }
        *found = {std::string(cert), logon, std::move(batch), total};
      }
      found->stream = stream;
      found->lease = std::move(lease);
      found->touched = now;
      nlohmann::json offsets = nlohmann::json::array();
      for (const auto &entry : found->batch->entries) {
        offsets.push_back(entry.received);
      }
      return {200, nlohmann::json {{"id", found->lease}, {"offsets", std::move(offsets)}}.dump()};
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
