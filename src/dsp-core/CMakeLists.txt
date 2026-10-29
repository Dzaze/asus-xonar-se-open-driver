# dsp-core : bibliothèque de traitement audio indépendante du matériel et de Windows.
add_library(xonar_dsp_core STATIC
    src/profile.cpp
    src/pipeline.cpp
)
add_library(xonar::dsp_core ALIAS xonar_dsp_core)

target_include_directories(xonar_dsp_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(xonar_dsp_core PUBLIC cxx_std_20)
xonar_apply_warnings(xonar_dsp_core)
