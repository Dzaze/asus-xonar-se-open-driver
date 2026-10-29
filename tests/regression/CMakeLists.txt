# Tests de non-régression : dsp-demo produit des fichiers que l'on compare aux
# références ("golden files") versionnées dans tests/dsp-golden-files/.
# Pour régénérer une référence après un changement VOULU du DSP :
#   cmake --build <build> --target regression-regenerate
find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
    message(STATUS "Python 3 absent : tests de non-régression désactivés")
    return()
endif()

set(GOLDEN_DIR "${CMAKE_SOURCE_DIR}/tests/dsp-golden-files")
set(WORK_DIR "${CMAKE_CURRENT_BINARY_DIR}/work")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(COMPARE "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/compare_wav.py")

# Scénario 1 : signal stéréo synthétique (FL 1 kHz à -20 dBFS, 0,1 s) puis upmix 5.1 side.
add_test(NAME regression.generate_stereo
    COMMAND xonar_dsp_demo --mode test --output-mask 0x3 --test-channel FL --test-freq 1000
            --test-level -20 --duration 0.1 --out "${WORK_DIR}/stereo_fl_1k.wav")
add_test(NAME regression.upmix
    COMMAND xonar_dsp_demo --mode upmix --in "${WORK_DIR}/stereo_fl_1k.wav"
            --out "${WORK_DIR}/upmix_51_side.wav" --format pcm24)
add_test(NAME regression.upmix.compare
    COMMAND ${COMPARE} "${GOLDEN_DIR}/upmix_51_side.wav" "${WORK_DIR}/upmix_51_side.wav" --tolerance 2e-6)

# Scénario 2 : downmix 5.1 -> 2.0 du résultat précédent.
add_test(NAME regression.downmix
    COMMAND xonar_dsp_demo --mode downmix --in "${WORK_DIR}/upmix_51_side.wav"
            --out "${WORK_DIR}/downmix_20.wav" --format pcm24)
add_test(NAME regression.downmix.compare
    COMMAND ${COMPARE} "${GOLDEN_DIR}/downmix_20.wav" "${WORK_DIR}/downmix_20.wav" --tolerance 2e-6)

# Scénario 3 : bass management 80 Hz + gain +12 dB sur un LFE 40 Hz -> limiteur sollicité.
add_test(NAME regression.generate_lfe
    COMMAND xonar_dsp_demo --mode test --output-mask 0x60F --test-channel LFE --test-freq 40
            --test-level -6 --duration 0.1 --out "${WORK_DIR}/lfe_40.wav")
add_test(NAME regression.bass_limiter
    COMMAND xonar_dsp_demo --mode passthrough --in "${WORK_DIR}/lfe_40.wav"
            --out "${WORK_DIR}/bass_limiter.wav" --bass 80 --master-db 12 --format pcm24)
add_test(NAME regression.bass_limiter.compare
    COMMAND ${COMPARE} "${GOLDEN_DIR}/bass_limiter.wav" "${WORK_DIR}/bass_limiter.wav" --tolerance 2e-6)

# Scénario 4 : 2.1 virtuel depuis la stéréo (caisson passe-bas 80 Hz).
add_test(NAME regression.two_point_one
    COMMAND xonar_dsp_demo --mode 2.1 --in "${WORK_DIR}/stereo_fl_1k.wav"
            --out "${WORK_DIR}/two_point_one.wav" --format pcm24)
add_test(NAME regression.two_point_one.compare
    COMMAND ${COMPARE} "${GOLDEN_DIR}/two_point_one.wav" "${WORK_DIR}/two_point_one.wav" --tolerance 2e-6)

set_tests_properties(regression.upmix PROPERTIES DEPENDS regression.generate_stereo)
set_tests_properties(regression.two_point_one PROPERTIES DEPENDS regression.generate_stereo)
set_tests_properties(regression.two_point_one.compare PROPERTIES DEPENDS regression.two_point_one)
set_tests_properties(regression.upmix.compare PROPERTIES DEPENDS regression.upmix)
set_tests_properties(regression.downmix PROPERTIES DEPENDS regression.upmix)
set_tests_properties(regression.downmix.compare PROPERTIES DEPENDS regression.downmix)
set_tests_properties(regression.bass_limiter PROPERTIES DEPENDS regression.generate_lfe)
set_tests_properties(regression.bass_limiter.compare PROPERTIES DEPENDS regression.bass_limiter)

# Cible utilitaire : recopie les sorties courantes comme nouvelles références.
add_custom_target(regression-regenerate
    COMMAND ${CMAKE_COMMAND} -E copy "${WORK_DIR}/upmix_51_side.wav" "${GOLDEN_DIR}/upmix_51_side.wav"
    COMMAND ${CMAKE_COMMAND} -E copy "${WORK_DIR}/downmix_20.wav" "${GOLDEN_DIR}/downmix_20.wav"
    COMMAND ${CMAKE_COMMAND} -E copy "${WORK_DIR}/bass_limiter.wav" "${GOLDEN_DIR}/bass_limiter.wav"
    COMMAND ${CMAKE_COMMAND} -E copy "${WORK_DIR}/two_point_one.wav" "${GOLDEN_DIR}/two_point_one.wav"
    COMMENT "Remplacement des fichiers golden par les sorties courantes (lancer ctest avant)."
)
