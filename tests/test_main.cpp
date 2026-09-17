#include "midisynth/synth.h"
#include "midisynth/midi_file.h"
#include "midisynth/soundfont.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <new>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::atomic<std::uint64_t> allocationCount{0};
}

void* operator new(std::size_t size) {
    allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void near(float actual, float expected, const std::string& message,
          float tolerance = 1.0e-6F) {
    if (std::fabs(actual - expected) > tolerance) {
        throw std::runtime_error(message + ": got " + std::to_string(actual) +
                                 ", expected " + std::to_string(expected));
    }
}

midisynth::SampleBank basicBank() {
    midisynth::SampleBank bank;
    const std::vector<float> sample{1.0F, 0.5F, -0.5F, -1.0F};
    bank.addOneShot(sample);
    bank.addLoop(sample, 1, 4);
    return bank;
}

midisynth::PreparedSoundFont preparedFixture() {
    midisynth::PreparedSoundFont font;
    const std::vector<float> sample{1.0F, 0.5F, -0.5F, -1.0F};
    auto& samples = font.preparationSamples();
    const auto oneShot = samples.addOneShot(sample);
    const auto loop = samples.addLoop(sample, 1, 4);
    auto& preset = font.addPreset(0, 5, "layers");
    preset.regions.push_back({oneShot, 60, 60, 0, 63, 60, 44100.0F});
    preset.regions.push_back({loop, 60, 72, 64, 127, 60, 44100.0F});
    preset.regions.push_back({oneShot, 60, 72, 64, 127, 72, 44100.0F,
                              100.0F, 0.0F, 0.5F});
    auto& other = font.addPreset(1, 7);
    other.regions.push_back({oneShot, 0, 127, 0, 127, 60, 44100.0F});
    font.finalize();
    return font;
}

void preparedInstrumentSelectionAndPitch() {
    auto font = preparedFixture();
    const auto* preset = font.findPreset(0, 5);
    require(preset != nullptr && font.findPreset(1, 7) != nullptr,
            "bank/program preset lookup failed");
    require(font.matchingRegions(*preset, 60, 40).size() == 1,
            "key/low velocity region selection failed");
    require(font.matchingRegions(*preset, 60, 100).size() == 2,
            "multiple high-velocity regions were not selected");
    require(font.matchingRegions(*preset, 59, 100).empty(),
            "key range admitted an out-of-range note");
    bool immutable = false;
    try { (void)font.preparationSamples(); } catch (const std::logic_error&) { immutable = true; }
    require(immutable, "prepared instrument remained mutable after finalize");
    std::vector<midisynth::Event> events; std::uint32_t sequence = 0;
    midisynth::appendPreparedNoteOn(events, *preset, font, 0, sequence, 2, 60, 40, 44100.0);
    near(events[0].phaseIncrement, 1.0F, "root-key pitch is incorrect");
    events.clear(); sequence = 0;
    midisynth::appendPreparedNoteOn(events, *preset, font, 0, sequence, 2, 72, 100, 44100.0);
    require(events.size() == 2, "layered NoteOn did not launch every match");
    near(events[0].phaseIncrement, 2.0F, "transposed pitch is incorrect", 1.0e-5F);
    near(events[1].phaseIncrement, 1.0F, "alternate root-key pitch is incorrect", 1.0e-5F);
    require(font.sampleBank().descriptor(events[0].sample).looping &&
            !font.sampleBank().descriptor(events[1].sample).looping,
            "one-shot/looping sample selection is incorrect");
}

void velocityCurveAndPanLaw() {
    midisynth::PreparedSoundFont font;
    const std::vector<float> sample{1.0F, 1.0F};
    const auto handle = font.preparationSamples().addLoop(sample, 0, 2);
    auto& preset = font.addPreset(0, 0);
    preset.regions.push_back({handle, 0, 127, 0, 127, 60, 44100.0F});
    font.finalize();
    constexpr std::uint8_t velocities[]{1,16,32,64,96,127};
    float fullLeft = 0.0F;
    for (const auto velocity : velocities) {
        std::vector<midisynth::Event> events;
        std::uint32_t sequence = 0;
        midisynth::appendPreparedNoteOn(events, preset, font, 0, sequence, 0, 60,
                                        velocity, 44100.0);
        const float normalized = static_cast<float>(velocity) / 127.0F;
        const float expected = normalized * normalized * std::sqrt(0.5F);
        near(events[0].leftGain, expected, "quadratic velocity left gain", 2e-7F);
        near(events[0].rightGain, expected, "quadratic velocity right gain", 2e-7F);
        if (velocity == 127) fullLeft = events[0].leftGain;

        midisynth::Synth scalar(font.sampleBank(), {.voiceCapacity=2,.maxBlockFrames=2});
        std::vector<float> left(1), right(1);
        scalar.render(0, events, left, right);
        near(scalar.activeVoices()[0].leftGain, events[0].leftGain,
             "scalar initial velocity gain differs");
        if (midisynth::Synth::avx2Supported()) {
            midisynth::Synth avx(font.sampleBank(), {.voiceCapacity=2,.maxBlockFrames=2,
                .backend=midisynth::Backend::Avx2});
            std::vector<float> al(1), ar(1);
            avx.render(0, events, al, ar);
            require(avx.activeVoices()[0].leftGain == scalar.activeVoices()[0].leftGain &&
                    avx.activeVoices()[0].rightGain == scalar.activeVoices()[0].rightGain,
                    "scalar/AVX2 prepared initial gains differ");
        }
    }
    near(fullLeft, std::sqrt(0.5F), "velocity 127 was not exactly unity before pan");

    const float pans[]{-1.0F, 0.0F, 1.0F, -0.5F, 0.5F};
    for (const float pan : pans) {
        auto mutableFont = midisynth::PreparedSoundFont{};
        const auto sampleId = mutableFont.preparationSamples().addLoop(sample,0,2);
        auto& p = mutableFont.addPreset(0,0);
        midisynth::PreparedRegion region;
        region.sample=sampleId; region.pan=pan;
        p.regions.push_back(region); mutableFont.finalize();
        std::vector<midisynth::Event> events; std::uint32_t sequence=0;
        midisynth::appendPreparedNoteOn(events,p,mutableFont,0,sequence,0,60,127,44100.0);
        const float angle=(pan+1.0F)*std::numbers::pi_v<float>*0.25F;
        near(events[0].leftGain,std::cos(angle),"constant-power pan left",2e-7F);
        near(events[0].rightGain,std::sin(angle),"constant-power pan right",2e-7F);
        near(events[0].leftGain*events[0].leftGain+
             events[0].rightGain*events[0].rightGain,1.0F,"pan power changed",3e-7F);
    }
}

void fullEnvelopeExactBoundaries() {
    midisynth::SampleBank bank;
    const std::vector<float> sample{1.0F,1.0F};
    const auto handle=bank.addLoop(sample,0,2);
    auto on=midisynth::Event::noteOn(0,0,60,handle,1,1,1,2,2);
    on.delayFrames=2; on.holdFrames=2; on.decayFrames=2; on.sustainGain=0.5F;
    const std::vector events{on,midisynth::Event::noteOff(9,1,60)};
    const std::vector<float> expected{0,0,0,0.5F,1,1,1,0.75F,0.5F,0.5F,0.25F,0};
    auto run=[&](midisynth::Backend backend,std::uint32_t workers){
        midisynth::Synth synth(bank,{.voiceCapacity=4,.tileSize=1,.maxBlockFrames=16,
            .workerThreads=workers,.backend=backend,.workerDispatchMinimumFrames=0});
        std::vector<float> left(expected.size()),right(expected.size());
        synth.render(0,events,left,right);
        for(std::size_t i=0;i<expected.size();++i) near(left[i],expected[i],
            "DAHDSR exact boundary frame="+std::to_string(i));
        require(synth.activeVoiceCount()==0,"DAHDSR release did not retire");
        return left;
    };
    const auto scalar=run(midisynth::Backend::Scalar,0);
    require(run(midisynth::Backend::Scalar,2)==scalar,"threaded DAHDSR output differs");
    if(midisynth::Synth::avx2Supported()) {
        const auto avx=run(midisynth::Backend::Avx2,0);
        for(std::size_t i=0;i<scalar.size();++i) near(avx[i],scalar[i],"AVX2 DAHDSR differs",2e-6F);
        require(run(midisynth::Backend::Avx2,2)==avx,"threaded AVX2 DAHDSR differs");
    }
}

void keyScaledEnvelopePreparation() {
    midisynth::PreparedSoundFont font;
    const std::vector<float> sample{1,1};
    const auto handle=font.preparationSamples().addLoop(sample,0,2);
    auto& preset=font.addPreset(0,0);
    midisynth::PreparedRegion region; region.sample=handle; region.holdSeconds=1;
    region.decaySeconds=1; region.sustainGain=0.5F;
    region.keynumToHoldTimecents=100; region.keynumToDecayTimecents=100;
    preset.regions.push_back(region); font.finalize();
    std::vector<midisynth::Event> middle,upper; std::uint32_t sequence=0;
    midisynth::appendPreparedNoteOn(middle,preset,font,0,sequence,0,60,127,44100);
    sequence=0; midisynth::appendPreparedNoteOn(upper,preset,font,0,sequence,0,72,127,44100);
    require(middle[0].holdFrames==44100&&middle[0].decayFrames==44100&&
            upper[0].holdFrames==22050&&upper[0].decayFrames==22050,
            "key-number hold/decay scaling was not prepared per note");
}

void logarithmicSf2Envelope() {
    midisynth::SampleBank bank; const std::vector<float> sample{1,1};
    const auto handle=bank.addLoop(sample,0,2);
    auto event=midisynth::Event::noteOn(0,0,60,handle,1,1,1,0,2);
    event.decayFrames=2; event.sustainGain=0.25F; event.logarithmicEnvelope=true;
    const std::vector events{event,midisynth::Event::noteOff(3,1,60)};
    midisynth::Synth synth(bank,{.voiceCapacity=2,.maxBlockFrames=8});
    std::vector<float> left(6),right(6); synth.render(0,events,left,right);
    const float expected[]{1,0.5F,0.25F,0.25F,0.25F*std::sqrt(1e-5F),0};
    for(std::size_t i=0;i<left.size();++i) near(left[i],expected[i],
        "logarithmic SF2 envelope frame="+std::to_string(i),2e-6F);
}

void channelControlsAndPitchBend() {
    midisynth::SampleBank bank;
    const std::vector<float> sample{1.0F,1.0F,1.0F,1.0F};
    const auto handle=bank.addLoop(sample,0,4);
    const std::vector events{
        midisynth::Event::noteOn(0,0,60,handle),
        midisynth::Event::channelControl(1,1,midisynth::EventType::ChannelVolume,0,64),
        midisynth::Event::channelControl(2,2,midisynth::EventType::ChannelExpression,0,64),
        midisynth::Event::channelControl(3,3,midisynth::EventType::ChannelPan,0,0),
        midisynth::Event::channelControl(4,4,midisynth::EventType::PitchBend,0,16383)};
    midisynth::Synth synth(bank,{.voiceCapacity=2,.maxBlockFrames=8});
    std::vector<float> left(5),right(5); synth.render(0,events,left,right);
    near(left[0],1,"initial channel gain");
    near(left[1],64.0F/127.0F,"channel volume did not affect active voice");
    near(left[2],(64.0F/127.0F)*(64.0F/127.0F),"expression did not affect active voice");
    near(right[3],0,"hard-left channel pan leaked right");
    require(synth.activeVoices()[0].phase > 1.1F && synth.activeVoices()[0].phase < 1.13F,
            "pitch bend did not affect active voice phase increment");
}

void overlappingSustainOwnership() {
    auto font=preparedFixture();
    midisynth::MidiFile midi;
    midi.compactData={0,2,0x30,5,0x10,60,100,
        1,2,0x20,64,127,0x00,60,
        1,1,0x10,60,100,
        1,1,0x20,64,0,
        1,1,0x00,60};
    midi.lastFrame=4;
    const auto performance=midisynth::preparePerformance(midi,font,44100.0);
    std::vector<const midisynth::Event*> ons,offs;
    for(const auto& event:performance.events) {
        if(event.type==midisynth::EventType::NoteOn) ons.push_back(&event);
        if(event.type==midisynth::EventType::NoteOff) offs.push_back(&event);
    }
    require(ons.size()==4&&offs.size()==2,"overlapping sustain event expansion differs");
    require(offs[0]->frame==3&&offs[0]->noteInstance==ons[0]->noteInstance,
            "pedal-up did not release only the deferred key instance");
    require(offs[1]->frame==4&&offs[1]->noteInstance==ons[2]->noteInstance,
            "newly re-pressed note was released by pedal-up");
}

void compareState(const midisynth::Synth& actual, const midisynth::Synth& expected,
                  std::uint64_t seed, const std::string& label);

void sameFrameProgramChangeAndPreparedParity() {
    auto font = preparedFixture();
    midisynth::MidiFile midi;
    midi.compactData = {0, 2, 0x30, 5, 0x10, 60, 100,
                        4, 1, 0x00, 60};
    midi.lastFrame = 4;
    const auto performance = midisynth::preparePerformance(midi, font, 44100.0);
    require(performance.events.size() == 3 &&
            performance.events[0].type == midisynth::EventType::NoteOn &&
            performance.events[1].type == midisynth::EventType::NoteOn,
            "same-frame program change did not select layered preset");
    midisynth::Synth scalar(font.sampleBank(), {.voiceCapacity=8,.tileSize=3,.maxBlockFrames=16});
    midisynth::SynthConfig threadedScalarConfig{.voiceCapacity=8,.tileSize=1,
        .maxBlockFrames=16,.workerThreads=2};
    threadedScalarConfig.workerDispatchMinimumFrames=0;
    midisynth::Synth threadedScalar(font.sampleBank(),threadedScalarConfig);
    std::vector<float> sl(10), sr(10), tsl(10), tsr(10);
    scalar.render(0, performance.events, sl, sr);
    threadedScalar.render(0,performance.events,tsl,tsr);
    require(sl==tsl&&sr==tsr,"SF2-prepared threaded scalar output differs");
    compareState(threadedScalar,scalar,0,"SF2 prepared threaded scalar");
    if (midisynth::Synth::avx2Supported()) {
        midisynth::Synth avx(font.sampleBank(), {.voiceCapacity=8,.tileSize=3,.maxBlockFrames=16,.backend=midisynth::Backend::Avx2});
        midisynth::Synth threaded(font.sampleBank(), {.voiceCapacity=8,.tileSize=3,.maxBlockFrames=16,.workerThreads=2,.backend=midisynth::Backend::Avx2});
        std::vector<float> al(10), ar(10), tl(10), tr(10);
        avx.render(0, performance.events, al, ar); threaded.render(0, performance.events, tl, tr);
        require(al == tl && ar == tr, "SF2-prepared threaded AVX2 output differs");
        for (std::size_t i=0;i<sl.size();++i) { near(al[i],sl[i],"SF2-prepared AVX2 left differs",2e-6F); near(ar[i],sr[i],"SF2-prepared AVX2 right differs",2e-6F); }
        compareState(avx, scalar, 0, "SF2 prepared AVX2");
    }
}

void put16(std::vector<unsigned char>& b, std::uint16_t v) { b.push_back(v&255); b.push_back(v>>8); }
void put32(std::vector<unsigned char>& b, std::uint32_t v) { put16(b,v&65535); put16(b,v>>16); }
void four(std::vector<unsigned char>& b, const char* s) { b.insert(b.end(),s,s+4); }
void fixedName(std::vector<unsigned char>& b, const char* s) { std::size_t n=0; for(;s[n]&&n<20;++n)b.push_back(s[n]); while(n++<20)b.push_back(0); }
void chunk(std::vector<unsigned char>& out,const char* id,const std::vector<unsigned char>& data){four(out,id);put32(out,static_cast<std::uint32_t>(data.size()));out.insert(out.end(),data.begin(),data.end());if(data.size()&1)out.push_back(0);}
void list(std::vector<unsigned char>& out,const char* type,const std::vector<unsigned char>& children){std::vector<unsigned char>d;four(d,type);d.insert(d.end(),children.begin(),children.end());chunk(out,"LIST",d);}

void syntheticSf2Parsing() {
    std::vector<unsigned char> phdr;
    auto preset=[&](const char* n,std::uint16_t program,std::uint16_t bank,std::uint16_t bag){fixedName(phdr,n);put16(phdr,program);put16(phdr,bank);put16(phdr,bag);put32(phdr,0);put32(phdr,0);put32(phdr,0);};
    preset("Test",5,0,0);preset("EOP",0,0,1);
    std::vector<unsigned char> pbag;put16(pbag,0);put16(pbag,0);put16(pbag,3);put16(pbag,0);
    std::vector<unsigned char> pgen;put16(pgen,52);put16(pgen,10);put16(pgen,34);put16(pgen,1200);put16(pgen,41);put16(pgen,0);
    std::vector<unsigned char> inst;fixedName(inst,"Instrument");put16(inst,0);fixedName(inst,"EOI");put16(inst,1);
    std::vector<unsigned char> ibag;put16(ibag,0);put16(ibag,0);put16(ibag,15);put16(ibag,0);
    std::vector<unsigned char> igen;
    auto gen=[&](std::uint16_t op,std::int16_t amount){put16(igen,op);put16(igen,static_cast<std::uint16_t>(amount));};
    gen(33,-12000);gen(34,-12000);gen(35,-12000);gen(36,0);gen(37,120);
    gen(38,-12000);gen(39,100);gen(40,100);gen(18,0);gen(19,0);gen(5,25);
    gen(48,60);gen(52,20);gen(56,50);gen(53,0);
    std::vector<unsigned char> shdr;
    auto sample=[&](const char* n,std::uint32_t start,std::uint32_t end,std::uint16_t type){fixedName(shdr,n);put32(shdr,start);put32(shdr,end);put32(shdr,start+1);put32(shdr,end-1);put32(shdr,44100);shdr.push_back(60);shdr.push_back(0);put16(shdr,0);put16(shdr,type);};
    sample("Sample",0,4,1);sample("EOS",4,4,1);
    std::vector<unsigned char> pdta;chunk(pdta,"phdr",phdr);chunk(pdta,"pbag",pbag);chunk(pdta,"pgen",pgen);chunk(pdta,"inst",inst);chunk(pdta,"ibag",ibag);chunk(pdta,"igen",igen);chunk(pdta,"shdr",shdr);
    std::vector<unsigned char> smpl;for(auto v:{32767,16384,-16384,-32768})put16(smpl,static_cast<std::uint16_t>(static_cast<std::int16_t>(v)));
    std::vector<unsigned char> sdta;chunk(sdta,"smpl",smpl);std::vector<unsigned char> body;four(body,"sfbk");list(body,"sdta",sdta);list(body,"pdta",pdta);
    std::vector<unsigned char> file;four(file,"RIFF");put32(file,static_cast<std::uint32_t>(body.size()));file.insert(file.end(),body.begin(),body.end());
    const std::string path="midisynth_synthetic_test.sf2";{std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(file.data()),static_cast<std::streamsize>(file.size()));}
    const auto font=midisynth::loadSoundFont(path);std::remove(path.c_str());
    const auto* p=font.findPreset(0,5);require(p&&p->regions.size()==1,"synthetic SF2 preset/region parse failed");
    require(!font.sampleBank().descriptor(p->regions[0].sample).looping,"synthetic one-shot sample parsed as loop");
    const auto& region=p->regions[0];
    near(region.attackSeconds,std::exp2(-10800.0F/1200.0F),
         "preset-relative/instrument-absolute envelope hierarchy",1e-7F);
    near(region.decaySeconds,0.12F,"SF2 decay/sustain duration conversion",1e-6F);
    near(region.sustainGain,std::pow(10.0F,-120.0F/200.0F),
         "SF2 sustain centibels conversion",1e-6F);
    near(region.gain,std::pow(10.0F,-60.0F/200.0F),
         "SF2 initial attenuation conversion",1e-6F);
    near(region.tuningCents,30.0F,"preset/instrument fine tuning hierarchy");
    near(region.scaleTuning,50.0F,"SF2 scale tuning preparation");
    require(region.keynumToHoldTimecents==100&&region.keynumToDecayTimecents==100,
            "key-scaled volume-envelope generators were not prepared");
    std::string warnings;
    for(const auto& warning:font.warnings()) warnings+=warning;
    require(warnings.find("modLfoToPitch (5)")!=std::string::npos,
            "unsupported audible generator warning was not named");
    require(warnings.find("unused2") == std::string::npos &&
            warnings.find("unused3") == std::string::npos,
            "reserved generators were reported as compatibility failures");
}

