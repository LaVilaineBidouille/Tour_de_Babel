# Projet « Tour de Babel » – Plan de projet

Chef de projet : Éric · Responsables techniques : Éric et Samuel (PCB) · Première version : 31 oct. 2026

Mettre à jour la colonne Statut en modifiant ce fichier.

**Avancement global : 9/28 tâches terminées (32 %)**

## 1. Décisions et lancement

| N° | Tâche | Responsable | Début | Fin | Statut | Notes |
|---|---|---|---|---|---|---|
| 1.1 | Réunion de lancement : périmètre, rôles, point hebdomadaire | Éric | — | — | ✅ Terminé | Projet démarré |
| 1.2 | Confirmer les langues des sons (FR (4), EN(2), ES(1), gallo (3)...) | À attribuer | 01/10 | 07/10 | 🔄 En cours | « À confirmer » dans le briefing. Statut à fermer complet |
| 1.3 | Décider : écriture du profil sur la puce RFID à l'accueil dans la V1 ? | Éric | 01/10 | 07/10 | 🔄 En cours | Non défini dans le briefing. Suggestion : hors V1. Statut à vérifier |
| 1.4 | Convenir avec le partenaire du format et de la date de livraison des sons | Éric | 01/10 | 07/10 | 🔄 En cours | Statut à vérifier |
| 1.5 | Décider : décoration LED dans la V1 ou non | À attribuer | 01/10 | 07/10 | 🔄 En cours | « Si nécessaire » dans le briefing. Statut à vérifier |

## 2. Prototype électronique (plaque d'essai)

| N° | Tâche | Responsable | Début | Fin | Statut | Notes |
|---|---|---|---|---|---|---|
| 2.1 | Choisir et commander les composants (Arduino, DFPlayer, lecteur RFID + badges, capteurs, haut-parleur, alimentation) | Éric / Samuel | — | 07/10 | 🔄 En cours | Matériel en grande partie reçu. Lister ce qui manque |
| 2.2 | RFID : lire l'identifiant du badge de façon fiable | Éric / Samuel | — | — | ✅ Terminé | D'après l'avancement du logiciel. Statut à vérifier |
| 2.3 | Audio : lire les fichiers de la carte SD avec le DFPlayer | Éric / Samuel | — | — | ✅ Terminé | Statut à vérifier |
| 2.4 | Capteurs du haut de la tour : choix de la langue et lecture/pause | Éric / Samuel | — | — | ✅ Terminé | Statut à vérifier |
| 2.5 | Assembler RFID, capteurs et audio sur une même plaque | Éric / Samuel | — | — | ✅ Terminé | Statut à vérifier |

## 3. Logiciel

| N° | Tâche | Responsable | Début | Fin | Statut | Notes |
|---|---|---|---|---|---|---|
| 3.1 | Structure du programme : repos, badge détecté, lecture, pause | Éric / Samuel | — | — | ✅ Terminé |  |
| 3.2 | Associer l'identifiant du badge à un flux de langue (séquence de sons) | Éric / Samuel | — | 14/10 |  |plus necessaire, language selectionner pas les boutons sur la tour
| 3.3 | Lecture continue : fonctionner des heures sans défaut | Éric / Samuel | 08/10 | 21/10 | Non commencé | Le briefing insiste sur la fiabilité |
| 3.4 | LED clignotantes (facultatif) | À attribuer | 15/10 | 28/10 | Non commencé | Seulement si décidé en 1.5 |

## 4. Contenu sonore

| N° | Tâche | Responsable | Début | Fin | Statut | Notes |
|---|---|---|---|---|---|---|
| 4.1 | Le partenaire enregistre et rassemble les sons et histoires | Partenaire contenu | 01/10 | 21/10 | 🔄 En cours | Hors du contrôle du groupe. Relancer chaque semaine. Statut à vérifier |
| 4.2 | Trier et nommer les fichiers par langue pour le DFPlayer | À attribuer | 15/10 | 24/10 | Non commencé |  |
| 4.3 | Charger la carte SD et tester sur le prototype | À attribuer | 22/10 | 28/10 | Non commencé |  |

## 5. Structure de la tour

| N° | Tâche | Responsable | Début | Fin | Statut | Notes |
|---|---|---|---|---|---|---|
| 5.1 | Concevoir la tour : forme, compartiment électronique, position des capteurs | Éric / Samuel | — | — | ✅ Terminé | Tour construite |
| 5.2 | Découpe laser d'un essai en carton pour vérifier l'ajustement | Éric / Samuel | — | — | ✅ Terminé |  |
| 5.3 | Découper et assembler la tour définitive | Éric / Samuel | — | — | ✅ Terminé | Tour construite |
| 5.4 | Installer l'électronique, le haut-parleur et les capteurs dans la tour | Éric / Samuel | 01/10 | 14/10 | 🔄 En cours |  |

## 6. Circuit imprimé (PCB)

| N° | Tâche | Responsable | Début | Fin | Statut | Notes |
|---|---|---|---|---|---|---|
| 6.1 | Dessiner le schéma à partir de la plaque d'essai | Samuel | — | 07/10 | 🔄 En cours | Statut à vérifier |
| 6.2 | DÉCISION : PCB dans la V1 ou plus tard ? | Éric / Samuel | 14/10 | 14/10 | Non commencé | Fabrication et livraison peuvent dépasser le 31 oct. Plan B : carte de prototype en V1, PCB en V2 |
| 6.3 | Router et commander la carte | Samuel | 08/10 | 17/10 | Non commencé | Seulement si le PCB reste dans la V1 |
| 6.4 | Assembler et tester la carte | Samuel | 18/10 | 28/10 | Non commencé | Seulement si le PCB reste dans la V1 |

## 7. Essais et livraison

| N° | Tâche | Responsable | Début | Fin | Statut | Notes |
|---|---|---|---|---|---|---|
| 7.1 | Essai complet de la tour assemblée | Éric | 22/10 | 29/10 | Non commencé |  |
| 7.2 | Essai avec quelques bénévoles, relevé des problèmes | Éric | 29/10 | 30/10 | Non commencé |  |
| 7.3 | Première version prête | Éric | 31/10 | 31/10 | Non commencé | Jalon |

## Risques et points ouverts

- **Risque** : PCB : délai de fabrication et de livraison par rapport au 31 oct. → Samuel confirme le délai de commande. Décision le 14 oct. Plan B : carte de prototype en V1
- **Risque** : Le contenu sonore dépend du partenaire, hors du contrôle du groupe → Fixer une date de livraison et relancer chaque semaine. Tester avec des sons provisoires
- **Risque** : Composants encore manquants → Lister ce qui manque et commander cette semaine
- **Risque** : Lecture continue : fiabilité non encore prouvée sur la durée → Essai de plusieurs heures avant le 21 oct.
- **Ouvert** : Liste définitive des langues (gallo inclus ?) → Confirmer
- **Ouvert** : Profil écrit sur la puce RFID à l'accueil : dans la V1 ? → Décider. Non défini dans le briefing
- **Ouvert** : Qui s'occupe de quoi ? Plusieurs tâches sont « À attribuer » → Répartir à la prochaine réunion
- **Ouvert** : Qui est le partenaire de contenu et quel format de fichier livre-t-il ? → Éric confirme
