cmake_minimum_required(VERSION 3.20)
project(next_midisynth LANGUAGES CXX)

option(MIDISYNTH_ENABLE_AVX2 "Build the AVX2 voice-lane kernel" ON)

add_library(midisynth
    src/sample_bank.cpp
    src/midi_file.cpp
    src/soundfont.cpp
    src/synth.cpp
    src/wav_writer.cpp
)
target_include_directories(midisynth PUBLIC include)
target_compile_features(midisynth PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(midisynth PRIVATE /W4 /permissive-)
else()
    target_compile_options(midisynth PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MIDISYNTH_ENABLE_AVX2)
    target_sources(midisynth PRIVATE src/avx2_kernel.cpp)
    target_compile_definitions(midisynth PRIVATE MIDISYNTH_HAS_AVX2=1)
    if(MSVC)
        set_source_files_properties(src/avx2_kernel.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
    else()
        set_source_files_properties(src/avx2_kernel.cpp PROPERTIES COMPILE_OPTIONS "-mavx2")
    endif()
else()
    target_compile_definitions(midisynth PRIVATE MIDISYNTH_HAS_AVX2=0)
endif()

add_executable(midisynth_tests tests/test_main.cpp)
target_link_libraries(midisynth_tests PRIVATE midisynth)

add_executable(midisynth_midi_tests tests/midi_compact_test.cpp)
target_link_libraries(midisynth_midi_tests PRIVATE midisynth)

add_executable(midisynth_wav_tests tests/wav_writer_test.cpp)
target_link_libraries(midisynth_wav_tests PRIVATE midisynth)

add_executable(midisynth_bench bench/benchmark.cpp)
target_link_libraries(midisynth_bench PRIVATE midisynth)

add_executable(midisynth_midi_bench bench/midi_benchmark.cpp)
target_link_libraries(midisynth_midi_bench PRIVATE midisynth)

add_executable(midisynth_render tools/render_midi.cpp)
target_link_libraries(midisynth_render PRIVATE midisynth)

enable_testing()
add_test(NAME midisynth_tests COMMAND midisynth_tests)
add_test(NAME midisynth_midi_tests COMMAND midisynth_midi_tests)
add_test(NAME midisynth_wav_tests COMMAND midisynth_wav_tests)
