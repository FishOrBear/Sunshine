/**
 * @file src/session_setup_timing.h
 * @brief Measure synchronous work on the GameStream launch and resume paths.
 */
#pragma once

#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace nvhttp {
  /** @brief Per-request timings up to the point where the HTTPS response is queued. */
  struct session_setup_timing_t {
    using clock = std::chrono::steady_clock;  ///< Monotonic clock for all stages.
    using duration = std::optional<std::chrono::milliseconds>;  ///< Empty when the stage was skipped.

    clock::time_point started {clock::now()};  ///< Request handler entry.
    duration display;  ///< Display configuration.
    duration encoder;  ///< Encoder availability check, including display enumeration.
    duration app;  ///< Application execution and preparation commands.

    /**
     * @brief Time one synchronous setup operation and pass its result back to the handler.
     * @param elapsed Destination for the measured duration.
     * @param operation Setup operation to execute once.
     * @return The operation's result, including failure codes.
     */
    template<class F>
    static auto measure(duration &elapsed, F operation) {
      const auto begin = clock::now();
      auto result = operation();
      elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - begin);
      return result;
    }

    /**
     * @brief Format measured and skipped stages together for one completed request.
     * @param verb GameStream operation name.
     * @param status GameStream response status code.
     * @return Log message with all timings in milliseconds.
     */
    std::string summary(std::string_view verb, int status) const {
      const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - started);
      const auto stage = [](const duration &d) {
        return d ? std::format("{}ms", d->count()) : "skipped";
      };
      return std::format("Session setup /{} status={} total={}ms display={} encoder={} app={}", verb, status, total.count(), stage(display), stage(encoder), stage(app));
    }
  };
}  // namespace nvhttp
