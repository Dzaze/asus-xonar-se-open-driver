# Architecture

Le pilote `xonarse` gère les interfaces audio de la Xonar SE ; les effets sont
réalisés dans une DLL utilisateur chargée par le moteur audio Windows.

```text
Lecteur mono/stéréo/5.1
    → LFX : routage par source, selon le format déclaré
    → mélange Windows six canaux
    → GFX : EQ, bass management, délais, gains, protection
    → pilote xonarse → Xonar SE
```

Les classes du code nommées SFX et EFX sont ici utilisées dans les slots historiques
LFX et GFX. Les propriétés modernes 5/7 ne sont pas mélangées à la famille 1/2.

Le centre de contrôle WinUI 3 lit le matériel, conserve un profil et publie une
configuration atomique. Un veilleur APO hors callback relit celle-ci et la transmet
au DSP par une boîte SPSC. La callback ne fait ni allocation, ni E/S, ni attente
de verrou ajoutée par le contrôle ou la télémétrie.

Les réglages de sortie sont appliqués une seule fois, dans le GFX. Les formats
source et les mesures sont publiés hors callback. Sans observation valide dans
audiodg, le diagnostic conserve un état inconnu. Les informations de présence
physique de jack et les chemins non validés ne sont pas simulés.

Le Setup est un programme .NET Framework utilisant les composants déjà présents
sur Windows 11. Il contient une archive vérifiée par SHA-256, une application
autonome et des scripts d’installation avec sauvegarde et restauration.
