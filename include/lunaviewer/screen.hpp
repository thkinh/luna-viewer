#pragma once
#include "lunaviewer/types.hpp"
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

namespace lunaviewer {
// Local resource policies, not wire-format constants.
struct ScreenLimits {
    int maximum_dimension = 8192;
    std::size_t maximum_pixels = 16 * 1024 * 1024;
    std::size_t maximum_payload_bytes = 64 * 1024 * 1024;
    std::size_t maximum_decompressed_bytes = 128 * 1024 * 1024;
    std::size_t maximum_records = 16384;
    std::size_t maximum_cache_bytes = 64 * 1024 * 1024;
    std::size_t maximum_auxiliary_tiles = 1048576;
    int maximum_atlas_dimension = 65535;
};
class ScreenError : public std::runtime_error {
  public:
    explicit ScreenError(const std::string &message) : std::runtime_error(message) {
    }
};
// Own on one serialized executor. Command 0 configures fixed geometry; command 1
// returns an immutable top-down opaque BGRA frame. Failures preserve frame,
// converter, cache and sequence. Command 48 is handled by set_tile_size(parts).
class ScreenRenderer {
  public:
    static constexpr std::int32_t reset_sequence = -2147483647;
    explicit ScreenRenderer(ScreenLimits limits = {});
    ~ScreenRenderer();
    ScreenRenderer(const ScreenRenderer &) = delete;
    ScreenRenderer &operator=(const ScreenRenderer &) = delete;
    // Empty path explicitly selects dictionary-free decoding (offline fixtures).
    // A supplied path must contain the authenticated 112640-byte 8bit.dat asset.
    // No dictionary asset is distributed with this module.
    void load_dictionary(const std::string &path);
    void configure(const Parts &parts);
    FrameSnapshot apply(const Parts &parts);
    void set_tile_size(const Parts &parts);
    void reset(std::int32_t baseline = reset_sequence);
    void clear_cache();
    Geometry geometry() const;
    int tile_size() const;
    std::int32_t last_sequence() const;
    std::size_t cache_entries() const;
    FrameSnapshot snapshot() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace lunaviewer
