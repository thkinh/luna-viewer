#pragma once
#include <array>
#include <climits>
#include <cstdint>
#include <string>
namespace lunaviewer {
// Matches the reference's three nonnegative decimal components. No speculative
// UTF-16 conversion, suffix trimming or version replacement is applied.
struct PeerVersion {
    std::array<unsigned,3> components{};
    bool valid=false;
    static PeerVersion parse(const std::string& text) {
        PeerVersion out;
        if(text.empty()||text.size()>64)return out;
        unsigned field=0;bool have_digit=false;
        for(unsigned char c:text) {
            if(c=='.') {if(!have_digit||field==2)return PeerVersion{};++field;have_digit=false;continue;}
            if(c<'0'||c>'9')return PeerVersion{};
            auto& value=out.components[field];const unsigned digit=c-'0';
            if(value>(static_cast<unsigned>(INT_MAX)-digit)/10)return PeerVersion{};
            value=value*10+digit;have_digit=true;
        }
        out.valid=field==2&&have_digit;
        return out;
    }
    bool at_least(std::array<unsigned,3> minimum)const{return valid&&components>=minimum;}
    static std::string describe(const std::string& text) {
        static const char* hex="0123456789abcdef";
        std::string out;
        for(std::size_t i=0;i<text.size()&&i<64;++i) {
            const auto c=static_cast<unsigned char>(text[i]);
            if(c>=32&&c<=126&&c!='\\'&&c!='"')out+=static_cast<char>(c);
            else {out+="\\x";out+=hex[c>>4];out+=hex[c&15];}
        }
        if(text.size()>64)out+="...";
        return out;
    }
};
}
