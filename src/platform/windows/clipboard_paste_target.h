/**
 * @file src/platform/windows/clipboard_paste_target.h
 * @brief Bind a file paste to the foreground shell folder selected by the desktop user.
 */
#pragma once

#include "clipboard_win32.h"

namespace platf::clipboard_upload {
  struct paste_target_t {
    std::filesystem::path directory;
    std::vector<win_clipboard::detail::file_handle> pins;
  };

  /** @brief Return 200 for a pinned local folder, or 204 for a different kind of paste target. */
  int capture_paste_target(HANDLE user_token, paste_target_t &target);
}  // namespace platf::clipboard_upload
