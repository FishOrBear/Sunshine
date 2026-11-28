/**
 * @file tests/unit/test_desktop_wallpaper.cpp
 * @brief Verify wallpaper format detection and bounded local file reads.
 */
// test includes
#include "../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <fstream>

  // local includes
  #include "src/platform/windows/desktop_wallpaper.h"
  #include "src/uuid.h"

namespace {
  namespace wallpaper = platf::wallpaper;
  namespace fs = std::filesystem;

  /** @brief Use an isolated location for each on-disk wallpaper fixture. */
  struct image_file_t {
    fs::path path = fs::temp_directory_path() / ("sunshine-wallpaper-" + uuid_util::uuid_t::generate().string());

    ~image_file_t() {
      std::error_code error;
      fs::remove(path, error);
    }
  };
}  // namespace

TEST(DesktopWallpaper, IdentifiesSupportedPictureFormats) {
  EXPECT_EQ(wallpaper::image_type("\xFF\xD8\xFF"), "image/jpeg");
  EXPECT_EQ(wallpaper::image_type("\x89PNG\r\n\x1A\n"), "image/png");
  EXPECT_EQ(wallpaper::image_type("BM"), "image/bmp");
  EXPECT_TRUE(wallpaper::image_type("not an image").empty());
}

TEST(DesktopWallpaper, ReadsPictureDataWithoutRequiringAnExtension) {
  image_file_t file;
  const std::string png = "\x89PNG\r\n\x1A\n";
  std::ofstream(file.path, std::ios::binary).write(png.data(), png.size());
  const auto image = wallpaper::read_image(file.path);
  EXPECT_EQ(image.bytes, png);
  EXPECT_EQ(image.type, "image/png");
}

TEST(DesktopWallpaper, RejectsMissingNonPictureAndOversizedFiles) {
  image_file_t file;
  EXPECT_TRUE(wallpaper::read_image(file.path).bytes.empty());
  std::ofstream(file.path, std::ios::binary) << "plain text";
  EXPECT_TRUE(wallpaper::read_image(file.path).bytes.empty());
  {
    std::ofstream output(file.path, std::ios::binary);
    output << "BM";
    output.seekp(wallpaper::max_image_bytes);
    output.put('\0');
  }
  EXPECT_TRUE(wallpaper::read_image(file.path).bytes.empty());
}
#endif
