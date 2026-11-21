/*
  Tour de Babel – La Villaine Bidouille
  Version brouillon v0.8.0 (Arduino Nano + PN532 + PCF8574 avec interruption) – à valider sur le matériel
  v0.5 : démarrage du DFPlayer corrigé et diagnostics détaillés sur le moniteur série (115200 bauds)
  v0.6 : surveillance batterie désactivable,
         commandes de test au clavier du moniteur série (taper ? pour l'aide)
  V0.7 : (IR) Changement de navigation : le dossier = le flux, le fichier = la langue.
  v0.7.x : noms de dossiers/fichiers corrigés pour le DFPlayer, commande de lecture affichée, messages de batterie
           masqués par défaut (commande b), fin de piste trop rapide ignorée, startStream() explique ses refus
  v0.8.0 : nouveau fonctionnement demandé par le chef de projet :
           - sans activité pendant IDLE_TIMEOUT_MIN minutes : démonstration (flux fixe ou au hasard, dans toutes les langues)
           - badge passé : les LED langue et lecture clignotent ; la lecture ne démarre qu'après appui sur une langue ET sur lecture

  Fonctions
  - Lit le secteur 2, bloc 0 (bloc absolu 8) d'un badge MIFARE Classic avec la clé A.
    Premier octet du bloc (valeur binaire 1..99) = numéro du flux à jouer.
  - 8 boutons au sommet de la tour, lus par un PCF8574 (I2C) dont la broche INT
    est reliée à D2 (INT0 de l'Arduino) : 4 langues, lecture/pause,
    volume +, volume -, 1 de réserve (broches P0..P7 dans cet ordre).
  - Le DFPlayer Mini joue  /SS/LLL.mp3  (SS = flux 01..99, LLL = langue 001..004). (V0.7)
  - Un ADS1115 mesure la tension de la batterie ; alerte sonore + LED si trop basse.
  - Des LED adressables (WS2812) jouent une séquence pour chaque événement.

  Bibliothèques (Gestionnaire de bibliothèques) :
    Adafruit PN532, DFRobotDFPlayerMini, Adafruit ADS1X15, Adafruit NeoPixel
    (Wire, SoftwareSerial et EEPROM sont fournies avec l'IDE)

  Carte SD du DFPlayer. Règles imposées par le module :
    - dossiers numérotés sur 2 chiffres : 01 à 99 (le numéro du flux)
    - fichiers dont le nom COMMENCE par 3 chiffres : 001 à 255 (suite libre : 001.mp3 ou 001_fr.mp3)
    - dans le programme : player.playFolder(flux, langue)  ->  /SS/LLL.mp3
    Exemples : /01/001.mp3 = flux 1 langue 1 ; /04/002.mp3 = flux 4 langue 2 ; /99/004.mp3 = flux 99 langue 4
    Attention : "0001.mp3" commence par "000" = fichier 0, il n'est jamais trouvé. Un dossier "001" non plus.

    /MP3/0001.mp3  alerte batterie faible
    /MP3/0002.mp3  alerte batterie critique
    /MP3/0003.mp3  (facultatif) badge non reconnu

  Bus I2C partagé : PCF8574 (0x20 à 0x27, ou 0x38 à 0x3F pour le PCF8574A), PN532 (0x24), ADS1115 (0x48).
  Attention : un PCF8574 avec A2=1, A1=0, A0=0 serait à 0x24, comme le PN532.
  La broche IRQ du PN532 doit être câblée (la bibliothèque Adafruit l'utilise en I2C).
*/

#include <Wire.h>
#include <EEPROM.h>
#include <SoftwareSerial.h>
#include <Adafruit_PN532.h>
#include <DFRobotDFPlayerMini.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_NeoPixel.h>

// ---------------------------------------------------------------- Réglages
#define DEBUG 1
#if DEBUG
  #define LOG(x)    Serial.println(F(x))
  #define LOGV(a,b) do{Serial.print(F(a));Serial.println(b);}while(0)
#else
  #define LOG(x)
  #define LOGV(a,b)
#endif

// Broches (Arduino Nano). I2C : A4 = SDA, A5 = SCL.
const uint8_t PIN_PN532_IRQ = A3;
const uint8_t PIN_PN532_RST = A2;
const uint8_t PIN_DF_RX     = 5;   // Nano reçoit  <- TX du DFPlayer
const uint8_t PIN_DF_TX     = 6;   // Nano émet    -> RX du DFPlayer (résistance 1 kΩ en série)
const uint8_t PIN_LED       = 4;

// Boutons, dans l'ordre : langues 1..4, lecture/pause, volume +, volume -, réserve
const uint8_t NB_LANG = 4;
const uint8_t PCF_ADDR   = 0x20;      // PCF8574 : 0x20..0x27 ; PCF8574A : 0x38..0x3F
const uint8_t PIN_PCF_INT = 2;        // INT du PCF8574 -> D2 (INT0), actif à l'état bas
const bool    PADS_ACTIVE_HIGH = false; // false : TTP223 en mode momentané, 0 quand touché (pont A relié)
const unsigned long PAD_SETTLE_MS = 25; // délai de stabilisation (anti-rebond) après une interruption
const uint8_t PAD_PLAY   = NB_LANG;
const uint8_t PAD_VOL_UP = NB_LANG + 1;
const uint8_t PAD_VOL_DN = NB_LANG + 2;
const uint8_t PAD_SPARE  = NB_LANG + 3;

