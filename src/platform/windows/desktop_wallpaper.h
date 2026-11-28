/**
 * @file src/platform/windows/desktop_wallpaper.h
 * @brief Read the interactive Windows user's selected desktop wallpaper.
 */
#pragma once

// standard includes
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// local includes
#include "clipboard_win32.h"
#include "interactive_desktop.h"

namespace platf::wallpaper {
  constexpr DWORD max_image_bytes = 16 * 1024 * 1024;  ///< Maximum encoded wallpaper response.

  /** @brief Encoded wallpaper and its identified media type. */
  struct image_t {
    std::string bytes;  ///< Picture data, without a path or any other user information.
    std::string_view type;  ///< MIME type inferred from the picture signature.
  };

  /** @brief Identify a Windows wallpaper image by its file signature. */
  inline std::string_view image_type(std::string_view bytes) {
    if (bytes.starts_with("\xFF\xD8\xFF")) {
      return "image/jpeg";
    }
    if (bytes.starts_with("\x89PNG\r\n\x1A\n")) {
      return "image/png";
    }
    if (bytes.starts_with("BM")) {
      return "image/bmp";
    }
    return {};
  }

  /** @brief Read a bounded, regular local image without following a final reparse point. */
  inline image_t read_image(const std::filesystem::path &path) {
    if (!win_clipboard::detail::local_file_path(path)) {
      return {};
    }
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      return {};
    }
    win_clipboard::detail::file_handle file(handle);
    BY_HANDLE_FILE_INFORMATION info {};
    if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &info) || info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) || info.nFileSizeHigh || !info.nFileSizeLow || info.nFileSizeLow > max_image_bytes) {
      return {};
    }

    std::string bytes(info.nFileSizeLow, '\0');
    DWORD read {};
    if (!ReadFile(handle, bytes.data(), info.nFileSizeLow, &read, nullptr) || read != info.nFileSizeLow) {
      return {};
    }
    const auto type = image_type(bytes);
    return type.empty() ? image_t {} : image_t {std::move(bytes), type};
  }

  /**
   * @brief Read the wallpaper selected in the interactive shell user's settings.
   * @return The selected image, or nullopt when the user desktop is inaccessible.
   */
  inline std::optional<image_t> current() {
    desktop::shell_impersonation_t shell;
    if (!shell.begin() || !desktop::on_user_desktop()) {
      return std::nullopt;
    }

    HKEY profile {};
    if (RegOpenCurrentUser(KEY_READ, &profile) != ERROR_SUCCESS) {
      return std::nullopt;
    }
    std::wstring path(32768, L'\0');
    DWORD bytes = static_cast<DWORD>(path.size() * sizeof(wchar_t));
    const auto result = RegGetValueW(profile, L"Control Panel\\Desktop", L"WallPaper", RRF_RT_REG_SZ, nullptr, path.data(), &bytes);
    RegCloseKey(profile);
    if (result != ERROR_SUCCESS || path.front() == L'\0') {
      return image_t {};
    }
    path.resize(path.find(L'\0'));
    return read_image(path);
  }
}  // namespace platf::wallpaper
