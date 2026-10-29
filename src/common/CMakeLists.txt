# common : utilitaires partagés sans dépendance Windows (WAV, plus tard logs et erreurs).
add_library(xonar_common STATIC
    src/wav.cpp
)
add_library(xonar::common ALIAS xonar_common)

target_include_directories(xonar_common PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(xonar_common PUBLIC cxx_std_20)
xonar_apply_warnings(xonar_common)

# ---------------------------------------------------------------------------
# Point d'ancrage « en-têtes communs » (cible INTERFACE) pour les futurs couloirs
# (APO, audio-control). Aucune source : la cible expose seulement les en-têtes
# partagés de src/common/include. Les couloirs à venir pourront lier
# xonar::headers pour partager les contrats d'en-tête sans dépendre du .lib
# compilé xonar::common. E0-05 fournit l'ancrage ; les sources restent à créer
# dans leurs propres dossiers, jamais hors de src/common/ depuis ici.
# ---------------------------------------------------------------------------
add_library(xonar_headers INTERFACE)
add_library(xonar::headers ALIAS xonar_headers)
target_include_directories(xonar_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(xonar_headers INTERFACE cxx_std_20)