// LED
const uint8_t NB_LED = 12;
const uint8_t LED_BRIGHTNESS = 80;    // 0..255, limite la consommation
const unsigned long LED_FRAME_MS = 30; // strip.show() coupe les interruptions ~0,4 ms : on limite à ~33 images/s
                                       // pour ne pas perturber la réception SoftwareSerial du DFPlayer

// Badge
const uint8_t BLOCK_STREAM = 8;       // secteur 2, bloc 0 = 2*4 + 0
uint8_t KEY_A[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};   // clé par défaut, à remplacer si besoin
const unsigned long RFID_POLL_MS    = 200;
const unsigned long RFID_LOCKOUT_MS = 3000;           // pause après une lecture (badge laissé sur le lecteur)

// Audio
const uint8_t VOLUME_DEFAULT = 10;    // 0..30
const unsigned long DF_POWERUP_MS = 1500;  // le DFPlayer a besoin de ~1 s après la mise sous tension
const uint8_t DF_ATTEMPTS = 3;
const unsigned long MIN_PLAY_MS = 800;    // une "fin de piste" reçue plus tôt après PLAY est ignorée (flux utilisateur)

// Fonctionnement (modes) -------------------------------------------------
const uint16_t IDLE_TIMEOUT_MIN   = 5;    // minutes sans activité avant la démonstration
const uint8_t  DEMO_STREAM        = 0;    // 0 = flux tiré au hasard ; sinon numéro du flux joué en démonstration
const uint8_t  DEMO_RANDOM_MIN    = 1;    // plage du tirage au hasard : ne mettre que des flux présents sur la carte SD
const uint8_t  DEMO_RANDOM_MAX    = 10;
const unsigned long DEMO_GAP_MS   = 3000; // pause entre deux flux de démonstration
const unsigned long DEMO_STALL_MS = 600000UL;  // sécurité : si aucune fin de piste après 10 min, on passe à la suite
const unsigned long WAIT_USER_TIMEOUT_S = 60;  // après un badge : délai pour appuyer sur une langue ET sur lecture
const bool     REPEAT_USER_STREAM = false;     // true : le flux de l'utilisateur recommence en boucle ; false : retour en attente
// Position des "LED de boutons" dans la guirlande (numéro de pixel 0..NB_LED-1) : à adapter au montage
const uint8_t  LED_LANG_PIX[NB_LANG] = {0, 1, 2, 3};
const uint8_t  LED_PLAY_PIX          = 4;
const uint8_t VOLUME_STEP    = 2;
const uint8_t VOLUME_MAX     = 30;

// Batterie (ADS1115, voie A0)
#define BATT_MONITOR 0                // 1 = alertes et blocage de la lecture selon la batterie ; 0 = mesure affichée seulement
const float DIVIDER_RATIO = 4.0;      // Vbat = Vmesuré * ratio (selon le pont diviseur)
const float BATT_LOW      = 3.50;     // volts : alerte
const float BATT_CRITICAL = 3.30;     // volts : arrêt de la lecture
const float BATT_HYST     = 0.10;     // retour à la normale = seuil + hystérésis
const unsigned long BATT_PERIOD_MS  = 5000;
const unsigned long LOW_REMINDER_MS = 30000;          // rappel LED en batterie faible

// ---------------------------------------------------------------- Objets
Adafruit_PN532 nfc(PIN_PN532_IRQ, PIN_PN532_RST);
SoftwareSerial FPSerial(PIN_DF_RX, PIN_DF_TX);
DFRobotDFPlayerMini player;
Adafruit_ADS1115 ads;
Adafruit_NeoPixel strip(NB_LED, PIN_LED, NEO_GRB + NEO_KHZ800);

// ---------------------------------------------------------------- État
uint8_t  lang   = 1;      // 1..NB_LANG
uint8_t  stream = 0;      // 0 = pas encore de badge, sinon 1..99
uint8_t  volume = VOLUME_DEFAULT;
bool     paused = false;
bool     playing = false;
unsigned long lastPlayAt = 0;     // instant du dernier PLAY envoyé
bool     battLog = false;         // afficher les mesures de batterie (commande b pour basculer)

enum Mode : uint8_t { M_IDLE, M_DEMO, M_WAIT_USER, M_PLAYING };
Mode     mode = M_IDLE;
unsigned long lastActivityAt = 0; // dernier appui de bouton ou badge
unsigned long waitStartAt = 0;
bool     langChosen = false, playChosen = false;     // en attente de l'utilisateur après un badge
uint8_t  demoStream = 1, demoLang = 1;
bool     demoWaiting = false;     // pause entre deux flux de démonstration
unsigned long demoNextAt = 0;

