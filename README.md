# ASUS Xonar SE Open Driver — Windows 11

Pilote ouvert et centre de contrôle pour la **carte son ASUS Xonar SE** :
5.1, upmix stéréo, 2.0, 2.1, égaliseur, bass management, gains et délais par enceinte.

**Auteur : Benoist Pirolley.** Projet indépendant, sans affiliation avec ASUS ou C-Media.

## Télécharger et installer

➡️ **[Télécharger XonarSE Setup pour Windows 11 x64](https://github.com/Dzaze/asus-xonar-se-open-driver/releases/download/v0.5.0-preview.1/XonarSE-Setup-0.5.0-preview.1-x64.exe)**

Première version publique **0.5.0-preview.1**. Elle a été validée sur la Xonar SE du
développeur, sous Windows 11 build 26200. Les autres configurations restent à valider.
Consulter les [instructions d’installation et de retour arrière](docs/INSTALLATION.fr.md).

Le correctif **0.5.0-preview.2 est en préparation** : il protège les associations
APO contre leur modification par les services Windows, provisionne les effets
depuis l’INF et corrige la récupération de la page de routage. La préversion
téléchargeable ci-dessus ne contient pas encore cette protection. Voir les
[détails et commandes de protection](docs/APO-PROTECTION.fr.md).

**Le pilote est signé avec un certificat de test ; il n’est pas certifié Microsoft
WHQL. Le mode test de Windows doit être activé avant l’installation.** L’assistant
vérifie ce prérequis et demande l’accord pour installer le certificat public du
projet. Il ne change pas automatiquement le démarrage Windows ou Secure Boot.
L’installation et son certificat de test peuvent déclencher un avertissement Windows.

L’installateur inclut l’application, .NET, Windows App SDK, le pilote x64, l’APO
et le redistribuable Microsoft Visual C++. Aucun outil de développement n’est
nécessaire pour utiliser le paquet. Aucune clé privée n’est distribuée.

## Fonctions

- **Auto** : mono/stéréo vers les six enceintes ; routage des sources natives 5.1
  conservé, même lors de lectures simultanées.
- Modes manuels **5.1, Upmix, 2.0 et 2.1**, appliqués à chaud sur le transport
  Windows six canaux. Pour une vidéo 5.1, laisser Auto et choisir une sortie PCM
  multicanal dans le lecteur.
- Égaliseur, gain, mute et délai par canal ; crossover et dérivation des basses.
- Limiteur, mesure de la marge numérique et atténuation proposée par bouton.
- Test des six enceintes ; profils persistants, import et export.
- Diagnostic des formats réellement reçus, récupération après interruption audio.

La calibration est appliquée une seule fois, après mélange. La décision Auto repose
sur le format déclaré par le lecteur, sans deviner le contenu à partir des silences.
Si le lecteur a déjà converti une piste 5.1 en stéréo, le format original est perdu.

## Périmètre validé

Rendu partagé mono, stéréo et six canaux, sortie **float32 / 6 canaux / 0x60F /
48 kHz**, avec la paire LFX/GFX historique. Les mesures précédant la publication
comptent 43 tests du centre de contrôle, 33 cas DSP, 8 scénarios d’installation
APO, 17 contrôles Auto et 13 contrôles manuels sur la carte.

Les chemins RAW, exclusif, offload, ASIO, S/PDIF, quadraphonie et 7.1 ne sont pas
couverts par cette validation. La détection physique de prise n’est pas annoncée.
Le S/PDIF reste prévu dans l’interface ; cette préversion ne promet pas toutes les
fonctions du panneau propriétaire ASUS.

Le paquet public a également été vérifié par 15 tests CTest, 5 scénarios isolés
de l’installateur complet et un cycle réel d’installation/désinstallation sur le
PC déjà équipé du pilote. Le remplacement initial d’un autre pilote n’a pas été
qualifié sur une seconde machine : cette version reste une préversion.

## Code et contribution

- [Compiler les sources](docs/BUILD.fr.md)
- [Architecture](docs/ARCHITECTURE.fr.md)
- [Signaler un problème](https://github.com/Dzaze/asus-xonar-se-open-driver/issues)

Les sources du projet sont sous [Apache-2.0](LICENSE). Les composants Microsoft
et tiers fournis avec l’installateur conservent leurs licences d’origine.
Aucun binaire propriétaire ASUS, C-Media ou ASMedia n’est fourni.

---

**Benoist Pirolley**

- [Site web](https://benoist-pirolley.web.app/)
- [LinkedIn](https://www.linkedin.com/in/benoist-pirolley-11546a317/)
