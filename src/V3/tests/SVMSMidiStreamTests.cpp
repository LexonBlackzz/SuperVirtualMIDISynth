#include "SVMSMidiStream.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

namespace {
bool Collect(const svms::PackedMidiEvent& e, void* user) {
    static_cast<std::vector<svms::PackedMidiEvent>*>(user)->push_back(e);
    return true;
}
bool WriteFixture(const wchar_t* path) {
    // Format 1, 480 PPQN. Track 0 establishes 120 BPM then 240 BPM at tick
    // 480. Track 1 uses running status for the second note-on.
    static const unsigned char bytes[] = {
        'M','T','h','d',0,0,0,6,0,1,0,2,1,0xe0,
        'M','T','r','k',0,0,0,19,
        0,0xff,0x51,3,7,0xa1,0x20,
        0x83,0x60,0xff,0x51,3,3,0xd0,0x90,
        0,0xff,0x2f,0,
        'M','T','r','k',0,0,0,17,
        0,0x90,60,100,
        0x83,0x60,64,100,
        0x83,0x60,0x80,60,0,
        0,0xff,0x2f,0
    };
    FILE* f = _wfopen(path,L"wb"); if(!f)return false;
    const bool ok=fwrite(bytes,1,sizeof(bytes),f)==sizeof(bytes);fclose(f);return ok;
}
}
int wmain() {
    wchar_t dir[MAX_PATH],path[MAX_PATH];GetTempPathW(MAX_PATH,dir);GetTempFileNameW(dir,L"svm",0,path);
    if(!WriteFixture(path))return 1;
    svms::MappedMidiFile file;std::string error;if(!file.Open(path,error)){DeleteFileW(path);return 2;}
    svms::MidiStreamDecoder decoder;svms::MidiStreamInfo info{};
    if(!decoder.Scan(file,48000,info,error)){DeleteFileW(path);return 3;}
    if(info.eventCount!=3||info.noteOnCount!=2||info.totalFrames!=36000||info.format!=1||info.tracks!=2)return 4;
    std::vector<svms::PackedMidiEvent> events;std::atomic<bool> cancel{false};
    if(!decoder.Decode(file,48000,Collect,&events,&cancel,nullptr,error))return 5;
    DeleteFileW(path);
    if(events.size()!=3)return 6;
    if(events[0].outputFrame!=0||events[1].outputFrame!=24000||events[2].outputFrame!=36000)return 7;
    if(events[0].message!=0x00643c90||events[1].message!=0x00644090||events[2].message!=0x00003c80)return 8;
    svms::ParsedEventRing ring(1);if(!ring.IsValid()||ring.Capacity()!=65536)return 9;
    for(const auto& e:events)if(!ring.Push(e,cancel))return 10;
    svms::PackedMidiEvent e{};for(const auto& expected:events){if(!ring.Pop(e)||e.sequence!=expected.sequence)return 11;}
    return ring.Size()==0?0:12;
}
