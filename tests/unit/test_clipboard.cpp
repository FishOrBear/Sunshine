/**
 * @file tests/unit/test_clipboard.cpp
 * @brief Exercise per-session clipboard negotiation and Windows clipboard message validation.
 */
// test includes
#include "../tests_common.h"

// local includes
#include "src/clipboard.h"

#ifdef _WIN32
  // platform includes
  #include "src/platform/windows/clipboard.h"
  #include "src/platform/windows/clipboard_win32.h"
#endif

TEST(ClipboardGrantTest, RequiresMutualConsentAndValidCapability) {
  const std::string token(64, 'a');
  const auto text = clipboard::negotiate({true, true}, {true, false}, token);
  EXPECT_TRUE(text.matches(token));
  EXPECT_FALSE(text.matches(std::string(64, 'b')));
  EXPECT_TRUE(text.formats.text);
  EXPECT_FALSE(text.formats.files);
  const auto files = clipboard::negotiate({false, true}, {true, true}, token);
  EXPECT_FALSE(files.formats.text);
  EXPECT_TRUE(files.formats.files);
  EXPECT_FALSE(clipboard::negotiate({true, false}, {false, true}, token).formats.matches_any());
  for (const auto &invalid : {std::string {}, std::string(63, '0'), std::string(64, '!')}) {
    const auto grant = clipboard::negotiate({true, true}, {true, true}, invalid);
    EXPECT_FALSE(grant.matches(invalid));
    EXPECT_FALSE(grant.formats.matches_any());
  }
}

TEST(ClipboardGrantTest, DisclosesOnlyNewCopiesAfterSessionStartup) {
  clipboard::formats_t formats {true, true, 1234};
  EXPECT_FALSE(formats.should_read(1234, 0));
  EXPECT_FALSE(formats.should_read(4321, std::nullopt));
  EXPECT_FALSE(formats.should_read(4321, 4321));
  EXPECT_TRUE(formats.should_read(4321, 1234));
}

#ifdef _WIN32
TEST(ClipboardGrantTest, FirstPollPreservesCopyMadeAfterStreamStartup) {
  const std::uint32_t current = platf::clipboard_sequence();
  const clipboard::formats_t formats {true, false, current - 1};
  std::string payload;
  std::uint32_t reported {};
  EXPECT_EQ(platf::exchange_clipboard(formats, false, std::nullopt, {}, payload, reported), 204);
  EXPECT_TRUE(payload.empty());
  EXPECT_EQ(reported, formats.initial_sequence);
  EXPECT_TRUE(formats.should_read(current, reported));
}

TEST(ClipboardCodecTest, RejectsUnsafeNamesAndMalformedFrames) {
  using namespace win_clipboard;
  for (const auto *name : {L"", L"..", L"../copy", L"dir\\copy", L"C:copy", L"nul.txt", L"copy.", L"com1 ", L"my|file", L"AUX"}) {
    EXPECT_FALSE(detail::valid_name(name));
  }
  EXPECT_TRUE(detail::valid_name(L"folder list.txt"));
  EXPECT_TRUE(detail::valid_name(L"conventional.jpg"));
  std::string encoded;
  detail::append_size(encoded, 3);
  encoded += "abc";
  std::string_view valid(encoded), extracted;
  EXPECT_TRUE(detail::take_field(valid, extracted));
  EXPECT_EQ(extracted, "abc");
  EXPECT_TRUE(valid.empty());
  std::string_view missing(encoded.data(), encoded.size() - 1);
  EXPECT_FALSE(detail::take_field(missing, extracted));
  std::string_view short_header(encoded.data(), 2);
  EXPECT_FALSE(detail::take_field(short_header, extracted));
}

TEST(ClipboardCodecTest, RejectsDisallowedOrMalformedWritesBeforeOpeningClipboard) {
  DWORD sequence {};
  EXPECT_EQ(win_clipboard::write(false, true, "Tprivate", sequence, {}), 403);
  EXPECT_EQ(win_clipboard::write(true, false, "Fprivate", sequence, {}), 403);
  EXPECT_EQ(win_clipboard::write(true, true, "Tcopied", sequence, {}, std::nullopt, []() {
              return false;
            }),
            403);
  EXPECT_EQ(win_clipboard::write(true, true, "?", sequence, {}), 400);
  EXPECT_EQ(win_clipboard::write(true, true, {}, sequence, {}), 400);
  EXPECT_EQ(win_clipboard::write(true, true, "F", sequence, {}), 400);
  EXPECT_EQ(win_clipboard::write(true, true, std::string("Ta\0b", 4), sequence, {}), 400);
  EXPECT_EQ(win_clipboard::write(true, true, std::string("T\xff", 2), sequence, {}), 400);
  EXPECT_EQ(win_clipboard::write(true, true, std::string(win_clipboard::max_text + 2, 'T'), sequence, {}), 413);
  EXPECT_EQ(win_clipboard::write(true, true, std::string(win_clipboard::max_body + 1, 'F'), sequence, {}), 413);
}
#endif
