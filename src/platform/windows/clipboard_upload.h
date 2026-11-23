/**
 * @file src/platform/windows/clipboard_upload.h
 * @brief Stage a bounded file batch in the interactive user's clipboard cache.
 */
#pragma once

#include "clipboard.h"
#include "clipboard_tree.h"
#include "clipboard_win32.h"
#include "file_transfer.h"

#include <nlohmann/json.hpp>

namespace platf::clipboard_upload {
  namespace fs = std::filesystem;
  using win_clipboard::detail::file_handle;
  constexpr std::size_t chunk_limit = 4U << 20;  ///< Maximum body for one append.

  /** @brief Describe one file before accepting any payload bytes. */
  struct entry_t {
    std::wstring name;  ///< Validated relative name within the selection.
    std::uint64_t size;  ///< Expected byte length.
    std::uint64_t received {};  ///< Number of bytes written.
    file_handle handle;  ///< Pinned destination file.
    std::string sha256;  ///< Expected content digest for resumable files.
    file_digest_t hash;  ///< Incremental content digest when verification is enabled.
    std::string resolution;  ///< Empty asks on collision; "skip", "keep-both", or the confirmed destination version.
    bool directory {};  ///< Directory entries carry no payload bytes.

    /** @brief Accept a conflict decision for this manifest entry only. */
    bool set_resolution(const nlohmann::json &value) {
      if (!value.is_string()) {
        return false;
      }
      const auto &decision = value.get_ref<const std::string &>();
      if (decision != "skip" && decision != "keep-both" && (decision.empty() || decision.size() > 100 || decision.find_first_not_of("0123456789-") != std::string::npos)) {
        return false;
      }
      resolution = decision;
      return true;
    }

    /** @brief Initialize verification before creating either kind of staging file. */
    int start_digest() {
      if (sha256.empty()) {
        return 200;
      }
      if (!hash.start()) {
        return 500;
      }
      return !size && hash.finish() != sha256 ? 422 : 200;
    }
  };

  /** @brief Validate all metadata before creating any files. */
  inline int manifest(std::string_view body, std::vector<entry_t> &entries, std::uint64_t &total, bool resumable = false, bool interactive = false) {
    if (body.size() > win_clipboard::tree::max_manifest) {
      return 413;
    }
    const auto json = nlohmann::json::parse(body, nullptr, false);
    if (!json.is_array() || json.empty() || json.size() > win_clipboard::tree::max_entries) {
      return 400;
    }
    win_clipboard::tree::layout names;
    for (const auto &item : json) {
      if (!item.is_object() || !item.contains("name") || !item["name"].is_string() || !item.contains("size") || !item["size"].is_number_unsigned()) {
        return 400;
      }
      const auto &encoded = item["name"].get_ref<const std::string &>();
      fs::path name;
      bool directory;
      if (!names.add(win_clipboard::detail::from_utf8(encoded), name, directory)) {
        return 400;
      }
      const auto size = item["size"].get<std::uint64_t>();
      if (directory && (size || item.contains("resolution") || item.contains("sha256"))) {
        return 400;
      }
      // Windows file offsets are signed 64-bit; the batch byte count is unsigned.
      if (size > INT64_MAX || size > UINT64_MAX - total) {
        return 413;
      }
      total += size;
      auto &entry = entries.emplace_back();
      entry.name = name.native();
      entry.directory = directory;
      entry.size = size;
      if (interactive && item.contains("resolution") && !entry.set_resolution(item.at("resolution"))) {
        return 400;
      }
      if (resumable && !directory) {
        auto sha = item.find("sha256");
        if (sha == item.end() || !sha->is_string()) {
          return 400;
        }
        entry.sha256 = sha->get<std::string>();
        if (!file_digest_t::valid(entry.sha256)) {
          return 400;
        }
      }
    }
    return 200;
  }

  /** @brief Control deletion on a pinned handle; a pending file is also removed if the process exits abruptly. */
  inline bool delete_on_close(const file_handle &handle, bool enabled) {
    FILE_DISPOSITION_INFO disposition {static_cast<BOOLEAN>(enabled)};
    return handle && SetFileInformationByHandle(handle.get(), FileDispositionInfo, &disposition, sizeof(disposition));
  }

