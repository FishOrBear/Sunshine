/**
 * @file src/platform/windows/file_transfer.h
 * @brief Native file staging, incremental source identification and reconnect leases.
 */
#pragma once

#include "clipboard_win32.h"
#include "file_digest.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>

namespace platf::file_transfer {
  namespace fs = std::filesystem;
  using win_clipboard::detail::file_handle;
  constexpr std::uint64_t chunk_size = 1024 * 1024;  ///< Maximum bytes per file chunk.

  /** @brief Return an HTTP error for a Win32 file operation. */
  inline int file_error(DWORD error = GetLastError()) {
    switch (error) {
      case ERROR_FILE_EXISTS:
      case ERROR_ALREADY_EXISTS:
      case ERROR_SHARING_VIOLATION:
      case ERROR_DIR_NOT_EMPTY:
        return 409;
      case ERROR_PATH_NOT_FOUND:
      case ERROR_FILE_NOT_FOUND:
        return 404;
      case ERROR_ACCESS_DENIED:
        return 403;
      case ERROR_DISK_FULL:
        return 507;
      default:
        return 500;
    }
  }

  /** @brief Restrict access to mounted local volumes. */
  inline bool local_volume(const fs::path &root) {
    const auto chars = root.native();
    if (chars.size() != 3 || chars[1] != L':' || chars[2] != L'\\' || !((chars[0] >= L'A' && chars[0] <= L'Z') || (chars[0] >= L'a' && chars[0] <= L'z'))) {
      return false;
    }
    const auto type = GetDriveTypeW(chars.c_str());
    return type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_RAMDISK;
  }

  /** @brief Hold each directory without delete sharing to prevent a concurrent junction swap. */
  inline int pin_directory(const fs::path &dir, std::vector<file_handle> &pins) {
    auto current = dir.root_path();
    if (!local_volume(current)) {
      return 400;
    }
    for (const auto &part : dir.relative_path()) {
      if (part.empty()) {
        continue;
      }
      if (!win_clipboard::detail::valid_name(part.native())) {
        return 400;
      }
      current /= part;
      HANDLE raw = CreateFileW(current.c_str(), FILE_TRAVERSE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (raw == INVALID_HANDLE_VALUE) {
        return file_error();
      }
      pins.emplace_back(raw);
      BY_HANDLE_FILE_INFORMATION info {};
      if (!GetFileInformationByHandle(raw, &info)) {
        return file_error();
      }
      if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return 403;
      }
    }
    return 200;
  }

  /** @brief Convert a Win32 UTC timestamp to milliseconds since the Unix epoch. */
  inline std::int64_t milliseconds(FILETIME value) {
    ULARGE_INTEGER stamp;
    stamp.LowPart = value.dwLowDateTime;
    stamp.HighPart = value.dwHighDateTime;
    return static_cast<std::int64_t>(stamp.QuadPart / 10000) - 11644473600000LL;
  }

