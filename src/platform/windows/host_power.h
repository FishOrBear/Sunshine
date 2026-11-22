/**
 * @file src/platform/windows/host_power.h
 * @brief Serialize explicit power requests and dispatch them after their replies have been written.
 */
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

namespace host_power {
  using action_t = std::function<bool()>;  ///< Returns whether the OS accepted the request.
  using completion_t = std::function<void(bool)>;  ///< Invoked when the HTTP response completes.

  /** @brief Decode the two fixed protocol actions, rejecting every other input. */
  inline std::optional<bool> restart_requested(std::string_view action) {
    if (action == "restart") {
      return true;
    }
    if (action == "shutdown") {
      return false;
    }
    return std::nullopt;
  }

  /** @brief Acquire a private Windows shutdown privilege before accepting a request. */
  action_t prepare_windows_action(bool restart);

  /** @brief Permit one pending power change per host, including its OS countdown. */
  class queue_t {
  public:
    /** @brief Reserve an action until reply delivery, rechecking authorization before dispatch. */
    std::pair<int, completion_t> prepare(const std::function<action_t()> &make_action, std::function<bool()> authorized) {
      std::lock_guard lock(mutex);
      if (!pending.expired() || std::chrono::steady_clock::now() < next_request) {
        return {409, {}};
      }
      auto action = std::make_shared<action_t>(make_action());
      if (!*action) {
        return {503, {}};
      }
      pending = action;
      return {202, [this, action, authorized = std::move(authorized)](bool sent) {
                std::lock_guard lock(mutex);
                if (!*action) {
                  return;
                }
                auto execute = std::move(*action);
                *action = {};
                if (sent && authorized() && execute()) {
                  next_request = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                }
                pending.reset();
              }};
    }

  private:
    std::mutex mutex;  ///< Protects acceptance and completion.
    std::weak_ptr<action_t> pending;  ///< Reservation expires if the network callback is discarded.
    std::chrono::steady_clock::time_point next_request {};  ///< End of the last accepted countdown.
  };
}  // namespace host_power
