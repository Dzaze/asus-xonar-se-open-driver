# dsp-live-render : prototype « notre application fait le rendu » (architecture B).
# Rend l'audio traité par xonar::dsp sur un endpoint WASAPI, sans APO. Construit en
# standalone (compile directement les sources dsp-core) pour rester indépendant du
# couloir APO/driver sur lequel travaille Codex.
cmake_minimum_required(VERSION 3.20)
project(dsp_live_render LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "Outil Windows (WASAPI) : MSVC requis.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DSP "${CMAKE_CURRENT_SOURCE_DIR}/../../src/dsp-core")
set(COMMON "${CMAKE_CURRENT_SOURCE_DIR}/../../src/common")

add_executable(dsp-live-render
    main.cpp
    "${DSP}/src/profile.cpp"
    "${DSP}/src/pipeline.cpp"
    "${COMMON}/src/wav.cpp"
)
target_include_directories(dsp-live-render PRIVATE "${DSP}/include" "${COMMON}/include")
target_compile_options(dsp-live-render PRIVATE /W4 /permissive-)
# ole32 : COM/CoTaskMem. avrt : MMCSS (Pro Audio). mmdevapi n'est pas requis (__uuidof).
target_link_libraries(dsp-live-render PRIVATE ole32 avrt)
