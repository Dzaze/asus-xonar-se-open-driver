# Options de composants — déclare les « couloirs » de build activables séparément.
#
# Objectif E0-05 : rendre compilables séparément le noyau, les tests, les outils et
# les futures branches utilisateur (APO, centre de contrôle), SANS qu'aucune option
# n'entraîne installation, signature ou activation du mode test. Toutes les options
# lourdes (noyau, APO, audio-control) sont OFF par défaut, pour garantir une
# configuration portable sans WDK ni SDK Windows supplémentaire.
#
# Ce module est inclus par le CMakeLists racine APRÈS project().

if(DEFINED XONAR_COMPONENTS_CMAKE_INCLUDED)
    return()
endif()
set(XONAR_COMPONENTS_CMAKE_INCLUDED TRUE)

# --- Composants portables / déjà livrés (activés par défaut) ---------------
option(XONAR_BUILD_TESTS "Compiler les tests (unit dsp-core/common, régression, contrats)" ON)
option(XONAR_BUILD_TOOLS "Compiler les outils en ligne de commande (dsp-demo, usb-descriptor-dump, endpoint-probe)" ON)

# --- Composants optionnels / lourds (OFF par défaut) -----------------------
# Chacun exige une dépendance que la configuration portable ne doit pas réclamer
# (WDK pour le pilote ; SDK Windows/COM pour l'APO ; Windows App SDK/.NET pour l'agent).
option(XONAR_BUILD_DRIVER "Compiler le pilote noyau xonarse.sys (exige le WDK ; jamais requis pour le reste)" OFF)
option(XONAR_BUILD_APO "Compiler l'APO Windows (SFX/EFX) — couloir futur src/windows-apo/" OFF)
option(XONAR_BUILD_AUDIO_CONTROL "Compiler l'agent et le centre de contrôle — couloir futur src/audio-control/ et apps/" OFF)

# --- Transversal -----------------------------------------------------------
option(XONAR_WARNINGS_AS_ERRORS "Traiter les avertissements comme des erreurs" OFF)

# ---------------------------------------------------------------------------
# xonar_register_component(<enabled> <libellé> <chemin>)
#
# Enregistre un composant optionnel : n'ajoute son sous-répertoire que si le
# CMakeLists du composant existe RÉELLEMENT. Si l'option est ON mais le composant
# pas encore livré, on le SIGNALE (avertissement) sans produire de fausse cible ni
# échouer la configuration. C'est le point exact du critère d'acceptation :
# « absence d'un composant non encore livré signalée sans faux succès d'une cible
# demandée ». On n'invente donc aucune cible de substitution, et on ne détourne
# aucun .cpp de bouchon pour simuler une livraison.
# ---------------------------------------------------------------------------
function(xonar_register_component enabled label path)
    if(NOT enabled)
        return()
    endif()
    if(EXISTS "${path}/CMakeLists.txt")
        message(STATUS "Composant « ${label} » : activé, CMakeLists présent -> ajouté")
        add_subdirectory("${path}")
    else()
        message(WARNING
            "Composant « ${label} » demandé (option ON) mais NON ENCORE LIVRÉ : "
            "${path}/CMakeLists.txt est absent. Aucune cible n'est produite. "
            "Ceci n'est PAS un succès de build : livrez le composant, ou laissez "
            "l'option à OFF. (E0-05)")
    endif()
endfunction()
