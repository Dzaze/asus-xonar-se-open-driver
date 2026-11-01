# Installer, mettre à jour et restaurer l’APO Xonar SE

L’installateur gère la DLL utilisateur `XonarApo.dll`, son enregistrement COM
et ses associations **LFX/GFX historiques** aux haut-parleurs Xonar. Il préserve
le pilote noyau, l’INF, le format Windows, le volume et la configuration de démarrage.

## Utilisation

Depuis le dépôt, lancer **`Xonar-APO.cmd`**, ou utiliser PowerShell :

```powershell
# Vérifier le plan sans changer le système audio ; seul le journal TEMP est écrit.
.\install-xonar-apo.ps1 -WhatIf

# Installation ou mise à jour, avec élévation Windows si nécessaire.
.\install-xonar-apo.ps1

# DLL fournie explicitement dans un paquet.
.\install-xonar-apo.ps1 -SourceDll "C:\Mon paquet\XonarApo.dll"

# Retour à l’état sauvegardé avant une installation.
.\install-xonar-apo.ps1 -Action Restore -BackupDirectory "C:\Program Files\XonarSE\Backups\<dossier>"
```

Dans l’application, **Diagnostic → Réparer l’installation (administrateur)**
lance le même script et affiche son résultat. **Retrouver la carte et réappliquer**
ne demande pas d’élévation : il relit le matériel et réapplique le profil existant
quand le format et les associations sont disponibles.

La DLL source est cherchée dans la build du dépôt, à côté du script ou dans
`bin/XonarApo.dll` à la racine du paquet. Distribuer **`scripts/install` et
`scripts/apo` ensemble**, avec la DLL signée. La publication du projet reste différée.

## Signature

La DLL doit déjà avoir une signature Authenticode **valide**, du signataire attendu.
L’installateur ne continue plus silencieusement avec une DLL non signée.

L’empreinte par défaut est celle du certificat de test local du projet :
`FF68A5C11C614F5E45AD64594D5A978BD9E5D321`.
Un développeur qui signe sa propre build peut fournir l’empreinte de son certificat
avec `-TrustedSignerThumbprint`. Le certificat privé n’est jamais distribué.
Voir [la compilation et la signature](../../docs/build-and-signing.md).
Ces scripts n’activent pas le mode test de Windows. Ils protègent les seules clés
d’effets des haut-parleurs Xonar contre les modifications des comptes système.

## Déroulement et sauvegardes

1. Vérifier signature, identité matérielle et unicité des haut-parleurs ;
   refuser une association tierce incompatible ou un CLSID enregistré ailleurs.
2. Sauvegarder DLL, valeurs COM typées et ACL des fichiers de diagnostic.
3. Arrêter `Audiosrv`, remplacer la DLL et vérifier son hash.
4. Enregistrer les deux classes COM et préparer les ACL nécessaires.
5. Sauvegarder puis relire les `FxProperties` via le backend partagé.
6. Protéger les clés d’effets Xonar, redémarrer l’audio si le service était actif
   et vérifier que le pilote n’a pas changé.
7. En cas d’erreur après le début des modifications, restaurer DLL, COM, effets et ACL,
   puis relancer l’audio. Une restauration incomplète est signalée explicitement.

La famille utilisée est unique :

| Propriété FX | Valeur |
|---|---|
| D04E05A6-594B-4FB6-A80D-01AF5EED7D1D,0 | Association ANY, GUID nul |
| Même GUID,1 | Classe source `{C243A8CB-D535-4BA0-BCC5-CC4338833E7A}` en LFX |
| Même GUID,2 | Classe sortie `{D2FBC737-00C3-4482-B3DD-02D92C06003E}` en GFX |

LFX réalise le routage par source avant mélange ; GFX applique la calibration
et la protection une seule fois. Les slots modernes 5/7 ne sont pas mélangés à ceux-ci.

Les sauvegardes sont créées par défaut dans
`C:\Program Files\XonarSE\Backups\<date-identifiant>\`, avec un manifeste versionné.
`-BackupDirectory` permet d’en choisir l’emplacement. Les journaux sont dans
`%TEMP%\xonar-apo-install-*.log`. Les profils et leurs fichiers JSON restent en place.

Le compte LOCAL SERVICE lit la configuration et dispose de Modify sur les seuls
fichiers précréés `apo-log.txt` et `apo-live-status.json`. L’utilisateur peut
enregistrer son profil dans `C:\ProgramData\XonarSE`.

## Désinstallation

```powershell
.\uninstall-xonar-apo.ps1 -WhatIf
.\uninstall-xonar-apo.ps1
# Suppression explicitement demandée de la configuration utilisateur :
.\uninstall-xonar-apo.ps1 -RemoveConfig
```

La désinstallation retire nos CLSID, conserve les effets tiers et le choix Windows
`Disable_SysFx`, désenregistre le COM et supprime seulement la DLL. Elle conserve
les sauvegardes et le profil, sauf `-RemoveConfig` pour ce dernier.
Pour un retour exact à une installation précédente, utiliser `-Action Restore`.

## Vérifications

[Preuves locales, panne injectée et reprise automatique](../../docs/evidence/product-polish-validation-2026-10-29.md).

```powershell
# Huit scénarios isolés, sans modifier le registre ni les services réels :
..\..\tests\integration\test-apo-installer.ps1
```

`-FailurePoint` est réservé au banc avec `XONAR_INSTALL_TEST=1` ; il ne fait pas
partie du parcours normal d’installation.
