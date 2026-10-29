cmake_minimum_required(VERSION 3.25)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Version : SOURCE UNIQUE (doit précéder project()).
# cf. cmake/XonarVersion.cmake — tout consommateur lit XONAR_VERSION[_FULL].
# ---------------------------------------------------------------------------
include(XonarVersion)

project(XonarSeOpenDriver
    VERSION ${XONAR_VERSION}
    DESCRIPTION "Pilote ouvert, moteur DSP et centre de controle pour ASUS Xonar SE sous Windows"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Options de composants (couloirs activables séparément).
# cf. cmake/XonarComponents.cmake — toutes les options lourdes sont OFF par défaut
# pour que la configuration reste portable (sans WDK) tant que XONAR_BUILD_DRIVER=OFF.
# ---------------------------------------------------------------------------
include(XonarComponents)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(Warnings)

# ---------------------------------------------------------------------------
# Composants portables (aucune dependance Windows)
# ---------------------------------------------------------------------------
add_subdirectory(src/common)
add_subdirectory(src/dsp-core)

# ---------------------------------------------------------------------------
# Outils
# ---------------------------------------------------------------------------
if(XONAR_BUILD_TOOLS)
    add_subdirectory(tools/dsp-demo)
    if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/usb-descriptor-dump/CMakeLists.txt")
        add_subdirectory(tools/usb-descriptor-dump)
    endif()
    if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/endpoint-probe/CMakeLists.txt")
        add_subdirectory(tools/endpoint-probe)
    endif()
endif()

# ---------------------------------------------------------------------------
# Pilote noyau (optionnel, isole du reste). OFF par défaut : jamais requis pour
# configurer/compiler les composants portables, les tests ou les outils.
# ---------------------------------------------------------------------------
if(XONAR_BUILD_DRIVER AND WIN32)
    add_subdirectory(src/driver)
elseif(XONAR_BUILD_DRIVER AND NOT WIN32)
    message(WARNING "XONAR_BUILD_DRIVER=ON ignoré hors Windows : le pilote noyau n'est pas compilable ici.")
endif()

# ---------------------------------------------------------------------------
# Couloirs utilisateur futurs (APO, audio-control) : l'option existe déjà mais ne
# produit AUCUNE cible tant que le composant n'est pas livré. Une option activée sur
# un composant absent est signalée sans faux succès (cf. xonar_register_component).
# ---------------------------------------------------------------------------
xonar_register_component(${XONAR_BUILD_APO}
    "APO Windows (SFX/EFX)" "${CMAKE_CURRENT_SOURCE_DIR}/src/windows-apo")
xonar_register_component(${XONAR_BUILD_AUDIO_CONTROL}
    "Agent et centre de controle" "${CMAKE_CURRENT_SOURCE_DIR}/src/audio-control")

# ---------------------------------------------------------------------------
# Tests (enregistrement central dans tests/CMakeLists.txt).
# enable_testing() au niveau racine pour que ctest fonctionne depuis la racine du build.
# ---------------------------------------------------------------------------
if(XONAR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
