/**
 * @file src/clipboard.h
 * @brief Capabilities negotiated by a paired client for a single streaming session.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace clipboard {
  /**
   * @brief Clipboard formats enabled at both ends of a stream.
   */
  struct formats_t {
    bool text {};  ///< Plain UTF-8 text.
    bool files {};  ///< Regular local files.
    std::uint32_t initial_sequence {};  ///< Clipboard contents already present before this stream started.

    /** @brief Skip contents predating this stream and revisions the client already received. */
    bool should_read(std::uint32_t current, std::optional<std::uint32_t> since) const {
      return since && current != initial_sequence && current != *since;
    }

    /** @brief Check whether this stream has any clipboard permission. */
    bool matches_any() const {
      return text || files;
    }
  };

  /**
   * @brief Bind a negotiated set of formats to an unpredictable per-launch token.
   */
  struct grant_t {
    std::string token;  ///< Hex encoding of 32 random bytes provided over authenticated TLS.
    formats_t formats;  ///< Data types allowed for this session.

    /** @brief Match a capability only when a valid grant exists. */
    bool matches(std::string_view presented) const {
      return !token.empty() && token == presented;
    }
  };

  /** @brief Negotiate a grant only for explicitly enabled formats and a well-formed token. */
  inline grant_t negotiate(formats_t host, formats_t client, std::string token) {
    formats_t formats {host.text && client.text, host.files && client.files};
    if ((!formats.text && !formats.files) || token.size() != 64 || !std::all_of(token.begin(), token.end(), [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        })) {
      return {};
    }
    return {std::move(token), formats};
  }
}  // namespace clipboard