  /** @brief Open a regular file with the requested access and verify the listing against the pinned handle. */
  inline int open_source(const fs::path &path, std::uint64_t size, std::optional<std::int64_t> modified, file_handle &file, DWORD access = GENERIC_READ, DWORD share = FILE_SHARE_READ) {
    HANDLE raw = CreateFileW(path.c_str(), access, share, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
      return file_error();
    }
    file.reset(raw);
    BY_HANDLE_FILE_INFORMATION info {};
    if (!GetFileInformationByHandle(raw, &info)) {
      return file_error();
    }
    if (GetFileType(raw) != FILE_TYPE_DISK || info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
      return 403;
    }
    if (((static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow) != size || (modified && *modified != milliseconds(info.ftLastWriteTime))) {
      return 409;
    }
    return 200;
  }

  /** @brief Read a single chunk while checking the listing's size and modification time. */
  inline int read_file(const fs::path &path, std::uint64_t offset, std::uint64_t size, std::optional<std::int64_t> modified, std::string &output) {
    if (offset > size || size > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)())) {
      return 400;
    }
    file_handle file;
    const int status = open_source(path, size, modified, file);
    if (status != 200) {
      return status;
    }
    LARGE_INTEGER position;
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file.get(), position, nullptr, FILE_BEGIN)) {
      return file_error();
    }
    std::string chunk(static_cast<size_t>((std::min) (chunk_size, size - offset)), '\0');
    DWORD bytes {};
    if (!chunk.empty() && !ReadFile(file.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &bytes, nullptr)) {
      return file_error();
    }
    if (bytes != chunk.size()) {
      return 409;
    }
    output = std::move(chunk);
    return 200;
  }

  /** @brief Set or clear delete-on-close on the staging handle. */
  inline bool disposition(HANDLE file, bool remove) {
    FILE_DISPOSITION_INFO info {};
    info.DeleteFile = remove;
    return SetFileInformationByHandle(file, FileDispositionInfo, &info, sizeof(info));
  }

  /** @brief Identify the current regular file for a conditional replacement. */
  inline std::string replacement_version(const fs::path &path);

  /** @brief Publish the staging handle, replacing only an explicitly selected file version. */
  inline int rename_staged_file(HANDLE file, const fs::path &path, std::string_view replace = {}) {
    const auto &name = path.native();
    std::vector<unsigned char> data(sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t));
    auto info = reinterpret_cast<FILE_RENAME_INFO *>(data.data());
    if (!replace.empty()) {
      if (replacement_version(path) != replace) {
        return 409;
      }
      info->ReplaceIfExists = TRUE;
    }
    info->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::memcpy(info->FileName, name.data(), info->FileNameLength);
    if (!disposition(file, false)) {
      return file_error();
    }
    if (SetFileInformationByHandle(file, FileRenameInfo, info, static_cast<DWORD>(data.size()))) {
      return 204;
    }
    const auto error = GetLastError();
    disposition(file, true);
    return file_error(error);
  }

  /** @brief Flush a standalone transfer before acknowledging its final rename. */
  inline int publish_file(HANDLE file, const fs::path &path, std::string_view replace = {}) {
    return FlushFileBuffers(file) ? rename_staged_file(file, path, replace) : file_error();
  }

  /** @brief Create a private, delete-pending staging file on the destination volume. */
  inline int create_staging_in(const fs::path &directory, file_handle &file, DWORD attributes) {
    GUID guid {};
    if (FAILED(CoCreateGuid(&guid))) {
      return 500;
    }
    wchar_t guid_text[40] {};
    if (!StringFromGUID2(guid, guid_text, 40)) {
      return 500;
    }
    const auto tmp = directory / (std::wstring(L".sunshine-") + guid_text + L".part");
    HANDLE raw = CreateFileW(tmp.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE, 0, nullptr, CREATE_NEW, attributes, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
      return file_error();
    }
    file.reset(raw);
    if (!disposition(raw, true)) {
      const auto error = GetLastError();
      file.reset();
      DeleteFileW(tmp.c_str());
      return file_error(error);
    }
    return 200;
  }

  /** @brief Stage beside a free name or a matching, authorized replacement target. */
  inline int create_staging(const fs::path &path, file_handle &file, std::string_view replace = {}) {
    if (!replace.empty()) {
      if (replacement_version(path) != replace) {
        return 409;
      }
    } else if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return 409;
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
      return file_error();
    }
    return create_staging_in(path.parent_path(), file, FILE_ATTRIBUTE_NORMAL);
  }

  /** @brief Pinned source, verified metadata and incremental digest across bounded requests. */
  struct identity_job_t {
    fs::path path;
    std::uint64_t size {};
    std::optional<std::int64_t> modified;
    std::uint64_t offset {};
    file_handle file;
    file_digest_t hash;
    std::chrono::steady_clock::time_point touched;
  };

  /** @brief Hash at most 8 MiB before yielding the HTTPS worker back to other clients. */
  inline int source_identity(std::map<std::string, identity_job_t> &jobs, const std::string &id, const fs::path &path, std::uint64_t size, std::optional<std::int64_t> modified, std::string &output, const std::function<bool()> &active) {
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(jobs, [&](const auto &job) {
      return now - job.second.touched >= file_resume_lifetime;
    });
    auto iter = jobs.find(id);
    if (iter == jobs.end()) {
      if (jobs.size() >= 8) {
        return 429;
      }
      identity_job_t job;
      const auto status = open_source(path, size, modified, job.file);
      if (status != 200) {
        return status;
      }
      if (!job.hash.start()) {
        return 500;
      }
      job.path = path;
      job.size = size;
      job.modified = modified;
      iter = jobs.emplace(id, std::move(job)).first;
    }
    auto &job = iter->second;
    if (job.path != path || job.size != size || job.modified != modified) {
      return 409;
    }
    job.touched = now;
    const auto count = (std::min) (size - job.offset, 8 * chunk_size);
    const auto end = job.offset + count;
    std::string buffer(static_cast<std::size_t>((std::min) (count, chunk_size)), '\0');
    while (job.offset < end) {
      if (!active()) {
        jobs.erase(iter);
        return 403;
      }
      const DWORD wanted = static_cast<DWORD>((std::min) (end - job.offset, chunk_size));
      DWORD read {};
      if (!ReadFile(job.file.get(), buffer.data(), wanted, &read, nullptr) || read != wanted) {
        jobs.erase(iter);
        return 409;
      }
      if (!job.hash.update(std::string_view(buffer.data(), read))) {
        jobs.erase(iter);
        return 500;
      }
      job.offset += read;
    }
    if (job.offset != size) {
      output = nlohmann::json({{"offset", std::to_string(job.offset)}}).dump();
      return 202;
    }
    const auto sha = job.hash.finish();
    jobs.erase(iter);
    if (sha.empty()) {
      return 500;
    }
    output = nlohmann::json({{"sha256", sha}, {"size", std::to_string(size)}}).dump();
    return 200;
  }

  /** @brief Identify the published file for a retried acknowledgement without keeping it locked. */
  struct receipt_t {
    DWORD volume {};
    std::uint64_t index {};
    LONGLONG changed {};
    LONGLONG modified {};
    std::uint64_t size {};

    bool operator==(const receipt_t &) const = default;
  };

  /** @brief Capture the file ID and native change timestamps from the same handle. */
  inline std::optional<receipt_t> inspect(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info {};
    FILE_BASIC_INFO basic {};
    if (!GetFileInformationByHandle(handle, &info) || !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
      return std::nullopt;
    }
    return receipt_t {info.dwVolumeSerialNumber, (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow, basic.ChangeTime.QuadPart, basic.LastWriteTime.QuadPart, (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow};
  }

  /** @brief Read current identity of a destination while excluding writers and replacements. */
  inline std::optional<receipt_t> inspect_path(const fs::path &path) {
    const auto raw = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
      return {};
    }
    file_handle file(raw);
    return inspect(raw);
  }

  inline std::string replacement_version(const fs::path &path) {
    const auto info = inspect_path(path);
    if (!info) {
      return {};
    }
    return std::to_string(info->volume) + "-" + std::to_string(info->index) + "-" + std::to_string(info->changed) + "-" + std::to_string(info->size);
  }

  /** @brief Retain authenticated sequential prefixes and short-lived idempotent publication receipts. */
  class resume_store_t {
  public:
    using clock = std::chrono::steady_clock;

    /** @brief Release temporary files and directory pins after a disconnected client times out. */
    void expire(clock::time_point now) {
      std::lock_guard guard(mutex);
      expire_locked(now);
    }

    /** @brief Process one validated transfer under the active desktop user's security context. */
    int exchange(std::string_view operation, std::string_view cert, LUID logon, std::uint64_t session, const fs::path &path, const std::string &id, std::string_view sha, std::uint64_t offset, std::uint64_t size, std::string_view bytes, std::vector<file_handle> &pins, std::string &output, std::optional<std::uint64_t> &ack, const std::function<bool()> &active, std::string_view replace = {}) {
      std::lock_guard guard(mutex);
      if (cert.empty() || !session || !active()) {
        return 403;
      }
      const auto now = clock::now();
      expire_locked(now);
      const bool bind = operation == "resume" || operation == "discard";
      auto it = std::ranges::find_if(records, [&](const record_t &r) {
        return r.cert == cert && r.logon.LowPart == logon.LowPart && r.logon.HighPart == logon.HighPart && (bind ? r.task == id : r.lease == id);
      });
      if (bind) {
        if (!file_digest_t::valid(sha)) {
          return 400;
        }
        if (it != records.end() && it->session > session) {
          return 403;
        }
        if (operation == "discard") {
          if (it != records.end() && it->path == path && it->sha == sha) {
            records.erase(it);
          }
          return 204;
        }
        if (it == records.end()) {
          if (std::ranges::count_if(records, [](const record_t &r) {
                return bool(r.file);
              }) >= 128 ||
              std::ranges::count_if(records, [&](const record_t &r) {
                return r.cert == cert && r.file;
              }) >= 32) {
            return 429;
          }
          if (records.size() >= 4096) {
            // Keep the newest receipts for retries without limiting the number of files in a directory transfer.
            auto oldest = records.end();
            for (auto i = records.begin(); i != records.end(); ++i) {
              if (!i->file && (oldest == records.end() || i->touched < oldest->touched)) {
                oldest = i;
              }
            }
            records.erase(oldest);
          }
          record_t r;
          const int status = create_staging(path, r.file, replace);
          if (status != 200) {
            return status;
          }
          if (!r.hash.start()) {
            return 500;
          }
          r.cert = cert;
          r.task = id;
          r.logon = logon;
          r.path = path;
          r.size = size;
          r.sha = sha;
          r.replace = replace;
          r.pins = std::move(pins);
          it = records.insert(records.end(), std::move(r));
        } else if (it->path != path || it->size != size || it->sha != sha || it->replace != replace || (!it->file && !same_destination(*it))) {
          return 409;
        }
        const auto lease = new_file_lease();
        if (lease.empty()) {
          return 500;
        }
        it->lease = lease;
        it->session = session;
        it->touched = now;
        output = nlohmann::json({{"id", lease}, {"offset", std::to_string(it->offset)}, {"complete", !it->file}}).dump();
        return 200;
      }
      if (it == records.end() || it->session != session || it->path != path) {
        return 404;
      }
      if (operation == "chunk") {
        if (it->size != size || !it->file || bytes.empty() || bytes.size() > chunk_size || offset > size || bytes.size() > size - offset) {
          return 400;
        }
        ack = it->offset;
        if (offset != it->offset) {
          return 409;
        }
        DWORD written {};
        const bool wrote = WriteFile(it->file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        if (!wrote || written != bytes.size()) {
          const int status = wrote ? 507 : file_error();
          records.erase(it);
          ack.reset();
          return status;
        }
        if (!it->hash.update(bytes)) {
          records.erase(it);
          ack.reset();
          return 500;
        }
        it->offset += written;
        it->touched = now;
        ack = it->offset;
        return 204;
      }
      if (operation != "commit") {
        return 400;
      }
      if (it->replace != replace) {
        return 409;
      }
      if (!it->file) {
        return same_destination(*it) ? 204 : 409;
      }
      if (it->offset != it->size) {
        return 409;
      }
      if (it->hash.finish() != it->sha) {
        records.erase(it);
        return 422;
      }
      const auto before = inspect(it->file.get());
      if (!before || !active()) {
        records.erase(it);
        return before ? 403 : 500;
      }
      const auto status = publish_file(it->file.get(), path, replace);
      if (status != 204) {
        records.erase(it);
        return status;
      }
      it->file.reset();
      it->receipt = inspect_path(path);
      it->pins.clear();
      it->touched = now;
      if (!it->receipt || it->receipt->volume != before->volume || it->receipt->index != before->index || it->receipt->size != it->size) {
        it->receipt.reset();
        return 409;
      }
      return 204;
    }

  private:
    /** @brief State scoped to one paired certificate, Windows logon and persistent task ID. */
    struct record_t {
      std::string cert, task, sha, lease;
      std::string replace;  ///< Listed target version approved for replacement, empty for a new file.
      LUID logon {};
      fs::path path;
      std::uint64_t session {}, size {}, offset {};
      clock::time_point touched;
      std::vector<file_handle> pins;
      file_handle file;
      file_digest_t hash;
      std::optional<receipt_t> receipt;
    };

    bool same_destination(const record_t &r) const {
      return r.receipt && r.receipt == inspect_path(r.path);
    }

    void expire_locked(clock::time_point now) {
      std::erase_if(records, [&](const auto &r) {
        return now - r.touched >= file_resume_lifetime;
      });
    }

    std::mutex mutex;
    std::vector<record_t> records;
  };
}  // namespace platf::file_transfer
