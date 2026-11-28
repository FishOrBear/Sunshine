/**
 * @file src/platform/windows/local_sessions.cpp
 * @brief Publish host activity through a local-only Windows named pipe.
 */
#include "local_sessions.h"

// standard includes
#include <algorithm>
#include <array>

// lib includes
#include <nlohmann/json.hpp>
#include <windows.h>

// The security descriptor declarations use Win32 handle and SID types.
#include <sddl.h>

// local includes
#include "src/crypto.h"
#include "src/logging.h"
#include "src/utility.h"
#include "src/uuid.h"

namespace platf::local_sessions {
  using handle_t = util::safe_ptr_v2<void, BOOL, &CloseHandle>;  ///< Owns a valid pipe or event handle.

  roster_t &current() {
    static roster_t roster;
    return roster;
  }

  std::string audio_host_id() {
    static const auto id = util::hex_vec(crypto::rand(16));
    static const handle_t marker {CreateEventA(nullptr, TRUE, FALSE, ("Local\\Sunshine.AudioHost." + id).c_str())};
    return marker ? id : std::string {};
  }

  std::string roster_t::recovery_token(const std::string &identity, bool create) {
    std::lock_guard lock {mutex};
    if (const auto it = recovery_tokens.find(identity); it != recovery_tokens.end()) {
      return it->second;
    }
    if (!create || identity.empty()) {
      return {};
    }
    return recovery_tokens.emplace(identity, uuid_util::uuid_t::generate().string()).first->second;
  }

  void roster_t::revoke_recovery(const std::string &identity) {
    std::lock_guard lock {mutex};
    if (identity.empty()) {
      recovery_tokens.clear();
    } else {
      recovery_tokens.erase(identity);
    }
  }

  void roster_t::add(std::uint32_t stream_id, const std::string &identity, const std::string &name, bool view_only, std::function<void()> stop, bool companion, const std::string &recovery) {
    std::lock_guard lock {mutex};
    if (!recovery.empty()) {
      // The host can disconnect the device between HTTPS admission and RTSP startup.
      const auto it = recovery_tokens.find(identity);
      if (it == recovery_tokens.end() || it->second != recovery) {
        stop();
        return;
      }
    }
    auto device = std::ranges::find(devices, identity, &device_t::identity);
    if (companion && (device == devices.end() || device->closing)) {
      // Display prewarming can finish its RTSP handshake after the local user disconnects.
      stop();
      return;
    }
    if (device != devices.end() && device->closing) {
      // A fresh primary connection gets its own controls even while old workers finish.
      devices.erase(device);
      device = devices.end();
    }
    if (device == devices.end()) {
      const auto now = GetTickCount64();
      if (devices.empty()) {
        since = now;
      }
      devices.push_back({identity, uuid_util::uuid_t::generate().string(), name, now, false, {}});
      device = std::prev(devices.end());
    }
    device->streams.emplace(stream_id, stream_t {view_only, std::move(stop)});
  }

  void roster_t::remove(std::uint32_t stream_id) {
    std::lock_guard lock {mutex};
    for (auto device = devices.begin(); device != devices.end(); ++device) {
      if (device->streams.erase(stream_id)) {
        if (device->streams.empty()) {
          devices.erase(device);
        }
        break;
      }
    }
    if (devices.empty()) {
      since = 0;
    }
  }

  std::string roster_t::reply(std::string_view request) {
    std::lock_guard lock {mutex};
    const bool all = request == "disconnect-all";
    if (all) {
      recovery_tokens.clear();
    }
    const std::string_view prefix = "disconnect ";
    const auto target = request.starts_with(prefix) ? request.substr(prefix.size()) : std::string_view {};
    bool ok = all || request == "list";
    auto rows = nlohmann::json::array();
    for (auto &device : devices) {
      if (all || target == device.id) {
        if (!device.closing) {
          device.closing = true;
          recovery_tokens.erase(device.identity);
          for (const auto &[id, stream] : device.streams) {
            stream.stop();
          }
        }
        ok = true;
      }
      const bool view_only = std::ranges::all_of(device.streams, [](const auto &entry) {
        return entry.second.view_only;
      });
      rows.push_back({{"id", device.id}, {"name", device.name}, {"since", device.since}, {"viewOnly", view_only}, {"closing", device.closing}});
    }
    return nlohmann::json({{"ok", ok}, {"since", since}, {"devices", std::move(rows)}}).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  }

