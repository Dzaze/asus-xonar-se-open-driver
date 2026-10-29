add_executable(xonar_dsp_demo main.cpp)
set_target_properties(xonar_dsp_demo PROPERTIES OUTPUT_NAME dsp-demo)
target_link_libraries(xonar_dsp_demo PRIVATE xonar::dsp_core xonar::common)
xonar_apply_warnings(xonar_dsp_demo)