  /** @brief Make room for a new paste while retaining any cache directory referenced by the clipboard. */
  inline void reclaim_cache(const fs::path &root) {
    win_clipboard::detail::clipboard_lock lock(nullptr);
    if (!lock.opened) {
      return;
    }
    fs::path current;
    if (IsClipboardFormatAvailable(CF_HDROP)) {
      auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
      if (!drop) {
        return;
      }
      const auto count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
      if (count > win_clipboard::max_files) {
        return;
      }
      auto paths = win_clipboard::detail::file_paths(drop, count);
      if (paths.empty()) {
        return;
      }
      current = paths.front().parent_path();
      // If the shell combined selections from several directories, leave all of them intact.
      for (const auto &path : paths) {
        if (path.parent_path() != current) {
          return;
        }
      }
    }
    win_clipboard::detail::expire_batches(root, current, 1);
  }

  /** @brief Report write and volume allocation failures to the sender. */
  inline int storage_error() {
    const auto code = GetLastError();
    return code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL ? 507 : 500;
  }

  /** @brief One per-stream transaction with open, sequentially written file handles. */
  struct batch_t {
    std::string id;  ///< Random identifier independent of the session token.
    fs::path directory;  ///< Unique private staging directory.
    file_handle directory_handle;  ///< Pinned directory for teardown.
    std::vector<entry_t> entries;  ///< Files in original clipboard order.
    bool published {};  ///< Whether Explorer may still need the files.
    bool interactive {};  ///< Require a decision for every direct-paste collision.

    ~batch_t() {
      if (!published) {
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
          delete_on_close(it->handle, true);
          it->handle.reset();
        }
        delete_on_close(directory_handle, true);
      }
    }

    /** @brief Create one set of empty local files without overwriting existing data. */
    int begin(const fs::path &root, std::string_view body) {
      std::uint64_t total {};
      int result = manifest(body, entries, total);
      if (result != 200) {
        return result;
      }
      return stage(root, total);
    }

