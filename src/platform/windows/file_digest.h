/**
 * @file src/platform/windows/file_digest.h
 * @brief Incremental SHA-256 shared by resumable clipboard and standalone file transfers.
 */
#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <string>
#include <string_view>

namespace platf {
  constexpr auto file_resume_lifetime = std::chrono::minutes(30);  ///< Maximum idle retention across connection changes.

  /** @brief Generate a fresh 128-bit lease for a verified resume operation. */
  inline std::string new_file_lease() {
    std::array<unsigned char, 16> bytes {};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
      return {};
    }
    constexpr std::string_view hex = "0123456789abcdef";
    std::string lease;
    for (auto b : bytes) {
      lease.push_back(hex[b >> 4]);
      lease.push_back(hex[b & 15]);
    }
    return lease;
  }

  /** @brief Hash a byte stream using the wire protocol's lowercase SHA-256 encoding. */
  class file_digest_t {
  public:
    static bool valid(std::string_view text) {
      return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string_view::npos;
    }

    bool start() {
      ctx.reset(EVP_MD_CTX_new());
      return ctx && EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) == 1;
    }

    explicit operator bool() const {
      return bool(ctx);
    }

    bool update(std::string_view bytes) {
      return ctx && EVP_DigestUpdate(ctx.get(), bytes.data(), bytes.size()) == 1;
    }

    std::string finish() {
      std::array<unsigned char, 32> digest {};
      unsigned int length {};
      if (!ctx || !EVP_DigestFinal_ex(ctx.get(), digest.data(), &length) || length != digest.size()) {
        return {};
      }
      ctx.reset();
      constexpr std::string_view hex = "0123456789abcdef";
      std::string result;
      result.reserve(digest.size() * 2);
      for (auto b : digest) {
        result.push_back(hex[b >> 4]);
        result.push_back(hex[b & 15]);
      }
      return result;
    }

  private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx {nullptr, EVP_MD_CTX_free};
  };
}  // namespace platf
