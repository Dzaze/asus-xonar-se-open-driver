# Enregistrement central des tests (E0-05).
#
# Inclus par le CMakeLists racine uniquement sous if(XONAR_BUILD_TESTS). enable_testing()
# est appelé au niveau racine. AUCUN test ici n'installe, ne signe ni n'active le mode
# test ; tous respectent la règle S0 (hors matériel, sans mutation système).

# Tests unitaires dsp-core / common (déjà présents, CMakeLists propre au dossier).
add_subdirectory(unit)

# Tests de non-régression DSP (golden files) : s'appuient sur l'outil dsp-demo, donc
# seulement si les outils sont compilés. Ne pas dupliquer : enregistrés ici uniquement.
if(XONAR_BUILD_TOOLS)
    add_subdirectory(regression)
else()
    message(STATUS "tests : non-régression DSP ignorée (XONAR_BUILD_TOOLS=OFF, dsp-demo absent)")
endif()

# Tests des contrats v1 (E0-02). Leur CMakeLists est un module LOCAL AUTONOME (il sait
# se configurer seul en projet racine) ; ici il est simplement enregistré en
# sous-répertoire. On ne le réécrit pas.
add_subdirectory(contracts)
