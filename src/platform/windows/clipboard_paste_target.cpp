/**
 * @file src/platform/windows/clipboard_paste_target.cpp
 * @brief Resolve the focused Explorer tab or desktop while impersonating its user.
 */
#include "clipboard_paste_target.h"

#include "file_transfer.h"
#include "interactive_desktop.h"

#include <exdisp.h>
#include <servprov.h>
#include <shlguid.h>
#include <wrl/client.h>

namespace platf::clipboard_upload {
  namespace {
    using Microsoft::WRL::ComPtr;
    using directory_t = std::filesystem::path;

    std::wstring window_class(HWND window) {
      wchar_t name[128] {};
      GetClassNameW(window, name, 128);
      return name;
    }

    HWND focused_pane(HWND window, bool desktop_window) {
      GUITHREADINFO info {sizeof(info)};
      if (!GetGUIThreadInfo(GetWindowThreadProcessId(window, nullptr), &info) || !info.hwndFocus || GetAncestor(info.hwndFocus, GA_ROOT) != window || info.hwndMenuOwner || info.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE)) {
        return nullptr;
      }
      for (auto child = info.hwndFocus; child && child != window; child = GetParent(child)) {
        const auto name = window_class(child);
        if (name == L"Edit" || name.find(L"SearchBox") != std::wstring::npos || name.find(L"Address Band") != std::wstring::npos) {
          return nullptr;
        }
        if (name == L"SHELLDLL_DefView" || name == L"SysListView32") {
          return child;
        }
      }
      return desktop_window && info.hwndFocus == window ? window : nullptr;
    }

    directory_t explorer_directory(HWND window, HWND pane) {
      ComPtr<IShellWindows> windows;
      if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER | CLSCTX_ENABLE_CLOAKING, IID_PPV_ARGS(windows.GetAddressOf())))) {
        return {};
      }
      CoSetProxyBlanket(windows.Get(), RPC_C_AUTHN_DEFAULT, RPC_C_AUTHZ_DEFAULT, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_DYNAMIC_CLOAKING);
      long count {};
      if (FAILED(windows->get_Count(&count))) {
        return {};
      }
      for (long i = 0; i < count; ++i) {
        VARIANT index {};
        index.vt = VT_I4;
        index.lVal = i;
        ComPtr<IDispatch> dispatch;
        ComPtr<IWebBrowserApp> browser;
        SHANDLE_PTR hwnd {};
        if (FAILED(windows->Item(index, &dispatch)) || !dispatch || FAILED(dispatch.As(&browser)) || FAILED(browser->get_HWND(&hwnd)) || reinterpret_cast<HWND>(hwnd) != window) {
          continue;
        }
        ComPtr<IServiceProvider> services;
        ComPtr<IShellBrowser> shell;
        ComPtr<IShellView> view;
        HWND view_window {};
        if (FAILED(dispatch.As(&services)) || FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(shell.GetAddressOf()))) || FAILED(shell->QueryActiveShellView(&view)) || FAILED(view->GetWindow(&view_window)) || (view_window != pane && !IsChild(view_window, pane))) {
          continue;
        }
        ComPtr<IFolderView> folder_view;
        ComPtr<IPersistFolder2> folder;
        PIDLIST_ABSOLUTE id {};
        if (FAILED(view.As(&folder_view)) || FAILED(folder_view->GetFolder(IID_PPV_ARGS(folder.GetAddressOf()))) || FAILED(folder->GetCurFolder(&id)) || !id) {
          continue;
        }
        std::unique_ptr<ITEMIDLIST, decltype(&CoTaskMemFree)> release_id(id, &CoTaskMemFree);
        ComPtr<IShellItem> item;
        PWSTR path {};
        if (SUCCEEDED(SHCreateItemFromIDList(id, IID_PPV_ARGS(item.GetAddressOf()))) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
          std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> release(path, &CoTaskMemFree);
          return directory_t(path);
        }
      }
      return {};
    }
  }  // namespace

  int capture_paste_target(HANDLE user_token, paste_target_t &target) {
    if (!desktop::on_user_desktop()) {
      return 403;
    }
    const auto window = GetForegroundWindow();
    if (!window) {
      return 204;
    }
    const auto name = window_class(window);
    const bool desktop_window = name == L"Progman" || name == L"WorkerW";
    if (!desktop_window && name != L"CabinetWClass" && name != L"ExploreWClass") {
      return 204;
    }
    if (desktop_window) {
      DWORD shell_pid {}, pid {};
      GetWindowThreadProcessId(GetShellWindow(), &shell_pid);
      GetWindowThreadProcessId(window, &pid);
      if (!shell_pid || pid != shell_pid) {
        return 204;
      }
    }
    const auto pane = focused_pane(window, desktop_window);
    if (!pane) {
      return 204;
    }
    const auto init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
      return 500;
    }

    struct com_scope {
      bool initialized;

      ~com_scope() {
        if (initialized) {
          CoUninitialize();
        }
      }
    } com {SUCCEEDED(init)};

    paste_target_t captured;
    if (desktop_window) {
      PWSTR path {};
      if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, user_token, &path)) || !path) {
        return 204;
      }
      std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> release(path, &CoTaskMemFree);
      captured.directory = path;
    } else {
      captured.directory = explorer_directory(window, pane);
    }
    if (captured.directory.empty() || !file_transfer::local_volume(captured.directory.root_path())) {
      return 204;
    }
    const auto status = file_transfer::pin_directory(captured.directory, captured.pins);
    if (status != 200) {
      return status;
    }
    if (!desktop::on_user_desktop()) {
      return 403;
    }
    if (GetForegroundWindow() != window || focused_pane(window, desktop_window) != pane || (!desktop_window && explorer_directory(window, pane) != captured.directory)) {
      return 409;
    }
    target = std::move(captured);
    return 200;
  }
}  // namespace platf::clipboard_upload
