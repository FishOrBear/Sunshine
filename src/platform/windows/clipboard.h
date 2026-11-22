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
  /** @brief Return the clipboard sequence at stream startup without retrieving its contents. */
  std::uint32_t clipboard_sequence();

  /** @brief Read or replace an interactive user's clipboard under their shell token. */
  int exchange_clipboard(clipboard::formats_t formats, bool upload, std::optional<std::uint32_t> since, std::string_view input, std::string &output, std::uint32_t &sequence, const std::function<bool()> &grant_active = {});
}  // namespace platf
