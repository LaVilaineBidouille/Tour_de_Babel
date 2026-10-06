/*
  Tour de Babel – La Villaine Bidouille
  Version brouillon v0.3 (Arduino Nano + PN532 + PCF8574 en I2C) – à valider sur le matériel

  Fonctions
  - Lit le secteur 2, bloc 0 (bloc absolu 8) d'un badge MIFARE Classic avec la clé A.
    Premier octet du bloc (valeur binaire 1..99) = numéro du flux à jouer.
  - 8 boutons au sommet de la tour, lus par un PCF8574 (I2C) : 4 langues, lecture/pause,
    volume +, volume -, 1 de réserve (broches P0..P7 dans cet ordre).
  - Le DFPlayer Mini joue  /LL/SSS.mp3  (LL = langue 01..04, SSS = flux 001..099).
  - Un ADS1115 mesure la tension de la batterie ; alerte sonore + LED si trop basse.
  - Des LED adressables (WS2812) jouent une séquence pour chaque événement.

  Bibliothèques (Gestionnaire de bibliothèques) :
    Adafruit PN532, DFRobotDFPlayerMini, Adafruit ADS1X15, Adafruit NeoPixel
    (Wire, SoftwareSerial et EEPROM sont fournies avec l'IDE)

  Carte SD du DFPlayer :
    /01/001.mp3 ... /01/099.mp3   langue 1
    /02/001.mp3 ... /02/099.mp3   langue 2   (etc.)
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
const uint8_t PIN_DF_RX     = 4;   // Nano reçoit  <- TX du DFPlayer
const uint8_t PIN_DF_TX     = 5;   // Nano émet    -> RX du DFPlayer (résistance 1 kΩ en série)
const uint8_t PIN_LED       = 6;

// Boutons, dans l'ordre : langues 1..4, lecture/pause, volume +, volume -, réserve
const uint8_t NB_LANG = 4;
const uint8_t PCF_ADDR   = 0x20;      // PCF8574 : 0x20..0x27 ; PCF8574A : 0x38..0x3F
const bool    PADS_ACTIVE_HIGH = false;  // true : capteur tactile qui passe à 1 ; false : bouton vers la masse
const uint8_t NB_PADS    = 8;
const unsigned long PAD_POLL_MS = 10;
const uint8_t PAD_PLAY   = NB_LANG;
const uint8_t PAD_VOL_UP = NB_LANG + 1;
const uint8_t PAD_VOL_DN = NB_LANG + 2;
const uint8_t PAD_SPARE  = NB_LANG + 3;

// LED
const uint8_t NB_LED = 12;
const uint8_t LED_BRIGHTNESS = 80;    // 0..255, limite la consommation

// Badge
const uint8_t BLOCK_STREAM = 8;       // secteur 2, bloc 0 = 2*4 + 0
uint8_t KEY_A[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};   // clé par défaut, à remplacer si besoin
const unsigned long RFID_POLL_MS    = 200;
const unsigned long RFID_LOCKOUT_MS = 3000;           // pause après une lecture (badge laissé sur le lecteur)

// Audio
const uint8_t VOLUME_DEFAULT = 20;    // 0..30
const uint8_t VOLUME_STEP    = 2;
const uint8_t VOLUME_MAX     = 30;

// Batterie (ADS1115, voie A0)
const float DIVIDER_RATIO = 2.0;      // Vbat = Vmesuré * ratio (selon le pont diviseur)
const float BATT_LOW      = 3.50;     // volts : alerte
const float BATT_CRITICAL = 3.30;     // volts : arrêt de la lecture
const float BATT_HYST     = 0.10;     // retour à la normale = seuil + hystérésis
const unsigned long BATT_PERIOD_MS  = 5000;
const unsigned long LOW_REMINDER_MS = 30000;          // rappel LED en batterie faible

// ---------------------------------------------------------------- Objets
Adafruit_PN532 nfc(PIN_PN532_IRQ, PIN_PN532_RST);
SoftwareSerial dfSerial(PIN_DF_RX, PIN_DF_TX);
DFRobotDFPlayerMini player;
Adafruit_ADS1115 ads;
Adafruit_NeoPixel strip(NB_LED, PIN_LED, NEO_GRB + NEO_KHZ800);

// ---------------------------------------------------------------- État
uint8_t  lang   = 1;      // 1..NB_LANG
uint8_t  stream = 0;      // 0 = pas encore de badge, sinon 1..99
uint8_t  volume = VOLUME_DEFAULT;
bool     paused = false;
bool     playing = false;

enum BattState : uint8_t { BATT_OK, BATT_LOW_STATE, BATT_CRIT_STATE };
BattState battState = BATT_OK;
float    battVolts = 0;
bool     adsOk = false;
bool     pn532Ok = false;

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
    // Ambiance de fond
    if (stream == 0) {
      fillAll(scale(0x201040, breathe(now, 4000)));            // en attente d'un badge
    } else if (paused) {
      fillAll(scale(0x804000, 60 + breathe(now, 3000) / 4));   // pause : ambre tamisé
    } else {
      uint32_t c = LANG_COLOR[lang - 1];                       // lecture : couleur de la langue
      for (uint8_t i = 0; i < NB_LED; i++) {
        uint8_t k = 70 + breathe(now + i * 200UL, 3000) * 185 / 255;
        strip.setPixelColor(i, scale(c, k));
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
void startStream() {
  if (stream == 0 || battState == BATT_CRIT_STATE) return;
  LOGV("Lecture langue ", lang); LOGV("  flux ", stream);
  player.playFolder(lang, stream);     // /LL/SSS.mp3
  playing = true;
  paused = false;
}

void handlePlayerEvents() {
  if (!player.available()) return;
  uint8_t type = player.readType();
  if (type == DFPlayerPlayFinished && playing && !paused) {
    startStream();                     // lecture en continu : on recommence le flux
  } else if (type == DFPlayerError) {
    LOGV("Erreur DFPlayer ", player.read());
  }
}

// Volume : mémorisé en EEPROM 3 s après le dernier changement (ménage la mémoire)
unsigned long volumeSaveAt = 0;
void changeVolume(int8_t delta) {
  int16_t v = (int16_t)volume + delta;
  if (v < 0) v = 0;
  if (v > VOLUME_MAX) v = VOLUME_MAX;
  volume = v;
  player.volume(volume);
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
    LOGV("Badge OK, flux ", s);
    if (s == stream && playing && !paused) return;   // même badge laissé sur le lecteur : on ne relance pas
    stream = s;
    ledStart(EV_TAG_OK);
    startStream();
  } else {
    LOG("Badge non reconnu");
    ledStart(EV_TAG_BAD);
    // player.playMp3Folder(3);         // décommenter pour un message vocal /MP3/0003.mp3
  }
}

// ---------------------------------------------------------------- Boutons
struct Pad { bool state; unsigned long changed; unsigned long nextRepeat; };
Pad pads[8];
const unsigned long DEBOUNCE_MS = 40;

// Lecture du PCF8574 : un octet, un bit par bouton (bit 0 = P0 = langue 1 ...)
uint8_t padBits = 0;                   // 1 = appuyé
bool    pcfOk = false;

void pollPcf() {
  static unsigned long last = 0;
  unsigned long now = millis();
  if (now - last < PAD_POLL_MS) return;
  last = now;
  if (Wire.requestFrom(PCF_ADDR, (uint8_t)1) == 1) {
    uint8_t b = Wire.read();
    padBits = PADS_ACTIVE_HIGH ? b : (uint8_t)~b;
    pcfOk = true;
  } else {
    if (pcfOk) LOG("PCF8574 ne répond plus");
    pcfOk = false;                     // on garde le dernier état connu
  }
}

bool padPressed(uint8_t i) {           // vrai une seule fois à l'appui
  Pad &p = pads[i];
  bool raw = (padBits >> i) & 1;
  unsigned long now = millis();
  if (raw != p.state && now - p.changed > DEBOUNCE_MS) {
    p.state = raw; p.changed = now;
    return raw;
  }
  return false;
}

void pollTouch() {
  pollPcf();
  for (uint8_t i = 0; i < NB_LANG; i++) {
    if (padPressed(i)) {
      lang = i + 1;
      LOGV("Langue ", lang);
      ledStart(EV_LANG);
      startStream();                   // sans effet tant qu'aucun badge n'a été lu
    }
  }
  if (padPressed(PAD_PLAY) && stream) {
    if (paused) { player.start(); paused = false; ledStart(EV_RESUME); LOG("Reprise"); }
    else        { player.pause(); paused = true;  ledStart(EV_PAUSE);  LOG("Pause"); }
  }

  // Volume : un pas à l'appui, puis répétition tant que le bouton reste appuyé
  unsigned long now = millis();
  const uint8_t volPads[2] = {PAD_VOL_UP, PAD_VOL_DN};
  const int8_t  volStep[2] = {VOLUME_STEP, -(int8_t)VOLUME_STEP};
  for (uint8_t k = 0; k < 2; k++) {
    uint8_t i = volPads[k];
    if (padPressed(i)) { changeVolume(volStep[k]); pads[i].nextRepeat = now + 500; }
    else if (pads[i].state && (long)(now - pads[i].nextRepeat) >= 0) {
      changeVolume(volStep[k]); pads[i].nextRepeat = now + 200;
    }
  }

  if (padPressed(PAD_SPARE)) { LOG("Bouton de réserve"); /* à définir */ }
}

