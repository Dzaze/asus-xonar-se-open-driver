# endpoint-probe : outil Windows de diagnostic des endpoints audio (MMDevice, WASAPI,
# topologie KS des jacks). Lecture seule par défaut ; la commande "tone" joue un sinus
# plafonné à -20 dBFS sur un endpoint Xonar uniquement (sauf --any-device).
add_executable(xonar_endpoint_probe
    main.cpp
    guids.cpp
    probe_common.cpp
    cmd_list.cpp
    cmd_tone.cpp
    cmd_watch.cpp
)
set_target_properties(xonar_endpoint_probe PROPERTIES OUTPUT_NAME endpoint-probe)
target_compile_features(xonar_endpoint_probe PRIVATE cxx_std_20)
target_compile_definitions(xonar_endpoint_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(xonar_endpoint_probe PRIVATE ole32 uuid avrt cfgmgr32)
xonar_apply_warnings(xonar_endpoint_probe)
