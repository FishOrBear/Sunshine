/**
 * @file src/platform/windows/clipboard.cpp
 * @brief Access the interactive clipboard and file system with the desktop user's privileges.
 */
#include "clipboard.h"

#include "clipboard_win32.h"

namespace platf {
  namespace {
    /** @brief Release only resources actually acquired by this request. */
    struct shell_impersonation_t {
      HANDLE token {};  ///< Token of the interactive shell.
      bool active {};  ///< Whether to restore the original identity.

      ~shell_impersonation_t() {
        if (active && !RevertToSelf()) {
          std::terminate();
        }
        if (token) {
          CloseHandle(token);
        }
      }

      /** @brief Impersonate the interactive shell, including when Sunshine runs as SYSTEM. */
      bool begin() {
        DWORD pid {};
        if (!GetWindowThreadProcessId(GetShellWindow(), &pid)) {
          return false;
        }
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) {
          return false;
        }
        const bool opened = OpenProcessToken(process, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE, &token);
        CloseHandle(process);
        if (!opened || !ImpersonateLoggedOnUser(token)) {
          return false;
        }
        active = true;
        return true;
      }
    };

    /** @brief Refuse clipboard access when Windows is showing a locked or secure desktop. */
    bool on_user_desktop() {
      HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
      if (!desktop) {
        return false;
      }
      wchar_t name[64] {};
      DWORD needed {};
      bool result = GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &needed) &&
                    CompareStringOrdinal(name, -1, L"Default", -1, TRUE) == CSTR_EQUAL;
      CloseDesktop(desktop);
      return result;
    }
  }  // namespace

  std::uint32_t clipboard_sequence() {
    return GetClipboardSequenceNumber();
  }

  int exchange_clipboard(clipboard::formats_t formats, bool upload, std::optional<std::uint32_t> since, std::string_view input, std::string &output, std::uint32_t &sequence, const std::function<bool()> &grant_active) {
    // Report the stream-start revision even when the user copied something before the first poll.
    if (!upload && !since) {
      output.clear();
      sequence = formats.initial_sequence;
      return 204;
    }
    shell_impersonation_t user;
    if (!user.begin() || !on_user_desktop()) {
      return 409;
    }
    DWORD current = GetClipboardSequenceNumber();
    sequence = current;
    if (!upload && !formats.should_read(current, since)) {
      return 204;
    }

    int status {};
    if (upload) {
      std::filesystem::path staging;
      if (formats.files && !input.empty() && input.front() == 'F') {
        PWSTR app_data {};
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, user.token, &app_data))) {
          return 500;
        }
        auto release = std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)>(app_data, &CoTaskMemFree);
        staging = std::filesystem::path(app_data) / L"Sunshine" / L"Clipboard";
      }
      status = win_clipboard::write(formats.text, formats.files, input, current, staging, since, grant_active);
    } else {
      status = win_clipboard::read(formats.text, formats.files, *since, output, current);
      if (status == 200 && current == formats.initial_sequence) {
        output.clear();
        status = 204;
      }
    }
    sequence = current;
    return status;
  }
}  // namespace platf