void syntheticMidiParsing() {
    std::vector<unsigned char> track{0x00,0xc0,0x05,
        0x00,0xff,0x51,0x03,0x0f,0x42,0x40,
        0x00,0x90,60,100,
        0x83,0x60,0x80,60,0, 0x00,0xff,0x2f,0x00};
    std::vector<unsigned char> file; four(file,"MThd");
    file.push_back(0);file.push_back(0);file.push_back(0);file.push_back(6);
    file.push_back(0);file.push_back(0);file.push_back(0);file.push_back(1);
    file.push_back(1);file.push_back(0xe0); four(file,"MTrk");
    file.push_back(0);file.push_back(0);file.push_back(0);file.push_back(static_cast<unsigned char>(track.size()));
    file.insert(file.end(),track.begin(),track.end());
    const std::string path="midisynth_synthetic_test.mid";{std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(file.data()),static_cast<std::streamsize>(file.size()));}
    const auto midi=midisynth::loadMidiFile(path,44100.0);std::remove(path.c_str());
    auto reader=midi.reader();midisynth::MidiMessage messages[3];
    require(reader.next(messages[0])&&reader.next(messages[1])&&reader.next(messages[2])&&
            !reader.next(messages[0]),"synthetic SMF message parsing failed");
    require(messages[0].type==midisynth::MidiMessageType::ProgramChange &&
            messages[1].type==midisynth::MidiMessageType::NoteOn &&
            messages[2].type==midisynth::MidiMessageType::NoteOff,
            "synthetic SMF stable event order failed");
    require(messages[2].frame==44100,"SMF tick/tempo to sample-frame conversion failed");
}

