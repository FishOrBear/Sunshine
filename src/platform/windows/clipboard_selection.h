#pragma once

#include "clipboard_win32.h"
#include "interactive_desktop.h"

#include <charconv>
#include <cstring>
#include <userenv.h>
#include <vector>

namespace platf::selection {
  /** @brief Shell information requested from the interactive user's process context. */
  enum class operation_t : DWORD {
    clipboard,
    drag_source,
    drop_target
  };

  // One private, inherited section per request. The helper receives no network
  // credentials, filenames or file handles from the service.
  struct packet {
    operation_t operation;  ///< Clipboard revision or shell hit-test request.
    POINT point;  ///< Physical screen coordinates for a drag gesture.
    DWORD revision;
    DWORD count;
    wchar_t paths[win_clipboard::max_files * 32768];
  };

  struct unmap {
    void operator()(void *view) const {
      UnmapViewOfFile(view);
    }
  };

  using view_t = std::unique_ptr<packet, unmap>;

  inline int read_as_user(HANDLE shell, DWORD revision, std::vector<std::filesystem::path> &paths, operation_t operation = operation_t::clipboard, POINT point = {}) {
    SECURITY_ATTRIBUTES inherit {sizeof(inherit), nullptr, TRUE};
    win_clipboard::detail::file_handle section(CreateFileMappingW(INVALID_HANDLE_VALUE, &inherit, PAGE_READWRITE, 0, sizeof(packet), nullptr));
    if (!section) {
      return 500;
    }
    view_t view(static_cast<packet *>(MapViewOfFile(section.get(), FILE_MAP_WRITE, 0, 0, sizeof(packet))));
    if (!view) {
      return 500;
    }
    view->revision = revision;
    view->operation = operation;
    view->point = point;

    HANDLE primary {};
    if (!DuplicateTokenEx(shell, TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE, nullptr, SecurityImpersonation, TokenPrimary, &primary)) {
      return 500;
    }
    win_clipboard::detail::file_handle token(primary);
    wchar_t exe[32768];
    const DWORD length = GetModuleFileNameW(nullptr, exe, 32768);
    if (!length || length == 32768) {
      return 500;
    }
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --clipboard-file-selection " + std::to_wstring(reinterpret_cast<uintptr_t>(section.get()));
    STARTUPINFOEXW startup {};
    startup.StartupInfo.cb = sizeof(startup);
    wchar_t desktop[] = L"winsta0\\default";
    startup.StartupInfo.lpDesktop = desktop;
    SIZE_T bytes {};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> attributes(bytes);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) {
      return 500;
    }
    HANDLE only = section.get();
    PROCESS_INFORMATION process {};
    void *environment {};
    const bool started = CreateEnvironmentBlock(&environment, primary, FALSE) && UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &only, sizeof(only), nullptr, nullptr) &&
                         CreateProcessAsUserW(primary, exe, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, environment, std::filesystem::path(exe).parent_path().c_str(), &startup.StartupInfo, &process);
    if (environment) {
      DestroyEnvironmentBlock(environment);
    }
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!started) {
      return 500;
    }
    win_clipboard::detail::file_handle child(process.hProcess), thread(process.hThread);
    if (WaitForSingleObject(child.get(), 5000) != WAIT_OBJECT_0) {
      TerminateProcess(child.get(), 1);
      return 409;
    }
    DWORD code {};
    if (!GetExitCodeProcess(child.get(), &code)) {
      return 500;
    }
    if (code != 200) {
      return code == 204 || code == 409 || code == 413 ? static_cast<int>(code) : 500;
    }
    const auto limit = operation == operation_t::clipboard ? win_clipboard::max_files : 4096;
    if (!view->count || view->count > limit) {
      return 400;
    }
    const wchar_t *pos = view->paths, *end = pos + std::size(view->paths);
    for (DWORD i = 0; i < view->count; ++i) {
      const auto nul = std::find(pos, end, L'\0');
      if (nul == end || nul == pos || nul - pos > 32767) {
        return 400;
      }
      paths.emplace_back(std::wstring(pos, nul));
      pos = nul + 1;
    }
    return 200;
  }
}  // namespace platf::selection
