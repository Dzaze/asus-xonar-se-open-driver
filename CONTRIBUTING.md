# Contribuer

Merci de commencer par un ticket décrivant le matériel, la version Windows et le
comportement attendu. Ne joignez ni jeton, ni mot de passe, ni certificat privé.
Les rapports locaux peuvent contenir des chemins et identifiants : utiliser l’export
expurgé de Diagnostic et relire les informations avant de les publier.

Compiler et tester les composants portables avant de proposer une modification.
Pour un changement noyau, travailler d’abord sur un environnement isolé et conserver
un paquet fonctionnel et une procédure de retour arrière. Aucun test ne doit cibler
une interface différente de `USB\VID_0B05&PID_189D&MI_00`.

Les contributions doivent être compatibles avec Apache-2.0 et conserver les mentions
des composants tiers. Aucun binaire propriétaire ASUS/C-Media/ASMedia ni procédé
d’usurpation de signature n’est accepté. Les pilotes de développement utilisent
un vrai certificat de test, clairement identifié comme tel.
