# audio-control/profiles : codec de profils v1 (E2-01). Bibliothèque portable,
# sans dépendance Windows, consommée plus tard par l'agent de session (E2-02).
# Point d'inclusion à ajouter par E0-05 dans le CMakeLists.txt racine :
#   add_subdirectory(src/audio-control)
add_library(xonar_audio_control_profiles STATIC
    profiles/json_value.cpp
    profiles/profile_codec.cpp
    profiles/migrate_v0.cpp
    profiles/dsp_compiler.cpp
)
add_library(xonar::audio_control_profiles ALIAS xonar_audio_control_profiles)

target_include_directories(xonar_audio_control_profiles PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/profiles>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/contracts/generated/cpp/include>
)
target_link_libraries(xonar_audio_control_profiles PUBLIC xonar::dsp_core)
target_compile_features(xonar_audio_control_profiles PUBLIC cxx_std_20)
xonar_apply_warnings(xonar_audio_control_profiles)