enum BattState : uint8_t { BATT_OK, BATT_LOW_STATE, BATT_CRIT_STATE };
BattState battState = BATT_OK;
float    battVolts = 0;
bool     adsOk = false;
bool     pn532Ok = false;
bool     dfOk = false;            // DFPlayer démarré correctement

// Une couleur par langue
const uint32_t LANG_COLOR[NB_LANG] = {0x0000FF, 0xFF0000, 0xFFA000, 0x00C000};

// ---------------------------------------------------------------- LED : événements
enum LedEvent : uint8_t { EV_NONE, EV_BOOT, EV_TAG_OK, EV_TAG_BAD, EV_LANG, EV_PAUSE, EV_RESUME,
                          EV_VOLUME, EV_LOW_BATT, EV_CRIT_BATT };
LedEvent ledEvent = EV_NONE;
unsigned long ledEventStart = 0;
unsigned long ledEventDuration = 0;
unsigned long ledLastLowReminder = 0;

void ledStart(LedEvent e) {
  ledEvent = e;
  ledEventStart = millis();
  switch (e) {
    case EV_BOOT:      ledEventDuration = 1500; break;
    case EV_TAG_OK:    ledEventDuration = 1000; break;
    case EV_TAG_BAD:   ledEventDuration = 900;  break;
    case EV_LANG:      ledEventDuration = 700;  break;
    case EV_PAUSE:     ledEventDuration = 500;  break;
    case EV_RESUME:    ledEventDuration = 500;  break;
    case EV_VOLUME:    ledEventDuration = 800;  break;
    case EV_LOW_BATT:  ledEventDuration = 3000; break;
    case EV_CRIT_BATT: ledEventDuration = 6000; break;
    default:           ledEventDuration = 0;
  }
}

void fillAll(uint32_t c) { for (uint8_t i = 0; i < NB_LED; i++) strip.setPixelColor(i, c); }

uint32_t scale(uint32_t c, uint8_t k) {   // k 0..255
  uint8_t r = ((c >> 16) & 0xFF) * k / 255;
  uint8_t g = ((c >> 8)  & 0xFF) * k / 255;
  uint8_t b = (c & 0xFF) * k / 255;
  return strip.Color(r, g, b);
}

// Respiration douce (0..255) avec une période donnée
uint8_t breathe(unsigned long t, unsigned long period) {
  float x = (t % period) / (float)period;
  float v = x < 0.5 ? x * 2 : (1 - x) * 2;
  return (uint8_t)(v * v * 255);
}

// Appelée à chaque tour de loop() : aucune attente bloquante
void ledUpdate() {
  unsigned long now = millis();
  static unsigned long lastFrame = 0;
  if (now - lastFrame < LED_FRAME_MS) return;
  lastFrame = now;
  unsigned long t = now - ledEventStart;

  if (ledEvent != EV_NONE && t >= ledEventDuration) ledEvent = EV_NONE;

  if (ledEvent != EV_NONE) {
    strip.clear();
    switch (ledEvent) {
      case EV_BOOT: {                      // remplissage progressif en blanc chaud
        uint8_t n = (uint32_t)NB_LED * t / ledEventDuration;
        for (uint8_t i = 0; i <= n && i < NB_LED; i++) strip.setPixelColor(i, 0xFFB060);
        break; }
      case EV_TAG_OK: {                    // tour de vert
        uint8_t head = (t / 60) % NB_LED;
        for (uint8_t k = 0; k < 4; k++) strip.setPixelColor((head + NB_LED - k) % NB_LED, scale(0x00FF00, 255 - k * 60));
        break; }
      case EV_TAG_BAD:                     // deux clignotements rouges
        if ((t / 150) % 2 == 0) fillAll(0xFF0000);
        break;
      case EV_LANG:                        // couleur de la langue, qui s'éteint
        fillAll(scale(LANG_COLOR[lang - 1], 255 - (uint32_t)255 * t / ledEventDuration));
        break;
      case EV_PAUSE:  fillAll(0xFF8000); break;
      case EV_RESUME: fillAll(0x00FF00); break;
      case EV_VOLUME: {                    // jauge blanche proportionnelle au volume
        uint8_t n = ((uint16_t)NB_LED * volume + VOLUME_MAX - 1) / VOLUME_MAX;
        for (uint8_t i = 0; i < n && i < NB_LED; i++) strip.setPixelColor(i, 0xFFFFFF);
        break; }
      case EV_LOW_BATT:
      case EV_CRIT_BATT:                   // clignotement rouge rapide
        if ((t / 250) % 2 == 0) fillAll(0xFF0000);
        break;
      default: break;
    }
  } else {
    // Ambiance de fond selon le mode
    switch (mode) {
      case M_IDLE:                                                 // en attente d'un badge
        fillAll(scale(0x201040, breathe(now, 4000)));
        break;
      case M_WAIT_USER: {                                          // badge lu : LED langue et lecture clignotent
        strip.clear();
        bool on = (now / 350) % 2 == 0;
        for (uint8_t i = 0; i < NB_LANG; i++) {
          bool show = langChosen ? (lang == i + 1) : on;           // langue choisie : fixe, les autres éteintes
          if (show) strip.setPixelColor(LED_LANG_PIX[i], LANG_COLOR[i]);
        }
        if (playChosen || on) strip.setPixelColor(LED_PLAY_PIX, 0x00FF00);
        break; }
      default:                                                     // démonstration ou lecture
        if (paused) {
          fillAll(scale(0x804000, 60 + breathe(now, 3000) / 4));   // pause : ambre tamisé
        } else {
          uint32_t c = LANG_COLOR[lang - 1];                       // couleur de la langue en cours
          for (uint8_t i = 0; i < NB_LED; i++) {
            uint8_t k = 70 + breathe(now + i * 200UL, 3000) * 185 / 255;
            strip.setPixelColor(i, scale(c, k));
          }
        }
    }
    // Rappel périodique en batterie faible
    if (battState == BATT_LOW_STATE && now - ledLastLowReminder > LOW_REMINDER_MS) {
      ledLastLowReminder = now;
      ledStart(EV_LOW_BATT);
    }
  }
  strip.show();
}

