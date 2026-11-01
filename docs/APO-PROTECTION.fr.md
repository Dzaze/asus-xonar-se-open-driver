# Protection des associations DSP

Les effets Xonar sont déclarés dans l’INF des haut-parleurs et associés à deux
classes APO : LFX avant mixage, GFX après mixage. Le fichier de configuration et
les profils utilisateur restent séparés de ces associations.

L’installation protège la clé `FxProperties` des seuls haut-parleurs Xonar et
les deux clés `TopologySpeaker/Device Parameters/FX/0` existantes correspondant
à cette carte. Leur héritage de permissions est désactivé ; SYSTEM, LOCAL SERVICE
et NETWORK SERVICE peuvent lire les clés mais ne peuvent pas en écrire ou supprimer
les valeurs, supprimer les clés ou modifier leurs permissions. Les administrateurs
conservent l’accès pour les opérations du produit. Les autres périphériques et
leurs clés ne sont pas concernés.

La protection est persistante. Les services audio sont redémarrés lors de son
application pour fermer leurs anciens handles d’écriture. Les gains, l’égaliseur,
les modes et les profils restent modifiables à chaud dans l’application.

Les permissions et propriétaires d’origine sont sauvegardés dans
`C:\Program Files\XonarSE\State\apo-effects-protection.clixml`. La désinstallation
les restaure avant le retrait de notre pilote et de nos associations. Notre
installateur enlève aussi la protection avant de remplacer le pilote et la remet
après installation. Une erreur d’installation restaure les permissions antérieures.

Commandes depuis le dossier du paquet, avec élévation pour modifier la protection :

```powershell
.\Scripts\apo\protect-speaker-effects.ps1 -Action Status
.\Scripts\apo\protect-speaker-effects.ps1 -Action Protect
.\Scripts\apo\protect-speaker-effects.ps1 -Action Restore
```

`-Preview` affiche le plan sans modifier les clés. Le script de réparation de
l’application restaure les associations si nécessaire puis réapplique la protection.

Cette ACL bloque les accès ordinaires des services concernés. Un administrateur
utilisant explicitement les privilèges de restauration, ou du code noyau, peut
la contourner. Une suppression complète du périphérique peut aussi créer un
nouvel endpoint : son association et sa protection doivent alors être réinstallées.

La préparation d’une version refuse un INF dépourvu des déclarations LFX/GFX
persistantes ou non couvert par le catalogue signé fourni. Le manifeste du paquet
reprend la version réellement inscrite dans cet INF.
