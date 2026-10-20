#include "peer_version.hpp"
#include <iostream>
#include <stdexcept>
using namespace lunaviewer;
void check(bool c,const char* message){if(!c)throw std::runtime_error(message);}
int main(){try{
    check(!PeerVersion::parse("6.2.9").at_least({6,2,13}),"numeric9 falsely newer than13");
    check(PeerVersion::parse("6.2.13").at_least({6,2,13}),"threshold rejected");
    check(PeerVersion::parse("6.2.113").at_least({6,2,13}),"three-digit patch rejected");
    check(PeerVersion::parse("6.6.133").at_least({6,2,13}),"current reference rejected");
    check(PeerVersion::parse("06.02.013").at_least({6,2,13}),"decimal zero padding");
    for(const auto& text:{"","6.2","6.2.","6..13","6.2.13.0","v6.2.13","6.2.13beta","6.-2.13"," 6.2.13","6.2.999999999999"})
        check(!PeerVersion::parse(text).valid,"invalid version accepted");
    std::string utf16("6\0.\0" "2\0.\0" "1\0" "3\0",12);
    check(!PeerVersion::parse(utf16).valid,"unexpected UTF16 version silently accepted");
    check(PeerVersion::describe(std::string("6.2.13\0",7))=="6.2.13\\x00","NUL diagnostic ambiguous");
    check(PeerVersion::describe(std::string(500,'A')).size()==67,"version preview unbounded");
    std::cout<<"Peer version parsing/diagnostic tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