// ---------------------------------------------------------------- Audio
#if DEBUG
// Décode les messages du DFPlayer (même principe que l'exemple DFRobot)
void printDetail(uint8_t type, int value) {
  switch (type) {
    case TimeOut:               Serial.println(F("DF: Time Out !")); break;
    case WrongStack:            Serial.println(F("DF: Stack Wrong !")); break;
    case DFPlayerCardInserted:  Serial.println(F("DF: carte SD insérée")); break;
    case DFPlayerCardRemoved:   Serial.println(F("DF: carte SD retirée")); break;
    case DFPlayerCardOnline:    Serial.println(F("DF: carte SD en ligne")); break;
    case DFPlayerUSBInserted:   Serial.println(F("DF: USB inséré")); break;
    case DFPlayerUSBRemoved:    Serial.println(F("DF: USB retiré")); break;
    case DFPlayerPlayFinished:
      Serial.print(F("DF: piste ")); Serial.print(value); Serial.println(F(" terminée"));
      break;
    case DFPlayerError:
      Serial.print(F("DF: ERREUR : "));
      switch (value) {
        case Busy:             Serial.println(F("carte SD introuvable / module occupé")); break;
        case Sleeping:         Serial.println(F("en veille")); break;
        case SerialWrongStack: Serial.println(F("trame reçue incorrecte")); break;
        case CheckSumNotMatch: Serial.println(F("somme de contrôle fausse (liaison série perturbée ?)")); break;
        case FileIndexOut:     Serial.println(F("numéro de fichier hors limites")); break;
        case FileMismatch:     Serial.println(F("fichier introuvable (dossier ou nom incorrect ?)")); break;
        case Advertise:        Serial.println(F("en cours d'annonce (/MP3)")); break;
        default:               Serial.print(F("code ")); Serial.println(value); break;
      }
      break;
    default: break;
  }
}