void exactNoteOnAndPhase() {
    auto bank = basicBank();
    midisynth::Synth synth(bank, {.voiceCapacity = 8, .maxBlockFrames = 16});
    const std::vector events{midisynth::Event::noteOn(3, 0, 60, 0, 0.5F)};
    std::vector<float> left(7), right(7);
    synth.render(0, events, left, right);
    near(left[2], 0.0F, "note-on leaked before its frame");
    near(left[3], 1.0F, "note-on did not sound on its frame");
    near(left[4], 0.75F, "linear interpolation is incorrect");
    const auto voices = synth.activeVoices();
    require(voices.size() == 1, "voice retired too early");
    near(voices[0].phase, 2.0F, "phase did not advance once per rendered frame");
}

void exactNoteOffAndReleaseRetirement() {
    auto bank = basicBank();
    midisynth::Synth synth(bank, {.voiceCapacity = 8, .maxBlockFrames = 16});
    const std::vector events{
        midisynth::Event::noteOn(0, 0, 60, 1, 1.0F, 1.0F, 1.0F, 0, 2),
        midisynth::Event::noteOff(2, 1, 60)};
    std::vector<float> left(6), right(6);
    synth.render(0, events, left, right);
    near(left[0], 1.0F, "sustain frame 0");
    near(left[1], 0.5F, "sustain frame 1");
    near(left[2], -0.5F, "release must begin at exact event frame");
    near(left[3], -0.5F, "second release sample has half gain");
    near(left[4], 0.0F, "released voice played too long");
    require(synth.activeVoiceCount() == 0, "release did not retire voice");
}

