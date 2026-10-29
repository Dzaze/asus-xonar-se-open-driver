# src/windows-apo : DLL APO Windows (SFX/EFX) enveloppant le moteur xonar::dsp_core.
#
# Couloir activé par -DXONAR_BUILD_APO=ON (OFF par défaut : exige le SDK Windows et
# n'est pas portable). Produit XonarApo.dll, une DLL COM en mode utilisateur chargée
# par le moteur audio (audiodg.exe). L'enregistrement COM et le rattachement à un
# endpoint sont des opérations SÉPARÉES (regsvr32 + clés FxProperties), jamais faites
# par la compilation.
if(NOT WIN32)
    message(FATAL_ERROR "XONAR_BUILD_APO=ON mais la cible n'est pas Windows : l'APO exige le SDK Windows.")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "XONAR_BUILD_APO=ON exige MSVC (en-têtes APO du SDK/WDK : audioenginebaseapo.h).")
endif()

add_library(xonar_apo SHARED
    src/dllmain.cpp
    src/xonar_apo.cpp
    src/apo_settings.cpp
    src/xonar_apo.def
)
add_library(xonar::apo ALIAS xonar_apo)

target_include_directories(xonar_apo PRIVATE src)
# ole32 : CoTaskMemAlloc / StringFromGUID2. advapi32 : API registre (Reg*).
# Les IID (__uuidof) ne demandent aucune bibliothèque d'IID.
target_link_libraries(xonar_apo PRIVATE xonar::dsp_core ole32 advapi32 shell32 uuid)

set_target_properties(xonar_apo PROPERTIES OUTPUT_NAME "XonarApo")

# APO chargé par audiodg : PAS de manifeste embarqué. Un manifeste embarqué fait
# échouer le chargement de l'APO une fois la clé de test retirée (piège documenté,
# cf. scratchpad/apo-attach-mechanism.md / E0-04).
target_link_options(xonar_apo PRIVATE /MANIFEST:NO)

target_compile_features(xonar_apo PRIVATE cxx_std_20)
xonar_apply_warnings(xonar_apo)
