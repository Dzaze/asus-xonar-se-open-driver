# packaging

Installation et distribution des composants Windows.

## État actuel

**Vide.** Rien n'est installable à ce stade.

## Ce qui est prévu

1. **Prototype (jalon M3)** : scripts PowerShell d'enregistrement et de désenregistrement de l'APO par le registre, limités aux endpoints Xonar, idempotents, avec sauvegarde préalable des clés. Ils vivront dans `scripts/`, pas ici.
2. **Distribution (après M3)** : INF d'extension attachée à `USB\VID_0B05&PID_189D&MI_00`, qui complète le paquet Microsoft `usbaudio2.inf` sans le remplacer, avec son catalogue signé par attestation (Partner Center, certificat EV). Voir [docs/build-and-signing.md](../../docs/build-and-signing.md) §4.2.
3. **Centre de contrôle** : paquet MSIX ou installateur classique, décidé au jalon M3.

## Interdits

- Aucun paquet qui remplace `usbaudio2.sys`.
- Aucun fichier ASUS ou C-Media.
- Aucune installation sans procédure de retour arrière testée.
