# usb-descriptor-dump : outil Windows de lecture seule des descripteurs USB (style USBView).
add_executable(xonar_usb_descriptor_dump main.cpp)
set_target_properties(xonar_usb_descriptor_dump PROPERTIES OUTPUT_NAME usb-descriptor-dump)
target_compile_features(xonar_usb_descriptor_dump PRIVATE cxx_std_20)
target_compile_definitions(xonar_usb_descriptor_dump PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(xonar_usb_descriptor_dump PRIVATE setupapi hid)
xonar_apply_warnings(xonar_usb_descriptor_dump)
