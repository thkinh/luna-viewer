#include "lunaviewer/screen.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <turbojpeg.h>
#include <zstd.h>
using namespace lunaviewer;
namespace {
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F f, const std::string& message) {
    bool rejected=false;
    try { f(); } catch (const ScreenError&) { rejected=true; }
    require(rejected, message);
}
Bytes words(std::initializer_list<int> values) {
    Bytes b;
    for (int v : values) { b.push_back(static_cast<std::uint8_t>(v)); b.push_back(static_cast<std::uint8_t>(v>>8)); }
    return b;
}
Bytes number(std::int32_t n) {
    auto v=static_cast<std::uint32_t>(n);
    return {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v>>8), static_cast<std::uint8_t>(v>>16), static_cast<std::uint8_t>(v>>24)};
}
void append(Bytes& a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); }
Bytes compressed(const Bytes& data, const Bytes& dictionary={}) {
    Bytes out(ZSTD_compressBound(data.size()));
    ZSTD_CCtx* context=ZSTD_createCCtx();
    require(context!=nullptr, "Zstandard compressor allocation");
    const auto n=ZSTD_compress_usingDict(context, out.data(), out.size(), data.data(), data.size(), dictionary.data(), dictionary.size(), 1);
    ZSTD_freeCCtx(context);
    require(!ZSTD_isError(n), "Zstandard fixture generation"); out.resize(n); return out;
}
// Hand-encoded standard Zstandard raw block: independent of compressor library.
Bytes raw_zstd(const Bytes& raw) {
    require(raw.size()<256, "Small raw fixture");
    Bytes out{0x28,0xb5,0x2f,0xfd,0x20,static_cast<std::uint8_t>(raw.size())};
    const auto header=(raw.size()<<3)|1;
    out.push_back(static_cast<std::uint8_t>(header)); out.push_back(static_cast<std::uint8_t>(header>>8)); out.push_back(static_cast<std::uint8_t>(header>>16));
    append(out, raw); return out;
}
Parts init(int w, int h) { return {{0}, number(w), number(h)}; }
Parts mission(int sequence, Bytes rectangles={}, Bytes raw={}) {
    return {{1}, std::move(rectangles), raw.empty()?Bytes{}:raw_zstd(raw), {}, number(sequence), {}, {}, {}, {}, {}, {}};
}
std::array<unsigned char,4> pixel(const FrameSnapshot& f, int x, int y) {
    auto offset=(std::size_t(y)*f->geometry.width+x)*4;
    return {f->bgra[offset],f->bgra[offset+1],f->bgra[offset+2],f->bgra[offset+3]};
}
void color(const FrameSnapshot& f, int x, int y, int blue, int green, int red) {
    require(pixel(f,x,y)==std::array<unsigned char,4>{{static_cast<unsigned char>(blue),static_cast<unsigned char>(green),static_cast<unsigned char>(red),255}}, "Unexpected pixel at "+std::to_string(x)+","+std::to_string(y));
}
Bytes jpeg(int width, int height, const Bytes& rgb) {
    tjhandle c=tjInitCompress(); require(c!=nullptr,"JPEG fixture compressor");
    unsigned char* output=nullptr; unsigned long length=0;
    int status=tjCompress2(c, rgb.data(), width, width*3, height, TJPF_RGB, &output, &length, TJSAMP_444, 100, TJFLAG_ACCURATEDCT);
    Bytes bytes;
    if (status==0) bytes.assign(output,output+length);
    tjFree(output); tjDestroy(c); require(status==0,"JPEG fixture encode"); return bytes;
}
void geometry_and_parser() {
    ScreenRenderer r;
    rejects([&]{r.apply(mission(1));},"Unconfigured renderer");
    rejects([&]{r.configure(init(8193,1));},"Dimension limit");
    rejects([&]{r.configure(init(-1,1));},"Negative dimensions");
    auto p=init(3,2); p[0].push_back(99); p[1].push_back(99); r.configure(p);
    require(r.geometry().width==3 && r.tile_size()==16,"Initialization consumed first fields");
    auto initial=r.snapshot(); color(initial,0,0,0,0,0);
    p=init(3,2); p.resize(7); p[5]={0}; rejects([&]{r.configure(p);},"Missing paired version flag");
    p.resize(8); p[7]={1}; r.configure(p);
    p=init(3,2); p.resize(15); p[5]={0}; p[7]={1}; p[9]=number(0); p[10]=number(0); p[11]={0}; p[12]={0,0}; p[14]={2};
    r.configure(p); require(r.tile_size()==2,"Optional tile size");
    r.set_tile_size({{48,99},{1,99}}); require(r.tile_size()==1,"Command48 first-byte tile");
    rejects([&]{r.set_tile_size({{48},{0}});},"Zero tile size");
    rejects([&]{r.configure(init(4,2));},"Dynamic geometry rejected");
    require(r.snapshot()==initial,"Same geometry preserves snapshot");
    auto m=mission(ScreenRenderer::reset_sequence+1); m[1]={0}; rejects([&]{r.apply(m);},"Partial region");
    m=mission(ScreenRenderer::reset_sequence+1); m.resize(10); rejects([&]{r.apply(m);},"Missing part10");
    m=mission(ScreenRenderer::reset_sequence+1); m[5]=words({0,0,-1,1}); rejects([&]{r.apply(m);},"Signed negative width");
    m=mission(ScreenRenderer::reset_sequence+1); m[6]=words({0,0,0,1}); rejects([&]{r.apply(m);},"Truncated scroll");
    m=mission(ScreenRenderer::reset_sequence+1); m[4].push_back(99); m[0].push_back(99); m.push_back({99});
    r.apply(m); require(r.last_sequence()==ScreenRenderer::reset_sequence+1,"Trailing bytes/parts ignored");
    rejects([&]{r.apply(m);},"Repeated sequence rejected");
    r.reset(2147483647); r.configure(init(1,1)); rejects([&]{r.apply(mission(-2147483648));},"Sequence does not wrap");
}
void pixels_padding_clipping_scroll() {
    ScreenRenderer r; r.reset(0); r.configure(init(3,2));
    auto first=r.apply(mission(1,words({1,0,3,2}),words({0x7c00,0x03e0,0x001f,0x7fff,0x0000,0x7c00})));
    color(first,0,0,0,0,0); color(first,1,0,0,0,255); color(first,2,0,0,255,0);
    color(first,0,1,0,0,0); color(first,1,1,255,255,255); color(first,2,1,0,0,0);
    // Raw stages at (0,0), so a later temp copy exposes source pixels at those
    // coordinates rather than at the previous raw rectangle destination.
    auto second=mission(2); second[5]=words({0,0,1,1}); auto f=r.apply(second); color(f,0,0,0,0,255);
    require(first->bgra!=f->bgra,"Old immutable snapshot preserved"); color(first,0,0,0,0,0);
    ScreenRenderer s; s.reset(0); s.configure(init(1,3));
    s.apply(mission(1,words({0,0,1,3}),words({0x7c00,0x03e0,0x001f})));
    auto scroll=mission(2); scroll[6]=words({1,0,0,1,3,0,0,1,2});
    f=s.apply(scroll); color(f,0,0,0,255,0); color(f,0,1,255,0,0); color(f,0,2,255,0,0);
    // Negative destination clips the cache source with the same offset.
    ScreenRenderer c; c.reset(0); c.configure(init(2,1));
    auto cache=mission(1,words({0,0,2,1}),words({0x7c00,0x03e0})); cache[10]=words({0,0,2,1}); append(cache[10],number(10)); c.apply(cache);
    auto copy=mission(2); copy[9]=words({-1,0}); append(copy[9],number(10)); copy[5]=words({0,0,1,1});
    f=c.apply(copy); color(f,0,0,0,255,0);
    ScreenRenderer channels; channels.reset(0); channels.configure(init(32,1));
    Bytes values;
    for (int n=0;n<32;++n) append(values,words({n|0x8000}));
    f=channels.apply(mission(1,words({0,0,32,1}),values));
    const int expected[32]={0,8,16,24,33,41,49,57,66,74,82,90,99,107,115,123,132,140,148,156,165,173,181,189,198,206,214,222,231,239,247,255};
    for (int n=0;n<32;++n) color(f,n,0,expected[n],0,0);
}
void cache_order_and_atomicity() {
    ScreenRenderer r; r.reset(0); r.configure(init(3,2)); r.set_tile_size({{48},{1}});
    auto first=mission(1,words({0,0,1,1}),words({0x7c00})); first[10]=words({0,0,1,1}); append(first[10],number(10)); r.apply(first);
    const Bytes atlas=jpeg(2,2,{200,100,50,200,100,50,20,40,80,20,40,80});
    auto second=mission(2,words({2,1,1,1}),words({0x7fff}));
    second[9]=words({1,0}); append(second[9],number(10)); second[5]=words({1,0,1,1});
    second[7]=atlas; second[8]=words({2,0,1,2}); second[10]=words({2,0,1,1}); append(second[10],number(20));
    append(second[10],words({2,1,1,1})); append(second[10],number(10));
    auto f=r.apply(second); color(f,1,0,0,0,255); color(f,2,1,255,255,255);
    auto jpeg_pixel=pixel(f,2,0);
    auto third=mission(3); third[9]=words({0,1}); append(third[9],number(20)); append(third[9],words({1,1})); append(third[9],number(10)); third[5]=words({0,1,1,1,1,1,1,1});
    f=r.apply(third); color(f,1,1,0,0,255);
    auto quantize=[](int n){return (n&0xf8)|(n>>5);};
    color(f,0,1,quantize(jpeg_pixel[0]),quantize(jpeg_pixel[1]),quantize(jpeg_pixel[2])); require(r.cache_entries()==2,"Cache existing hash retained");
    auto before=r.snapshot();
    auto bad=mission(4,words({0,0,1,1}),words({0x001f})); bad[10]=words({3,0,1,1}); append(bad[10],number(99));
    rejects([&]{r.apply(bad);},"Out of bounds cache capture after drawing");
    require(r.snapshot()==before && r.last_sequence()==3 && r.cache_entries()==2,"Failed late mission is atomic");
    // A failed raw stage must also leave temp unchanged.
    auto expose=mission(4); expose[5]=words({0,0,1,1}); color(r.apply(expose),0,0,255,255,255);
    ScreenRenderer lru; lru.reset(0); lru.configure(init(1,1));
    auto fill=mission(1); for (int hash=0;hash<1000;++hash) {append(fill[10],words({0,0,1,1}));append(fill[10],number(hash));} lru.apply(fill);
    auto hit=mission(2); hit[9]=words({0,0});append(hit[9],number(0));hit[10]=words({0,0,1,1});append(hit[10],number(1000));lru.apply(hit);
    require(lru.cache_entries()==1000,"Native thousand-entry eviction");
    auto missing=mission(3);missing[9]=words({0,0});append(missing[9],number(1));rejects([&]{lru.apply(missing);},"Oldest unreferenced entry evicted");
    missing[9]=words({0,0});append(missing[9],number(0));lru.apply(missing);
    lru.clear_cache();require(lru.cache_entries()==0,"Clear cache");
    ScreenLimits limits; limits.maximum_cache_bytes=2;
    ScreenRenderer bounded(limits); bounded.reset(0); bounded.configure(init(2,1));
    auto budget=mission(1,words({0,0,2,1}),words({0x7c00,0x03e0})); budget[10]=words({0,0,1,1});append(budget[10],number(1));append(budget[10],words({1,0,1,1}));append(budget[10],number(2));
    rejects([&]{bounded.apply(budget);},"Cache byte limit"); require(bounded.cache_entries()==0 && bounded.last_sequence()==0,"Cache byte failure atomic");
}
void jpeg_atlas() {
    const Bytes image=jpeg(2,2,{200,100,50,200,100,50,20,40,80,20,40,80});
    ScreenRenderer r;r.reset(0);r.configure(init(4,1));r.set_tile_size({{48},{1}});
    auto m=mission(1);m[7]=image;m[8]=words({0,0,3,1,3,0,1,1});
    auto f=r.apply(m);
    for(int x=0;x<4;++x) {
        auto actual=pixel(f,x,0); const std::array<int,3> expected=x%2==0?std::array<int,3>{{80,40,20}}:std::array<int,3>{{50,100,200}};
        for(int channel=0;channel<3;++channel) require(std::abs(int(actual[channel])-expected[channel])<=3,"JPEG bottom-up shared column cursor");
    }
    auto bad=mission(2);bad[7]=image;bad[8]=words({0,0,4,1,0,0,1,1});auto before=r.snapshot();
    rejects([&]{r.apply(bad);},"JPEG cursor overflow");require(r.snapshot()==before,"JPEG failure atomic");
    bad[7]={1,2,3};bad[8]=words({0,0,1,1});rejects([&]{r.apply(bad);},"Invalid JPEG");
    bad[7]=image;bad[7].resize(image.size()/2);rejects([&]{r.apply(bad);},"Truncated JPEG");
    ScreenLimits limits;limits.maximum_auxiliary_tiles=1;ScreenRenderer limited(limits);limited.reset(0);limited.configure(init(2,1));limited.set_tile_size({{48},{1}});
    m[8]=words({0,0,2,1});rejects([&]{limited.apply(m);},"JPEG tile budget");
    // Atlas height may exceed configured screen dimension independently.
    limits=ScreenLimits{}; limits.maximum_dimension=2;
    ScreenRenderer tall(limits);tall.reset(0);tall.configure(init(1,1));tall.set_tile_size({{48},{1}});
    m=mission(1);m[7]=jpeg(1,3,{255,0,0,0,255,0,0,0,255});m[8]=words({0,0,1,1});
    auto actual=pixel(tall.apply(m),0,0);require(actual[0]>250 && actual[1]<4 && actual[2]<4,"Tall JPEG independent dimensions");
}
void compression_and_limits() {
    ScreenRenderer r;r.reset(0);r.configure(init(2,1));
    auto m=mission(1,words({0,0,2,1}),words({0x7c00,0x03e0}));
    m[2]=raw_zstd(words({0x7c00}));append(m[2],raw_zstd(words({0x03e0})));auto f=r.apply(m);
    color(f,0,0,0,0,255);color(f,1,0,0,255,0);
    auto before=r.snapshot();auto bad=mission(2,words({0,0,2,1}),words({0x7fff}));
    rejects([&]{r.apply(bad);},"Too few decompressed bytes");
    bad[2]=raw_zstd(words({0,0,0}));rejects([&]{r.apply(bad);},"Too many decompressed bytes");
    bad[2]=compressed(words({1,2}));bad[2].pop_back();rejects([&]{r.apply(bad);},"Truncated compressed stream");
    bad[2]={1,2,3};rejects([&]{r.apply(bad);},"Corrupt compressed stream");
    bad[2]=raw_zstd(words({1,2}));bad[2].push_back(1);rejects([&]{r.apply(bad);},"Garbage after Zstd frame");
    require(r.snapshot()==before && r.last_sequence()==1,"Zstd failures preserve committed state");
    m=mission(2,words({0,0,2,1}),words({0x001f,0x7fff}));m[2]=compressed(words({0x001f,0x7fff}));r.apply(m);
    // Full-output-chunk boundary checks drain logic and complete-frame detection.
    ScreenRenderer large;large.reset(0);large.configure(init(256,128));Bytes raw(65536,0);m=mission(1,words({0,0,256,128}));m[2]=compressed(raw);large.apply(m);
    ScreenLimits limits;limits.maximum_decompressed_bytes=2;ScreenRenderer small(limits);small.reset(0);small.configure(init(2,1));
    rejects([&]{small.apply(mission(1,words({0,0,2,1}),words({1,2})));},"Decompressed output budget");
    limits=ScreenLimits{};limits.maximum_records=1;ScreenRenderer records(limits);records.reset(0);records.configure(init(2,1));
    rejects([&]{records.apply(mission(1,words({0,0,1,1,1,0,1,1}),words({1,2})));},"Record count budget");
    limits=ScreenLimits{};limits.maximum_pixels=1;ScreenRenderer area(limits);rejects([&]{area.configure(init(2,1));},"Area budget");
    limits=ScreenLimits{};limits.maximum_payload_bytes=10;ScreenRenderer payload(limits);payload.configure(init(1,1));m=mission(ScreenRenderer::reset_sequence+1);m[3]=Bytes(20);rejects([&]{payload.apply(m);},"Aggregate payload budget");
    rejects([&]{r.load_dictionary("/no/such/screen-dictionary.dat");},"Missing dictionary rejected");r.load_dictionary("");
}
void dictionary(const std::string& path) {
    ScreenRenderer r;r.reset(0);r.configure(init(2,1));r.load_dictionary(path);
    std::ifstream file(path,std::ios::binary);Bytes dict((std::istreambuf_iterator<char>(file)),{});
    require(dict.size()==112640,"Optional dictionary fixture size");
    auto m=mission(1,words({0,0,2,1}));m[2]=compressed(words({0x7c00,0x03e0}),dict);
    ScreenRenderer missing;missing.reset(0);missing.configure(init(2,1));rejects([&]{missing.apply(m);},"Dictionary-dependent fixture rejects missing dictionary");
    for(int n=1;n<=2;++n) { m[4]=number(n);auto f=r.apply(m);color(f,0,0,0,0,255);color(f,1,0,0,255,0); }
    r.reset(0);r.configure(init(2,1));m[4]=number(1);r.apply(m); // installed dictionary survives reset
}
} // namespace
int main(int argc, char** argv) {
    try {
        geometry_and_parser(); pixels_padding_clipping_scroll(); cache_order_and_atomicity(); jpeg_atlas(); compression_and_limits();
        if(argc>1) dictionary(argv[1]);
        std::cout << "Screen fixtures passed" << (argc>1?" (including local dictionary)":" (dictionary asset not used)") << '\n';return 0;
    } catch(const std::exception& e) {std::cerr<<"Screen test failure: "<<e.what()<<'\n';return 1;}
}