    /** @brief Pin staged destinations for an already validated manifest. */
    int stage(const fs::path &root, std::uint64_t total) {
      std::error_code ec;
      fs::create_directories(root, ec);
      if (ec || !win_clipboard::detail::local_file_path(root)) {
        return 500;
      }
      auto space = fs::space(root, ec);
      if (!ec && space.available < total) {
        reclaim_cache(root);
        space = fs::space(root, ec);
      }
      if (ec) {
        return 500;
      }
      if (space.available < total) {
        return 507;
      }

      GUID guid {};
      wchar_t leaf[39] {};
      if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, leaf, 39)) {
        return 500;
      }
      directory = root / leaf;
      for (wchar_t ch : leaf) {
        if (ch >= L'0' && ch <= L'9') {
          id.push_back(static_cast<char>(ch));
        } else if (ch >= L'A' && ch <= L'F') {
          id.push_back(static_cast<char>(ch + 32));
        } else if (ch >= L'a' && ch <= L'f') {
          id.push_back(static_cast<char>(ch));
        }
      }
      if (!CreateDirectoryW(directory.c_str(), nullptr)) {
        return storage_error();
      }
      auto dir = CreateFileW(directory.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
      if (dir == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(directory.c_str());
        return 500;
      }
      directory_handle.reset(dir);
      for (auto &entry : entries) {
        const auto path = directory / entry.name;
        if (entry.directory) {
          if (!CreateDirectoryW(path.c_str(), nullptr)) {
            return storage_error();
          }
          auto raw = CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
          if (raw == INVALID_HANDLE_VALUE) {
            RemoveDirectoryW(path.c_str());
            return 500;
          }
          entry.handle.reset(raw);
          BY_HANDLE_FILE_INFORMATION info {};
          if (!GetFileInformationByHandle(raw, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            return 409;
          }
          continue;
        }
        const auto status = entry.start_digest();
        if (status != 200) {
          return status;
        }
        auto file = CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
          return storage_error();
        }
        entry.handle.reset(file);
        if (!delete_on_close(entry.handle, true)) {
          return 500;
        }
      }
      return 200;
    }

    /** @brief Inspect one captured filename and describe an unresolved collision with its current replacement version. */
    int inspect_conflict(size_t index, const fs::path &path, bool allow_replace, nlohmann::json &conflict) {
      auto &decision = entries[index].resolution;
      if (decision == "skip" || decision == "keep-both") {
        return 200;
      }
      std::vector<file_handle> pins;
      const auto parent_status = file_transfer::pin_directory(path.parent_path(), pins);
      if (parent_status != 200) {
        return parent_status == 404 ? 200 : parent_status;
      }
      const DWORD attributes = GetFileAttributesW(path.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) {
          return file_transfer::file_error(error);
        }
        decision.clear();  // A missing target must be created with no-replace semantics.
        return 200;
      }
      std::string version;
      std::string_view reason;
      if (entries[index].directory) {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) ? 200 : 409;
      }
      if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        reason = "folder";
      } else if (!allow_replace) {
        reason = "permission";
      } else {
        version = file_transfer::replacement_version(path);
        if (version.empty()) {
          reason = "unavailable";
        } else if (decision == version) {
          return 200;
        }
      }
      conflict = {
        {"file", index},
        {"path", win_clipboard::detail::to_utf8(path.native())},
        {"version", version},
        {"reason", reason}
      };
      return 409;
    }

    /** @brief Return a single fresh conflict when a destination changes during delivery. */
    clipboard_upload_result_t check_conflict(size_t index, const fs::path &path, bool allow_replace) {
      nlohmann::json conflict;
      const auto status = inspect_conflict(index, path, allow_replace, conflict);
      return conflict.is_null() ? clipboard_upload_result_t {status} : clipboard_upload_result_t {status, conflict.dump(), {}, {}, "file"};
    }

    /** @brief Put partial files on the pinned Explorer volume, without touching unrelated directory entries. */
    int stage_for_delivery(const fs::path &target, std::uint64_t total) {
      std::error_code ec;
      const auto space = fs::space(target, ec);
      if (ec) {
        return 500;
      }
      if (space.available < total) {
        return 507;
      }
      for (auto &entry : entries) {
        if (entry.directory) {
          continue;
        }
        if (entry.resolution == "skip") {
          entry.received = entry.size;  // The client's resume offsets also omit skipped bytes.
          continue;
        }
        auto status = entry.start_digest();
        if (status == 200) {
          status = file_transfer::create_staging_in(target, entry.handle, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_SEQUENTIAL_SCAN);
        }
        if (status != 200) {
          return status;
        }
      }
      return 200;
    }

    /** @brief Preserve both files when the Explorer folder already contains the supplied name. */
    static fs::path copy_name(const fs::path &original, unsigned int number) {
      if (number == 1) {
        return original;
      }
      const auto suffix = L" (" + std::to_wstring(number) + L")";
      auto extension = original.extension().native();
      // Leave room for a stem and the counter even for an unusually long extension.
      extension.resize((std::min) (extension.size(), 254U - suffix.size()));
      auto stem = original.stem().native();
      stem.resize((std::min) (stem.size(), 255U - suffix.size() - extension.size()));
      if (!stem.empty() && IS_HIGH_SURROGATE(stem.back())) {
        stem.pop_back();
      }
      if (!extension.empty() && IS_HIGH_SURROGATE(extension.back())) {
        extension.pop_back();
      }
      return stem + suffix + extension;
    }

    /** @brief Deliver verified files once, rechecking the approved version at the final rename. */
    clipboard_upload_result_t deliver(const fs::path &target, const std::function<bool()> &active, bool allow_replace, std::string_view input) {
      if (std::any_of(entries.begin(), entries.end(), [](const auto &entry) {
            return entry.received != entry.size;
          })) {
        return {400};
      }
      if (!input.empty()) {
        const auto decision = nlohmann::json::parse(input, nullptr, false);
        if (!decision.contains("file") || !decision["file"].is_number_unsigned() || !decision.contains("resolution")) {
          return {400};
        }
        const auto index = decision["file"].get<std::uint64_t>();
        if (index >= entries.size() || (entries[index].handle && !entries[index].set_resolution(decision["resolution"]))) {
          return {400};
        }
      }
      for (size_t index = 0; index < entries.size(); ++index) {
        auto &entry = entries[index];
        const auto original = target / entry.name;
        std::vector<file_handle> parents;
        if (entry.directory || entry.handle) {
          const auto status = file_transfer::pin_directory(original.parent_path(), parents);
          if (status != 200 || !active()) {
            return {status != 200 ? status : 403};
          }
        }
        if (entry.directory) {
          if (!CreateDirectoryW(original.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            return {file_transfer::file_error()};
          }
          file_handle pin;
          if (!win_clipboard::tree::pin_folder(original, pin)) {
            return {409};
          }
          continue;
        }
        if (!entry.handle) {
          continue;
        }
        if (entry.resolution == "skip") {
          entry.handle.reset();  // Still delete-on-close.
          continue;
        }
        if (interactive) {
          const auto check = check_conflict(index, original, allow_replace);
          if (check.status != 200) {
            return check;
          }
        }
        FILE_BASIC_INFO visible {};
        visible.FileAttributes = FILE_ATTRIBUTE_ARCHIVE;
        if (!SetFileInformationByHandle(entry.handle.get(), FileBasicInfo, &visible, sizeof(visible))) {
          return {file_transfer::file_error()};
        }
        const bool keep_both = !interactive || entry.resolution == "keep-both";
        const unsigned int first_number = entry.resolution == "keep-both" ? 2 : 1;
        for (unsigned int number = first_number; number <= 1024; ++number) {
          const auto path = original.parent_path() / copy_name(original.filename(), number);
          if (keep_both && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            continue;
          }
          if (!active()) {
            return {403};
          }
          const auto status = file_transfer::rename_staged_file(entry.handle.get(), path, keep_both ? std::string_view {} : entry.resolution);
          if (status == 204) {
            entry.handle.reset();
            break;
          }
          if (!keep_both && (status == 409 || status == 403)) {
            const auto check = check_conflict(index, original, allow_replace);
            return check.status == 200 ? clipboard_upload_result_t {status} : check;
          }
          if (status != 409 || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return {status};
          }
        }
        if (entry.handle) {
          return {409};
        }
      }
      published = true;
      return {204};
    }

    /** @brief Append exactly the next bytes within the declared file size. */
    int append(std::uint32_t file, std::uint64_t offset, std::string_view bytes) {
      if (file >= entries.size() || bytes.empty()) {
        return 400;
      }
      auto &entry = entries[file];
      if (entry.received != offset) {
        return 409;
      }
      if (bytes.size() > chunk_limit || bytes.size() > entry.size - offset) {
        return 413;
      }
      DWORD written {};
      if (!WriteFile(entry.handle.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size()) {
        return storage_error();
      }
      if (entry.hash && !entry.hash.update(bytes)) {
        return 500;
      }
      entry.received += written;
      if (entry.hash && entry.received == entry.size && entry.hash.finish() != entry.sha256) {
        return 422;
      }
      return 204;
    }

    /** @brief Publish all files together after each declared length has been received. */
    int commit(std::optional<DWORD> expected, DWORD &revision, const std::function<bool()> &active) {
      std::wstring paths;
      for (const auto &entry : entries) {
        if (entry.received != entry.size) {
          return 400;
        }
        if (!fs::path(entry.name).has_parent_path()) {
          paths += (directory / entry.name).native();
          paths.push_back(L'\0');
        }
      }
      if (active && !active()) {
        return 403;
      }
      for (auto &entry : entries) {
        if (!entry.directory && !delete_on_close(entry.handle, false)) {
          return 500;
        }
      }
      const auto result = win_clipboard::publish(paths, true, revision, expected, active);
      if (result == 204) {
        published = true;
        // Explorer may open the published files as soon as the client receives its acknowledgement.
        for (auto &entry : entries) {
          entry.handle.reset();
        }
        directory_handle.reset();
        win_clipboard::detail::expire_batches(directory.parent_path(), directory, 2);
      } else {
        for (auto &entry : entries) {
          if (!entry.directory) {
            delete_on_close(entry.handle, true);
          }
        }
      }
      return result;
    }
  };
}  // namespace platf::clipboard_upload