void stableSameFrameOrder() {
    auto bank = basicBank();
    std::vector<float> left(2), right(2);
    {
        midisynth::Synth synth(bank, {.voiceCapacity = 8, .maxBlockFrames = 8});
        const std::vector events{
            midisynth::Event::noteOn(0, 1, 60, 1),
            midisynth::Event::noteOff(0, 2, 60)};
        synth.render(0, events, left, right);
        near(left[0], 0.0F, "note-on then immediate note-off order was lost");
    }
    {
        midisynth::Synth synth(bank, {.voiceCapacity = 8, .maxBlockFrames = 8});
        const std::vector events{
            midisynth::Event::noteOff(0, 1, 60),
            midisynth::Event::noteOn(0, 2, 60, 1)};
        synth.render(0, events, left, right);
        near(left[0], 1.0F, "note-off then note-on order was lost");
    }
}

void channelLocalNoteOff() {
    auto bank = basicBank();
    midisynth::Synth synth(bank,{.voiceCapacity=4,.maxBlockFrames=8});
    const std::vector events{midisynth::Event::noteOn(0,0,60,1,1,1,1,0,0,0),
        midisynth::Event::noteOn(0,1,60,1,1,1,1,0,0,1),
        midisynth::Event::noteOff(1,2,60,0)};
    std::vector<float> left(3),right(3);synth.render(0,events,left,right);
    near(left[0],2.0F,"same note on separate channels did not layer");
    near(left[1],0.5F,"NoteOff crossed MIDI channel boundary");
    const auto voices=synth.activeVoices();require(voices.size()==1&&voices[0].channel==1,
        "channel-local NoteOff retired the wrong voice");
}

