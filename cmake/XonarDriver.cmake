# Compilation d'un pilote noyau Windows (WDM) avec le WDK 10, sans MSBuild ni VSIX.
#
# Localise le WDK (Windows Kits\10), choisit la version de build demandée ou la plus
# récente qui possède des en-têtes km et des bibliothèques km x64, puis fournit :
#   xonar_add_kernel_driver(<cible> SOURCES ... [LIBS ...] [ENTRY <symbole>])
# qui crée un .sys x64 avec les options MSVC d'un projet pilote classique.
#
# À appeler depuis un sous-répertoire dédié : la fonction modifie les drapeaux de
# compilation de ce répertoire (retrait de /EHsc, /GR, /RTC1, runtime C) et ils ne
# doivent pas fuir vers les cibles utilisateur.
#
# Inspiré du modèle public FindWDK (licence MIT), réécrit pour nos besoins.

set(XONAR_WDK_VERSION "" CACHE STRING "Version du WDK à utiliser (ex. 10.0.28000.0) ; vide = la plus récente avec km")

function(_xonar_locate_wdk)
    set(_root "")
    foreach(_candidate "$ENV{WDKContentRoot}" "$ENV{ProgramFiles\(x86\)}/Windows Kits/10" "C:/Program Files (x86)/Windows Kits/10")
        if(_candidate AND EXISTS "${_candidate}/Include")
            set(_root "${_candidate}")
            break()
        endif()
    endforeach()
    if(NOT _root)
        set(XONAR_WDK_ROOT "" PARENT_SCOPE)
        set(XONAR_WDK_BUILD "" PARENT_SCOPE)
        return()
    endif()

    set(_version "${XONAR_WDK_VERSION}")
    if(NOT _version)
        file(GLOB _dirs RELATIVE "${_root}/Include" "${_root}/Include/10.*")
        list(SORT _dirs COMPARE NATURAL ORDER DESCENDING)
        foreach(_d ${_dirs})
            if(EXISTS "${_root}/Include/${_d}/km/ntddk.h" AND EXISTS "${_root}/Lib/${_d}/km/x64/ntoskrnl.lib")
                set(_version "${_d}")
                break()
            endif()
        endforeach()
    endif()
    set(XONAR_WDK_ROOT "${_root}" PARENT_SCOPE)
    set(XONAR_WDK_BUILD "${_version}" PARENT_SCOPE)
endfunction()

_xonar_locate_wdk()

if(XONAR_WDK_ROOT AND XONAR_WDK_BUILD)
    set(XONAR_WDK_FOUND TRUE)
    set(XONAR_WDK_INCLUDE_KM "${XONAR_WDK_ROOT}/Include/${XONAR_WDK_BUILD}/km")
    set(XONAR_WDK_INCLUDE_SHARED "${XONAR_WDK_ROOT}/Include/${XONAR_WDK_BUILD}/shared")
    set(XONAR_WDK_INCLUDE_CRT "${XONAR_WDK_ROOT}/Include/${XONAR_WDK_BUILD}/km/crt")
    set(XONAR_WDK_LIB_KM "${XONAR_WDK_ROOT}/Lib/${XONAR_WDK_BUILD}/km/x64")
    set(XONAR_WDK_BIN "${XONAR_WDK_ROOT}/bin/${XONAR_WDK_BUILD}/x64")
    set(XONAR_WDK_BIN_X86 "${XONAR_WDK_ROOT}/bin/${XONAR_WDK_BUILD}/x86")
    message(STATUS "WDK trouvé : ${XONAR_WDK_ROOT} (build ${XONAR_WDK_BUILD})")
else()
    set(XONAR_WDK_FOUND FALSE)
    message(STATUS "WDK introuvable : le pilote noyau ne sera pas compilé")
endif()

function(xonar_add_kernel_driver target)
    cmake_parse_arguments(ARG "" "ENTRY" "SOURCES;LIBS" ${ARGN})
    if(NOT ARG_ENTRY)
        set(ARG_ENTRY "GsDriverEntry") # fourni par BufferOverflowFastFailK.lib, appelle DriverEntry
    endif()

    # Les drapeaux par défaut de CMake pour MSVC visent le mode utilisateur ; on les
    # retire pour ce répertoire (exceptions, RTTI, contrôles d'exécution, runtime C).
    set(_strip "/EHsc|/GR|/RTC1|/RTCs|/RTCu|/MDd|/MD|/MTd|/MT|/DWIN32|/D_WINDOWS")
    foreach(_var CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL)
        string(REGEX REPLACE "${_strip}" "" _cleaned "${${_var}}")
        set(${_var} "${_cleaned}" PARENT_SCOPE)
    endforeach()
    # Ni bibliothèques Win32 (kernel32, user32...) ni édition de liens incrémentale.
    set(CMAKE_CXX_STANDARD_LIBRARIES "" PARENT_SCOPE)
    foreach(_var CMAKE_EXE_LINKER_FLAGS CMAKE_EXE_LINKER_FLAGS_DEBUG CMAKE_EXE_LINKER_FLAGS_RELEASE CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO)
        string(REGEX REPLACE "/INCREMENTAL(:[A-Z]+)?|/subsystem:console" "" _cleaned "${${_var}}")
        set(${_var} "${_cleaned}" PARENT_SCOPE)
    endforeach()

    add_executable(${target} ${ARG_SOURCES})
    set_target_properties(${target} PROPERTIES
        SUFFIX ".sys"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        MSVC_RUNTIME_LIBRARY ""      # aucun runtime C utilisateur
        CXX_STANDARD 20
        CXX_EXTENSIONS OFF)

    target_include_directories(${target} SYSTEM PRIVATE
        "${XONAR_WDK_INCLUDE_KM}" "${XONAR_WDK_INCLUDE_SHARED}" "${XONAR_WDK_INCLUDE_CRT}")

    target_compile_definitions(${target} PRIVATE
        _WIN64 _AMD64_ AMD64 WINNT=1 # _KERNEL_MODE est défini par /kernel
        POOL_NX_OPTIN=1 DEPRECATE_DDK_FUNCTIONS=1 _WIN32_WINNT=0x0A00 WINVER=0x0A00)

    # Options d'un projet pilote MSBuild : mode noyau, pas de RTTI ni d'exceptions,
    # wchar_t comme le WDK l'attend, symboles de débogage, avertissements élevés.
    target_compile_options(${target} PRIVATE
        /kernel /GR- /EHs-c- /Zc:wchar_t- /W4 /utf-8 /Gy /Gw /Zi /permissive-
        /wd4201 # structures anonymes, omniprésentes dans les en-têtes WDK
        $<$<CONFIG:Debug>:/Od>
        $<$<CONFIG:Release>:/O2>)

    target_link_directories(${target} PRIVATE "${XONAR_WDK_LIB_KM}")
    target_link_options(${target} PRIVATE
        /DRIVER /SUBSYSTEM:NATIVE,10.0 /NODEFAULTLIB /ENTRY:${ARG_ENTRY}
        /MANIFEST:NO # CMake ajoute /MANIFEST aux exécutables, interdit avec /DRIVER
        /INTEGRITYCHECK /DEBUG /OPT:REF /OPT:ICF /RELEASE
        /MERGE:_TEXT=.text /MERGE:_PAGE=PAGE /SECTION:INIT,d /NXCOMPAT /DYNAMICBASE)
    target_link_libraries(${target} PRIVATE
        ntoskrnl.lib hal.lib wmilib.lib BufferOverflowFastFailK.lib ${ARG_LIBS})
endfunction()
