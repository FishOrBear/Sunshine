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
    HDESK original_desktop {};  ///< Borrowed thread desktop to restore after the request.
    HDESK shell_desktop {};  ///< Owned handle while temporarily attached to the shell desktop.

    ~shell_impersonation_t() {
      if (active && !RevertToSelf()) {
        std::terminate();
      }
      if (token) {
        CloseHandle(token);
      }
      if (shell_desktop) {
        if (original_desktop && !SetThreadDesktop(original_desktop)) {
          std::terminate();
        }
        CloseDesktop(shell_desktop);
      }
    }

    /** @brief Impersonate the interactive shell, including when Sunshine runs as SYSTEM. */
    bool begin() {
      // Encoder probing can attach the startup thread to Winlogon while the PC
      // is locked. HTTP workers inherit that desktop, which has no shell even
      // after the user unlocks the PC. Bind this request to Default explicitly;
      // on_user_desktop() still rejects access while the input desktop is secure.
      wchar_t name[64] {};
      DWORD needed {};
      const auto current = GetThreadDesktop(GetCurrentThreadId());
      if (!GetUserObjectInformationW(current, UOI_NAME, name, sizeof(name), &needed)) {
        return false;
      }
      if (CompareStringOrdinal(name, -1, L"Default", -1, TRUE) != CSTR_EQUAL) {
        shell_desktop = OpenDesktopW(L"Default", 0, FALSE, GENERIC_ALL);
        if (!shell_desktop || !SetThreadDesktop(shell_desktop)) {
          return false;
        }
        original_desktop = current;
      }
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