void loopingBehavior() {
    auto bank = basicBank();
    midisynth::Synth synth(bank, {.voiceCapacity = 4, .maxBlockFrames = 16});
    const std::vector events{midisynth::Event::noteOn(0, 0, 1, 1)};
    std::vector<float> left(7), right(7);
    synth.render(0, events, left, right);
    const std::vector<float> expected{1.0F, 0.5F, -0.5F, -1.0F, 0.5F, -0.5F, -1.0F};
    for (std::size_t i = 0; i < expected.size(); ++i) near(left[i], expected[i], "loop output");
    require(synth.activeVoiceCount() == 1, "looping voice retired");
}

void oneShotRetirement() {
    auto bank = basicBank();
    midisynth::Synth synth(bank, {.voiceCapacity = 4, .maxBlockFrames = 16});
    const std::vector events{midisynth::Event::noteOn(0, 0, 1, 0)};
    std::vector<float> left(6), right(6);
    synth.render(0, events, left, right);
    near(left[3], -1.0F, "last one-shot frame missing");
    near(left[4], 0.0F, "one-shot exceeded sample end");
    require(synth.activeVoiceCount() == 0, "one-shot did not retire");
}

void deterministicOutput() {
    auto bank = basicBank();
    const std::vector events{
        midisynth::Event::noteOn(0, 0, 60, 1, 0.75F, 0.7F, 0.2F, 3, 4),
        midisynth::Event::noteOn(1, 1, 61, 0, 0.5F, 0.1F, 0.9F),
        midisynth::Event::noteOff(5, 2, 60)};
    std::vector<float> l1(12), r1(12), l2(12), r2(12);
    midisynth::Synth a(bank, {.voiceCapacity = 8, .maxBlockFrames = 16});
    midisynth::Synth b(bank, {.voiceCapacity = 8, .maxBlockFrames = 16});
    a.render(0, events, l1, r1);
    b.render(0, events, l2, r2);
    require(l1 == l2 && r1 == r2, "scalar output is not deterministic");
}

