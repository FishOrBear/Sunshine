/**
 * @file src/platform/windows/local_sessions.h
 * @brief Incoming stream status and disconnect controls for the local Windows desktop.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace platf::local_sessions {
  /** @brief Tracks one visit per paired device, including its companion display streams. */
  class roster_t {
  public:
    /** @brief Register a running stream and its normal stop operation. */
    void add(std::uint32_t stream_id, const std::string &identity, const std::string &name, bool view_only, std::function<void()> stop, bool companion = false, const std::string &recovery = {});

    /** @brief Read a paired device's recovery token, creating one only for a fresh connection. */
    std::string recovery_token(const std::string &identity, bool create = false);

    /** @brief Invalidate automatic resume for one paired device, or every device on an app-wide quit. */
    void revoke_recovery(const std::string &identity = {});

    /** @brief Remove a stream after teardown and expire the visit when its last stream ends. */
    void remove(std::uint32_t stream_id);

    /** @brief Handle a local list or disconnect request and return a JSON snapshot. */
    std::string reply(std::string_view request);

  private:
    /** @brief Input mode and lifetime-bound stop operation of a registered stream. */
    struct stream_t {
      bool view_only;  ///< True when Sunshine suppresses remote input for this stream.
      std::function<void()> stop;  ///< Initiate normal stream teardown.
    };

    /** @brief Streams from the same paired client during one continuous connection. */
    struct device_t {
      std::string identity;  ///< Internal grouping key, normally the paired TLS certificate.
      std::string id;  ///< Opaque visit ID, renewed when the device reconnects.
      std::string name;  ///< Friendly name established at pairing.
      std::uint64_t since;  ///< Windows uptime in milliseconds when this device connected.
      bool closing;  ///< The host requested that all streams from this visit stop.
      std::map<std::uint32_t, stream_t> streams;  ///< Running streams keyed by launch ID.
    };

    std::mutex mutex;  ///< Protects membership and the lifetime of stop operations.
    std::map<std::string, std::string> recovery_tokens;  ///< One epoch per paired device, retained across transport loss.
    std::vector<device_t> devices;  ///< Connected devices in arrival order.
    std::uint64_t since {};  ///< Start of the current uninterrupted period of host activity.
  };

  /** @brief Return the roster shared by stream workers and the local status pipe. */
  roster_t &current();

  /** @brief Return this host's audio marker while its Windows session owns the corresponding named event. */
  std::string audio_host_id();

  /** @brief Serve requests from interactive clients in Sunshine's own Windows session until stopped. */
  void serve(std::stop_token stop, roster_t &roster, const std::wstring &pipe_name);

  /** @brief Return the local status pipe name for this Windows session. */
  std::wstring pipe_name();
}  // namespace platf::local_sessions
