# Xonar SE 0.5.0-preview.2 — 1 novembre 2026

Cette préversion corrige la perte des associations APO qui pouvait désactiver le
DSP alors que les haut-parleurs Xonar restaient présents dans Windows.

L’installation protège automatiquement les clés d’effets des seuls haut-parleurs
Xonar contre les écritures, suppressions et changements de permissions ordinaires
des comptes système. Notre installateur conserve la possibilité de mettre à jour
le pilote et de retirer la protection. Les permissions d’origine sont sauvegardées.

Le paquet pilote 0.5.0.1 déclare LFX/GFX dans l’INF. La protection et la restauration
suivent la sortie définitive si Windows la recrée avec un autre GUID. Une mise à
jour d’INF est appliquée même lorsque le binaire noyau est inchangé. La préparation
du Setup vérifie ces déclarations et la couverture de l’INF par le catalogue signé.

La page de routage réagit au retour des capacités. La synchronisation des boutons
ne réécrit plus le mode choisi. Le démarrage utilisateur de l’application peut se
faire dans la zone de notification ; l’APO reste chargé par le moteur audio Windows.

Validation sur Windows 11 build 26200 : 44 tests du centre de contrôle, 7 scénarios
de protection, 9 scénarios d’installation APO, 6 scénarios d’installation complète,
8 scénarios de récupération et 5 contrôles de préparation. Cycle réel de mise à
jour, désinstallation vers l’état précédent et réinstallation réussi, avec profils
conservés et DSP actif. Le binaire noyau reste identique à celui du paquet précédent.

Le Setup, le catalogue et l’application sont signés avec le certificat de test du
projet ; le pilote n’est pas certifié Microsoft WHQL. Les prérequis de mode test
restent inchangés. Aucun redémarrage complet de Windows n’a été effectué pendant
ce cycle de validation. Cette version reste une préversion, validée sur la carte
du développeur, sans qualification du remplacement initial d’un autre pilote sur
une seconde machine.

La protection ACL n’empêche pas un administrateur utilisant explicitement les
privilèges de restauration, ou du code noyau, de modifier les clés. Voir
[la protection des associations DSP](APO-PROTECTION.fr.md) et
[les instructions d’installation](INSTALLATION.fr.md).