int freeRam() {                        // mémoire vive libre (2 Ko seulement sur un Nano)
  extern int __heap_start, *__brkval;
  int v;
  return (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
}

// Après un échec de begin() : combien d'octets le DFPlayer envoie-t-il vraiment ?
//   0 octet      -> rien n'arrive sur la broche RX du Nano : TX/RX inversés, masse non commune,
//                   mauvaise broche, ou module non alimenté
//   1 à 9 octets -> liaison perturbée (parasites, interruptions, mauvais débit)
//   10 octets ou plus -> la liaison fonctionne : regarder la carte SD (FAT32, <= 32 Go, bien insérée)
void dfRawProbe() {
  while (FPSerial.available()) FPSerial.read();
  player.reset();                                  // le module répond : acquittement + "carte en ligne"
  unsigned long t = millis();
  uint8_t n = 0;
  Serial.print(F("  octets reçus du DFPlayer :"));
  while (millis() - t < 2500) {
    if (FPSerial.available()) {
      uint8_t b = FPSerial.read();
      if (n < 24) { Serial.print(' '); if (b < 16) Serial.print('0'); Serial.print(b, HEX); }
      n++;
    }
  }
  Serial.println();
  LOGV("  nombre d'octets : ", n);
}

// Les questions de la bibliothèque (état, volume, nombre de fichiers...) renvoient des réponses décalées
// tant que l'acquittement est actif, et disableACK() n'est pas accessible : on ne les utilise pas.
// Le vrai test est de jouer un fichier : commande "t1" dans le moniteur série ; les messages
// du module (printDetail) disent alors si le fichier est introuvable.
void dfDiagnostics() {
  LOG("DFPlayer : prêt. Test : taper t1 (joue /01/001.mp3), l2 pour la langue 2, ? pour l'aide");
}
#else
void printDetail(uint8_t, int) {}
void dfRawProbe() {}
void dfDiagnostics() {}
#endif

bool dfInit() {
  LOGV("DFPlayer : attente de démarrage du module (ms) : ", DF_POWERUP_MS);
  delay(DF_POWERUP_MS);
  FPSerial.begin(9600);
  for (uint8_t attempt = 1; attempt <= DF_ATTEMPTS; attempt++) {
    LOGV("DFPlayer : tentative ", attempt);
    if (player.begin(FPSerial, /*isACK=*/true, /*doReset=*/true)) {
      LOG("DFPlayer en ligne.");
      return true;
    }
    LOG("  ECHEC. A vérifier : 1) câblage (TX du module -> broche RX du Nano, RX -> TX via 1 kΩ) et masse commune");
    LOG("  2) carte SD insérée (FAT32, 32 Go max)   3) alimentation 5 V suffisante (pics au démarrage)");
    dfRawProbe();
  }
  return false;
}

// ---------------------------------------------------------------- Modes de fonctionnement
bool canPlay() {
  if (!dfOk)                        { LOG("Pas de lecture : DFPlayer non démarré"); return false; }
  if (battState == BATT_CRIT_STATE) { LOG("Pas de lecture : batterie critique"); return false; }
  return true;
}

// Envoie la commande au DFPlayer : dossier /FF (flux), fichier 00L.mp3 (langue)
void sendPlay(uint8_t folder, uint8_t file) {
#if DEBUG
  Serial.print(F("PLAY flux=")); Serial.print(folder);
  Serial.print(F(" langue="));   Serial.print(file);
  Serial.print(F(" -> playFolder(")); Serial.print(folder); Serial.print(F(", ")); Serial.print(file);
  Serial.print(F(") = /")); if (folder < 10) Serial.print('0');
  Serial.print(folder); Serial.print(F("/00")); Serial.print(file); Serial.println(F(".mp3"));
#endif
  static unsigned long lastCmd = 0;    // le DFPlayer perd les commandes trop rapprochées
  unsigned long since = millis() - lastCmd;
  if (since < 150) delay(150 - since);
  lastCmd = millis();
  player.playFolder(folder, file);
  lastPlayAt = millis();
  LOG("  commande envoyée au DFPlayer");
}

void startStream() {                   // flux de l'utilisateur : stream + lang
  if (stream == 0) { LOG("startStream : aucun flux choisi"); return; }
  if (!canPlay()) return;
  sendPlay(stream, lang);
  playing = true;
  paused = false;
}

void stopAudio() {
  if (dfOk) player.stop();
  playing = false;
  paused = false;
}

void goIdle() {                        // retour en attente d'un badge
  mode = M_IDLE;
  stopAudio();
  stream = 0;
  langChosen = playChosen = false;
  demoWaiting = false;
}

void userActivity() { lastActivityAt = millis(); }

void togglePause() {
  if (mode != M_PLAYING) return;
  if (paused) { if (dfOk) player.start(); paused = false; ledStart(EV_RESUME); LOG("Reprise"); }
  else        { if (dfOk) player.pause(); paused = true;  ledStart(EV_PAUSE);  LOG("Pause"); }
}

// --- Démonstration : un flux (fixe ou au hasard) joué successivement dans toutes les langues
uint8_t demoPick() { return DEMO_STREAM ? DEMO_STREAM : (uint8_t)random(DEMO_RANDOM_MIN, DEMO_RANDOM_MAX + 1); }

void demoPlay() {
  lang = demoLang;                     // pour la couleur des LED
  if (!canPlay()) { goIdle(); userActivity(); return; }
  sendPlay(demoStream, demoLang);
  playing = true;
  paused = false;
}

void demoStart() {
  LOG("Démonstration : démarrage");
  mode = M_DEMO;
  demoStream = demoPick();
  demoLang = 1;
  demoWaiting = false;
  demoPlay();
}

void demoNext() {                      // piste terminée (ou inutilisable) : langue suivante, puis flux suivant
  demoLang++;
  if (demoLang > NB_LANG) {
    demoLang = 1;
    demoStream = demoPick();
    demoWaiting = true;
    demoNextAt = millis() + DEMO_GAP_MS;
    return;
  }
  demoPlay();
}

// --- Actions de l'utilisateur (boutons, badge, commandes série)
void beginStreaming() {
  mode = M_PLAYING;
  LOG("Lecture lancée");
  startStream();
}

void acceptBadge(uint8_t s) {
  userActivity();
  if ((mode == M_WAIT_USER || mode == M_PLAYING) && s == stream) return;   // même badge laissé sur le lecteur
  if (mode == M_PLAYING || mode == M_DEMO) stopAudio();
  stream = s;
  mode = M_WAIT_USER;
  waitStartAt = millis();
  langChosen = playChosen = false;
  demoWaiting = false;
  LOGV("Badge accepté, flux ", s);
  LOG("  en attente : appuyer sur une langue ET sur lecture");
  ledStart(EV_TAG_OK);
}

void onLanguage(uint8_t idx) {         // idx = 0..NB_LANG-1
  userActivity();
  switch (mode) {
    case M_DEMO:      LOG("Démonstration arrêtée"); goIdle(); break;
    case M_IDLE:      LOG("Passer d'abord un badge"); break;
    case M_WAIT_USER:
      lang = idx + 1; langChosen = true;
      LOGV("Langue choisie ", lang);
      ledStart(EV_LANG);
      if (playChosen) beginStreaming(); else LOG("  appuyer maintenant sur lecture");
      break;
    case M_PLAYING:
      lang = idx + 1;
      LOGV("Langue ", lang);
      ledStart(EV_LANG);
      startStream();
      break;
  }
}

void onPlay() {
  userActivity();
  switch (mode) {
    case M_DEMO:      LOG("Démonstration arrêtée"); goIdle(); break;
    case M_IDLE:      LOG("Passer d'abord un badge"); break;
    case M_WAIT_USER:
      playChosen = true;
      LOG("Lecture demandée");
      ledStart(EV_RESUME);
      if (langChosen) beginStreaming(); else LOG("  appuyer maintenant sur une langue");
      break;
    case M_PLAYING:   togglePause(); break;
  }
}

// Appelée à chaque tour de loop() : démonstration, délais d'attente
void modeTick() {
  unsigned long now = millis();
  if (mode == M_DEMO) {
    if (demoWaiting && (long)(now - demoNextAt) >= 0) { demoWaiting = false; demoPlay(); }
    else if (!demoWaiting && now - lastPlayAt > DEMO_STALL_MS) { LOG("Démonstration : pas de fin de piste, on continue"); demoNext(); }
    return;
  }
  const unsigned long idleMs = (unsigned long)IDLE_TIMEOUT_MIN * 60000UL;
  if (mode == M_IDLE) {
    if (dfOk && battState != BATT_CRIT_STATE && now - lastActivityAt >= idleMs) demoStart();
  } else if (mode == M_WAIT_USER) {
    if (now - waitStartAt >= WAIT_USER_TIMEOUT_S * 1000UL) { LOG("Pas de réponse : retour en attente"); goIdle(); }
  } else if (mode == M_PLAYING) {
    if (paused && now - lastActivityAt >= idleMs) { LOG("Pause trop longue : retour en attente"); goIdle(); }
  }
}

void handlePlayerEvents() {
  if (!dfOk || !player.available()) return;
  uint8_t type = player.readType();
  int value = player.read();
  printDetail(type, value);            // tous les messages du module apparaissent dans le moniteur série
  if (type == DFPlayerPlayFinished) {
    unsigned long dur = millis() - lastPlayAt;
    LOGV("   durée depuis PLAY (ms) : ", dur);
    if (mode == M_DEMO) { demoNext(); return; }
    if (dur < MIN_PLAY_MS) {
      LOG("   fin trop rapide : ignorée (fichier vide ou illisible ? ou fin de la piste précédente)");
      return;
    }
    if (mode == M_PLAYING && !paused) {
      if (REPEAT_USER_STREAM) startStream();
      else { LOG("Flux terminé : retour en attente"); goIdle(); }
    }
  } else if (mode == M_DEMO && (type == DFPlayerError || type == TimeOut)) {
    LOG("Démonstration : piste inutilisable, flux suivant");
    demoLang = NB_LANG;
    demoNext();
  }
}

// Volume : mémorisé en EEPROM 3 s après le dernier changement (ménage la mémoire)
unsigned long volumeSaveAt = 0;
void changeVolume(int8_t delta) {
  int16_t v = (int16_t)volume + delta;
  if (v < 0) v = 0;
  if (v > VOLUME_MAX) v = VOLUME_MAX;
  volume = v;
  if (dfOk) player.volume(volume);
  LOGV("Volume ", volume);
  ledStart(EV_VOLUME);
  volumeSaveAt = millis() + 3000;
}
void saveVolumeIfDue() {
  if (volumeSaveAt && (long)(millis() - volumeSaveAt) >= 0) {
    EEPROM.update(0, volume);
    volumeSaveAt = 0;
  }
}

// ---------------------------------------------------------------- Badge
bool readStreamFromTag(uint8_t *uid, uint8_t uidLen, uint8_t &out) {
  if (!nfc.mifareclassic_AuthenticateBlock(uid, uidLen, BLOCK_STREAM, 0, KEY_A)) {   // 0 = clé A
    LOG("Auth refusée");
    return false;
  }
  uint8_t buf[16];
  if (!nfc.mifareclassic_ReadDataBlock(BLOCK_STREAM, buf)) {
    LOG("Lecture refusée");
    return false;
  }
  out = buf[0];                        // valeur binaire 1..99
  return out >= 1 && out <= 99;
}

void pollRfid() {
  static unsigned long lastPoll = 0, lockoutUntil = 0;
  unsigned long now = millis();
  if (!pn532Ok || now - lastPoll < RFID_POLL_MS || (long)(now - lockoutUntil) < 0) return;
  lastPoll = now;

  uint8_t uid[7], uidLen;
  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 60)) return;
  lockoutUntil = millis() + RFID_LOCKOUT_MS;

  uint8_t s;
  if (readStreamFromTag(uid, uidLen, s)) {
    acceptBadge(s);
  } else {
    LOG("Badge non reconnu");
    userActivity();
    if (mode == M_DEMO) goIdle();
    ledStart(EV_TAG_BAD);
    // player.playMp3Folder(3);         // décommenter pour un message vocal /MP3/0003.mp3
  }
}

