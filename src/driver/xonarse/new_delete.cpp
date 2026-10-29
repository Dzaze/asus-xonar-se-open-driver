// Opérateurs new/delete du pilote (voir common.h). ExAllocatePool2 rend une mémoire
// mise à zéro ; un échec renvoie nullptr (pas d'exception en mode noyau).
#include "common.h"

PVOID __cdecl operator new(size_t size, POOL_FLAGS flags, ULONG tag)
{
    return ExAllocatePool2(flags, size ? size : 1, tag);
}

PVOID __cdecl operator new[](size_t size, POOL_FLAGS flags, ULONG tag)
{
    return ExAllocatePool2(flags, size ? size : 1, tag);
}

// operator delete(void*) n'est PAS défini ici : stdunk.lib (implémentation de CUnknown)
// le fournit déjà, avec ExFreePool, compatible avec ExAllocatePool2. Le redéfinir
// provoque LNK2005, comme l'a relevé l'intégration du module Topology.

void __cdecl operator delete(PVOID p, size_t)
{
    if (p != nullptr) ExFreePool(p);
}

void __cdecl operator delete[](PVOID p)
{
    if (p != nullptr) ExFreePool(p);
}

void __cdecl operator delete[](PVOID p, size_t)
{
    if (p != nullptr) ExFreePool(p);
}
