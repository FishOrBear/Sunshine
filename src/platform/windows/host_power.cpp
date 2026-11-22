/**
 * @file src/platform/windows/host_power.cpp
 * @brief Apply user-confirmed Windows power operations using a temporary thread privilege.
 */
#include "host_power.h"

#include "src/logging.h"

#include <reason.h>
#include <windows.h>

namespace host_power {
  action_t prepare_windows_action(bool restart) {
    HANDLE process_token {};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE, &process_token)) {
      return {};
    }
    HANDLE token {};
    const BOOL duplicated = DuplicateTokenEx(process_token, TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES | TOKEN_IMPERSONATE, nullptr, SecurityImpersonation, TokenImpersonation, &token);
    CloseHandle(process_token);
    if (!duplicated) {
      return {};
    }
    std::shared_ptr<void> privilege(token, CloseHandle);
    TOKEN_PRIVILEGES privileges {};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(nullptr, L"SeShutdownPrivilege", &privileges.Privileges[0].Luid) || !AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) || GetLastError() != ERROR_SUCCESS) {
      return {};
    }
    return [privilege = std::move(privilege), restart]() {
      if (!SetThreadToken(nullptr, privilege.get())) {
        BOOST_LOG(error) << "Unable to assume the Windows power privilege: " << GetLastError();
        return false;
      }
      const BOOL accepted = InitiateSystemShutdownExW(nullptr, nullptr, 10, FALSE, restart, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
      const auto win_error = accepted ? ERROR_SUCCESS : GetLastError();
      if (!RevertToSelf()) {
        std::terminate();
      }
      if (!accepted) {
        BOOST_LOG(error) << "Windows declined the paired client's power request: " << win_error;
      }
      return accepted != FALSE;
    };
  }
}  // namespace host_power
