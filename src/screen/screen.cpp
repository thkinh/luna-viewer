// Screen mission behavior follows MacViewer source, copyright (c) 2026
// MacViewer contributors, used under its MIT license. See SCREEN-NOTICE.txt.
#include "lunaviewer/screen.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <utility>
#include <openssl/evp.h>
#include <turbojpeg.h>
#include <zstd.h>

namespace lunaviewer {
namespace {
[[noreturn]] void fail(const std::string& reason) { throw ScreenError(reason); }
std::size_t product(std::size_t a, std::size_t b) {
    if (b && a > std::numeric_limits<std::size_t>::max() / b) fail("Screen allocation overflow");
    return a * b;
}
std::uint16_t u16(const Bytes& b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | (std::uint16_t(b[at+1]) << 8));
}
int i16(const Bytes& b, std::size_t at) {
    auto n = u16(b, at);
    return n < 0x8000 ? int(n) : int(n) - 65536;
}
std::int32_t i32(const Bytes& b, std::size_t at) {
    std::uint32_t n = std::uint32_t(b[at]) | (std::uint32_t(b[at+1]) << 8)
        | (std::uint32_t(b[at+2]) << 16) | (std::uint32_t(b[at+3]) << 24);
    const auto signed_value = n < 0x80000000u ? std::int64_t(n) : std::int64_t(n) - 4294967296LL;
    return static_cast<std::int32_t>(signed_value);
}
const Bytes& part(const Parts& p, std::size_t at, std::size_t minimum = 0) {
    if (at >= p.size() || p[at].size() < minimum) fail("Missing or malformed screen part " + std::to_string(at));
    return p[at];
}
void command(const Parts& p, std::uint8_t expected) {
    if (part(p, 0, 1)[0] != expected) fail("Unexpected screen command");
}
void check_parts(const Parts& parts, const ScreenLimits& limits) {
    std::size_t count = 0;
    for (const auto& p : parts) {
        if (p.size() > limits.maximum_payload_bytes - count) fail("Screen payload limit exceeded");
        count += p.size();
    }
}
void check_geometry(int width, int height, const ScreenLimits& limits, bool atlas = false) {
    const int dimension = atlas ? limits.maximum_atlas_dimension : limits.maximum_dimension;
    if (width <= 0 || height <= 0 || width > dimension || height > dimension
        || product(std::size_t(width), std::size_t(height)) > limits.maximum_pixels)
        fail("Screen geometry exceeds supported limits");
}
struct Region { int x, y, width, height; };
Region region(const Bytes& b, std::size_t at, bool is_signed, const ScreenLimits& limits) {
    auto read = [&](std::size_t offset) { return is_signed ? i16(b, offset) : int(u16(b, offset)); };
    Region r{read(at), read(at+2), read(at+4), read(at+6)};
    check_geometry(r.width, r.height, limits);
    return r;
}
void records(const Bytes& b, std::size_t stride, const ScreenLimits& limits) {
    if (b.size() % stride || b.size() / stride > limits.maximum_records) fail("Malformed screen record array");
}
std::vector<Region> regions(const Bytes& b, bool is_signed, const ScreenLimits& limits) {
    records(b, 8, limits);
    std::vector<Region> result;
    result.reserve(b.size()/8);
    for (std::size_t at=0; at<b.size(); at+=8) result.push_back(region(b, at, is_signed, limits));
    return result;
}
struct Reference { int x, y; std::int32_t hash; };
struct Addition { Region region; std::int32_t hash; };
struct Mission {
    std::int32_t sequence;
    std::vector<Region> main, copy, auxiliary, scroll_copy;
    bool has_scroll = false;
    int delta = 0;
    Region capture{};
    std::vector<Reference> references;
    std::vector<Addition> additions;
};
Mission parse(const Parts& p, const ScreenLimits& limits) {
    check_parts(p, limits);
    command(p, 1);
    part(p, 10);
    Mission m;
    m.sequence = i32(part(p, 4, 4), 0);
    m.main = regions(p[1], false, limits);
    m.copy = regions(p[5], true, limits);
    m.auxiliary = regions(p[8], true, limits);
    if (!p[6].empty()) {
        if (p[6].size() < 10 || (p[6].size()-10)%8 || (p[6].size()-10)/8 > limits.maximum_records)
            fail("Malformed scroll mission");
        m.has_scroll = true;
        m.delta = i16(p[6], 0);
        m.capture = region(p[6], 2, true, limits);
        for (std::size_t at=10; at<p[6].size(); at+=8) m.scroll_copy.push_back(region(p[6], at, true, limits));
    }
    records(p[9], 8, limits);
    for (std::size_t at=0; at<p[9].size(); at+=8) m.references.push_back({i16(p[9], at), i16(p[9], at+2), i32(p[9], at+4)});
    records(p[10], 12, limits);
    for (std::size_t at=0; at<p[10].size(); at+=12) m.additions.push_back({region(p[10], at, true, limits), i32(p[10], at+8)});
    return m;
}
void zcheck(std::size_t code) {
    if (ZSTD_isError(code)) fail(std::string("Zstandard: ") + ZSTD_getErrorName(code));
}
struct ZstdDeleter { void operator()(ZSTD_DCtx* p) const { ZSTD_freeDCtx(p); } };
struct JpegDeleter { void operator()(void* p) const { if (p) tjDestroy(p); } };
struct Atlas { int width=0, height=0; Bytes pixels; };
Atlas decode_jpeg(const Bytes& bytes, const ScreenLimits& limits) {
    if (bytes.empty()) fail("Missing JPEG atlas");
    std::unique_ptr<void, JpegDeleter> decoder(tjInitDecompress());
    if (!decoder) fail("Cannot create JPEG decoder");
    int subsampling=0, color=0;
    Atlas atlas;
    if (tjDecompressHeader3(decoder.get(), bytes.data(), static_cast<unsigned long>(bytes.size()),
        &atlas.width, &atlas.height, &subsampling, &color) != 0) fail("Invalid JPEG atlas header");
    check_geometry(atlas.width, atlas.height, limits, true);
    atlas.pixels.resize(product(product(std::size_t(atlas.width), std::size_t(atlas.height)), 4));
    if (tjDecompress2(decoder.get(), bytes.data(), static_cast<unsigned long>(bytes.size()), atlas.pixels.data(),
        atlas.width, atlas.width*4, atlas.height, TJPF_BGRA, TJFLAG_ACCURATEDCT | TJFLAG_STOPONWARNING) != 0)
        fail("Invalid JPEG atlas data");
    return atlas;
}
} // namespace