// ---------------------------------------------------------------- Boutons
// Le PCF8574 tire INT à l'état bas à chaque changement d'une entrée (appui ET relâchement).
// INT ne revient à l'état haut qu'après une lecture du port : on lit donc dès l'interruption,
// puis une seconde fois après PAD_SETTLE_MS sans nouvelle interruption (anti-rebond).
volatile bool pcfInt = false;
void onPcfInt() { pcfInt = true; }     // rien d'autre ici : pas de Serial, pas d'I2C

uint8_t padBits = 0;                   // état stable, 1 = appuyé (bit 0 = P0 = langue 1 ...)
uint8_t padNewPress = 0;               // appuis détectés pendant ce tour de loop()
bool    pcfOk = false;
bool    pcfPending = false;
unsigned long pcfSettleAt = 0;
unsigned long volRepeatAt[2] = {0, 0};

bool readPcf(uint8_t &bits) {          // lit le port (ce qui efface INT) ; bits : 1 = appuyé
  if (Wire.requestFrom(PCF_ADDR, (uint8_t)1) != 1) {
    if (pcfOk) LOG("PCF8574 ne répond plus");
    pcfOk = false;
    return false;
  }
  uint8_t b = Wire.read();
  bits = PADS_ACTIVE_HIGH ? b : (uint8_t)~b;
  pcfOk = true;
  return true;
}

