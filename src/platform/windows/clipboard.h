/**
 * @file src/platform/windows/clipboard.h
 * @brief Access the Windows clipboard on behalf of the interactive desktop user.
 */
#pragma once

#include "src/clipboard.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace platf {
  int export_clipboard_paths(const char *section_handle);

  /** @brief Allocate upload state with the lifetime of one paired stream. */
  std::shared_ptr<file_browser_session_t> make_file_browser_session();

  /** @brief Refresh the capture geometry used to resolve file drops for this stream. */
  void set_file_drag_display(file_browser_session_t &session, int left, int top, int width, int height, int stream_width, int stream_height);

  /** @brief Serve a file operation as the interactive user. Deletion and conditional replacement require the deletion grant. */
  int exchange_files(file_browser_session_t &session, std::string_view operation, std::string_view path, const std::string &id, std::uint64_t offset, std::uint64_t size, std::optional<std::int64_t> modified, std::string_view input, std::string &output, const std::function<bool()> &grant_active, std::string_view cert = {}, std::string_view digest = {}, std::optional<std::uint64_t> *ack = nullptr, bool allow_delete = false, std::optional<std::pair<unsigned, unsigned>> drag_point = std::nullopt, std::string_view replace = {}, std::string_view name_filter = {});

  /** @brief Allocate the clipboard upload state for one paired stream. */
  std::shared_ptr<clipboard_upload_session_t> make_clipboard_upload_session(std::string cert);

  /** @brief Release abandoned uploads on the periodic host maintenance tick. */
  void expire_clipboard_uploads();

  /** @brief Release abandoned standalone file transfers after their resume window. */
  void expire_file_transfers();

  /** @brief Result of a bounded clipboard upload operation. */
  struct clipboard_upload_result_t {
    int status;  ///< HTTP status.
    std::string id {};  ///< Identifier issued by begin, or JSON containing the lease and offsets after resume.
    std::optional<std::uint64_t> offset {};  ///< Acknowledged next byte on a chunk.
    std::optional<std::uint32_t> sequence {};  ///< Host revision after publication or a commit conflict.
    std::string_view conflict {};  ///< Fixed, non-sensitive reason returned for HTTP 409.
    std::string task {};  ///< Canonical task selected for a captured paste gesture.
  };

  /** @brief Execute a file-batch operation under the interactive user's token. */
  clipboard_upload_result_t exchange_clipboard_batch(clipboard_upload_session_t &session, std::string_view op, std::string_view id, std::uint32_t file, std::uint64_t offset, std::optional<std::uint32_t> since, std::string_view input, const std::function<bool()> &grant_active, std::string_view task = {}, std::string_view previous = {});

  /** @brief Return the clipboard sequence at stream startup without retrieving its contents. */
  std::uint32_t clipboard_sequence();

  /** @brief One request for pinned files from a post-launch clipboard selection. */
  struct clipboard_download_request_t {
    std::string op;
    std::uint64_t lease {};
    std::uint32_t file {};
    std::uint64_t offset {};
    std::shared_ptr<clipboard_upload_session_t> offer_source {};  ///< Set by the server from the active stream grant.
    bool metadata_only {};  ///< Publish file identities without retaining any open files.
  };

  // Only read handles already pinned at paste time. No clipboard access or path lookup.
  int read_pinned_transfer(clipboard_upload_session_t &session, const clipboard_download_request_t &request, std::uint32_t revision, std::string &output);

  /** @brief Read or replace an interactive user's clipboard under their shell token. */
  int exchange_clipboard(clipboard::formats_t formats, bool upload, std::optional<std::uint32_t> since, std::string_view input, std::string &output, std::uint32_t &sequence, const std::function<bool()> &grant_active = {}, const clipboard_download_request_t &download = {});
}  // namespace platf
