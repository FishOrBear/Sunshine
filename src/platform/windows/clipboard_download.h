/**
 * @file src/platform/windows/clipboard_download.h
 * @brief Pin a copied file selection and export it one bounded read at a time.
 */
#pragma once

#include "clipboard.h"
#include "clipboard_tree.h"
#include "file_transfer.h"

namespace platf {
  /** @brief A stream-scoped snapshot of the files and directories copied by the interactive user. */
  struct clipboard_download_t {
    /** @brief A read handle to the original file that permits renaming and deletion. */
    struct file_t {
      file_transfer::file_handle handle;
      std::uint64_t size;
    };

    struct offered_file_t {
      std::filesystem::path path;
      BY_HANDLE_FILE_INFORMATION identity;
      std::string name;  ///< Relative wire name retained for an independent paste.
    };

    /// File identities retained until the next selection on this stream.
    std::vector<offered_file_t> offered;

    std::mutex mutex;
    std::vector<file_t> files;
    std::uint64_t lease {};
    DWORD revision {};
    LUID logon {};
    std::chrono::steady_clock::time_point last_read {};

    /** @brief Open the selected files. Return an empty optional to inspect other formats. */
    std::optional<int> begin(DWORD since, DWORD initial, DWORD &current, LUID owner, std::string &output, bool metadata_only = false, bool folders = false) {
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
      return pin(paths, current, owner, output, metadata_only, folders);
    }

    /** @brief Snapshot file identities, retaining read handles only for clients requesting a readable lease. */
    int pin(const std::vector<std::filesystem::path> &paths, DWORD current, LUID owner, std::string &output, bool metadata_only, bool folders = false) {
      std::vector<file_t> pinned;
      std::vector<offered_file_t> selected;
      auto manifest = nlohmann::json::array();
      const int status = win_clipboard::tree::walk(paths, folders, metadata_only, [&](const auto &path, const auto &name, const auto &identity, auto &handle) {
        const bool directory = (identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const auto size = directory ? 0 : (static_cast<std::uint64_t>(identity.nFileSizeHigh) << 32) | identity.nFileSizeLow;
        selected.push_back({path, identity, win_clipboard::detail::to_utf8(name)});
        if (!metadata_only) {
          pinned.push_back({directory ? nullptr : std::move(handle), size});
        }
        manifest.push_back({{"name", selected.back().name}, {"size", std::to_string(size)}});
      });
      if (status != 200) {
        return status;
      }
      const auto id = std::to_string(lease + 1);
      auto packet = "M" + nlohmann::json({{"lease", id}, {"offer", id}, {"metadataOnly", metadata_only}, {"files", std::move(manifest)}}).dump();
      if (packet.size() > win_clipboard::tree::max_manifest) {
        return 413;
      }
      if (current != GetClipboardSequenceNumber()) {
        return 409;
      }
      revision = current;
      logon = owner;
      files = std::move(pinned);
      offered = std::move(selected);
      last_read = std::chrono::steady_clock::now();
      ++lease;
      output = std::move(packet);
      return 200;
    }

    // Caller holds both snapshot locks; destination is a new independent transfer.
    int reopen(std::uint64_t id, DWORD expected, LUID owner, clipboard_download_t &destination, std::string &output) const {
      if (offered.empty() || lease != id || revision != expected || logon.HighPart != owner.HighPart || logon.LowPart != owner.LowPart) {
        return 409;
      }
      std::vector<file_t> pinned;
      auto manifest = nlohmann::json::array();
      for (const auto &[path, original, name] : offered) {
        std::vector<file_transfer::file_handle> parents;
        auto status = file_transfer::pin_directory(path.parent_path(), parents);
        if (status != 200) {
          return status;
        }
        const bool directory = (original.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const auto size = directory ? 0 : (static_cast<std::uint64_t>(original.nFileSizeHigh) << 32) | original.nFileSizeLow;
        file_t file {{}, size};
        status = directory ? (win_clipboard::tree::pin_folder(path, file.handle) ? 200 : 409) :
                             file_transfer::open_source(path, size, {}, file.handle, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE);
        if (status != 200) {
          return status;
        }
        BY_HANDLE_FILE_INFORMATION current {};
        if (!GetFileInformationByHandle(file.handle.get(), &current)) {
          return 500;
        }
        if (current.dwVolumeSerialNumber != original.dwVolumeSerialNumber || current.nFileIndexHigh != original.nFileIndexHigh || current.nFileIndexLow != original.nFileIndexLow || CompareFileTime(&current.ftCreationTime, &original.ftCreationTime) || CompareFileTime(&current.ftLastWriteTime, &original.ftLastWriteTime)) {
          return 409;
        }
        if (directory) {
          file.handle.reset();
        }
        pinned.emplace_back(std::move(file));
        manifest.push_back({{"name", name}, {"size", std::to_string(size)}});
      }
      destination.revision = revision;
      destination.logon = owner;
      destination.files = std::move(pinned);
      destination.last_read = std::chrono::steady_clock::now();
      output = "M" + nlohmann::json({{"lease", std::to_string(++destination.lease)}, {"files", std::move(manifest)}}).dump();
      return 200;
    }

    /** @brief Return one indexed chunk or release a completed transfer. */
    int read(const clipboard_download_request_t &request, DWORD expected, std::optional<DWORD> current, LUID owner, std::string &output) {
      if (files.empty() || lease != request.lease) {
        return 409;
      }
      if (revision != expected || (current && revision != *current) || logon.LowPart != owner.LowPart || logon.HighPart != owner.HighPart) {
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
      return !current || GetClipboardSequenceNumber() == revision ? 200 : 409;
    }
  };
}  // namespace platf
