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
  /** @brief Allocate the clipboard upload state for one paired stream. */
  std::shared_ptr<clipboard_upload_session_t> make_clipboard_upload_session();

  /** @brief Result of a bounded clipboard upload operation. */
  struct clipboard_upload_result_t {
    int status;  ///< HTTP status.
    std::string id {};  ///< Identifier issued by begin.
    std::optional<std::uint64_t> offset {};  ///< Acknowledged next byte on a chunk.
    std::optional<std::uint32_t> sequence {};  ///< Host revision after publication or a commit conflict.
    std::string_view conflict {};  ///< Fixed, non-sensitive reason returned for HTTP 409.
  };

  /** @brief Execute a file-batch operation under the interactive user's token. */
  clipboard_upload_result_t exchange_clipboard_batch(clipboard_upload_session_t &session, std::string_view op, std::string_view id, std::uint32_t file, std::uint64_t offset, std::optional<std::uint32_t> since, std::string_view input, const std::function<bool()> &grant_active);

  /** @brief Return the clipboard sequence at stream startup without retrieving its contents. */
  std::uint32_t clipboard_sequence();

  /** @brief Read or replace an interactive user's clipboard under their shell token. */
  int exchange_clipboard(clipboard::formats_t formats, bool upload, std::optional<std::uint32_t> since, std::string_view input, std::string &output, std::uint32_t &sequence, const std::function<bool()> &grant_active = {});
}  // namespace platf
