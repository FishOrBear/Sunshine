/**
 * @file src/platform/windows/parsec_display.h
 * @brief Own a temporary monitor on an installed Parsec Virtual Display Adapter.
 * @see https://github.com/nomi-san/parsec-vdd/blob/main/docs/VDD_LIBRARY_USAGE.md
 */
#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

// SetupAPI requires the Windows base types above.
#include <setupapi.h>

namespace platf {
  /** @brief Keeps exactly one successfully allocated Parsec output plugged in. */
  class parsec_display_t {
  public:
    std::string device_id;  ///< Windows monitor ID assigned after the driver reports arrival.

    /** @brief Open the installed adapter and request a temporary output. */
    static std::unique_ptr<parsec_display_t> create() {
      constexpr GUID adapter {0x00b41627, 0x04c4, 0x429e, {0xa2, 0x6e, 0x02, 0x65, 0xcf, 0x50, 0xc8, 0xfa}};
      const auto devices = SetupDiGetClassDevsW(&adapter, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
      if (devices == INVALID_HANDLE_VALUE) {
        return {};
      }
      SP_DEVICE_INTERFACE_DATA iface {};
      iface.cbSize = sizeof(iface);
      HANDLE handle = INVALID_HANDLE_VALUE;
      if (SetupDiEnumDeviceInterfaces(devices, nullptr, &adapter, 0, &iface)) {
        DWORD size {};
        SetupDiGetDeviceInterfaceDetailW(devices, &iface, nullptr, 0, &size, nullptr);
        if (size >= sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
          std::vector<BYTE> buffer(size);
          auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
          detail->cbSize = sizeof(*detail);
          if (SetupDiGetDeviceInterfaceDetailW(devices, &iface, detail, size, nullptr, nullptr)) {
            handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
          }
        }
      }
      SetupDiDestroyDeviceInfoList(devices);
      if (handle == INVALID_HANDLE_VALUE) {
        return {};
      }
      auto display = std::unique_ptr<parsec_display_t>(new parsec_display_t(handle));
      DWORD transferred {};
      if (!display->control(0x22e004, 0, display->index, transferred) || transferred != sizeof(DWORD) || display->index >= 16) {
        return {};
      }
      display->allocated = true;
      display->heartbeat = std::jthread([instance = display.get()](std::stop_token stop) {
        while (!stop.stop_requested()) {
          DWORD ignored {}, bytes {};
          if (!instance->control(0x22a00c, 0, ignored, bytes)) {
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
      });
      return display;
    }

    /** @brief Stop keepalives and release only this allocation. */
    ~parsec_display_t() {
      if (heartbeat.joinable()) {
        heartbeat.request_stop();
        heartbeat.join();
      }
      if (allocated) {
        DWORD ignored {}, bytes {};
        control(0x22a008, static_cast<WORD>(index << 8), ignored, bytes);
      }
      CloseHandle(handle);
    }

  private:
    explicit parsec_display_t(HANDLE handle):
        handle(handle) {}

    /** @brief Submit a synchronous driver request with the protocol's fixed-size input. */
    bool control(DWORD code, WORD value, DWORD &result, DWORD &bytes) const {
      std::array<WORD, 16> input {};
      input[0] = value;
      return DeviceIoControl(handle, code, input.data(), sizeof(input), &result, sizeof(result), &bytes, nullptr);
    }

    HANDLE handle;  ///< Adapter file object.
    DWORD index {};  ///< Driver-assigned monitor slot.
    bool allocated {};  ///< Set only after an acknowledged allocation.
    std::jthread heartbeat;  ///< Driver keepalive worker.
  };
}  // namespace platf
