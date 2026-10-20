#include "session.hpp"
#include <zstd.h>
#include <iostream>
#include <stdexcept>
using namespace lunaviewer;
namespace {
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
Bytes i32(std::int32_t n){auto u=static_cast<std::uint32_t>(n);return {std::uint8_t(u),std::uint8_t(u>>8),std::uint8_t(u>>16),std::uint8_t(u>>24)};}
std::vector<std::string> split(const std::string& text){std::vector<std::string> out;std::size_t p=0;for(;;){auto e=text.find(':',p);out.push_back(text.substr(p,e==std::string::npos?e:e-p));if(e==std::string::npos)break;p=e+1;}return out;}
std::string join(const std::vector<std::string>& f){std::string out;for(std::size_t i=0;i<f.size();++i){if(i)out+=':';out+=f[i];}return out;}
struct Harness {
    std::vector<std::pair<Channel,Bytes>> sent;
    std::vector<std::pair<Channel,TlsConfig>> opened;
    std::vector<std::string> order;
    FrameSnapshot frame;Parts initialization;
    bool permission=false,accepted=false;
    std::string status;std::vector<std::string> diagnostics;
    int closed=0, accepted_events=0;
    std::unique_ptr<Session> session;
    protocol::Route route{"192.0.2.2",4444};
    Harness() {
        SessionConfig config;
        config.require_dictionary=false;
        config.command={"127.0.0.1","ultraviewer","command.example","command.example",443};
        config.registration=std::vector<std::string>(28,"0");
        config.registration[0]="3";config.registration[1]="explicit-device";config.registration[2]="local-password";
        config.registration[4]="192.0.2.1";config.registration[6]="bGludXg=";config.registration[7]="6.6.133";
        config.registration[12]="";config.registration[25]="";config.registration[26]="";config.registration[27]="";
        SessionCallbacks c;
        c.open=[this](Channel ch,const TlsConfig& tls){opened.push_back({ch,tls});};
        c.send=[this](Channel ch,const Bytes& bytes){sent.push_back({ch,bytes});if(ch==Channel::Peer){auto envelope=protocol::parse_envelope(bytes);if(envelope.payload==protocol::bytes("18"))order.push_back("ack");}};
        c.close=[this]{++closed;};c.status=[this](const std::string& s){status=s;};
        c.diagnostic=[this](const std::string& message){diagnostics.push_back(message);};
        c.permission=[this](bool b){permission=b;};c.accepted=[this](bool b){accepted=b;++accepted_events;};
        c.frame=[this](FrameSnapshot f){frame=std::move(f);order.push_back("frame");};
        session=std::make_unique<Session>(config,std::move(c));
    }
    void start(){const bool first=closed==0;session->start({"123456","peer-password"},0);check(opened.size()==1,"command connection missing");if(first)check(accepted_events==0,"startup published disconnected state while connecting");session->ready(Channel::Command);}
    int register_and_lookup() {
        session->receive(Channel::Command,protocol::system(protocol::bytes("3:1000:42:192.0.2.1:x:0::0:20/10/2026::opaque")));
        auto envelope=protocol::parse_envelope(sent.back().second);
        check(envelope.system&&!envelope.payload.empty()&&envelope.payload[0]==0,"lookup command wrapping");
        auto ciphertext=Bytes(envelope.payload.begin()+1,envelope.payload.end());
        auto f=split(protocol::text(protocol::command_decrypt(ciphertext)));
        check(f.size()==17&&f[0]=="8"&&f[1]=="123456","lookup serialization");
        check(f[16].empty(),"relay-only lookup must not advertise LAN");
        return std::stoi(f[9]);
    }
    Bytes lookup_reply(int correlation,int status=1) {
        if(status!=1)return protocol::system(protocol::bytes("8:"+std::to_string(status)+":"+std::to_string(correlation)));
        std::vector<std::string> f(23,"");f[0]="8";f[1]="1";f[2]="192.168.1.2";f[4]=route.address;
        f[5]="1000";f[6]="0";f[7]="0";f[8]="cGVlcg==";f[9]=std::to_string(route.port);
        f[11]="192.0.2.50";f[12]="0";f[13]="-1";f[14]="relay.example";f[20]="secret-token";f[22]=std::to_string(correlation);
        return protocol::system(protocol::bytes(join(f)));
    }
    void connect(int prior_permission=-1,const std::string& peer_version="6.6.133") {
        start();auto correlation=register_and_lookup();
        session->receive(Channel::Command,lookup_reply(correlation+1));check(opened.size()==1,"mismatched correlation accepted");
        session->receive(Channel::Command,lookup_reply(correlation));check(opened.size()==2,"peer relay missing");
        const auto& tls=opened.back().second;
        check(tls.connect_address=="192.0.2.50"&&tls.sni=="ultraviewer"&&tls.verify_name=="relay.example"&&tls.http_host=="relay.example","independent relay identities");
        session->ready(Channel::Peer);
        auto login=split(protocol::text(protocol::parse_envelope(sent.back().second).payload));
        check(login.size()==23&&login[0]=="login"&&login[20]=="secret-token","login fields");
        if(prior_permission>=0)receive({Bytes{49},Bytes{static_cast<std::uint8_t>(prior_permission)}});
        Parts init(11);init[0]={0};init[1]=i32(1);init[2]=i32(1);init[5]={0};init[6]=protocol::bytes(peer_version);init[7]={1};init[9]=i32(-10);init[10]=i32(-20);
        initialization=init;
        session->control_enabled(true);InputEvent early;early.kind=InputEvent::Kind::Key;early.virtual_key='A';early.scan_code=30;early.pressed=true;
        const auto before_init=sent.size();session->input(early);check(sent.size()==before_init,"input escaped active-session gate before init");session->control_enabled(false);
        receive(init);check(session->active()&&accepted,"init not accepted");
        check(protocol::parse_envelope(sent.back().second).payload==protocol::bytes("17"),"init ACK missing");
    }
    void receive(const Parts& p){session->receive(Channel::Peer,protocol::forward(route,protocol::multipart(p)));}
    Parts drawing() {
        Parts p(11);p[0]={1};p[1]={0,0,0,0,1,0,1,0};p[4]=i32(-2147483646);
        Bytes raw{0,0x7c};p[2].resize(ZSTD_compressBound(raw.size()));auto n=ZSTD_compress(p[2].data(),p[2].size(),raw.data(),raw.size(),1);check(!ZSTD_isError(n),"test compression");p[2].resize(n);return p;
    }
};
}
int main(){try {
    Harness older;older.connect(-1,"6.2.9");older.session->control_enabled(true);
    InputEvent blocked;blocked.kind=InputEvent::Kind::Key;blocked.virtual_key='Z';blocked.scan_code=44;blocked.pressed=true;
    auto older_before=older.sent.size();auto diagnostics_before=older.diagnostics.size();
    for(int i=0;i<500;++i)older.session->input(blocked);
    check(older.sent.size()==older_before,"unsupported keyboard gate removed");
    check(older.status.find("peer reports 6.2.9")!=std::string::npos,"blocked status omitted observed version");
    check(older.diagnostics.size()==diagnostics_before+1,"blocked keyboard diagnostic flooded per event");
    older.session->disconnect();check(older.diagnostics.back().find("blocked_keyboard_events=500")!=std::string::npos,"keyboard counter summary missing");
    for(const auto& line:older.diagnostics)check(line.find("peer-password")==std::string::npos&&line.find("local-password")==std::string::npos,"credential logged");
    Harness threshold;threshold.connect(-1,"6.2.13");threshold.session->control_enabled(true);auto threshold_before=threshold.sent.size();threshold.session->input(blocked);check(threshold.sent.size()==threshold_before+1,"binary threshold did not enable keyboard");threshold.session->disconnect();
    Harness implicit;implicit.connect();check(implicit.permission,"omitted command49 left accepted session denied");
    check(implicit.diagnostics.back().find("accepted-init-default")!=std::string::npos,"implicit permission provenance missing");
    InputEvent probe;probe.kind=InputEvent::Kind::Key;probe.virtual_key='A';probe.scan_code=30;probe.pressed=true;
    auto implicit_before=implicit.sent.size();implicit.session->input(probe);check(implicit.sent.size()==implicit_before,"default capability bypassed local toggle");
    implicit.session->control_enabled(true);implicit.session->input(probe);check(implicit.sent.size()==implicit_before+1,"source default did not permit locally enabled input");implicit.session->disconnect();
    Harness denied;denied.connect(1);check(!denied.permission,"prior host denial overridden by init default");
    check(denied.diagnostics.back().find("host-command49-preserved")!=std::string::npos,"explicit denial provenance missing");
    denied.session->control_enabled(true);auto denied_before=denied.sent.size();denied.session->input(probe);check(denied.sent.size()==denied_before,"input bypassed prior host denial");
    denied.receive({Bytes{49},Bytes{0}});check(denied.permission,"explicit host grant not applied");denied.session->input(probe);check(denied.sent.size()==denied_before+1,"explicit grant did not unlock local gate");
    denied.receive({Bytes{49},Bytes{7}});check(!denied.permission,"nonzero host denial ignored");
    denied.receive(denied.initialization);check(!denied.permission,"explicit denial lost during reinitialization");
    denied.session->disconnect();denied.opened.clear();denied.sent.clear();denied.connect();check(denied.permission,"new connection retained previous explicit denial");
    auto reconnected_before=denied.sent.size();denied.session->input(probe);check(denied.sent.size()==reconnected_before,"reconnect retained previous local enablement");
    Harness h;h.connect();
    h.receive(h.drawing());check(h.frame&&h.frame->bgra==Bytes({0,0,255,255}),"first decoded pixel");
    check(h.order==std::vector<std::string>({"frame","ack"}),"ACK precedes commit publication");
    auto before=h.sent.size();InputEvent key;key.kind=InputEvent::Kind::Key;key.virtual_key='A';key.scan_code=30;key.pressed=true;
    h.session->input(key);check(h.sent.size()==before,"input without permissions sent");
    h.receive({Bytes{49},Bytes{0}});check(h.permission,"permission grant missing");h.session->input(key);check(h.sent.size()==before,"input without local enablement sent");
    h.session->control_enabled(true);h.session->input(key);check(h.sent.size()==before+1,"gated key missing");
    h.receive({Bytes{49},Bytes{1}});check(!h.permission,"permission revoke missing");
    auto release=protocol::parse_envelope(h.sent.back().second).payload;check(release.size()==11&&release[4]==2,"held key not released on revoke");
    // A malformed packet from an unrecognized route cannot close the session.
    h.session->receive(Channel::Peer,protocol::forward({"192.0.2.99",99},Bytes{255}));check(h.session->active(),"unknown source altered state");
    before=h.sent.size();h.session->tick(10000);check(h.sent.size()==before+2,"heartbeat routes");
    for(int tick=2;tick<=6;++tick)h.session->tick(tick*10000);
    check(h.sent.back().first==Channel::Command,"sixth heartbeat command missing");
    // Corrupt drawing must not ACK or commit a new frame.
    before=h.order.size();auto bad=h.drawing();bad[4]=i32(-2147483644);h.receive(bad);
    check(!h.session->active()&&h.order.size()==before,"corrupt frame acknowledged");
    Harness wrong;wrong.start();auto correlation=wrong.register_and_lookup();wrong.session->receive(Channel::Command,wrong.lookup_reply(correlation,2));check(!wrong.session->active()&&wrong.status.find("password")!=std::string::npos,"password failure status");
    Harness timeout;timeout.start();timeout.session->tick(20000);check(timeout.status.find("timed out")!=std::string::npos,"setup deadline");
    Harness disconnect;disconnect.connect();disconnect.receive({Bytes{49},Bytes{0}});disconnect.session->control_enabled(true);disconnect.session->input(key);disconnect.session->disconnect();
    check(protocol::parse_envelope(disconnect.sent[disconnect.sent.size()-2].second).payload[4]==2,"disconnect release");
    check(protocol::parse_envelope(disconnect.sent.back().second).payload==protocol::bytes("19"),"disconnect command");
    std::cout<<"Session offline integration tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
