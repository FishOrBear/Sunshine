/**
 * @file src/clipboard.h
 * @brief Capabilities negotiated by a paired client for a single streaming session.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace platf {
  struct clipboard_upload_session_t;
  struct file_browser_session_t;
}  // namespace platf

namespace clipboard {
  constexpr std::size_t max_image_packet = 64 * 1024 * 1024;  ///< PNG clipboard packet, including its format byte.

  /**
   * @brief Clipboard formats enabled at both ends of a stream.
   */
  struct formats_t {
    bool text {};  ///< Plain UTF-8 text.
    bool files {};  ///< Regular local files.
    std::uint32_t initial_sequence {};  ///< Clipboard contents already present before this stream started.

    bool browser {};  ///< Allow directory listing and explicit file transfers.
    std::shared_ptr<platf::file_browser_session_t> browser_session {};  ///< Pending transfers owned by this stream.

    std::shared_ptr<platf::clipboard_upload_session_t> upload_session {};  ///< One in-progress clipboard batch, owned by this stream.

    bool resumable {};  ///< Client opted into content-verified reconnect support.

    bool images {};  ///< Clipboard bitmap exchange is enabled on both peers.

    /** @brief Skip contents predating this stream and revisions the client already received. */
    bool should_read(std::uint32_t current, std::optional<std::uint32_t> since) const {
      return since && current != initial_sequence && current != *since;
    }

    /** @brief Check whether this stream has any clipboard permission. */
    bool matches_any() const {
      return text || files || images;
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

  /** @brief Check a client capability encoded as exactly 32 hexadecimal bytes. */
  inline bool valid_token(std::string_view token) {
    return token.size() == 64 && std::all_of(token.begin(), token.end(), [](char c) {
             return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
           });
  }

  /** @brief Negotiate a grant only for explicitly enabled formats and a well-formed token. */
  inline grant_t negotiate(formats_t host, formats_t client, std::string token) {
    formats_t formats {host.text && client.text, host.files && client.files};
    formats.images = host.images && client.images;
    formats.browser = host.browser && client.browser;
    if ((!formats.matches_any() && !formats.browser) || !valid_token(token)) {
      return {};
    }
    return {std::move(token), formats};
  }
}  // namespace clipboard
