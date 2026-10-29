# Installation — Xonar SE Open Driver

Cette préversion cible **Windows 11 x64 sur processeur Intel/AMD** et uniquement
`USB\VID_0B05&PID_189D&MI_00` de l’ASUS Xonar SE. ARM64 n’est pas supporté.

## 1. Préparer Windows

Le pilote est signé en test, sans attestation Microsoft WHQL. Windows doit autoriser
le chargement de pilotes de test. Dans un terminal administrateur :

```powershell
bcdedit /set testsigning on
```

Redémarrer ensuite Windows. Si Windows refuse cette commande parce que Secure Boot
protège le paramètre, consulter la [documentation Microsoft du mode test](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option)
et décider si ce mode convient à votre machine. L’installateur ne désactive pas
Secure Boot et ne change pas la configuration de démarrage.

L’interface de l’assistant indique clairement qu’il s’agit d’un pilote expérimental.
Le coût d’une distribution avec signature de production n’est pas pris en charge
par cette première version gratuite. « Signé en test » ne signifie pas « certifié
Microsoft » : Windows peut afficher un éditeur inconnu avant la mise en confiance
du certificat public du projet.

## 2. Installer

1. Télécharger le fichier `XonarSE-Setup-0.5.0-preview.1-x64.exe` depuis la
   [release officielle du projet](https://github.com/Dzaze/asus-xonar-se-open-driver/releases/tag/v0.5.0-preview.1).
2. Comparer son SHA-256 à `SHA256SUMS.txt` si vous souhaitez vérifier le téléchargement.
3. Fermer les logiciels qui utilisent la carte pendant le remplacement initial du pilote.
4. Lancer le Setup et accepter l’élévation Windows.
5. Lire l’information de signature de test, cocher l’accord puis cliquer **Installer / mettre à jour**.
6. Attendre la fin et ouvrir le centre de contrôle. Redémarrer Windows si demandé.

L’assistant vérifie les fichiers embarqués et l’identité de la carte. Il prépare
le certificat public, sauvegarde le paquet pilote précédent lorsqu’il est exportable,
installe le pilote et les effets, puis l’application. Une erreur déclenche une
restauration ; le journal indique si celle-ci ne peut pas être terminée.
Les sauvegardes sont dans `C:\Program Files\XonarSE\Backups` et les journaux dans
`C:\Program Files\XonarSE\Logs`. Ne les supprimer qu’après vérification du résultat.

Le paquet validé du pilote 0.5.0.0 est utilisé sans modifier ses octets. Si le même
pilote et la même paire d’effets fonctionnent déjà, l’assistant les conserve.
Les profils existants ne sont pas écrasés. Au premier lancement sans profil, Auto
démarre avec un gain prudent et le limiteur activé.

## 3. Utiliser

Sélectionner **Haut-parleurs Xonar SE** comme sortie dans Windows ou dans le lecteur.
Le format attendu pour la sortie partagée est six canaux. Dans l’application :

- **Auto** pour alterner musique stéréo et vidéo 5.1 ;
- **5.1** pour conserver le routage natif sans dérivation de la stéréo ;
- **Upmix** pour imposer le routage de dérivation ;
- **2.0** ou **2.1** pour le downmix correspondant.

Une vidéo 5.1 reste native si son lecteur transmet six canaux PCM. Une sortie déjà
réduite en stéréo ne peut pas retrouver ses canaux originaux. Le traitement de
cette préversion a été validé en mode partagé, pas en exclusif ou ASIO.

## Récupération

**Diagnostic → Retrouver la carte et réappliquer** relit le matériel et le profil.
Si une réinstallation du pilote a supprimé les associations d’effets, utiliser
**Réparer l’installation (administrateur)** ou relancer le Setup.
L’absence de publication DSP est affichée comme une information indisponible,
sans attribuer arbitrairement une cause au silence.

## Désinstaller

Utiliser **Paramètres Windows → Applications → ASUS Xonar SE Open Driver**.
La désinstallation restaure l’état sauvegardé avant la première installation de
ce paquet. Les composants déjà présents avant celle-ci sont conservés. Les profils
sont gardés, sauf option explicite pour les supprimer. Les sauvegardes et journaux
restent disponibles.

Quand aucun pilote de test ne doit plus être utilisé, on peut rétablir le démarrage
normal manuellement, dans un terminal administrateur, puis redémarrer :

```powershell
bcdedit /set testsigning off
```

Ne pas désactiver le mode test tant qu’un pilote de test utilisé en dépend.

## Signaler un problème

Indiquer la version de Windows, celle du paquet et le mode utilisé. Joindre si
possible le diagnostic expurgé et le journal d’installation après avoir retiré
les chemins et informations personnels. Ne jamais transmettre de clé privée,
de mot de passe ou de jeton GitHub.
