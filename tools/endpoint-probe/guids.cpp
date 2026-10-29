// Définition unique des PROPERTYKEY / DEVPROPKEY utilisés par l'outil.
// Avec <initguid.h> inclus en premier, DEFINE_PROPERTYKEY et DEFINE_DEVPROPKEY émettent
// des définitions (DECLSPEC_SELECTANY) au lieu de simples déclarations externes, ce qui
// évite d'avoir à lier des bibliothèques supplémentaires pour PKEY_AudioEndpoint_* etc.
#include <initguid.h>

#include "probe_common.hpp"