struct ScreenRenderer::Impl {
    struct Cached { int width, height; std::shared_ptr<const Bytes> pixels; };
    struct State {
        std::shared_ptr<Frame> frame;
        Bytes temporary; // RGB555 bottom-up, DWORD-aligned rows.
        std::unordered_map<std::int32_t, Cached> cache;
        std::vector<std::int32_t> order;
        std::size_t cache_bytes = 0;
    };
    ScreenLimits limits;
    Geometry geometry;
    int tile = 16;
    std::int32_t sequence = ScreenRenderer::reset_sequence;
    std::size_t stride = 0;
    State state;
    std::unique_ptr<ZSTD_DCtx, ZstdDeleter> decoder;

    explicit Impl(ScreenLimits l) : limits(l), decoder(ZSTD_createDCtx()) {
        if (l.maximum_dimension <= 0 || !l.maximum_pixels || !l.maximum_payload_bytes
            || !l.maximum_decompressed_bytes || !l.maximum_records || !l.maximum_cache_bytes
            || !l.maximum_auxiliary_tiles || l.maximum_atlas_dimension <= 0) fail("Invalid screen resource limits");
        if (!decoder) fail("Cannot create Zstandard decoder");
        zcheck(ZSTD_DCtx_setParameter(decoder.get(), ZSTD_d_windowLogMax, 27));
    }
    bool inside(const Region& r) const {
        return r.x >= 0 && r.y >= 0 && r.width <= geometry.width && r.height <= geometry.height
            && r.x <= geometry.width-r.width && r.y <= geometry.height-r.height;
    }
    std::size_t temp_at(int x, int y) const { return std::size_t(geometry.height-1-y)*stride + std::size_t(x)*2; }
    std::size_t main_at(int x, int y) const { return (std::size_t(y)*geometry.width + x)*4; }
    struct Window { int sx, sy, dx, dy, width, height; };
    Window clip(int sx, int sy, int dx, int dy, int width, int height) const {
        // All operands derive from bounded 16-bit records or validated screen
        // dimensions, so these signed differences remain within int range.
        int left = std::max({0, -sx, -dx}), top = std::max({0, -sy, -dy});
        int right = std::min({width, geometry.width-sx, geometry.width-dx});
        int bottom = std::min({height, geometry.height-sy, geometry.height-dy});
        return {sx+left, sy+top, dx+left, dy+top, std::max(0, right-left), std::max(0, bottom-top)};
    }
    void stage(State& target, const Bytes& pixels, std::size_t offset, int width, int height, int x, int y) const {
        // Source and destination have different extents. Packed source remains
        // top-down even when the destination clip skips rows or columns.
        int left = std::max(0, -x), top = std::max(0, -y);
        int right = std::min(width, geometry.width-x), bottom = std::min(height, geometry.height-y);
        if (left >= right || top >= bottom) return;
        for (int row=top; row<bottom; ++row) {
            const auto from = offset + (std::size_t(row)*width + left)*2;
            const auto to = temp_at(x+left, y+row);
            std::memcpy(target.temporary.data()+to, pixels.data()+from, std::size_t(right-left)*2);
        }
    }
    void temporary_to_main(State& target, int sx, int sy, const Region& destination) const {
        auto w = clip(sx, sy, destination.x, destination.y, destination.width, destination.height);
        for (int row=0; row<w.height; ++row) {
            for (int col=0; col<w.width; ++col) {
                auto from = temp_at(w.sx+col, w.sy+row), to = main_at(w.dx+col, w.dy+row);
                auto value = u16(target.temporary, from);
                auto expand = [](unsigned int v) { return static_cast<std::uint8_t>((v<<3)|(v>>2)); };
                target.frame->bgra[to] = expand(value & 31);
                target.frame->bgra[to+1] = expand((value>>5) & 31);
                target.frame->bgra[to+2] = expand((value>>10) & 31);
                target.frame->bgra[to+3] = 255;
            }
        }
    }
    void main_to_temporary(State& target, const Region& source, int dx, int dy) const {
        auto w = clip(source.x, source.y, dx, dy, source.width, source.height);
        for (int row=0; row<w.height; ++row) {
            for (int col=0; col<w.width; ++col) {
                auto from = main_at(w.sx+col, w.sy+row), to = temp_at(w.dx+col, w.dy+row);
                const auto& b = target.frame->bgra;
                auto value = unsigned(b[from]>>3) | (unsigned(b[from+1]>>3)<<5) | (unsigned(b[from+2]>>3)<<10);
                target.temporary[to] = static_cast<std::uint8_t>(value);
                target.temporary[to+1] = static_cast<std::uint8_t>(value>>8);
            }
        }
    }
    Bytes extract(const State& target, const Region& r) const {
        Bytes result(product(product(std::size_t(r.width), std::size_t(r.height)), 2));
        for (int row=0; row<r.height; ++row)
            std::memcpy(result.data()+std::size_t(row)*r.width*2, target.temporary.data()+temp_at(r.x, r.y+row), std::size_t(r.width)*2);
        return result;
    }
    Bytes decompress(const Bytes& compressed, std::size_t expected) {
        zcheck(ZSTD_DCtx_reset(decoder.get(), ZSTD_reset_session_only));
        Bytes raw;
        raw.reserve(expected);
        if (!compressed.empty()) {
            ZSTD_inBuffer input{compressed.data(), compressed.size(), 0};
            std::array<std::uint8_t, 65536> buffer{};
            std::size_t remaining=0;
            for (;;) {
                const auto previous = input.pos;
                ZSTD_outBuffer output{buffer.data(), buffer.size(), 0};
                remaining = ZSTD_decompressStream(decoder.get(), &output, &input);
                zcheck(remaining);
                if (output.pos > expected-raw.size()) fail("Screen decompressed pixel count exceeds rectangles");
                raw.insert(raw.end(), buffer.begin(), buffer.begin()+output.pos);
                if (input.pos == input.size && (!remaining || output.pos < output.size)) break;
                if (previous == input.pos && !output.pos) fail("Zstandard decoder made no progress");
            }
            if (remaining) fail("Truncated Zstandard screen frame");
        }
        if (raw.size() != expected) fail("Screen decompressed pixel count does not match rectangles");
        return raw;
    }
    void auxiliary(State& target, const Atlas& atlas, const std::vector<Region>& rectangles) const {
        std::size_t tiles=0;
        int column=0, source_y=0;
        for (const auto& r : rectangles) {
            if (!inside(r)) fail("JPEG destination lies outside screen");
            for (int dx=0; dx<r.width; dx+=tile) {
                for (int dy=0; dy<r.height; dy+=tile) {
                    if (++tiles > limits.maximum_auxiliary_tiles) fail("JPEG tile count limit exceeded");
                    int tw=std::min(tile, r.width-dx), th=std::min(tile, r.height-dy);
                    if (source_y+th > atlas.height) { source_y=0; ++column; }
                    // Use a wide cursor: malicious oversized rectangle lists
                    // must never overflow before failing the atlas bound check.
                    const auto source_x = std::int64_t(column)*tile;
                    if (tw > atlas.width || th > atlas.height || source_x > atlas.width-tw || source_y > atlas.height-th)
                        fail("JPEG atlas tile lies outside image");
                    for (int row=0; row<th; ++row) {
                        auto from = (std::size_t(atlas.height-1-source_y-row)*atlas.width + std::size_t(source_x))*4;
                        auto to = main_at(r.x+dx, r.y+dy+row);
                        std::memcpy(target.frame->bgra.data()+to, atlas.pixels.data()+from, std::size_t(tw)*4);
                    }
                    source_y += th;
                }
            }
        }
    }
};

