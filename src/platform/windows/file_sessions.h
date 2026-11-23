/**
 * @file src/platform/windows/file_sessions.h
 * @brief Bound the lifetime of file-only grants independently of GameStream sessions.
 */
#pragma once

#include "clipboard.h"

#include <chrono>
#include <map>
#include <mutex>

namespace file_sessions {
  /** @brief Keep pairing identities and random client tokens bound to their own staging files. */
  template<class Session>
  class basic_registry_t {
  public:
    using clock = std::chrono::steady_clock;  ///< Monotonic idle timer.
    using session_ptr = std::shared_ptr<Session>;  ///< Open file operations and incomplete uploads.
    static constexpr auto idle_timeout = std::chrono::minutes(30);  ///< Extend on open or authenticated use.

    /** @brief Open or renew a validated grant for an enabled pairing. */
    template<class Factory>
    int insert(const std::string &cert, const std::string &token, Factory create, bool renew, clock::time_point now = clock::now()) {
      if (cert.empty() || !clipboard::valid_token(token)) {
        return 400;
      }
      std::lock_guard lock(mutex);
      expire_locked(now);
      const auto key = std::make_pair(cert, token);
      if (auto found = sessions.find(key); found != sessions.end()) {
        if (!renew) {
          return 409;
        }
        found->second.expires = now + idle_timeout;
        return 204;
      }
      if (sessions.size() >= 32 || std::count_if(sessions.begin(), sessions.end(), [&](const auto &entry) {
                                     return entry.first.first == cert;
                                   }) >= 4) {
        return 429;
      }
      sessions.emplace(key, grant_t {now + idle_timeout, create()});
      return 204;
    }

    /** @brief Return and renew only an unexpired grant owned by this exact TLS peer. */
    session_ptr find(const std::string &cert, const std::string &token, clock::time_point now = clock::now()) {
      std::lock_guard lock(mutex);
      auto found = sessions.find({cert, token});
      if (found == sessions.end()) {
        return {};
      }
      if (found->second.expires <= now) {
        sessions.erase(found);
        return {};
      }
      found->second.expires = now + idle_timeout;
      return found->second.session;
    }

    /** @brief End a file-only session, including when a previous close already removed it. */
    void close(const std::string &cert, const std::string &token) {
      std::lock_guard lock(mutex);
      sessions.erase({cert, token});
    }

    /** @brief Release abandoned staging handles even when no more requests arrive. */
    void expire(clock::time_point now = clock::now()) {
      std::lock_guard lock(mutex);
      expire_locked(now);
    }

    /** @brief Drop a disabled or removed pairing's grants, or all grants at shutdown. */
    void revoke(const std::string &cert = {}) {
      std::lock_guard lock(mutex);
      std::erase_if(sessions, [&](const auto &entry) {
        return cert.empty() || entry.first.first == cert;
      });
    }

  private:
    struct grant_t {
      clock::time_point expires;  ///< Latest deadline refreshed on activity.
      session_ptr session;  ///< Destruction removes its unpublished uploads.
    };

    /** @brief Purge all expired entries while holding the registry lock. */
    void expire_locked(clock::time_point now) {
      std::erase_if(sessions, [&](const auto &entry) {
        return entry.second.expires <= now;
      });
    }

    std::mutex mutex;  ///< Serialize open, close and expiry across HTTPS requests.
    std::map<std::pair<std::string, std::string>, grant_t> sessions;  ///< Grants keyed by certificate and nonce.
  };

  class registry_t: public basic_registry_t<platf::file_browser_session_t> {
  public:
    int open(const std::string &cert, const std::string &token, clock::time_point now = clock::now()) {
      return insert(cert, token, platf::make_file_browser_session, true, now);
    }
  };
}  // namespace file_sessions