void sameFrameRetirementFreesCapacityInSequence() {
    auto bank = basicBank();
    midisynth::Synth synth(bank, {.voiceCapacity = 1, .maxBlockFrames = 8});
    const std::vector events{
        midisynth::Event::noteOn(0, 0, 60, 1),
        midisynth::Event::noteOff(1, 1, 60),
        midisynth::Event::noteOn(1, 2, 61, 1)};
    std::vector<float> left(3), right(3);
    synth.render(0, events, left, right);
    near(left[1], 1.0F, "same-frame later note-on did not reuse retired capacity");
    require(synth.stats().droppedNoteOns == 0, "same-frame slot reuse dropped a note");
    const auto voices = synth.activeVoices();
    require(voices.size() == 1 && voices[0].note == 61,
            "same-frame event sequence produced the wrong logical voice");
}

void scalarAvx2Differential() {
    if (!midisynth::Synth::avx2Supported()) {
        std::cout << "AVX2 unavailable; differential test skipped\n";
        return;
    }
    auto bank = basicBank();
    std::vector<midisynth::Event> events;
    std::uint32_t sequence = 0;
    for (std::uint32_t i = 0; i < 19; ++i) {
        events.push_back(midisynth::Event::noteOn(i % 3, sequence++,
            static_cast<std::uint8_t>(40 + i), i % 2, 0.25F + i * 0.03125F,
            0.01F * (i + 1), 0.02F * (20 - i), i % 5, 3 + i % 7));
    }
    events.push_back(midisynth::Event::noteOff(8, sequence++, 45));
    events.push_back(midisynth::Event::noteOff(9, sequence++, 48));
    events.push_back(midisynth::Event::noteOn(11, sequence++, 90, 1, 1.125F));
    std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
        return a.frame < b.frame || (a.frame == b.frame && a.sequence < b.sequence);
    });

    midisynth::Synth scalar(bank, {.voiceCapacity = 32, .tileSize = 11,
        .maxBlockFrames = 32, .backend = midisynth::Backend::Scalar});
    midisynth::Synth avx2(bank, {.voiceCapacity = 32, .tileSize = 11,
        .maxBlockFrames = 32, .backend = midisynth::Backend::Avx2});
    std::vector<float> sl(24), sr(24), vl(24), vr(24);
    scalar.render(0, events, sl, sr);
    avx2.render(0, events, vl, vr);
    for (std::size_t i = 0; i < sl.size(); ++i) {
        near(vl[i], sl[i], "AVX2 left output differs", 2.0e-6F);
        near(vr[i], sr[i], "AVX2 right output differs", 2.0e-6F);
    }
    const auto ss = scalar.activeVoices();
    const auto vs = avx2.activeVoices();
    require(ss.size() == vs.size(), "AVX2 active voice count differs");
    for (std::size_t i = 0; i < ss.size(); ++i) {
        require(ss[i].slot == vs[i].slot && ss[i].note == vs[i].note &&
                ss[i].channel == vs[i].channel &&
                ss[i].envelopeStage == vs[i].envelopeStage &&
                ss[i].looping == vs[i].looping,
                "AVX2 lifecycle state differs");
        near(vs[i].phase, ss[i].phase, "AVX2 phase differs");
        near(vs[i].envelopeGain, ss[i].envelopeGain, "AVX2 envelope differs");
        near(vs[i].envelopeStep, ss[i].envelopeStep, "AVX2 envelope step differs");
    }
}

void threadedDifferential(midisynth::Backend backend) {
    if (backend == midisynth::Backend::Avx2 && !midisynth::Synth::avx2Supported()) return;
    auto bank = basicBank();
    std::vector<midisynth::Event> events;
    std::uint32_t sequence = 0;
    for (std::uint32_t i = 0; i < 600; ++i) {
        events.push_back(midisynth::Event::noteOn(0, sequence++,
            static_cast<std::uint8_t>(i % 100), 1, 0.2F + (i % 11) * 0.03F,
            0.001F * (i % 7), 0.001F * (i % 9), i % 4, 5));
    }
    for (std::uint32_t note = 0; note < 50; ++note) {
        events.push_back(midisynth::Event::noteOff(13, sequence++,
            static_cast<std::uint8_t>(note)));
    }
    midisynth::Synth single(bank, {.voiceCapacity = 700, .tileSize = 64,
        .maxBlockFrames = 64, .workerThreads = 0, .backend = backend});
    midisynth::Synth threaded(bank, {.voiceCapacity = 700, .tileSize = 64,
        .maxBlockFrames = 64, .workerThreads = 3, .backend = backend});
    std::vector<float> al(40), ar(40), bl(40), br(40);
    single.render(0, events, al, ar);
    threaded.render(0, events, bl, br);
    require(al == bl && ar == br, "thread scheduling changed deterministic output");
    const auto as = single.activeVoices();
    const auto bs = threaded.activeVoices();
    require(as.size() == bs.size(), "threading changed active voice count");
    for (std::size_t i = 0; i < as.size(); ++i) {
        require(as[i].slot == bs[i].slot && as[i].note == bs[i].note &&
                as[i].channel == bs[i].channel &&
                as[i].envelopeStage == bs[i].envelopeStage,
                "threading changed lifecycle state");
        near(as[i].phase, bs[i].phase, "threading changed phase");
        near(as[i].envelopeGain, bs[i].envelopeGain, "threading changed envelope");
    }
}

