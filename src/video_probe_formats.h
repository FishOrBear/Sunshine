/**
 * @file src/video_probe_formats.h
 * @brief Validate extended encoder formats in capture-depth order.
 */
#pragma once

#include "video.h"

#ifdef _WIN32
  #include <algorithm>
  #include <display_device/types.h>
#endif

namespace video {
#ifdef _WIN32
  /**
   * @brief Check that a Windows probe describes the same known display configuration.
   * @param previous Devices observed before the last successful probe.
   * @param current Devices observed after applying the requested session display settings.
   * @return True for matching nonempty snapshots, independent of enumeration order.
   */
  inline bool matching_probe_displays(const display_device::EnumeratedDeviceList &previous, const display_device::EnumeratedDeviceList &current) {
    return !current.empty() && std::ranges::is_permutation(previous, current);
  }
#endif

  /**
   * @brief Record optional codec capabilities, testing SDR modes before HDR modes.
   * @param encoder Encoder with completed base codec results.
   * @param yuv444 Whether this encoder implements 4:4:4 input.
   * @param validate Test a single configuration against the capture and encoder backends.
   */
  template<class F>
  void probe_optional_formats(encoder_t &encoder, bool yuv444, F validate) {
    const auto probe = [&](encoder_t::codec_t &codec, int format, bool hdr, bool chroma444, encoder_t::flag_e flag) {
      const config_t config {1920, 1080, 60, 6000, 1000, 1, 0, hdr ? 3 : 1, format, hdr, chroma444, 0};
      codec[flag] = codec[encoder_t::PASSED] && (!chroma444 || yuv444) && validate(config);
    };

    probe(encoder.h264, 0, false, true, encoder_t::YUV444);
    probe(encoder.hevc, 1, false, true, encoder_t::YUV444);
    probe(encoder.av1, 2, false, true, encoder_t::YUV444);

    encoder.h264[encoder_t::DYNAMIC_RANGE] = false;
    encoder.h264[encoder_t::DYNAMIC_RANGE_YUV444] = false;
    probe(encoder.hevc, 1, true, false, encoder_t::DYNAMIC_RANGE);
    probe(encoder.hevc, 1, true, true, encoder_t::DYNAMIC_RANGE_YUV444);
    probe(encoder.av1, 2, true, false, encoder_t::DYNAMIC_RANGE);
    probe(encoder.av1, 2, true, true, encoder_t::DYNAMIC_RANGE_YUV444);
  }
}  // namespace video
