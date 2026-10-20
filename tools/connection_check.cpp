#include "session.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <iostream>
#include <csignal>
using namespace lunaviewer;
int main(int argc,char** argv) {
    std::signal(SIGPIPE,SIG_IGN);QCoreApplication app(argc,argv);
    if(argc!=3&&argc!=4){std::cerr<<"Usage: connection_check private-profile.json dictionary-path; credentials JSON via stdin; optional --observe-permission waits 2 seconds without input\n";return 2;}
    try {
        QFile file(argv[1]);if(!file.open(QIODevice::ReadOnly)||file.size()>65536)throw std::runtime_error("Cannot read profile");
        auto object=QJsonDocument::fromJson(file.readAll()).object();SessionConfig config;
        for(auto f:object["registration_fields"].toArray())config.registration.push_back(f.toString().toStdString());
        config.dictionary_path=argv[2];
        config.command.connect_address=object.value("command_address").toString("functions3.ultraviewer.net").toStdString();
        config.command.verify_name=object.value("command_hostname").toString("functions3.ultraviewer.net").toStdString();
        config.command.http_host=config.command.verify_name;
        config.command.sni=object.value("sni").toString("ultraviewer").toStdString();
        config.command.port=object.value("command_port").toInt(443);
        config.command.ca_file=object.value("ca_file").toString().toStdString();
        std::string line;std::getline(std::cin,line);
        auto credentials=QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        Credentials peer{credentials["target_id"].toString().toStdString(),credentials["password"].toString().toStdString()};
        line.clear();credentials=QJsonObject{};
        TlsTransport command,relay;QElapsedTimer clock;clock.start();
        std::unique_ptr<Session> session;int result=1;bool stopping=false;const bool observe=argc==4&&std::string(argv[3])=="--observe-permission";
        auto stop=[&]{if(stopping)return;stopping=true;QTimer::singleShot(0,&app,[&]{session->disconnect();QTimer::singleShot(550,&app,&QCoreApplication::quit);});};
        SessionCallbacks c;
        c.open=[&](Channel ch,const TlsConfig& tls){
            auto& transport=ch==Channel::Command?command:relay;
            std::cout<<(ch==Channel::Command?"Command service":"Peer relay")<<": opening TLS to "<<tls.connect_address<<":"<<tls.port<<", SNI="<<tls.sni<<", verify="<<tls.verify_name<<std::endl;
            transport.ready=[&,ch]{std::cout<<(ch==Channel::Command?"Command service":"Peer relay")<<": TLS ready"<<std::endl;session->ready(ch);};
            transport.frame=[&,ch](Bytes body){session->receive(ch,body);};
            transport.error=[&,ch](std::string error){std::cerr<<(ch==Channel::Command?"Command service":"Peer relay")<<": "<<error<<std::endl;QTimer::singleShot(0,&app,[&,error]{session->transport_error(error);stop();});};
            transport.start(tls);
        };
        c.send=[&](Channel ch,const Bytes& body){(ch==Channel::Command?command:relay).send(body);};
        c.close=[&]{command.finish();relay.finish();};
        c.diagnostic=[](const std::string& message){std::cout<<"Session diagnostic: "<<message<<std::endl;};
        c.status=[&](const std::string& status){std::cout<<"Session: "<<status<<std::endl;};
        c.permission=[](bool allowed){std::cout<<"Permission state received: "<<(allowed?"allowed":"unavailable/blocked")<<"; local control remains disabled"<<std::endl;};
        c.accepted=[&](bool accepted){if(!accepted){stop();return;}if(accepted){result=0;std::cout<<"Peer initialization accepted; local control remains disabled"<<std::endl;if(observe)QTimer::singleShot(2000,&app,stop);else stop();}};
        c.frame=[](FrameSnapshot){std::cout<<"Screen frame received; no control sent"<<std::endl;};
        session=std::make_unique<Session>(config,std::move(c));
        QTimer tick;QObject::connect(&tick,&QTimer::timeout,&app,[&]{session->tick(clock.elapsed());});tick.start(100);
        QTimer::singleShot(65000,&app,[&]{std::cerr<<"Check deadline reached"<<std::endl;stop();});
        session->start(std::move(peer),clock.elapsed());
        app.exec();command.close();relay.close();
        std::cout<<"Check ended. No input/control events were sent."<<std::endl;return result;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