// ---------------------------------------------------------------- Batterie
void pollBattery() {
  static unsigned long last = 0;
  static bool first = true;
  unsigned long now = millis();
  if (!adsOk || (!first && now - last < BATT_PERIOD_MS)) return;
  first = false; last = now;

  int32_t sum = 0;
  for (uint8_t i = 0; i < 8; i++) sum += ads.readADC_SingleEnded(0);   // ~8 ms chacune
  battVolts = ads.computeVolts(sum / 8) * DIVIDER_RATIO;
  LOGV("Batterie V = ", battVolts);

  BattState prev = battState;
  if (battVolts < BATT_CRITICAL)                                   battState = BATT_CRIT_STATE;
  else if (battVolts < BATT_LOW && battState == BATT_OK)           battState = BATT_LOW_STATE;
  else if (battState == BATT_CRIT_STATE && battVolts > BATT_CRITICAL + BATT_HYST) battState = BATT_LOW_STATE;
  else if (battState == BATT_LOW_STATE  && battVolts > BATT_LOW + BATT_HYST)      battState = BATT_OK;

  if (battState != prev) {
    if (battState == BATT_LOW_STATE && prev == BATT_OK) {
      ledStart(EV_LOW_BATT); ledLastLowReminder = now;
      player.playMp3Folder(1);                     // /MP3/0001.mp3 ; le flux reprend à la fin
    } else if (battState == BATT_CRIT_STATE) {
      ledStart(EV_CRIT_BATT);
      playing = false;                             // empêche la reprise automatique
      player.playMp3Folder(2);                     // /MP3/0002.mp3
    } else if (battState == BATT_OK) {
      LOG("Batterie normale");
    }
  }
}