void renderDoesNotAllocate() {
    auto bank = basicBank();
    midisynth::Synth synth(bank, {.voiceCapacity = 64, .tileSize = 11,
        .maxBlockFrames = 64, .workerThreads = 3,
        .backend = midisynth::Backend::Scalar});
    std::vector<midisynth::Event> events;
    events.reserve(64);
    for (std::uint32_t i = 0; i < 48; ++i) {
        events.push_back(midisynth::Event::noteOn(0, i,
            static_cast<std::uint8_t>(i % 16), i % 2, 0.5F + (i % 5) * 0.25F,
            0.1F, 0.2F, i % 3, 4));
    }
    std::vector<float> left(64), right(64);
    const auto before = allocationCount.load(std::memory_order_acquire);
    synth.render(0, events, left, right);
    const auto after = allocationCount.load(std::memory_order_acquire);
    require(after == before, "Synth::render performed a heap allocation");
}

struct Random {
    std::uint64_t state;

    std::uint32_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<std::uint32_t>(state >> 16);
    }

    std::uint32_t below(std::uint32_t limit) { return next() % limit; }
};

std::vector<midisynth::Event> generatedEvents(std::uint64_t seed) {
    Random random{seed};
    std::vector<midisynth::Event> events;
    events.reserve(500);
    std::uint32_t sequence = 0;
    for (std::uint32_t frame = 0; frame < 96; ++frame) {
        const auto eventCount = frame == 0 ? 72U : random.below(7);
        for (std::uint32_t i = 0; i < eventCount; ++i) {
            const auto note = static_cast<std::uint8_t>(random.below(24));
            if (frame != 0 && random.below(100) < 42) {
                events.push_back(midisynth::Event::noteOff(frame, sequence++, note));
            } else {
                const float increments[] = {0.25F, 0.5F, 1.0F, 1.75F, 4.0F, 17.25F};
                auto event = midisynth::Event::noteOn(frame, sequence++, note,
                    random.below(2), increments[random.below(6)],
                    0.01F * static_cast<float>(1 + random.below(20)),
                    0.01F * static_cast<float>(1 + random.below(20)),
                    random.below(5), random.below(7));
                event.delayFrames = random.below(4);
                event.holdFrames = random.below(4);
                event.decayFrames = random.below(5);
                event.sustainGain = random.below(5) * 0.2F;
                event.logarithmicEnvelope = random.below(2) != 0;
                events.push_back(event);
            }
        }
        if (frame % 13 == 7) {
            const auto note = static_cast<std::uint8_t>(random.below(24));
            events.push_back(midisynth::Event::noteOff(frame, sequence++, note));
            events.push_back(midisynth::Event::noteOff(frame, sequence++, note));
            events.push_back(midisynth::Event::noteOn(frame, sequence++, note,
                random.below(2), 1.75F, 0.15F, 0.25F, 0, random.below(7)));
        }
        if (frame % 17 == 3) events.push_back(midisynth::Event::channelControl(
            frame,sequence++,midisynth::EventType::ChannelVolume,0,random.below(128)));
        if (frame % 19 == 5) events.push_back(midisynth::Event::channelControl(
            frame,sequence++,midisynth::EventType::ChannelPan,0,random.below(128)));
        if (frame % 23 == 11) events.push_back(midisynth::Event::channelControl(
            frame,sequence++,midisynth::EventType::PitchBend,0,random.below(16384)));
    }
    return events;
}

void compareState(const midisynth::Synth& actual, const midisynth::Synth& expected,
                  std::uint64_t seed, const std::string& label) {
    const auto a = actual.activeVoices();
    const auto e = expected.activeVoices();
    const auto context = label + " seed=" + std::to_string(seed);
    require(a.size() == e.size(), context + " active count differs");
    require(actual.stats().droppedNoteOns == expected.stats().droppedNoteOns,
            context + " dropped NoteOn count differs");
    for (std::size_t i = 0; i < a.size(); ++i) {
        require(a[i].slot == e[i].slot && a[i].note == e[i].note &&
                a[i].channel == e[i].channel &&
                a[i].noteInstance == e[i].noteInstance &&
                a[i].logarithmicEnvelope == e[i].logarithmicEnvelope &&
                a[i].envelopeStage == e[i].envelopeStage &&
                a[i].envelopeFramesRemaining == e[i].envelopeFramesRemaining &&
                a[i].looping == e[i].looping,
                context + " active identity or lifecycle differs");
        near(a[i].phase, e[i].phase, context + " phase differs", 2.0e-6F);
        near(a[i].phaseIncrement, e[i].phaseIncrement,
             context + " phase increment differs", 2.0e-6F);
        near(a[i].envelopeGain, e[i].envelopeGain,
             context + " envelope differs", 2.0e-6F);
        near(a[i].envelopeStep, e[i].envelopeStep,
             context + " envelope step differs", 2.0e-6F);
        near(a[i].sustainGain, e[i].sustainGain,
             context + " sustain gain differs", 2.0e-6F);
        near(a[i].leftGain, e[i].leftGain, context + " left gain differs", 2.0e-6F);
        near(a[i].rightGain, e[i].rightGain, context + " right gain differs", 2.0e-6F);
    }
}