void pollPcf() {
  padNewPress = 0;
  unsigned long now = millis();
  // Interruption reçue, ou INT resté bas (interruption manquée) : lecture pour libérer INT
  if (pcfInt || digitalRead(PIN_PCF_INT) == LOW) {
    noInterrupts(); pcfInt = false; interrupts();
    uint8_t dummy;
    readPcf(dummy);
    pcfPending = true;
    pcfSettleAt = now + PAD_SETTLE_MS;
  }
  // Lecture définitive une fois les contacts stables
  if (pcfPending && (long)(now - pcfSettleAt) >= 0) {
    pcfPending = false;
    uint8_t bits;
    if (readPcf(bits)) {
      padNewPress = bits & ~padBits;   // bits passés de 0 à 1
      padBits = bits;
    }
  }
}

bool padPressed(uint8_t i) { return (padNewPress >> i) & 1; }   // vrai une seule fois à l'appui
bool padHeld(uint8_t i)    { return (padBits >> i) & 1; }

void pollTouch() {
  pollPcf();
  for (uint8_t i = 0; i < NB_LANG; i++) {
    if (padPressed(i)) onLanguage(i);
  }
  if (padPressed(PAD_PLAY)) onPlay();

  // Volume : un pas à l'appui, puis répétition tant que le bouton reste appuyé (n'arrête pas la démonstration)
  unsigned long now = millis();
  const uint8_t volPads[2] = {PAD_VOL_UP, PAD_VOL_DN};
  const int8_t  volStep[2] = {VOLUME_STEP, -(int8_t)VOLUME_STEP};
  for (uint8_t k = 0; k < 2; k++) {
    uint8_t i = volPads[k];
    if (padPressed(i)) { userActivity(); changeVolume(volStep[k]); volRepeatAt[k] = now + 500; }
    else if (padHeld(i) && (long)(now - volRepeatAt[k]) >= 0) {
      userActivity(); changeVolume(volStep[k]); volRepeatAt[k] = now + 200;
    }
  }

  if (padPressed(PAD_SPARE)) { userActivity(); LOG("Bouton de réserve"); /* à définir */ }
}

#if DEBUG
// Commandes de test tapées dans le moniteur série (majuscules acceptées)
//   t12 = simule un badge, flux 12     l2 = bouton langue 2     p = bouton lecture/pause
//   d = lance la démonstration tout de suite     v15 = volume 15     b = messages batterie oui/non
void handleSerialCommands() {
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c >= 'A' && c <= 'Z') c += 32;
  if (c == '\n' || c == '\r' || c == ' ') return;
  long n = (c == 'p' || c == 'b' || c == 'd' || c == '?') ? 0 : Serial.parseInt();
  switch (c) {
    case 't': if (n >= 1 && n <= 99) { LOGV("[test] badge simulé, flux ", n); acceptBadge(n); } break;
    case 'l': if (n >= 1 && n <= NB_LANG) { LOGV("[test] bouton langue ", n); onLanguage(n - 1); } break;
    case 'p': LOG("[test] bouton lecture"); onPlay(); break;
    case 'd': if (mode != M_IDLE) goIdle(); demoStart(); break;
    case 'v': if (n >= 0 && n <= VOLUME_MAX) changeVolume((int8_t)(n - volume)); break;
    case 'b': battLog = !battLog; LOGV("[test] messages batterie : ", battLog); break;
    default:
      LOG("Commandes : t<flux> badge | l<langue> | p lecture | d démo | v<0-30> volume | b batterie");
  }
}
#endif

