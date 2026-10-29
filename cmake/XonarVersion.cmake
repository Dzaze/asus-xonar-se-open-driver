# Manifeste de version — SOURCE UNIQUE de la version du projet Xonar SE Open Driver.
#
# Toute version (projet « umbrella », pilote noyau, APO, centre de contrôle, paquets)
# DOIT dériver des quatre composantes définies ici. On ne redéfinit JAMAIS un numéro
# de version ailleurs : les consommateurs lisent les variables/la fonction exposées
# ci-dessous. Ce module est inclus par le CMakeLists racine AVANT project().
#
# -- Version alignée sur le binaire réellement livré --
#   Le défaut CMake historique du pilote était 0.2.0.0, mais le paquet validé sur
#   matériel réel le 28/10/2026 est la 0.3.3.0 (oem82.inf, identique au build
#   driver-integration ; cf. docs/evidence/baseline-2026-10-28.md, E0-01). La source
#   unique prend donc 0.3.3.0 : ainsi, quand src/driver/ consommera ce manifeste à la
#   place de son défaut, la version stampée restera >= à celle déjà installée et
#   Windows acceptera la mise à jour. Toute version future n'augmente qu'ici.
#   La reproductibilité binaire exacte (recompiler et retrouver oem82.inf) reste « à
#   confirmer » (E0-01) ; ce manifeste fixe le numéro, pas la preuve d'empreinte.

if(DEFINED XONAR_VERSION_CMAKE_INCLUDED)
    return()
endif()
set(XONAR_VERSION_CMAKE_INCLUDED TRUE)

set(XONAR_VERSION_MAJOR 0)
set(XONAR_VERSION_MINOR 3)
set(XONAR_VERSION_PATCH 3)
set(XONAR_VERSION_TWEAK 0)

# Forme sémantique à trois composantes, pour project(... VERSION ...).
set(XONAR_VERSION
    "${XONAR_VERSION_MAJOR}.${XONAR_VERSION_MINOR}.${XONAR_VERSION_PATCH}")

# Forme à quatre composantes exigée par stampinf/l'INF et les ressources Windows
# (p. ex. le futur XONAR_DRIVER_VERSION du pilote).
set(XONAR_VERSION_FULL
    "${XONAR_VERSION_MAJOR}.${XONAR_VERSION_MINOR}.${XONAR_VERSION_PATCH}.${XONAR_VERSION_TWEAK}")

# Accès par fonction pour les sous-répertoires qui préfèrent ne pas dépendre de la
# portée de variable (équivalent à lire directement ${XONAR_VERSION_FULL}).
function(xonar_driver_version out_var)
    set(${out_var} "${XONAR_VERSION_FULL}" PARENT_SCOPE)
endfunction()

message(STATUS "Version Xonar (source unique) : ${XONAR_VERSION_FULL}")