  std::wstring pipe_name() {
    DWORD session;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) {
      return {};
    }
    return LR"(\\.\pipe\Sunshine.HostSessions.v1.)" + std::to_wstring(session);
  }

  /** @brief Finish one overlapped operation, canceling it on shutdown or a stalled client. */
  static bool complete(HANDLE pipe, OVERLAPPED &io, BOOL immediate, HANDLE stop, DWORD timeout, DWORD &bytes) {
    if (!immediate) {
      if (GetLastError() != ERROR_IO_PENDING) {
        return false;
      }
      const HANDLE events[] {stop, io.hEvent};
      if (WaitForMultipleObjects(2, events, FALSE, timeout) != WAIT_OBJECT_0 + 1) {
        CancelIoEx(pipe, &io);
        GetOverlappedResult(pipe, &io, &bytes, TRUE);
        return false;
      }
    }
    return GetOverlappedResult(pipe, &io, &bytes, FALSE);
  }

  void serve(std::stop_token stop, roster_t &roster, const std::wstring &name) {
    DWORD session;
    PSECURITY_DESCRIPTOR descriptor {};
    // The desktop UI normally runs at medium integrity while the service host runs as SYSTEM.
    constexpr auto permissions = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)S:(ML;;NW;;;ME)";
    if (name.empty() || !ProcessIdToSessionId(GetCurrentProcessId(), &session) || !ConvertStringSecurityDescriptorToSecurityDescriptorW(permissions, SDDL_REVISION_1, &descriptor, nullptr)) {
      return;
    }
    SECURITY_ATTRIBUTES security {sizeof(security), descriptor, FALSE};
    HANDLE raw = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 128, 0, &security);
    LocalFree(descriptor);
    if (raw == INVALID_HANDLE_VALUE) {
      BOOST_LOG(warning) << "Could not open the local host sessions pipe: " << GetLastError();
      return;
    }
    handle_t pipe {raw};
    handle_t stopped {CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    handle_t ready {CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!stopped || !ready) {
      return;
    }
    std::stop_callback wake {stop, [&] {
                               SetEvent(stopped.get());
                             }};
    while (!stop.stop_requested()) {
      OVERLAPPED io {};
      io.hEvent = ready.get();
      DWORD bytes = 0;
      const BOOL connected = ConnectNamedPipe(pipe.get(), &io);
      if ((connected || GetLastError() != ERROR_PIPE_CONNECTED) && !complete(pipe.get(), io, connected, stopped.get(), INFINITE, bytes)) {
        DisconnectNamedPipe(pipe.get());
        continue;
      }
      auto disconnect = util::fail_guard([&] {
        DisconnectNamedPipe(pipe.get());
      });
      ULONG peer_session;
      if (!GetNamedPipeClientSessionId(pipe.get(), &peer_session) || peer_session != session) {
        continue;
      }
      std::array<char, 80> command {};
      if (!complete(pipe.get(), io, ReadFile(pipe.get(), command.data(), command.size(), nullptr, &io), stopped.get(), 2000, bytes)) {
        continue;
      }
      const auto response = roster.reply({command.data(), bytes});
      if (complete(pipe.get(), io, WriteFile(pipe.get(), response.data(), static_cast<DWORD>(response.size()), nullptr, &io), stopped.get(), 2000, bytes)) {
        // DisconnectNamedPipe discards unread data. Wait for the reader to close, with a deadline.
        complete(pipe.get(), io, ReadFile(pipe.get(), command.data(), 1, nullptr, &io), stopped.get(), 2000, bytes);
      }
    }
  }
}  // namespace platf::local_sessions