// ---------------------------------------------------------------- setup / loop
void setup() {
#if DEBUG
  Serial.begin(115200);
#endif
  strip.begin(); strip.setBrightness(LED_BRIGHTNESS); strip.show();
  ledStart(EV_BOOT);

  for (uint8_t i = 0; i < NB_PADS; i++) pads[i] = {false, 0, 0};

  Wire.begin();
  Wire.beginTransmission(PCF_ADDR);
  Wire.write(0xFF);                                 // toutes les broches en entrée (quasi-bidirectionnelles)
  pcfOk = (Wire.endTransmission() == 0);
  if (!pcfOk) LOG("PCF8574 introuvable");
  nfc.begin();
  pn532Ok = nfc.getFirmwareVersion() != 0;
  if (pn532Ok) nfc.SAMConfig();
  else LOG("PN532 introuvable");

  adsOk = ads.begin();
  if (adsOk) ads.setGain(GAIN_ONE);                 // ±4,096 V
  else LOG("ADS1115 introuvable");

  uint8_t v = EEPROM.read(0);
  volume = (v <= VOLUME_MAX) ? v : VOLUME_DEFAULT;  // 255 = EEPROM vierge

  dfSerial.begin(9600);
  if (!player.begin(dfSerial)) LOG("DFPlayer introuvable (carte SD ?)");
  player.volume(volume);

  while (millis() < 1500) { ledUpdate(); }          // laisser jouer la séquence de démarrage
  pollBattery();
}

void loop() {
  pollTouch();
  pollRfid();
  pollBattery();
  handlePlayerEvents();
  saveVolumeIfDue();
  ledUpdate();
}
