#include "ui_mailbox.hpp"
#include <condition_variable>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace lunaviewer;
void check(bool c,const char* message){if(!c)throw std::runtime_error(message);}
int main(){try {
    UiMailbox mailbox;
    auto first=mailbox.begin_generation();
    auto update=mailbox.take();check(!update.accepted,"startup must retain connecting UI state");
    auto frame=std::make_shared<Frame>();
    mailbox.accepted(first,true);mailbox.frame(first,frame);mailbox.accepted(first,false);
    update=mailbox.take();check(update.accepted&&!*update.accepted&&!update.frame,"queued frame survived disconnect");
    mailbox.frame(first,frame);check(!mailbox.take().frame,"late frame survived disconnected gate");
    // Deliberately delay an old callback until after the new generation begins.
    std::mutex mutex;std::condition_variable cv;bool waiting=false,resume=false;
    std::thread old([&]{
        {std::unique_lock<std::mutex> lock(mutex);waiting=true;cv.notify_one();cv.wait(lock,[&]{return resume;});}
        mailbox.accepted(first,true);mailbox.permission(first,true);mailbox.status(first,"stale");mailbox.frame(first,frame);
    });
    {std::unique_lock<std::mutex> lock(mutex);cv.wait(lock,[&]{return waiting;});}
    auto second=mailbox.begin_generation();
    {std::lock_guard<std::mutex> lock(mutex);resume=true;}cv.notify_one();old.join();
    update=mailbox.take();check(!update.accepted&&!update.permission&&!update.status&&!update.frame,"old generation repopulated mailbox");
    mailbox.accepted(second,true);mailbox.frame(second,frame);update=mailbox.take();check(update.connected&&update.frame==frame,"current frame missing");
    InputEvent move;move.kind=InputEvent::Kind::Move;
    for(int i=0;i<5000;++i){move.x=i;mailbox.input(move);}
    auto inputs=mailbox.take_input();check(inputs.size()==1&&inputs[0].second.x==4999,"moves not coalesced");
    InputEvent key;key.kind=InputEvent::Kind::Key;
    for(int i=0;i<257;++i)mailbox.input(key);
    inputs=mailbox.take_input();check(inputs.size()==1&&inputs[0].second.kind==InputEvent::Kind::ReleaseAll,"input overflow did not release held state");
    mailbox.input(key);mailbox.begin_generation();check(mailbox.take_input().empty(),"old input survived reconnect");
    auto third=mailbox.generation();mailbox.accepted(third,true);mailbox.frame(third,frame);
    mailbox.cancel();mailbox.accepted(third,true);mailbox.permission(third,true);mailbox.frame(third,frame);
    update=mailbox.take();check(!update.connected&&update.accepted&&!*update.accepted&&!update.frame&&update.permission&&!*update.permission,"local cancel repopulated accepted/frame state");
    mailbox.input(key);inputs=mailbox.take_input();check(inputs.size()==1&&inputs[0].second.kind==InputEvent::Kind::ReleaseAll,"local cancel retained pending input");
    check(mailbox.generation()==third,"local cancellation invalidated graceful release generation");
    auto fourth=mailbox.begin_generation();check(!mailbox.canceled(),"reconnect retained cancellation");mailbox.accepted(fourth,true);check(mailbox.take().connected,"reconnect after local cancel failed");
    std::cout<<"UI mailbox race/lifecycle tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
