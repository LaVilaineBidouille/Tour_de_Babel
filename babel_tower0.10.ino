/*
  Tour de Babel – La Villaine Bidouille
  Version brouillon v0.10.3 (Arduino Nano + PN532 + PCF8574 sur interruption + BUSY du DFPlayer) – à valider sur le matériel

  v0.10.0 : nouveau fonctionnement
           - la langue est une simple variable (LANG_DEFAULT au démarrage) ; les boutons langue la modifient
           - le bouton Play lit le badge à ce moment-là : le badge donne le dossier (flux), la variable langue donne le fichier
           - sans badge : une piste au hasard dans le dossier /MP3
           - alertes batterie : signalées par les LED seulement (plus de message sonore)
           - la lecture continue du lecteur de badge a disparu (plus de mode "attente après badge")

  Fonctionnement
  - Langue 1..4 : change la variable langue (effet au prochain lancement ; la piste en cours n'est pas interrompue).
  - Play, à l'arrêt : lecture du badge posé sur le lecteur.
        badge valide   -> joue /SS/00L.mp3   (SS = flux du badge, L = langue)
        pas de badge   -> joue /MP3/0NNN.mp3 (NNN au hasard entre 1 et RANDOM_TRACK_MAX)
        badge illisible ou invalide -> clignotement rouge, rien n'est joué
  - Play, pendant la lecture : pause / reprise.
  - Stop : arrête la lecture.
  - Volume +/- : réglage, répétition en maintenant l'appui.
  - Fin de piste : broche BUSY du DFPlayer -> retour à l'arrêt (ou boucle si REPEAT_STREAM).

  Câblage
  - Badge : secteur 2, bloc 0 (bloc absolu 8), clé A. Premier octet = numéro du flux (1..99), format au choix (BADGE_FORMAT).
  - PCF8574 (I2C) : P0..P3 langues 1..4, P4 play/pause, P5 volume +, P6 volume -, P7 stop. INT -> D2.
  - DFPlayer : TX -> D5 (RX du Nano), RX <- D6 (via 1 kΩ), BUSY (broche 16) -> D7.
  - LED WS2812 : D4. ADS1115 : batterie sur A0.
  - PN532 : I2C seulement (SDA/SCL, mode I2C sélectionné sur le module). IRQ et RST NON câblés :
    A2 et A3 restent libres (ne rien y brancher, la bibliothèque les manipule quand même).

  Bibliothèques : Adafruit PN532, DFRobotDFPlayerMini, Adafruit ADS1X15, Adafruit NeoPixel

  Carte SD du DFPlayer :
    - dossiers 01 à 99 (numéro du flux), fichiers commençant par 3 chiffres (001..004 = langues)
    - /SS/LLL.mp3 ; exemple : /04/002.mp3 = flux 4, langue 2
    - dossier /MP3 : pistes 0001.mp3 à 0NNN.mp3 (NNN = RANDOM_TRACK_MAX), jouées au hasard quand il n'y a pas de badge
  Bus I2C partagé : PCF8574 (0x20..0x27 ou 0x38..0x3F), PN532 (0x24), ADS1115 (0x48).
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
const uint8_t PIN_PCF_INT   = 2;   // INT du PCF8574 (INT0)
const uint8_t PIN_LED       = 4;
const uint8_t PIN_DF_RX     = 5;   // Nano reçoit  <- TX du DFPlayer
const uint8_t PIN_DF_TX     = 6;   // Nano émet    -> RX du DFPlayer (résistance 1 kΩ en série)
const uint8_t PIN_DF_BUSY   = 7;   // BUSY du DFPlayer : bas pendant la lecture, haut sinon
// PN532 : IRQ et RST ne sont pas câblés. Le constructeur I2C de la bibliothèque exige tout de même deux
// numéros de broche : on lui donne A2 et A3, laissées volontairement non connectées (ne rien y brancher).
const uint8_t PIN_PN532_RST = A2;   // non câblée
const uint8_t PIN_PN532_IRQ = A3;   // non câblée

// Boutons, dans l'ordre : langues 1..4, play/pause, volume +, volume -, stop
const uint8_t NB_LANG = 4;
const uint8_t LANG_DEFAULT = 1;       // langue au démarrage (1..NB_LANG)
const uint8_t PCF_ADDR   = 0x20;      // PCF8574 : 0x20..0x27 ; PCF8574A : 0x38..0x3F
const bool    PADS_ACTIVE_HIGH = false; // false : TTP223 en mode momentané, 0 quand touché
const unsigned long PAD_SETTLE_MS = 25; // stabilisation (anti-rebond) après une interruption
const unsigned long PCF_RETRY_MS  = 1000; // nouvel essai si le PCF8574 ne répond plus
const uint8_t PAD_PLAY   = NB_LANG;
const uint8_t PAD_VOL_UP = NB_LANG + 1;
const uint8_t PAD_VOL_DN = NB_LANG + 2;
const uint8_t PAD_STOP   = NB_LANG + 3;

// LED
const uint8_t NB_LED = 12;
const uint8_t LED_BRIGHTNESS = 80;      // 0..255
const unsigned long LED_FRAME_MS = 40;  // strip.show() coupe les interruptions ~0,4 ms : ~25 images/s
// Pixel de la guirlande qui marque la langue choisie (à l'arrêt) : à adapter au montage
const uint8_t LED_LANG_PIX[NB_LANG] = {0, 1, 2, 3};

// Badge
const uint8_t BLOCK_STREAM = 8;       // secteur 2, bloc 0
uint8_t KEY_A[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};   // clé par défaut, à remplacer si besoin
// Format du numéro de flux dans le bloc (octets du bloc 8, premier octet en premier) :
//   0 = binaire : 0x0C = flux 12
//   1 = décimal (BCD) : on écrit 12 en hexadécimal, soit 0x12 = flux 12 (chaque moitié d'octet doit être un chiffre 0..9)
//   2 = texte ASCII : "12" = 0x31 0x32 = flux 12 ; "7" = 0x37 = flux 7 (un ou deux chiffres)
#define BADGE_FORMAT 1
const uint16_t BADGE_READ_TIMEOUT_MS = 400;           // attente d'un badge après appui sur Play

// Lecture sans badge
const uint16_t RANDOM_TRACK_MAX = 10; // nombre de pistes présentes dans /MP3 (0001..0NNN) : à adapter à la carte SD

// Audio
const uint8_t VOLUME_DEFAULT = 10;    // 0..30
const uint8_t VOLUME_STEP    = 2;
const uint8_t VOLUME_MAX     = 30;
const unsigned long DF_POWERUP_MS = 1500;
const uint8_t DF_ATTEMPTS = 3;
const unsigned long DF_CMD_GAP_MS = 150;    // le DFPlayer perd les commandes trop rapprochées
const unsigned long BUSY_START_MS = 1000;   // après PLAY, BUSY met un moment à passer à l'état bas : on l'ignore
const unsigned long BUSY_DEBOUNCE_MS = 300; // BUSY doit rester haut ce temps pour valider la fin de piste

// Fonctionnement
const uint16_t PAUSE_TIMEOUT_MIN = 5;       // pause trop longue : retour à l'arrêt
const bool REPEAT_STREAM = false;           // true : la lecture recommence en boucle ; false : retour à l'arrêt

// Batterie (ADS1115, voie A0)
#define BATT_MONITOR 0                // 1 = alertes et blocage de la lecture selon la batterie ; 0 = mesure seulement
const float DIVIDER_RATIO = 4.0;
const float BATT_LOW      = 3.50;
const float BATT_CRITICAL = 3.30;
const float BATT_HYST     = 0.10;
const float BATT_SMOOTH   = 0.8;      // lissage : nouvelle valeur = 0,8 * ancienne + 0,2 * mesure
const unsigned long BATT_PERIOD_MS  = 5000;
const unsigned long LOW_REMINDER_MS = 30000;
// ---------------------------------------------------------------- Objets
Adafruit_PN532 nfc(PIN_PN532_IRQ, PIN_PN532_RST);
SoftwareSerial FPSerial(PIN_DF_RX, PIN_DF_TX);
DFRobotDFPlayerMini player;
Adafruit_ADS1115 ads;
Adafruit_NeoPixel strip(NB_LED, PIN_LED, NEO_GRB + NEO_KHZ800);

// ---------------------------------------------------------------- État
uint8_t  lang   = LANG_DEFAULT;   // 1..NB_LANG : variable modifiée par les boutons langue
uint8_t  stream = 0;              // flux (dossier) en cours, lu sur le badge ; 0 = aucun
bool     randomPlay = false;      // true : lecture au hasard dans /MP3
uint8_t  volume = VOLUME_DEFAULT;
bool     paused = false;
bool     playing = false;
unsigned long lastPlayAt = 0;     // dernier PLAY ou reprise envoyés
bool     battLog = false;

enum Mode : uint8_t { M_IDLE, M_PLAYING };
Mode     mode = M_IDLE;

// Doit rester ici, avant toute fonction : l'IDE génère les prototypes automatiquement
enum BadgeResult : uint8_t { BADGE_NONE, BADGE_OK, BADGE_BAD };
unsigned long lastActivityAt = 0;

enum BattState : uint8_t { BATT_OK, BATT_LOW_STATE, BATT_CRIT_STATE };
BattState battState = BATT_OK;
float    battVolts = 0;
bool     adsOk = false;
bool     pn532Ok = false;
bool     dfOk = false;

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

uint8_t breathe(unsigned long t, unsigned long period) {
  float x = (t % period) / (float)period;
  float v = x < 0.5 ? x * 2 : (1 - x) * 2;
  return (uint8_t)(v * v * 255);
}

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
      case EV_BOOT: {
        uint8_t n = (uint32_t)NB_LED * t / ledEventDuration;
        for (uint8_t i = 0; i <= n && i < NB_LED; i++) strip.setPixelColor(i, 0xFFB060);
        break; }
      case EV_TAG_OK: {
        uint8_t head = (t / 60) % NB_LED;
        for (uint8_t k = 0; k < 4; k++) strip.setPixelColor((head + NB_LED - k) % NB_LED, scale(0x00FF00, 255 - k * 60));
        break; }
      case EV_TAG_BAD:
        if ((t / 150) % 2 == 0) fillAll(0xFF0000);
        break;
      case EV_LANG:
        fillAll(scale(LANG_COLOR[lang - 1], 255 - (uint32_t)255 * t / ledEventDuration));
        break;
      case EV_PAUSE:  fillAll(0xFF8000); break;
      case EV_RESUME: fillAll(0x00FF00); break;
      case EV_VOLUME: {
        uint8_t n = ((uint16_t)NB_LED * volume + VOLUME_MAX - 1) / VOLUME_MAX;
        for (uint8_t i = 0; i < n && i < NB_LED; i++) strip.setPixelColor(i, 0xFFFFFF);
        break; }
      case EV_LOW_BATT:
      case EV_CRIT_BATT:
        if ((t / 250) % 2 == 0) fillAll(0xFF0000);
        break;
      default: break;
    }
  } else {
    switch (mode) {
      case M_IDLE:                                                 // à l'arrêt : respiration + langue choisie allumée
        fillAll(scale(0x201040, breathe(now, 4000)));
        strip.setPixelColor(LED_LANG_PIX[lang - 1], LANG_COLOR[lang - 1]);
        break;
      case M_PLAYING:
        if (paused) {
          fillAll(scale(0x804000, 60 + breathe(now, 3000) / 4));
        } else {
          uint32_t c = LANG_COLOR[lang - 1];
          for (uint8_t i = 0; i < NB_LED; i++) {
            uint8_t k = 70 + breathe(now + i * 200UL, 3000) * 185 / 255;
            strip.setPixelColor(i, scale(c, k));
          }
        }
        break;
    }
    if (battState == BATT_LOW_STATE && now - ledLastLowReminder > LOW_REMINDER_MS) {
      ledLastLowReminder = now;
      ledStart(EV_LOW_BATT);
    }
  }
  strip.show();
}

// ---------------------------------------------------------------- Audio
#if DEBUG
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
      Serial.print(F("DF: piste ")); Serial.print(value); Serial.println(F(" terminée (message série)"));
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

int freeRam() {
  extern int __heap_start, *__brkval;
  int v;
  return (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
}

// Après un échec de begin() : combien d'octets le DFPlayer envoie-t-il vraiment ?
//   0 octet : rien n'arrive sur RX (TX/RX inversés, masse non commune, module non alimenté)
//   1 à 9   : liaison perturbée
//   10+     : liaison OK, regarder la carte SD (FAT32, <= 32 Go)
void dfRawProbe() {
  while (FPSerial.available()) FPSerial.read();
  player.reset();
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
#else
void printDetail(uint8_t, int) {}
void dfRawProbe() {}
#endif

// Espace toutes les commandes envoyées au DFPlayer (appeler juste avant chaque player.xxx())
void dfGap() {
  static unsigned long last = 0;
  unsigned long d = millis() - last;
  if (d < DF_CMD_GAP_MS) delay(DF_CMD_GAP_MS - d);
  last = millis();
}

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
    LOG("  ECHEC. A vérifier : 1) câblage (TX module -> D5, RX module <- D6 via 1 kΩ) et masse commune");
    LOG("  2) carte SD insérée (FAT32, 32 Go max)   3) alimentation 5 V suffisante");
    dfRawProbe();
  }
  return false;
}

// ---------------------------------------------------------------- Lecture
bool canPlay() {
  if (!dfOk)                        { LOG("Pas de lecture : DFPlayer non démarré"); return false; }
  if (battState == BATT_CRIT_STATE) { LOG("Pas de lecture : batterie critique"); return false; }
  return true;
}

void sendPlay(uint8_t folder, uint8_t file) {
#if DEBUG
  Serial.print(F("PLAY dossier=")); Serial.print(folder);
  Serial.print(F(" fichier="));     Serial.print(file);
  Serial.print(F(" -> /")); if (folder < 10) Serial.print('0');
  Serial.print(folder); Serial.print('/');
  if (file < 100) Serial.print('0');
  if (file < 10)  Serial.print('0');
  Serial.print(file); Serial.println(F(".mp3"));
#endif
  dfGap();
  player.playFolder(folder, file);
  lastPlayAt = millis();
}

// Joue /MP3/NNNN.mp3
void sendPlayMp3(uint16_t n) {
#if DEBUG
  Serial.print(F("PLAY /MP3/"));
  if (n < 1000) Serial.print('0');
  if (n < 100)  Serial.print('0');
  if (n < 10)   Serial.print('0');
  Serial.print(n); Serial.println(F(".mp3"));
#endif
  dfGap();
  player.playMp3Folder(n);
  lastPlayAt = millis();
}

// Numéro de piste au hasard dans /MP3, différent de la précédente si possible
uint16_t pickRandomTrack() {
  static uint16_t last = 0;
  uint16_t n;
  do { n = random(1, RANDOM_TRACK_MAX + 1); } while (n == last && RANDOM_TRACK_MAX > 1);
  last = n;
  return n;
}

// Lance la lecture selon l'état : badge (stream + langue) ou hasard (/MP3)
bool startCurrent() {
  if (!canPlay()) return false;
  if (randomPlay) sendPlayMp3(pickRandomTrack());
  else            sendPlay(stream, lang);
  playing = true;
  paused = false;
  return true;
}

void stopAudio() {
  if (dfOk) { dfGap(); player.stop(); }
  playing = false;
  paused = false;
}

void userActivity() { lastActivityAt = millis(); }

void goIdle() {                        // retour à l'arrêt
  mode = M_IDLE;
  stopAudio();
  stream = 0;
  randomPlay = false;
  userActivity();
}

void beginPlayback(bool rnd, uint8_t s) {
  randomPlay = rnd;
  stream = s;
  mode = M_PLAYING;
  if (!startCurrent()) goIdle();       // lecture impossible (DFPlayer absent, batterie critique)
}

void togglePause() {
  if (mode != M_PLAYING) return;
  if (paused) {
    if (dfOk) { dfGap(); player.start(); }
    paused = false;
    lastPlayAt = millis();             // BUSY met un instant à repasser à l'état bas
    ledStart(EV_RESUME); LOG("Reprise");
  } else {
    if (dfOk) { dfGap(); player.pause(); }
    paused = true;
    ledStart(EV_PAUSE); LOG("Pause");
  }
}

// ---------------------------------------------------------------- Badge (lu seulement après appui sur Play)
bool readStreamFromTag(uint8_t *uid, uint8_t uidLen, uint8_t &out) {
  if (!nfc.mifareclassic_AuthenticateBlock(uid, uidLen, BLOCK_STREAM, 0, KEY_A)) {
    LOG("Auth refusée");
    return false;
  }
  uint8_t buf[16];
  if (!nfc.mifareclassic_ReadDataBlock(BLOCK_STREAM, buf)) {
    LOG("Lecture refusée");
    return false;
  }
#if BADGE_FORMAT == 0
  out = buf[0];
#elif BADGE_FORMAT == 1
  uint8_t hi = buf[0] >> 4, lo = buf[0] & 0x0F;
  if (hi > 9 || lo > 9) { LOG("Badge : octet non décimal (BCD)"); return false; }
  out = hi * 10 + lo;
#else
  if (buf[0] < '0' || buf[0] > '9') { LOG("Badge : premier caractère non numérique"); return false; }
  out = buf[0] - '0';
  if (buf[1] >= '0' && buf[1] <= '9') out = out * 10 + (buf[1] - '0');
#endif
#if DEBUG
  Serial.print(F("Badge : premier octet (hex) = 0x"));
  if (buf[0] < 16) Serial.print('0');
  Serial.println(buf[0], HEX);
#endif
  return out >= 1 && out <= 99;
}

BadgeResult readBadge(uint8_t &s) {
  if (!pn532Ok) { LOG("PN532 absent : pas de lecture de badge"); return BADGE_NONE; }
  uint8_t uid[7], uidLen;
  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, BADGE_READ_TIMEOUT_MS)) return BADGE_NONE;
  return readStreamFromTag(uid, uidLen, s) ? BADGE_OK : BADGE_BAD;
}

void playFromBadge() {
  uint8_t s = 0;
  BadgeResult r = readBadge(s);
  if (r == BADGE_BAD) {
    LOG("Badge non reconnu : rien n'est joué");
    ledStart(EV_TAG_BAD);
    return;
  }
  if (r == BADGE_OK) {
    LOGV("Badge lu, flux ", s);
    ledStart(EV_TAG_OK);
    beginPlayback(false, s);
  } else {
    LOG("Pas de badge : piste au hasard dans /MP3");
    ledStart(EV_RESUME);
    beginPlayback(true, 0);
  }
}

// ---------------------------------------------------------------- Actions de l'utilisateur
void onLanguage(uint8_t idx) {         // idx = 0..NB_LANG-1 : change seulement la variable langue
  userActivity();
  lang = idx + 1;
  LOGV("Langue ", lang);
  ledStart(EV_LANG);
}

void onPlay() {
  userActivity();
  if (mode == M_IDLE) playFromBadge();
  else                togglePause();
}

void onStop() {
  userActivity();
  if (mode == M_IDLE) { LOG("Stop : rien en cours"); return; }
  LOG("Stop");
  ledStart(EV_PAUSE);
  goIdle();
}

// Pause trop longue : retour à l'arrêt
void modeTick() {
  if (mode == M_PLAYING && paused &&
      millis() - lastActivityAt >= (unsigned long)PAUSE_TIMEOUT_MIN * 60000UL) {
    LOG("Pause trop longue : retour à l'arrêt");
    goIdle();
  }
}

// Fin de piste : broche BUSY (bas = lecture en cours). Ne dépend pas de la liaison série.
void pollBusy() {
  static unsigned long highSince = 0;
  unsigned long now = millis();
  if (mode != M_PLAYING || !playing || paused || now - lastPlayAt < BUSY_START_MS) { highSince = 0; return; }
  if (digitalRead(PIN_DF_BUSY) == LOW) { highSince = 0; return; }
  if (highSince == 0) { highSince = now; return; }
  if (now - highSince < BUSY_DEBOUNCE_MS) return;

  highSince = 0;
  playing = false;
  if (REPEAT_STREAM) {
    LOG("Fin de piste (BUSY) : on recommence");
    if (!startCurrent()) goIdle();
  } else {
    LOG("Fin de piste (BUSY) : retour à l'arrêt");
    goIdle();
  }
}

// Messages du DFPlayer : affichage et erreurs de fichier (la fin de piste vient de BUSY)
void handlePlayerEvents() {
  if (!dfOk || !player.available()) return;
  uint8_t type = player.readType();
  int value = player.read();
  printDetail(type, value);
  if (mode == M_PLAYING && type == DFPlayerError && value == FileMismatch) {
    LOG("Fichier introuvable : retour à l'arrêt");
    ledStart(EV_TAG_BAD);
    goIdle();
  }
}

// Volume : mémorisé en EEPROM 3 s après le dernier changement
unsigned long volumeSaveAt = 0;
void changeVolume(int8_t delta) {
  int16_t v = (int16_t)volume + delta;
  if (v < 0) v = 0;
  if (v > VOLUME_MAX) v = VOLUME_MAX;
  volume = v;
  if (dfOk) { dfGap(); player.volume(volume); }
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

// ---------------------------------------------------------------- Boutons (PCF8574 sur interruption)
// Le PCF8574 tire INT à l'état bas à chaque changement d'une entrée (appui ET relâchement) ;
// INT ne remonte qu'après une lecture du port. Le bus I2C n'est donc lu que si INT s'est déclenchée :
// une lecture tout de suite (libère INT), une seconde après PAD_SETTLE_MS (contacts stables).
// Lire l'état de la broche INT est gratuit (pas d'I2C) : filet de sécurité si une interruption est manquée.
volatile bool pcfInt = false;
void onPcfInt() { pcfInt = true; }     // rien d'autre ici : pas de Serial, pas d'I2C

uint8_t padBits = 0;                   // état stable, 1 = appuyé (bit 0 = P0 = langue 1 ...)
uint8_t padNewPress = 0;               // appuis détectés pendant ce tour de loop()
bool    pcfOk = false;
bool    pcfPending = false;
unsigned long pcfSettleAt = 0;
unsigned long pcfRetryAt = 0;
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

bool pcfInitPins() {                   // toutes les broches en entrée (quasi-bidirectionnelles)
  Wire.beginTransmission(PCF_ADDR);
  Wire.write(0xFF);
  return Wire.endTransmission() == 0;
}

void pollPcf() {
  padNewPress = 0;
  unsigned long now = millis();

  if (!pcfOk) {                        // reprise automatique si le PCF8574 est de retour
    if ((long)(now - pcfRetryAt) < 0) return;
    pcfRetryAt = now + PCF_RETRY_MS;
    uint8_t b;
    if (pcfInitPins() && readPcf(b)) { padBits = b; LOG("PCF8574 de nouveau en ligne"); }
    return;
  }

  if (pcfInt || digitalRead(PIN_PCF_INT) == LOW) {
    noInterrupts(); pcfInt = false; interrupts();
    uint8_t dummy;
    readPcf(dummy);                    // libère INT
    pcfPending = true;
    pcfSettleAt = now + PAD_SETTLE_MS;
  }
  if (pcfPending && (long)(now - pcfSettleAt) >= 0) {
    pcfPending = false;
    uint8_t bits;
    if (readPcf(bits)) {
      padNewPress = bits & ~padBits;
      padBits = bits;
    }
  }
}

bool padPressed(uint8_t i) { return (padNewPress >> i) & 1; }
bool padHeld(uint8_t i)    { return (padBits >> i) & 1; }

void pollTouch() {
  pollPcf();
  for (uint8_t i = 0; i < NB_LANG; i++) {
    if (padPressed(i)) onLanguage(i);
  }
  if (padPressed(PAD_PLAY)) onPlay();
  if (padPressed(PAD_STOP)) onStop();

  // Volume : un pas à l'appui, puis répétition tant que le bouton reste appuyé
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
}

#if DEBUG
// Commandes de test (moniteur série, 115200 bauds)
//   t12 = joue le flux 12 comme si un badge était lu   l2 = bouton langue 2
//   p = bouton play   x = bouton stop   r = play sans badge (piste au hasard)
//   v15 = volume 15   b = messages batterie oui/non   s = état
void handleSerialCommands() {
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c >= 'A' && c <= 'Z') c += 32;
  if (c == '\n' || c == '\r' || c == ' ') return;
  long n = (c == 'p' || c == 'x' || c == 'r' || c == 'b' || c == 's' || c == '?') ? 0 : Serial.parseInt();
  switch (c) {
    case 't': if (n >= 1 && n <= 99) { LOGV("[test] flux simulé ", n); userActivity(); if (mode == M_PLAYING) stopAudio(); beginPlayback(false, n); } break;
    case 'l': if (n >= 1 && n <= NB_LANG) { LOGV("[test] bouton langue ", n); onLanguage(n - 1); } break;
    case 'p': LOG("[test] bouton play"); onPlay(); break;
    case 'x': LOG("[test] bouton stop"); onStop(); break;
    case 'r': LOG("[test] lecture au hasard"); userActivity(); if (mode == M_PLAYING) stopAudio(); beginPlayback(true, 0); break;
    case 'v': if (n >= 0 && n <= VOLUME_MAX) changeVolume((int8_t)(n - volume)); break;
    case 'b': battLog = !battLog; LOGV("[test] messages batterie : ", battLog); break;
    case 's':
      LOGV("mode=", mode); LOGV("flux=", stream); LOGV("langue=", lang);
      LOGV("BUSY (0 = lecture)=", digitalRead(PIN_DF_BUSY)); LOGV("RAM libre=", freeRam());
      break;
    default:
      LOG("Commandes : t<flux> | l<langue> | p play | x stop | r hasard | v<0-30> volume | b batterie | s état");
  }
}
#endif

// ---------------------------------------------------------------- Batterie
void pollBattery() {
  static unsigned long last = 0;
  static bool first = true;
  unsigned long now = millis();
  if (!adsOk || (!first && now - last < BATT_PERIOD_MS)) return;
  last = now;

  int32_t sum = 0;
  for (uint8_t i = 0; i < 8; i++) sum += ads.readADC_SingleEnded(0);   // ~2 ms chacune à 475 SPS
  float vPin = ads.computeVolts(sum / 8);
  float raw = vPin * DIVIDER_RATIO;
  battVolts = first ? raw : battVolts * BATT_SMOOTH + raw * (1.0f - BATT_SMOOTH);
  first = false;
  if (battLog) {
    LOGV("Tension sur l'entrée ADS A0 = ", vPin);
    LOGV("Batterie V (lissée) = ", battVolts);
  }
#if !BATT_MONITOR
  return;
#endif

  BattState prev = battState;
  if (battVolts < BATT_CRITICAL)                                   battState = BATT_CRIT_STATE;
  else if (battVolts < BATT_LOW && battState == BATT_OK)           battState = BATT_LOW_STATE;
  else if (battState == BATT_CRIT_STATE && battVolts > BATT_CRITICAL + BATT_HYST) battState = BATT_LOW_STATE;
  else if (battState == BATT_LOW_STATE  && battVolts > BATT_LOW + BATT_HYST)      battState = BATT_OK;

  if (battState != prev) {
    if (battState == BATT_LOW_STATE && prev == BATT_OK) {
      ledStart(EV_LOW_BATT); ledLastLowReminder = now;
    } else if (battState == BATT_CRIT_STATE) {
      ledStart(EV_CRIT_BATT);
      goIdle();                                         // arrête la lecture ; canPlay() bloque tout nouveau départ
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
  Serial.println(F("=== Tour de Babel v0.10.3 ==="));
  Serial.print(F("RAM libre au démarrage : ")); Serial.println(freeRam());
#endif

  uint8_t v = EEPROM.read(0);
  volume = (v <= VOLUME_MAX) ? v : VOLUME_DEFAULT;  // 255 = EEPROM vierge
  if (lang < 1 || lang > NB_LANG) lang = 1;

  pinMode(PIN_DF_BUSY, INPUT_PULLUP);               // BUSY : bas pendant la lecture (le pull-up garantit un vrai niveau haut)

  // 1) Le DFPlayer d'abord : LED éteintes, bus I2C encore silencieux
  dfOk = dfInit();
  if (dfOk) {
    dfGap();
    player.volume(volume);
  } else {
    LOG("DFPlayer HORS SERVICE : le reste du programme démarre sans son.");
  }

  // 2) LED
  strip.begin(); strip.setBrightness(LED_BRIGHTNESS); strip.show();
  ledStart(dfOk ? EV_BOOT : EV_CRIT_BATT);

  // 3) Bus I2C : PCF8574, PN532, ADS1115
  Wire.begin();
#ifdef WIRE_HAS_TIMEOUT
  Wire.setWireTimeout(3000, true);                  // un bus bloqué ne fige plus le programme
#endif
  pcfOk = pcfInitPins();
  if (!pcfOk) LOG("PCF8574 introuvable (nouvel essai toutes les secondes)");
  else readPcf(padBits);                            // état initial : un doigt déjà posé ne compte pas comme un appui
  pinMode(PIN_PCF_INT, INPUT_PULLUP);               // INT du PCF8574 est à drain ouvert
  attachInterrupt(digitalPinToInterrupt(PIN_PCF_INT), onPcfInt, FALLING);

  nfc.begin();
  for (uint8_t attempt = 1; attempt <= 3 && !pn532Ok; attempt++) {   // sans RST : quelques essais, le module peut être lent à répondre
    pn532Ok = nfc.getFirmwareVersion() != 0;
    if (!pn532Ok) delay(200);
  }
  if (pn532Ok) nfc.SAMConfig();
  else LOG("PN532 introuvable (vérifier SDA/SCL, alimentation et mode I2C du module)");

  adsOk = ads.begin();
  if (adsOk) {
    ads.setGain(GAIN_ONE);                          // ±4,096 V
    ads.setDataRate(RATE_ADS1115_475SPS);           // mesures plus rapides, moins de blocage
  } else LOG("ADS1115 introuvable");

#if !BATT_MONITOR
  LOG("ATTENTION : surveillance de la batterie DESACTIVEE (BATT_MONITOR 0)");
#endif
  pollBattery();

  uint16_t seed = 0;                                // graine du tirage au hasard : bruit de A1 (libre) + micros()
  for (uint8_t i = 0; i < 16; i++) seed = (seed << 1) ^ analogRead(A1);
  randomSeed(seed ^ (uint16_t)micros());

  lastActivityAt = millis();
  LOGV("Langue au démarrage : ", lang);
  LOG("Démarrage terminé. Appuyer sur Play (avec ou sans badge). Taper ? pour les commandes de test.");
}

void loop() {
  pollTouch();
#if DEBUG
  handleSerialCommands();
#endif
  modeTick();
  pollBusy();
  pollBattery();
  handlePlayerEvents();
  saveVolumeIfDue();
  ledUpdate();
}
