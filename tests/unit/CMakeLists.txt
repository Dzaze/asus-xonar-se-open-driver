add_executable(xonar_unit_tests
    main.cpp
    test_channels.cpp
    test_filters.cpp
    test_pipeline.cpp
    test_live_control.cpp
    test_wav.cpp
)
target_link_libraries(xonar_unit_tests PRIVATE xonar::dsp_core xonar::common)

# Tests du codec de profils v1 (E2-01). La cible xonar::audio_control_profiles
# n'existe que si le couloir est compilé (XONAR_BUILD_AUDIO_CONTROL=ON). On
# n'ajoute la source et le lien que dans ce cas : les autres tests unitaires
# restent compilables seuls quand l'option est OFF.
if(TARGET xonar::audio_control_profiles)
    target_sources(xonar_unit_tests PRIVATE test_profile_codec.cpp)
    target_link_libraries(xonar_unit_tests PRIVATE xonar::audio_control_profiles)
endif()

xonar_apply_warnings(xonar_unit_tests)

add_test(NAME unit.all COMMAND xonar_unit_tests)
set_tests_properties(unit.all PROPERTIES TIMEOUT 120)