// ---------------------------------------------------------------- Batterie
void pollBattery() {
  static unsigned long last = 0;
  static bool first = true;
  unsigned long now = millis();
  if (!adsOk || (!first && now - last < BATT_PERIOD_MS)) return;
  first = false; last = now;

  int32_t sum = 0;
  for (uint8_t i = 0; i < 8; i++) sum += ads.readADC_SingleEnded(0);   // ~8 ms chacune
  float vPin = ads.computeVolts(sum / 8);
  battVolts = vPin * DIVIDER_RATIO;
  if (battLog) {
    LOGV("Tension sur l'entrée ADS A0 = ", vPin);
    LOGV("Batterie V (avec ratio) = ", battVolts);
  }
#if !BATT_MONITOR
  return;                                           // mesure affichée, aucune alerte
#endif

  BattState prev = battState;
  if (battVolts < BATT_CRITICAL)                                   battState = BATT_CRIT_STATE;
  else if (battVolts < BATT_LOW && battState == BATT_OK)           battState = BATT_LOW_STATE;
  else if (battState == BATT_CRIT_STATE && battVolts > BATT_CRITICAL + BATT_HYST) battState = BATT_LOW_STATE;
  else if (battState == BATT_LOW_STATE  && battVolts > BATT_LOW + BATT_HYST)      battState = BATT_OK;

  if (battState != prev) {
    if (battState == BATT_LOW_STATE && prev == BATT_OK) {
      ledStart(EV_LOW_BATT); ledLastLowReminder = now;
      if (dfOk) player.playMp3Folder(1);           // /MP3/0001.mp3 ; le flux reprend à la fin
    } else if (battState == BATT_CRIT_STATE) {
      ledStart(EV_CRIT_BATT);
      goIdle();                                    // arrête flux et démonstration
      if (dfOk) player.playMp3Folder(2);           // /MP3/0002.mp3
    } else if (battState == BATT_OK) {
      LOG("Batterie normale");
    }
  }
}

// ---------------------------------------------------------------- setup / loop
void setup() {
#if DEBUG
  Serial.begin(115200);
  Serial.setTimeout(30);
  Serial.println();
  Serial.println(F("=== Tour de Babel v0.8.0 ==="));
  Serial.print(F("RAM libre au démarrage : ")); Serial.println(freeRam());
#endif

  uint8_t v = EEPROM.read(0);
  volume = (v <= VOLUME_MAX) ? v : VOLUME_DEFAULT;  // 255 = EEPROM vierge

  // 1) Le DFPlayer d'abord : LED éteintes, bus I2C encore silencieux, consommation minimale
  dfOk = dfInit();
  if (dfOk) {
    player.volume(volume);
    dfDiagnostics();
  } else {
    LOG("DFPlayer HORS SERVICE : le reste du programme démarre sans son.");
  }

  // 2) LED
  strip.begin(); strip.setBrightness(LED_BRIGHTNESS); strip.show();
  ledStart(dfOk ? EV_BOOT : EV_CRIT_BATT);          // rouge clignotant 6 s si le DFPlayer est absent

  // 3) Bus I2C : PCF8574, PN532, ADS1115
  Wire.begin();
  Wire.beginTransmission(PCF_ADDR);
  Wire.write(0xFF);                                 // toutes les broches en entrée (quasi-bidirectionnelles)
  pcfOk = (Wire.endTransmission() == 0);
  if (!pcfOk) LOG("PCF8574 introuvable");
  else readPcf(padBits);                            // état initial : un doigt déjà posé ne compte pas comme un appui
  pinMode(PIN_PCF_INT, INPUT_PULLUP);               // INT du PCF8574 est à drain ouvert
  attachInterrupt(digitalPinToInterrupt(PIN_PCF_INT), onPcfInt, FALLING);

  nfc.begin();
  pn532Ok = nfc.getFirmwareVersion() != 0;
  if (pn532Ok) nfc.SAMConfig();
  else LOG("PN532 introuvable");

  adsOk = ads.begin();
  if (adsOk) ads.setGain(GAIN_ONE);                 // ±4,096 V
  else LOG("ADS1115 introuvable");

#if !BATT_MONITOR
  LOG("ATTENTION : surveillance de la batterie DESACTIVEE (BATT_MONITOR 0)");
#endif
  pollBattery();
  randomSeed(analogRead(A1));                        // A1 libre : bruit pour le tirage au hasard
  lastActivityAt = millis();
  LOG("Démarrage terminé. Taper ? pour les commandes de test.");
}

void loop() {
  pollTouch();
#if DEBUG
  handleSerialCommands();
#endif
  modeTick();
  pollRfid();
  pollBattery();
  handlePlayerEvents();
  saveVolumeIfDue();
  ledUpdate();
}
