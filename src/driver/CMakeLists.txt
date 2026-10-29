# Pilote noyau xonarse.sys : construit seulement si XONAR_BUILD_DRIVER=ON et WDK présent.
# Le build de dsp-core, des outils et des tests ne dépend jamais de ce dossier.
#
# Organisation en modules indépendants, chacun dans son sous-dossier :
#   xonarse/        adaptateur, contrats partagés (common.h, hw_xonarse.h, usb/usb_device.h)
#   xonarse/usb/    couche USB (XonarUsbDevice)
#   xonarse/topology/  miniport Topology
#   xonarse/wave/   miniport WaveRT et moteur isochrone
#   xonarse/stubs/  bouchons utilisés quand un module n'a pas de .cpp ou est forcé en bouchon
include(XonarDriver)

if(NOT XONAR_WDK_FOUND)
    message(WARNING "XONAR_BUILD_DRIVER=ON mais aucun WDK trouvé : cible xonarse ignorée")
    return()
endif()

option(XONAR_DRIVER_STUB_USB "Remplacer le module usb/ par son bouchon" OFF)
option(XONAR_DRIVER_STUB_TOPOLOGY "Remplacer le module topology/ par son bouchon" OFF)
option(XONAR_DRIVER_STUB_WAVE "Remplacer le module wave/ par son bouchon" OFF)

set(_dir "${CMAKE_CURRENT_SOURCE_DIR}/xonarse")
set(XONARSE_SOURCES
    ${_dir}/driver.cpp
    ${_dir}/adapter.cpp
    ${_dir}/new_delete.cpp
    ${_dir}/guids.cpp
    ${_dir}/trace.cpp
    ${_dir}/common.h
    ${_dir}/hw_xonarse.h
    ${_dir}/xonarse.h
    ${_dir}/trace.h
    ${_dir}/usb/usb_device.h
)

function(_xonar_module name stub_file force_stub)
    file(GLOB _cpp CONFIGURE_DEPENDS "${_dir}/${name}/*.cpp")
    file(GLOB _h CONFIGURE_DEPENDS "${_dir}/${name}/*.h")
    if(force_stub OR NOT _cpp)
        message(STATUS "xonarse : module ${name} -> bouchon")
        set(XONARSE_SOURCES ${XONARSE_SOURCES} "${_dir}/stubs/${stub_file}" ${_h} PARENT_SCOPE)
    else()
        message(STATUS "xonarse : module ${name} -> implémentation réelle")
        set(XONARSE_SOURCES ${XONARSE_SOURCES} ${_cpp} ${_h} PARENT_SCOPE)
    endif()
endfunction()

_xonar_module(usb usb_stub.cpp ${XONAR_DRIVER_STUB_USB})
_xonar_module(topology topology_stub.cpp ${XONAR_DRIVER_STUB_TOPOLOGY})
_xonar_module(wave wave_stub.cpp ${XONAR_DRIVER_STUB_WAVE})

xonar_add_kernel_driver(xonarse
    SOURCES ${XONARSE_SOURCES}
    LIBS portcls.lib stdunk.lib ksguid.lib libcntpr.lib usbd.lib usbdex.lib
)
target_include_directories(xonarse PRIVATE "${_dir}")

# INF : stampinf renseigne DriverVer à partir du .inx ; le paquet est assemblé dans
# build/<preset>/driver-package/ (xonarse.sys, xonarse.inf). Le .cat et la signature de
# test sont produits par scripts/driver/sign-test.ps1, jamais ici.
set(XONAR_DRIVER_VERSION "0.2.0.0" CACHE STRING "Version stampée dans l'INF")
set(XONAR_DRIVER_PACKAGE_DIR "${CMAKE_BINARY_DIR}/driver-package" CACHE PATH "Dossier du paquet pilote")
set(_stampinf "${XONAR_WDK_BIN}/stampinf.exe")
# stampinf n'accepte que des chemins Windows natifs (barres obliques inverses).
file(TO_NATIVE_PATH "${XONAR_DRIVER_PACKAGE_DIR}/xonarse.inf" _inf_native)
add_custom_command(
    OUTPUT "${XONAR_DRIVER_PACKAGE_DIR}/xonarse.inf"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${XONAR_DRIVER_PACKAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "${_dir}/xonarse.inx" "${XONAR_DRIVER_PACKAGE_DIR}/xonarse.inf"
    COMMAND "${_stampinf}" -f "${_inf_native}" -d "*" -v "${XONAR_DRIVER_VERSION}" -a amd64
    DEPENDS "${_dir}/xonarse.inx"
    COMMENT "stampinf : génération de xonarse.inf"
    VERBATIM
)
add_custom_target(xonarse_package ALL
    DEPENDS xonarse "${XONAR_DRIVER_PACKAGE_DIR}/xonarse.inf"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:xonarse>" "${XONAR_DRIVER_PACKAGE_DIR}/xonarse.sys"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_PDB_FILE:xonarse>" "${XONAR_DRIVER_PACKAGE_DIR}/xonarse.pdb"
    COMMENT "Assemblage du paquet pilote (non signé) dans ${XONAR_DRIVER_PACKAGE_DIR}"
)
