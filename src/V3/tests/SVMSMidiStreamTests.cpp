#include "SVMSMidiStream.h"
#include "SVMSEventCompile.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <codecvt>
#include <locale>
#include <unistd.h>
#endif
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
        'M','T','r','k',0,0,0,42,
        0,0xf0,8,0x43,0x10,0x4c,0x00,0x00,0x06,0x58,0xf7,
        0,0x90,60,100,
        0,60,100,
        0,0xb0,7,127,
        0,0x90,60,100,
        0,60,100,
        0x83,0x60,64,100,
        0x83,0x60,0x80,60,0,
        0,0xff,0x2f,0
    };
#if defined(_WIN32)
    FILE* f = _wfopen(path,L"wb");
#else
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    const std::string utf8 = converter.to_bytes(path);
    FILE* f = fopen(utf8.c_str(), "wb");
#endif
    if(!f)return false;
    const bool ok=fwrite(bytes,1,sizeof(bytes),f)==sizeof(bytes);fclose(f);return ok;
}
}
#if defined(_WIN32)
int wmain() {
    wchar_t dir[MAX_PATH],path[MAX_PATH];GetTempPathW(MAX_PATH,dir);GetTempFileNameW(dir,L"svm",0,path);
#else
int main() {
    char temporary[] = "/tmp/svms-midi-stream-XXXXXX";
    const int descriptor = mkstemp(temporary);
    if (descriptor < 0) return 1;
    close(descriptor);
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    const std::wstring widePath = converter.from_bytes(temporary);
    const wchar_t* path = widePath.c_str();
#endif
    if(!WriteFixture(path))return 1;
    svms::MappedMidiFile file;std::string error;if(!file.Open(path,error)){
#if defined(_WIN32)
        DeleteFileW(path);
#else
        unlink(temporary);
#endif
        return 2;
    }
    svms::MidiStreamDecoder decoder;svms::MidiStreamInfo info{};
    if(!decoder.Scan(file,48000,info,error)){
#if defined(_WIN32)
        DeleteFileW(path);
#else
        unlink(temporary);
#endif
        return 3;
    }
    if(info.eventCount!=8||info.noteOnCount!=5||info.totalFrames!=36000||info.format!=1||info.tracks!=2)return 4;
    if(info.peakEventsPerSecond!=8||info.peakNoteOnsPerSecond!=5||
       info.peakEventsAtFrame!=6||info.peakNoteOnsAtFrame!=4||
       info.exactDuplicateNoteOnCount!=3||info.keyDuplicateNoteOnCount!=3||
       info.peakExactDuplicateNoteOnsAtFrame!=3||
       info.peakKeyDuplicateNoteOnsAtFrame!=3||
       info.noteRunExactDuplicateCount!=2||
       info.peakNoteRunExactDuplicatesAtFrame!=2||
       info.adjacentExactDuplicateNoteOnCount!=2||info.noteOnFrameCount!=2)return 13;
    std::vector<svms::PackedMidiEvent> events;std::atomic<bool> cancel{false};
    if(!decoder.Decode(file,48000,Collect,&events,&cancel,nullptr,error))return 5;
#if defined(_WIN32)
    DeleteFileW(path);
#else
    unlink(temporary);
#endif
    if(events.size()!=8)return 6;
    if(events[0].outputFrame!=0||events[1].outputFrame!=0||
       events[2].outputFrame!=0||events[3].outputFrame!=0||
       events[4].outputFrame!=0||events[5].outputFrame!=0||
       events[6].outputFrame!=24000||events[7].outputFrame!=36000)return 7;
    if(events[0].message!=svms::MakeInternalMasterTransposeMessage(0x58)||
       events[1].message!=0x00643c90||events[2].message!=0x00643c90||
       events[3].message!=0x007f07b0||events[4].message!=0x00643c90||
       events[5].message!=0x00643c90||events[6].message!=0x00644090||
       events[7].message!=0x00003c80)return 8;
    svms::ParsedEventRing ring(1);if(!ring.IsValid()||ring.Capacity()!=65536)return 9;
    for(const auto& e:events)if(!ring.Push(e,cancel))return 10;
    svms::PackedMidiEvent e{};for(const auto& expected:events){if(!ring.Pop(e)||e.sequence!=expected.sequence)return 11;}
    return ring.Size()==0?0:12;
}
