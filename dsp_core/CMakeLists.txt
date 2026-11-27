cmake_minimum_required(VERSION 3.16)
project(DspCore VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DSPCORE_ENABLE_AVX2 "Enable AVX2/FMA SIMD optimization on x86_64" ON)
option(DSPCORE_BUILD_TESTS "Build DspCore C++ unit test suite" ON)

add_library(DspCore SHARED
    src/uso_source.cpp
    src/fir_resampler.cpp
    src/correlator.cpp
    src/frame_decoder.cpp
    src/dsp_core_api.cpp
)

target_include_directories(DspCore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(DspCore PRIVATE DSPCORE_EXPORTS)


if(MSVC)
    target_compile_options(DspCore PRIVATE /W4)
    # Если очень нужно включить /O2 даже в Debug-сборке, раскомментируйте строку ниже:
    # string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    if(DSPCORE_ENABLE_AVX2)
        target_compile_options(DspCore PRIVATE /arch:AVX2)
    endif()
    target_link_libraries(DspCore PRIVATE advapi32)
else()
    target_compile_options(DspCore PRIVATE -Wall -Wextra -Wpedantic -fvisibility=hidden)
    if(DSPCORE_ENABLE_AVX2 AND (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64"))
        target_compile_options(DspCore PRIVATE -mavx2 -mfma)
    endif()
    if(WIN32)
        target_link_libraries(DspCore PRIVATE advapi32)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(DspCore PRIVATE Threads::Threads)
    endif()
endif()



# Output name: DspCore.dll (Windows) / libDspCore.so (Linux)
set_target_properties(DspCore PROPERTIES
    OUTPUT_NAME "DspCore"
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
)

if(DSPCORE_BUILD_TESTS)
    enable_testing()
    add_executable(test_dsp_core tests/test_dsp_core.cpp)
    target_link_libraries(test_dsp_core PRIVATE DspCore)
    target_include_directories(test_dsp_core PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_test(NAME DspCoreUnitTests COMMAND test_dsp_core)
    target_compile_definitions(test_dsp_core PRIVATE DSP_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_executable(test_v2_receiver tests/test_v2_receiver.cpp)
    target_include_directories(test_v2_receiver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME V2ReceiverTests COMMAND test_v2_receiver)
endif()
