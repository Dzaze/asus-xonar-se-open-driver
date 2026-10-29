// Instancie une seule fois les GUID déclarés par portcls.h et ksmedia.h
// (CLSID_PortTopology, CLSID_PortWaveRT, IID_IMiniport*, KSNODETYPE_*, etc.).
// Les autres unités de compilation n'en voient que les déclarations.
#include <initguid.h>
#include "common.h"
