/**
 * @file src/platform/windows/file_drag.h
 * @brief Map a streamed file gesture to its captured display and describe shell files for transfer.
 */
#pragma once

#include "file_transfer.h"
#include "shellfiledrop.h"

#include <nlohmann/json.hpp>

namespace platf::file_drag {
  /** @brief Geometry belonging to one live file-enabled stream. Offsets are relative to the virtual desktop. */
  struct display_t {
    int left {}, top {};  ///< Captured monitor origin.
    int width {}, height {};  ///< Native capture dimensions.
    int stream_width {}, stream_height {};  ///< Encoded dimensions, including any host-side letterboxing.

    /** @brief Map normalized frame coordinates to physical shell coordinates, rejecting black bars. */
    std::optional<POINT> point(unsigned x, unsigned y, POINT origin) const {
      if (x > 65535 || y > 65535 || width <= 0 || height <= 0 || stream_width <= 0 || stream_height <= 0) {
        return std::nullopt;
      }
      const double scale = std::min(double(stream_width) / width, double(stream_height) / height);
      const double px = ((x + 0.5) * stream_width / 65536.0 - (stream_width - width * scale) / 2) / scale;
      const double py = ((y + 0.5) * stream_height / 65536.0 - (stream_height - height * scale) / 2) / scale;
      if (px < 0 || px >= width || py < 0 || py >= height) {
        return std::nullopt;
      }
      return POINT {origin.x + left + LONG(px), origin.y + top + LONG(py)};
    }
  };

  /** @brief Validate shell paths as the desktop user and serialize only real filesystem entries. */
  inline int describe(const std::vector<std::wstring> &paths, bool source, std::string &output) {
    namespace fs = std::filesystem;
    nlohmann::json entries = nlohmann::json::array();
    std::string directory;
    for (const auto &name : paths) {
      const fs::path path(name);
      std::vector<win_clipboard::detail::file_handle> pins;
      if (!source) {
        if (paths.size() != 1 || file_transfer::pin_directory(path, pins) != 200 || !fs::is_directory(path)) {
          return 403;
        }
        directory = win_clipboard::detail::to_utf8(path.generic_wstring());
        break;
      }
      if (!win_clipboard::detail::valid_name(path.filename().native()) || file_transfer::pin_directory(path.parent_path(), pins) != 200) {
        return 403;
      }
      WIN32_FILE_ATTRIBUTE_DATA info {};
      if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return 409;
      }
      const bool folder = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      const std::uint64_t size = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
      entries.push_back({{"name", win_clipboard::detail::to_utf8(path.filename().native())}, {"path", win_clipboard::detail::to_utf8(path.generic_wstring())}, {"directory", folder}, {"size", std::to_string(folder ? 0 : size)}, {"modified", std::to_string(file_transfer::milliseconds(info.ftLastWriteTime))}});
    }
    output = source ? nlohmann::json({{"entries", std::move(entries)}}).dump() : nlohmann::json({{"directory", directory}}).dump();
    return 200;
  }
}  // namespace platf::file_drag
