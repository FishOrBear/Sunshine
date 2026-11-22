/**
 * @file src/platform/windows/interactive_desktop.h
 * @brief Check and impersonate the interactive desktop for paired file and clipboard requests.
 */
#pragma once

#include <exception>
#include <windows.h>

namespace platf::desktop {
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
  inline bool on_user_desktop() {
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
}  // namespace platf::desktop