ScreenRenderer::ScreenRenderer(ScreenLimits limits) : impl_(new Impl(limits)) {}
ScreenRenderer::~ScreenRenderer() = default;
void ScreenRenderer::load_dictionary(const std::string& path) {
    Bytes dictionary;
    if (!path.empty()) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file || file.tellg() != 112640) fail("Screen dictionary must be the 112640-byte 8bit.dat asset");
        dictionary.resize(112640);
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(dictionary.data()), dictionary.size())) fail("Cannot read screen dictionary");
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned int length=0;
        if (!EVP_Digest(dictionary.data(), dictionary.size(), digest.data(), &length, EVP_sha256(), nullptr) || length != 32)
            fail("Cannot authenticate screen dictionary");
        const std::array<unsigned char, 32> expected{{0x9a,0xa4,0x57,0x89,0xd9,0xff,0x9d,0x32,0x9a,0x62,0x3a,0x68,0x5d,0x57,0xb6,0x7e,0x33,0xc7,0x77,0x01,0x9d,0xe9,0xce,0x1f,0x27,0x3f,0x21,0x86,0x0e,0x4b,0xfe,0x6b}};
        if (!std::equal(expected.begin(), expected.end(), digest.begin())) fail("Screen dictionary SHA-256 does not match");
    }
    zcheck(ZSTD_DCtx_reset(impl_->decoder.get(), ZSTD_reset_session_only));
    zcheck(ZSTD_DCtx_loadDictionary(impl_->decoder.get(), dictionary.data(), dictionary.size()));
}
void ScreenRenderer::configure(const Parts& p) {
    auto& d = *impl_;
    check_parts(p, d.limits);
    command(p, 0);
    Geometry g{i32(part(p, 1, 4), 0), i32(part(p, 2, 4), 0), 0, 0};
    check_geometry(g.width, g.height, d.limits);
    for (auto at : {5u, 11u}) if (p.size()>at) part(p, at, 1);
    if (p.size()>6) part(p, 7, 1);
    if (p.size()>8) { g.left=i32(part(p, 9, 4), 0); g.top=i32(part(p, 10, 4), 0); }
    if (p.size()>12) part(p, 12, 2);
    const int tile = p.size()>14 ? part(p, 14, 1)[0] : 16;
    if (!tile) fail("Screen tile size must be nonzero");
    if (d.state.frame) {
        if (g.width != d.geometry.width || g.height != d.geometry.height || g.left != d.geometry.left || g.top != d.geometry.top)
            fail("Dynamic screen geometry changes are not supported; reconnect");
        d.tile = tile;
        return;
    }
    if (p.size()>13) part(p, 13, 1);
    Impl::State state;
    auto frame = std::make_shared<Frame>();
    frame->geometry = g;
    frame->sequence = d.sequence;
    frame->bgra.resize(product(product(std::size_t(g.width), std::size_t(g.height)), 4));
    for (std::size_t at=3; at<frame->bgra.size(); at+=4) frame->bgra[at]=255;
    state.frame = std::move(frame);
    const auto stride = (std::size_t(g.width)*2+3) & ~std::size_t(3);
    state.temporary.resize(product(stride, std::size_t(g.height)));
    d.geometry=g; d.tile=tile; d.stride=stride; d.state=std::move(state);
}
FrameSnapshot ScreenRenderer::apply(const Parts& p) {
    auto& d = *impl_;
    if (!d.state.frame) fail("Screen renderer is not configured");
    const auto m = parse(p, d.limits);
    if (std::int64_t(m.sequence) != std::int64_t(d.sequence)+1) fail("Screen sequence mismatch");
    std::size_t expected=0;
    for (const auto& r : m.main) {
        const auto count=product(product(std::size_t(r.width), std::size_t(r.height)), 2);
        if (count > d.limits.maximum_decompressed_bytes-expected) fail("Decompressed screen limit exceeded");
        expected += count;
    }
    const auto raw = d.decompress(p[2], expected);
    auto next = d.state;
    next.frame = std::make_shared<Frame>(*d.state.frame);
    if (m.has_scroll) {
        d.main_to_temporary(next, m.capture, m.capture.x, m.capture.y-m.delta);
        for (const auto& r : m.scroll_copy) d.temporary_to_main(next, r.x, r.y, r);
    }
    for (const auto& ref : m.references) {
        auto found = next.cache.find(ref.hash);
        if (found == next.cache.end()) fail("Screen cache reference is missing");
        const auto& c = found->second;
        d.stage(next, *c.pixels, 0, c.width, c.height, ref.x, ref.y);
        next.order.erase(std::remove(next.order.begin(), next.order.end(), ref.hash), next.order.end());
        next.order.push_back(ref.hash);
    }
    for (const auto& r : m.copy) d.temporary_to_main(next, r.x, r.y, r);
    if (!m.auxiliary.empty()) d.auxiliary(next, decode_jpeg(p[7], d.limits), m.auxiliary);
    std::size_t offset=0;
    for (const auto& r : m.main) {
        d.stage(next, raw, offset, r.width, r.height, 0, 0);
        d.temporary_to_main(next, 0, 0, r);
        offset += std::size_t(r.width)*r.height*2;
    }
    for (const auto& addition : m.additions) {
        const auto& r=addition.region;
        if (!d.inside(r)) fail("Screen cache capture lies outside screen");
        d.main_to_temporary(next, r, r.x, r.y);
        auto pixels = std::make_shared<const Bytes>(d.extract(next, r));
        if (!next.cache.count(addition.hash)) {
            if (pixels->size() > d.limits.maximum_cache_bytes-next.cache_bytes) fail("Screen cache memory limit exceeded");
            next.cache.emplace(addition.hash, Impl::Cached{r.width, r.height, pixels});
            next.order.push_back(addition.hash);
            next.cache_bytes += pixels->size();
        }
    }
    while (next.order.size()>1000) {
        auto found=next.cache.find(next.order.front());
        if (found!=next.cache.end()) { next.cache_bytes-=found->second.pixels->size(); next.cache.erase(found); }
        next.order.erase(next.order.begin());
    }
    next.frame->sequence=m.sequence;
    d.state=std::move(next);
    d.sequence=m.sequence;
    return d.state.frame;
}
void ScreenRenderer::set_tile_size(const Parts& p) {
    check_parts(p, impl_->limits);
    command(p, 48);
    const int tile=part(p, 1, 1)[0];
    if (!tile) fail("Screen tile size must be nonzero");
    impl_->tile=tile;
}
void ScreenRenderer::reset(std::int32_t baseline) {
    impl_->state = Impl::State{};
    impl_->geometry = Geometry{};
    impl_->stride = 0;
    impl_->tile=16;
    impl_->sequence=baseline;
}
void ScreenRenderer::clear_cache() {
    impl_->state.cache.clear(); impl_->state.order.clear(); impl_->state.cache_bytes=0;
}
Geometry ScreenRenderer::geometry() const { return impl_->geometry; }
int ScreenRenderer::tile_size() const { return impl_->tile; }
std::int32_t ScreenRenderer::last_sequence() const { return impl_->sequence; }
std::size_t ScreenRenderer::cache_entries() const { return impl_->state.cache.size(); }
FrameSnapshot ScreenRenderer::snapshot() const { return impl_->state.frame; }
} // namespace lunaviewer
