/**
 * @file src/platform/windows/clipboard_download.h
 * @brief Pin a copied file selection and export it one bounded read at a time.
 */
#pragma once

#include "clipboard.h"
#include "file_transfer.h"

namespace platf {
  /** @brief A stream-scoped snapshot of regular files copied by the interactive user. */
  struct clipboard_download_t {
    /** @brief A file pinned against writes and deletion until the download completes. */
    struct file_t {
      file_transfer::file_handle handle;
      std::uint64_t size;
    };

    std::mutex mutex;
    std::vector<file_t> files;
    std::uint64_t lease {};
    DWORD revision {};
    LUID logon {};
    std::chrono::steady_clock::time_point last_read {};

    /** @brief Open the selected files. Return an empty optional to inspect other formats. */
    std::optional<int> begin(DWORD since, DWORD initial, DWORD &current, LUID owner, std::string &output) {
      files.clear();
      std::vector<std::filesystem::path> paths;
      {
        win_clipboard::detail::clipboard_lock lock(nullptr);
        if (!lock.opened) {
          return 409;
        }
        current = GetClipboardSequenceNumber();
        if (current == initial || current == since) {
          return 204;
        }
        if (!IsClipboardFormatAvailable(CF_HDROP)) {
          return std::nullopt;
        }
        auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
        if (!drop) {
          return 409;
        }
        const auto count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
        if (count > win_clipboard::max_files) {
          return 413;
        }
        paths = win_clipboard::detail::file_paths(drop, count);
      }
      if (paths.empty()) {
        return 204;
      }
      std::vector<file_t> pinned;
      auto manifest = nlohmann::json::array();
      for (const auto &path : paths) {
        std::vector<file_transfer::file_handle> parents;
        const auto name = path.filename().native();
        if (!win_clipboard::detail::valid_name(name) || file_transfer::pin_directory(path.parent_path(), parents) != 200) {
          return 204;
        }
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error) {
          return 204;
        }
        file_t file {{}, size};
        const int status = file_transfer::open_source(path, size, {}, file.handle);
        if (status != 200) {
          return status == 409 ? 409 : 204;
        }
        pinned.emplace_back(std::move(file));
        manifest.push_back({{"name", win_clipboard::detail::to_utf8(name)}, {"size", std::to_string(size)}});
      }
      if (current != GetClipboardSequenceNumber()) {
        return 409;
      }
      revision = current;
      logon = owner;
      files = std::move(pinned);
      last_read = std::chrono::steady_clock::now();
      output = "M" + nlohmann::json({{"lease", std::to_string(++lease)}, {"files", std::move(manifest)}}).dump();
      return 200;
    }

    /** @brief Return one indexed chunk or release a completed transfer. */
    int read(const clipboard_download_request_t &request, DWORD expected, DWORD current, LUID owner, std::string &output) {
      if (files.empty() || lease != request.lease) {
        return 409;
      }
      if (revision != expected || revision != current || logon.LowPart != owner.LowPart || logon.HighPart != owner.HighPart) {
        files.clear();
        return 409;
      }
      if (request.op == "finish") {
        files.clear();
        return 204;
      }
      if (request.op != "chunk" || request.file >= files.size()) {
        return 400;
      }
      last_read = std::chrono::steady_clock::now();
      const auto &file = files[request.file];
      if (request.offset >= file.size) {
        return 400;
      }
      LARGE_INTEGER pos;
      pos.QuadPart = static_cast<LONGLONG>(request.offset);
      const DWORD length = static_cast<DWORD>((std::min) (file.size - request.offset, file_transfer::chunk_size));
      output.resize(length);
      DWORD count {};
      if (!SetFilePointerEx(file.handle.get(), pos, nullptr, FILE_BEGIN) || !ReadFile(file.handle.get(), output.data(), length, &count, nullptr) || count != length) {
        return 500;
      }
      return GetClipboardSequenceNumber() == revision ? 200 : 409;
    }
  };
}  // namespace platf