void generatedDifferential() {
    auto bank = basicBank();
    for (const std::uint64_t seed : {0x12345678ULL, 0x9e3779b9ULL, 0xc001d00dULL,
                                    0x5eed5eedULL, 0xabcdef01ULL, 0x31415926ULL,
                                    0x27182818ULL, 0xf00dcafeULL}) {
        const auto events = generatedEvents(seed);
        try {
        const midisynth::SynthConfig scalarConfig{.voiceCapacity = 48, .tileSize = 11,
            .maxBlockFrames = 96, .workerThreads = 0,
            .backend = midisynth::Backend::Scalar};
        auto threadedScalarConfig = scalarConfig;
        threadedScalarConfig.workerThreads = 3;
        midisynth::Synth scalar(bank, scalarConfig);
        midisynth::Synth threadedScalar(bank, threadedScalarConfig);
        std::vector<float> sl(96), sr(96), tl(96), tr(96);
        scalar.render(0, events, sl, sr);
        threadedScalar.render(0, events, tl, tr);
        require(sl == tl && sr == tr,
                "threaded scalar output differs seed=" + std::to_string(seed));
        compareState(threadedScalar, scalar, seed, "threaded scalar");

        if (!midisynth::Synth::avx2Supported()) continue;
        auto avxConfig = scalarConfig;
        avxConfig.backend = midisynth::Backend::Avx2;
        auto threadedAvxConfig = avxConfig;
        threadedAvxConfig.workerThreads = 3;
        midisynth::Synth avx(bank, avxConfig);
        midisynth::Synth threadedAvx(bank, threadedAvxConfig);
        std::vector<float> vl(96), vr(96), vtl(96), vtr(96);
        avx.render(0, events, vl, vr);
        threadedAvx.render(0, events, vtl, vtr);
        require(vl == vtl && vr == vtr,
                "threaded AVX2 output differs seed=" + std::to_string(seed));
        for (std::size_t i = 0; i < sl.size(); ++i) {
            near(vl[i], sl[i], "generated AVX2 left differs seed=" +
                 std::to_string(seed) + " frame=" + std::to_string(i), 2.0e-6F);
            near(vr[i], sr[i], "generated AVX2 right differs seed=" +
                 std::to_string(seed) + " frame=" + std::to_string(i), 2.0e-6F);
        }
        compareState(avx, scalar, seed, "AVX2");
        compareState(threadedAvx, avx, seed, "threaded AVX2");
        if (seed == 0x12345678ULL) {
            auto referenceAvxConfig = avxConfig;
            referenceAvxConfig.enableAvx2FastAdvance = false;
            referenceAvxConfig.enableAvx2ContiguousLoads = false;
            midisynth::Synth referenceAvx(bank, referenceAvxConfig);
            std::vector<float> rl(96), rr(96);
            referenceAvx.render(0, events, rl, rr);
            require(vl == rl && vr == rr,
                    "AVX2 optimization switches changed output seed=" +
                    std::to_string(seed));
            compareState(referenceAvx, avx, seed, "AVX2 switches disabled");
        }
        } catch (...) {
            std::cerr << "Generated differential event stream for seed=" << seed << '\n';
            for (const auto& event : events) {
                std::cerr << "  frame=" << event.frame << " sequence=" << event.sequence
                          << " type=" << (event.type == midisynth::EventType::NoteOn ? "on" : "off")
                          << " note=" << static_cast<unsigned>(event.note);
                if (event.type == midisynth::EventType::NoteOn) {
                    std::cerr << " sample=" << event.sample
                              << " increment=" << event.phaseIncrement
                              << " attack=" << event.attackFrames
                              << " release=" << event.releaseFrames;
                }
                std::cerr << '\n';
            }
            throw;
        }
    }
}

} // namespace

int main() {
    try {
        preparedInstrumentSelectionAndPitch();
        velocityCurveAndPanLaw();
        fullEnvelopeExactBoundaries();
        keyScaledEnvelopePreparation();
        logarithmicSf2Envelope();
        channelControlsAndPitchBend();
        overlappingSustainOwnership();
        sameFrameProgramChangeAndPreparedParity();
        syntheticSf2Parsing();
        syntheticMidiParsing();
        exactNoteOnAndPhase();
        exactNoteOffAndReleaseRetirement();
        stableSameFrameOrder();
        channelLocalNoteOff();
        loopingBehavior();
        oneShotRetirement();
        deterministicOutput();
        sameFrameRetirementFreesCapacityInSequence();
        scalarAvx2Differential();
        threadedDifferential(midisynth::Backend::Scalar);
        threadedDifferential(midisynth::Backend::Avx2);
        renderDoesNotAllocate();
        generatedDifferential();
        std::cout << "All scalar tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TEST FAILURE: " << error.what() << '\n';
        return 1;
    }
}
