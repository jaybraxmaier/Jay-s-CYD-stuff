/*
 * CYD Creatures  (ESP32-2432S028R / Cheap Yellow Display)
 *
 * An artificial-life simulation inspired by the 1996 game Creatures by
 * Millennium Interactive. Norns live on a 2D world, eat, sleep, seek
 * company, learn from being tickled or slapped, and breed.
 *
 * =====================================================================
 * BRAIN FILES ON THE SD CARD
 *   Each creature's genome is a real GEN-format-style file in /brains/.
 *   The format follows the one documented on the Creatures Wiki: a
 *   sequence of genes, each introduced by the ASCII marker "gene",
 *   carrying a type and subtype, then gene data, ending with "gend".
 *
 *     gene 00 00 ...data...     brain lobe gene
 *     gene 02 00 ...data...     creature gene (traits)
 *     gend                      end of genome
 *
 *   Three starter brains ship in /brains/ (generation 0). A creature is
 *   born from one; its brain is loaded from that file, then mutated and
 *   saved back as a child genome. So lineage accumulates on the card.
 *
 *   Genomes are 64 bytes of creature genes: 8 traits x 8 gene slots.
 *   Traits: size, speed, appetite, metabolism, tolerance, and three
 *   pigment values that decide the creature's colour.
 *
 * =====================================================================
 * LIFESPAN
 *   Each creature is given a random lifespan at birth, between LIFESPAN_MIN
 *   and LIFESPAN_MAX ticks. When age reaches it, the creature dies of old
 *   age. Lifespan is partly inherited: a child's lifespan is drawn around
 *   its parents' average, with a spread, so lineages can drift longer or
 *   shorter over generations.
 *
 * =====================================================================
 * CONTROLS
 *   Tap a creature        - select it
 *   TICKLE                - reward: the creature learns to repeat what it
 *                           is doing, and its mood rises
 *   SLAP                  - punish: it learns to avoid that behaviour
 *   BREED                 - pair the selected creature with the nearest
 *                           adult of the other sex, if both are willing
 *   BRAINS                - list the genomes found on the SD card
 *   FOOD                  - drop a piece of food at the top of the field
 *
 *   Creatures wander, eat when hungry, sleep when tired, and approach
 *   each other when lonely.
 *
 * =====================================================================
 * BOARD: single USB-C CYD => ST7789_2_DRIVER in User_Setup.h
 *        Micro-USB CYD     => ILI9341_DRIVER
 *
 * LIBRARIES: TFT_eSPI (CYD User_Setup.h), XPT2046_Touchscreen, SD
 * =====================================================================
 */

#include <SPI.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <math.h>
#include <string.h>
#include <stdio.h>      // sscanf
#include <stdlib.h>     // atof, atoi

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ================= HARDWARE =================
#define BL_PIN    21

#define TOUCH_CS   33
#define TOUCH_IRQ  36
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK  25

#define SD_CS      5

// Calibrated from four corner taps on this panel:
//   upper-left  268, 360     upper-right 3737, 372
//   lower-left  238,3811     lower-right 3839,3867
// Raw range is therefore about 240..3860 on both axes - NOT the 200..3700
// the stock constants assume. Using the stock values pushed readings past
// the end of the range, so map() clamped everything to the screen edge.
#define TS_MINX 240
#define TS_MAXX 3860
#define TS_MINY 350
#define TS_MAXY 3880

// No offset needed once the range is right.
#define TOUCH_OFFSET_X  0
#define TOUCH_OFFSET_Y  0

// ================= SCREEN (landscape) =================
#define SCR_W 320
#define SCR_H 240

TFT_eSPI tft = TFT_eSPI();
SPIClass touchSPI(HSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

// ================= COLOURS =================
#define COL_BG      0x0000      // black, for panels and buttons
#define COL_GRENDEL      0x39E7
#define COL_GRENDEL_LT   0x5A49
#define COL_GRENDEL_BELLY 0x2124
#define COL_MOUTH          0x0000
#define COL_GRENDEL_DK   0x2104
#define COL_BLOOD        0xF800
#define COL_PAUSED       0xFFE0
#define COL_FRIENDLY 0x07E0     // green, for the SAVE button
#define COL_WARN     0xFD20     // amber, for the NEW GAME button
#define COL_SKY     0x39E7      // pale grey-blue sky, like Albia
#define COL_GROUND  0x7B44      // earthy soil
#define COL_GRASS   0x2D23
#define COL_PANEL   0x10A2
#define COL_TEXT    0xFFFF
#define COL_DIM     0x7BEF
#define COL_ACCENT  0x07FF
#define COL_SEL     0xFFE0      // selection ring
#define COL_FOOD    0xF800
#define COL_EGG     0xFFE0

// ================= WORLD =================
// HUD holds two status lines and THREE ROWS of buttons (seven total).
// Derived: 22px of text + 3*20px buttons + 2*4px gaps + 6px margin = 96.
#define HUD_H    96
#define WORLD_H  (SCR_H - HUD_H)
#define GROUND_Y (WORLD_H - 18)

// The world is three screens wide and scrolls horizontally. Creatures
// live in WORLD_W coordinates; the view shows a SCR_W-wide window.
#define WORLD_W  (SCR_W * 3)

// ================= CREATURES =================
#define MAX_CREATURES 5

// Grendel tuning
#define GRENDEL_SPEED_ROAM 0.26f     // ambling pace - slower than a Norn
#define GRENDEL_DAMAGE     2.0f      // a slap, not a fatal bite
#define GRENDEL_ATTACK_MS  5000.0f   // it will not slap the same norn twice quickly
#define GRENDEL_REACH      22.0f     // it is wider now, so it reaches a little further
#define MAX_EGGS      4
#define MAX_FOOD      6

// Genome: 8 traits stored as bytes
#define TRAIT_COUNT 9

enum {
  T_SIZE = 0,
  T_SPEED,
  T_APPETITE,
  T_METABOLISM,
  T_TOLERANCE,
  T_PIG_R,
  T_PIG_G,
  T_PIG_B,
  T_PATTERN          // 0..2 belly pattern: plain, spots, stripe
};

struct Genome {
  uint8_t trait[TRAIT_COUNT];
  char    name[12];
  uint8_t generation;
  uint8_t reserved[3];
};

// A byte array we read/write to SD, matching the "gene ... gend" shape
#define GENOME_BYTES (4 + 2 + TRAIT_COUNT + 2 + 6 + 3 + 4)

struct Drive {
  float hunger;
  float tired;
  float lonely;
  float fear;
  float mood;
};

enum Activity { ACT_WANDER = 0, ACT_EAT, ACT_SLEEP, ACT_SOCIAL, ACT_FLEE };

struct Creature {
  float x, y;
  float vx;
  int   facing;              // -1 or +1
  bool  alive;
  bool  female;
  float ageTicks;
  float lifespan;            // random per creature
  float sizePx;              // derived from T_SIZE

  Genome genome;
  Drive  drive;
  Activity act;

  // Learning: reward association per activity. Positive = do more.
  float learn[5];

  // Appearance animation
  float walkPhase;
  int   blinkTimer;

  // Health, reduced by Grendel attacks. At zero the Norn dies.
  float health;
};

struct Egg {
  float x, y;
  float hatchTimer;
  bool  alive;
  Genome genome;
};

struct Food {
  float x, y;
  float vy;
  bool  alive;
};

static Creature creatures[MAX_CREATURES];
static Egg      eggs[MAX_EGGS];
static Food     food[MAX_FOOD];

// ---------------------------------------------------------------
// The Grendel: a predator. It roams the world on its own, and when
// a Norn comes within sight it gives chase. Contact costs the Norn
// health and drives its fear up, which makes it flee.
// ---------------------------------------------------------------
struct Grendel {
  float x, y;
  float vx;
  int   facing;
  bool  alive;
  bool  hunting;             // true once a Norn is in sight
  float walkPhase;
  float attackTimer;         // rate-limits contact damage
};

static Grendel grendel;
static bool    paused = false;

static int  selected = -1;
static bool showBrains = false;
static char statusMsg[40] = "";
static unsigned long msgUntil = 0;

// Scrolling camera: worldX is the left edge of the visible window
static float cameraX = 0.0f;
static float cameraTarget = 0.0f;

// Wandering creatures pick a far-off spot and walk to it, which covers
// far more ground than flipping direction at random.
static float wanderTargetX[MAX_CREATURES];

// The scenery is static, so it is drawn once into a RAM canvas and then
// blitted. Redrawing it every frame is what caused the flicker.
//
// The canvas is optional: if neither a 16bpp nor an 8bpp allocation
// succeeds, drawWorldBackdrop() falls back to drawing straight to the
// panel. It must NEVER return without drawing anything - an earlier
// version did that, and a failed allocation left the screen blank.
static TFT_eSprite bgSprite(&tft);
static bool  bgReady  = false;      // canvas allocated and usable
static int   bgCamX   = -9999;      // camera the cache was built at
#define BG_REBUILD_PX 12

static unsigned long lastTick = 0;
static unsigned long lastFrame = 0;

// ================= TUNING =================
// Lifespans are in simulation ticks (10 per second). The old values gave
// a life of 1.5 to 4.3 minutes; these give roughly 5 to 20 minutes.
#define LIFESPAN_MIN 18000
#define LIFESPAN_MAX 54000

// ================= SAVE / LOAD =================
// The world is written to /save.dat as a fixed-size record: a header,
// then every creature (genome, drives, age, lifespan, learning), then
// every egg and every food item. Loading restores exactly.
#define SAVE_PATH "/save.dat"
#define SAVE_TXT  "/save.txt"     // the readable save, which is what you edit
#define SAVE_MAGIC 0x43524431UL      // "CRD1"
#define SAVE_VER   2

struct SaveHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t creatureCount;
  uint16_t eggCount;
  uint16_t foodCount;
  int16_t  selected;

  // Sizes of the structs this file was written with. If the genome grows
  // a trait, sizeof(Creature) changes and an old save is rejected rather
  // than read as garbage.
  uint16_t creaturesSize;
  uint16_t eggsSize;
  uint16_t foodSize;
  uint32_t traitCount;
};

// ================= NAMED-VALUE CONVERSION =================
// The text save stores human-readable values; the genome stores bytes.
// These two tables are the only place the mapping lives, so a trait can
// only be rescaled in one spot.

// trait value 0..255 -> lo..hi
static float traitF_named(uint8_t v, float lo, float hi) {
  return lo + (hi - lo) * ((float)v / 255.0f);
}

// lo..hi -> trait value 0..255, clamped
static uint8_t traitB_named(float v, float lo, float hi) {
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  float t = (v - lo) / (hi - lo);
  int b = (int)(t * 255.0f + 0.5f);
  if (b < 0) b = 0;
  if (b > 255) b = 255;
  return (uint8_t)b;
}

static const char* patternName(uint8_t p) {
  switch (p % 3) {
    case 1:  return "spots";
    case 2:  return "stripes";
    default: return "plain";
  }
}

static uint8_t patternFromName(const char* n) {
  if (!n) return 0;
  if (strncmp(n, "spot", 4) == 0) return 1;
  if (strncmp(n, "stri", 4) == 0) return 2;
  return 0;
}


// ---- write the readable text save ----
static bool saveGameText() {
  File f = SD.open(SAVE_TXT, FILE_WRITE);
  if (!f) return false;

  f.println("# CYD Creatures save");
  f.println("# Edit any value, save the file, then press LOAD on the CYD.");
  f.println("# Omitted lines keep their defaults. Unknown lines are ignored.");
  f.println();
  f.printf("world %d\n", WORLD_W);
  f.println();

  for (int i = 0; i < MAX_CREATURES; i++) {
    Creature& c = creatures[i];
    f.printf("creature %d\n", i);
    f.printf("  alive %s\n",       c.alive ? "yes" : "no");
    f.printf("  sex %s\n",         c.female ? "female" : "male");
    f.printf("  name %s\n",        c.genome.name);
    f.printf("  generation %d\n",  (int)c.genome.generation);
    f.printf("  position %.1f %.1f\n", c.x, c.y);
    f.printf("  facing %s\n",      c.facing > 0 ? "right" : "left");
    f.printf("  age %.0f\n",       c.ageTicks);
    f.printf("  lifespan %.0f\n",  c.lifespan);
    f.printf("  health %.0f\n",    c.health);

    f.printf("  drives hunger=%.0f tired=%.0f lonely=%.0f fear=%.0f mood=%.0f\n",
             c.drive.hunger, c.drive.tired, c.drive.lonely,
             c.drive.fear, c.drive.mood);

    f.printf("  size %.1f\n",      traitF_named(c.genome.trait[T_SIZE], 22.0f, 38.0f));
    f.printf("  speed %.2f\n",     traitF_named(c.genome.trait[T_SPEED], 0.34f, 1.05f));
    f.printf("  appetite %.2f\n",  traitF_named(c.genome.trait[T_APPETITE], 0.5f, 2.2f));
    f.printf("  metabolism %.2f\n",traitF_named(c.genome.trait[T_METABOLISM], 0.6f, 1.8f));
    f.printf("  tolerance %.2f\n", traitF_named(c.genome.trait[T_TOLERANCE], 0.0f, 1.0f));
    f.printf("  body %d,%d,%d\n",
             (int)c.genome.trait[T_PIG_R],
             (int)c.genome.trait[T_PIG_G],
             (int)c.genome.trait[T_PIG_B]);
    f.printf("  pattern %s\n",     patternName(c.genome.trait[T_PATTERN]));
    f.println();
  }

  f.printf("eggs %d\n", MAX_EGGS);
  for (int i = 0; i < MAX_EGGS; i++) {
    Egg& e = eggs[i];
    f.printf("egg %d alive %s x %.1f y %.1f hatch %.0f gen %d\n",
             i, e.alive ? "yes" : "no", e.x, e.y, e.hatchTimer,
             (int)e.genome.generation);
  }

  f.println();
  f.printf("food %d\n", MAX_FOOD);
  for (int i = 0; i < MAX_FOOD; i++) {
    Food& d = food[i];
    f.printf("food %d alive %s x %.1f y %.1f\n",
             i, d.alive ? "yes" : "no", d.x, d.y);
  }

  f.close();
  return true;
}

static bool saveGame() {
  File f = SD.open(SAVE_PATH, FILE_WRITE);
  if (!f) return false;

  SaveHeader h;
  h.magic    = SAVE_MAGIC;
  h.version  = SAVE_VER;
  h.creatureCount = MAX_CREATURES;
  h.eggCount      = MAX_EGGS;
  h.foodCount     = MAX_FOOD;
  h.selected      = (int16_t)selected;
  h.creaturesSize = (uint16_t)sizeof(creatures);
  h.eggsSize      = (uint16_t)sizeof(eggs);
  h.foodSize      = (uint16_t)sizeof(food);
  h.traitCount    = (uint32_t)TRAIT_COUNT;

  // Also write the readable version, which is what the player edits.
  saveGameText();

  f.write((const uint8_t*)&h, sizeof(h));
  f.write((const uint8_t*)creatures, sizeof(creatures));
  f.write((const uint8_t*)eggs, sizeof(eggs));
  f.write((const uint8_t*)food, sizeof(food));
  f.close();
  return true;
}

// ---- read a line's value if it starts with key ----
// Returns the value text, or nullptr when the line does not match.
static const char* afterKey(const char* line, const char* key) {
  size_t k = strlen(key);
  const char* p = line;
  while (*p == ' ' || *p == '\t') p++;
  if (strncmp(p, key, k) != 0) return nullptr;
  p += k;
  if (*p != ' ' && *p != '\t' && *p != '\0') return nullptr;
  while (*p == ' ' || *p == '\t') p++;
  return p;
}

// ---- read the readable text save ----
// Applies line by line onto whatever population already exists, so a
// partial or damaged file still leaves a playable world.
static bool loadGameText() {
  File f = SD.open(SAVE_TXT, FILE_READ);
  if (!f) return false;

  int applied = 0;
  int skipped = 0;
  int slot = -1;          // current creature block
  int lineNo = 0;

  char line[160];
  while (f.available()) {
    int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = '\0';
    lineNo++;

    // strip a trailing CR
    int L = (int)strlen(line);
    while (L > 0 && (line[L - 1] == '\r' || line[L - 1] == ' ')) line[--L] = '\0';

    // skip blanks and comments
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0' || *p == '#') continue;

    const char* v;

    if ((v = afterKey(p, "creature")) != nullptr) {
      slot = atoi(v);
      if (slot < 0 || slot >= MAX_CREATURES) slot = -1;
      applied++;
      continue;
    }

    if (strncmp(p, "egg", 3) == 0 || strncmp(p, "food", 4) == 0 ||
        strncmp(p, "world", 5) == 0) {
      skipped++;
      continue;
    }

    if (slot < 0) { skipped++; continue; }

    Creature& c = creatures[slot];

    if ((v = afterKey(p, "alive")) != nullptr) {
      c.alive = (v[0] == 'y' || v[0] == 'Y' || v[0] == '1');
    } else if ((v = afterKey(p, "sex")) != nullptr) {
      c.female = (v[0] == 'f' || v[0] == 'F');
    } else if ((v = afterKey(p, "name")) != nullptr) {
      strncpy(c.genome.name, v, sizeof(c.genome.name) - 1);
      c.genome.name[sizeof(c.genome.name) - 1] = '\0';
    } else if ((v = afterKey(p, "generation")) != nullptr) {
      c.genome.generation = (uint8_t)atoi(v);
    } else if ((v = afterKey(p, "position")) != nullptr) {
      float px, py;
      if (sscanf(v, "%f %f", &px, &py) == 2) { c.x = px; c.y = py; }
      else skipped++;
    } else if ((v = afterKey(p, "facing")) != nullptr) {
      c.facing = (v[0] == 'r' || v[0] == 'R') ? 1 : -1;
    } else if ((v = afterKey(p, "age")) != nullptr) {
      c.ageTicks = atof(v);
    } else if ((v = afterKey(p, "lifespan")) != nullptr) {
      c.lifespan = atof(v);
      if (c.lifespan < 100.0f) c.lifespan = 100.0f;
    } else if ((v = afterKey(p, "health")) != nullptr) {
      c.health = atof(v);
      if (c.health < 1.0f)   c.health = 1.0f;
      if (c.health > 100.0f) c.health = 100.0f;
    } else if ((v = afterKey(p, "drives")) != nullptr) {
      float h, t, l, fe, mo;
      if (sscanf(v, "hunger=%f tired=%f lonely=%f fear=%f mood=%f",
                 &h, &t, &l, &fe, &mo) == 5) {
        c.drive.hunger = h; c.drive.tired = t; c.drive.lonely = l;
        c.drive.fear = fe;  c.drive.mood = mo;
      } else skipped++;
    } else if ((v = afterKey(p, "size")) != nullptr) {
      c.genome.trait[T_SIZE] = traitB_named(atof(v), 22.0f, 38.0f);
    } else if ((v = afterKey(p, "speed")) != nullptr) {
      c.genome.trait[T_SPEED] = traitB_named(atof(v), 0.34f, 1.05f);
    } else if ((v = afterKey(p, "appetite")) != nullptr) {
      c.genome.trait[T_APPETITE] = traitB_named(atof(v), 0.5f, 2.2f);
    } else if ((v = afterKey(p, "metabolism")) != nullptr) {
      c.genome.trait[T_METABOLISM] = traitB_named(atof(v), 0.6f, 1.8f);
    } else if ((v = afterKey(p, "tolerance")) != nullptr) {
      c.genome.trait[T_TOLERANCE] = traitB_named(atof(v), 0.0f, 1.0f);
    } else if ((v = afterKey(p, "body")) != nullptr) {
      int r, g, b;
      if (sscanf(v, "%d,%d,%d", &r, &g, &b) == 3) {
        c.genome.trait[T_PIG_R] = (uint8_t)constrain(r, 0, 255);
        c.genome.trait[T_PIG_G] = (uint8_t)constrain(g, 0, 255);
        c.genome.trait[T_PIG_B] = (uint8_t)constrain(b, 0, 255);
      } else skipped++;
    } else if ((v = afterKey(p, "pattern")) != nullptr) {
      c.genome.trait[T_PATTERN] = patternFromName(v);
    } else {
      skipped++;
    }
    applied++;
  }
  f.close();

  // Rebuild the drawn body from the (possibly edited) genome
  int sane = 0;
  for (int i = 0; i < MAX_CREATURES; i++) {
    if (!creatures[i].alive) continue;
    applyGenomeToBody(creatures[i]);
    if (creatures[i].sizePx < 5 || creatures[i].sizePx > 60) {
      creatures[i].alive = false;
      continue;
    }
    sane++;
  }

  Serial.printf("text save: %d line(s) applied, %d skipped, %d living\n",
                applied, skipped, sane);

  if (sane == 0) {
    Serial.println("text save: no usable creatures after load");
    return false;
  }
  return true;
}

static bool loadGame() {
  // Prefer the readable text save; fall back to the binary one.
  if (SD.exists(SAVE_TXT) && loadGameText()) return true;

  File f = SD.open(SAVE_PATH, FILE_READ);
  if (!f) return false;

  SaveHeader h;
  if (f.read((uint8_t*)&h, sizeof(h)) != sizeof(h)) {
    f.close();
    Serial.println("save: header unreadable, discarding");
    return false;
  }

  // Reject anything written by a different build. Reading a mismatched
  // layout straight into the live arrays fills them with garbage - which
  // is what froze the game when the genome gained a ninth trait.
  if (h.magic != SAVE_MAGIC || h.version != SAVE_VER ||
      h.creaturesSize != (uint16_t)sizeof(creatures) ||
      h.eggsSize      != (uint16_t)sizeof(eggs) ||
      h.foodSize      != (uint16_t)sizeof(food) ||
      h.traitCount    != (uint32_t)TRAIT_COUNT) {
    f.close();
    Serial.println("save: version or layout mismatch, discarding");
    return false;
  }

  // Read into a scratch buffer first, so a short read cannot corrupt the
  // live population.
  static Creature tmpC[MAX_CREATURES];
  static Egg      tmpE[MAX_EGGS];
  static Food     tmpF[MAX_FOOD];

  bool ok = true;
  if (f.read((uint8_t*)tmpC, sizeof(tmpC)) != sizeof(tmpC)) ok = false;
  if (f.read((uint8_t*)tmpE, sizeof(tmpE)) != sizeof(tmpE)) ok = false;
  if (f.read((uint8_t*)tmpF, sizeof(tmpF)) != sizeof(tmpF)) ok = false;
  f.close();

  if (!ok) {
    Serial.println("save: short read, discarding");
    return false;
  }

  // Only now touch the live arrays
  memcpy(creatures, tmpC, sizeof(creatures));
  memcpy(eggs,      tmpE, sizeof(eggs));
  memcpy(food,      tmpF, sizeof(food));

  // Sanity check every restored creature, and drop any that look wrong
  int sane = 0;
  for (int i = 0; i < MAX_CREATURES; i++) {
    Creature& c = creatures[i];
    bool bad =
      c.x < 0 || c.x > WORLD_W ||
      c.y < 0 || c.y > WORLD_H + 40 ||
      c.ageTicks < 0 || c.ageTicks > 1.0e7f ||
      c.lifespan < 0 || c.lifespan > 1.0e7f ||
      c.sizePx < 5 || c.sizePx > 60 ||
      c.drive.hunger < 0 || c.drive.hunger > 200 ||
      c.drive.tired  < 0 || c.drive.tired  > 200 ||
      c.drive.lonely < 0 || c.drive.lonely > 200 ||
      c.drive.mood   < 0 || c.drive.mood   > 200;

    if (bad) {
      Serial.printf("save: creature %d failed sanity, dropped\n", i);
      c.alive = false;
    } else if (c.alive) {
      applyGenomeToBody(c);
      sane++;
    }
  }

  if (sane == 0) {
    Serial.println("save: no usable creatures, discarding");
    return false;
  }

  selected = h.selected;
  if (selected >= MAX_CREATURES) selected = -1;

  Serial.printf("save: restored %d creature(s)\n", sane);
  return true;
}

// ================= FORWARD DECLARATIONS =================
static void startNewGame();
static void renderScenery(TFT_eSPI& tgt, int cam);
static void seedWorld();
static void spawnGrendel();
static void drawHudChrome();
static int  countLivingNorns();
static int  eggsAlive();
static void endGame();
static bool saveExists();
static void drawHudStatus();
static void buildScenery();
static void makeBrainPath(char* out, size_t n, uint8_t generation);

// ================= HELPERS =================
static int irand(int lo, int hi) {
  return lo + (int)(esp_random() % (uint32_t)(hi - lo + 1));
}

static float frand(float lo, float hi) {
  return lo + (hi - lo) * ((esp_random() % 10000) / 10000.0f);
}

static void say(const char* m, int ms = 2200) {
  strncpy(statusMsg, m, sizeof(statusMsg) - 1);
  statusMsg[sizeof(statusMsg) - 1] = '\0';
  msgUntil = millis() + ms;
}

// Map a trait byte 0..255 to a float range
static float traitF(uint8_t v, float lo, float hi) {
  return lo + (hi - lo) * (v / 255.0f);
}

// ================= GENOME FILE FORMAT =================
// Written in the spirit of the real GEN format: "gene" markers with a
// type and subtype, then data, then "gend". Ours is a simplified 26-byte
// record so it fits easily and stays human-readable in a hex editor.
//
//   offset 0..3   "gene"
//   offset 4      gene type      (2 = creature gene)
//   offset 5      gene sub-type  (0)
//   offset 6..13  the eight traits
//   offset 14..19 creature name
//   offset 20      generation
//   offset 21..23 reserved
//   offset 24..27 "gend"

static const uint32_t GEN_MARKER = 0x656E6567;   // "gene" little-endian
static const uint32_t GEN_END    = 0x646E6567;   // "gend"

static bool saveBrain(const Genome& g, const char* path) {
  uint8_t buf[GENOME_BYTES];
  memset(buf, 0, sizeof(buf));

  memcpy(buf, &GEN_MARKER, 4);
  buf[4] = 2;                       // creature gene
  buf[5] = 0;
  memcpy(buf + 6, g.trait, TRAIT_COUNT);
  memcpy(buf + 14, g.name, 6);
  buf[20] = g.generation;
  memcpy(buf + 24, &GEN_END, 4);

  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  f.write(buf, GENOME_BYTES);
  f.close();
  return true;
}

static bool loadBrain(const char* path, Genome& g) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;

  uint8_t buf[GENOME_BYTES];
  int n = f.read(buf, GENOME_BYTES);
  f.close();

  if (n < (int)GENOME_BYTES) return false;

  uint32_t marker;
  memcpy(&marker, buf, 4);
  if (marker != GEN_MARKER) return false;

  memcpy(g.trait, buf + 6, TRAIT_COUNT);
  memcpy(g.name, buf + 14, 6);
  g.name[6] = '\0';
  g.generation = buf[20];
  return true;
}

// Breed: each trait comes from one parent or the other, then mutates.
// That is real recombination at gene boundaries, not averaging.
static void breedGenome(const Genome& a, const Genome& b,
                        Genome& child, bool motherIsA) {
  for (int i = 0; i < TRAIT_COUNT; i++) {
    const Genome& from = (esp_random() & 1) ? a : b;
    int v = from.trait[i];

    // Mutation: 1 in 8 chance per trait of a nudge
    if ((esp_random() % 8) == 0) {
      int delta = irand(-22, 22);
      v += delta;
      if (v < 0) v = 0;
      if (v > 255) v = 255;
    }
    child.trait[i] = (uint8_t)v;
  }

  child.generation = (a.generation > b.generation ? a.generation : b.generation) + 1;

  // Name: blend the parents', so lineage is visible
  snprintf(child.name, sizeof(child.name), "%.3s%.2s%d",
           motherIsA ? b.name : a.name,
           motherIsA ? a.name : b.name,
           (int)child.generation);
}

// Random starter genome, used if no brain file is present
// Body hues. Picking from a palette gives a population that reads as
// distinct individuals rather than a muddy cluster of similar browns.
// Each entry is a base R/G/B plus how far each channel may wander.
struct HueBand { uint8_t r, g, b, spread; };

static const HueBand HUES[] = {
  { 220, 120,  90, 45 },   // russet
  { 140, 190, 110, 45 },   // moss
  { 120, 160, 225, 45 },   // sky
  { 225, 200, 110, 45 },   // sand
  { 190, 130, 210, 45 },   // heather
  { 110, 200, 200, 45 },   // sea
  { 235, 235, 235, 35 },   // white
  {  70,  70,  80, 40 }    // charcoal
};
#define HUE_COUNT (sizeof(HUES) / sizeof(HUES[0]))

static uint8_t jitterChannel(uint8_t base, uint8_t spread) {
  int v = base + irand(-(int)spread, (int)spread);
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  return (uint8_t)v;
}

static void randomGenome(Genome& g, int gen) {
  const HueBand& hue = HUES[irand(0, HUE_COUNT - 1)];

  for (int i = 0; i < TRAIT_COUNT; i++) {
    if (i == T_PIG_R)      g.trait[i] = jitterChannel(hue.r, hue.spread);
    else if (i == T_PIG_G) g.trait[i] = jitterChannel(hue.g, hue.spread);
    else if (i == T_PIG_B) g.trait[i] = jitterChannel(hue.b, hue.spread);
    else if (i == T_PATTERN) g.trait[i] = (uint8_t)irand(0, 2);
    else                   g.trait[i] = (uint8_t)irand(60, 200);
  }

  // Name by hue, so a founder's colour is legible in the BRAINS list
  static const char* HUE_NAMES[] = {
    "russet", "moss", "sky", "sand", "heath", "sea", "white", "char"
  };
  int idx = (int)(&hue - HUES);
  snprintf(g.name, sizeof(g.name), "%.5s%u", HUE_NAMES[idx], (unsigned)gen);
  g.generation = (uint8_t)gen;
}

// ================= POPULATION FROM THE CARD =================
// Every creature owns one genome file. The card is the population: boot
// loads whatever it finds, and a newborn writes its own file so it comes
// back next time.
//
// Files are named /brains/<gen>-<id>.gen. Founders use gen 0.

#define BRAIN_DIR "/brains"

// Build a fresh filename that is not already taken
static void makeBrainPath(char* out, size_t n, uint8_t generation) {
  for (int tries = 0; tries < 40; tries++) {
    int id = irand(1000, 9999);
    snprintf(out, n, BRAIN_DIR "/g%u-%d.gen", (unsigned)generation, id);
    if (!SD.exists(out)) return;
  }
  // Fall back to a fixed name if the random ones all collide
  snprintf(out, n, BRAIN_DIR "/g%u-x.gen", (unsigned)generation);
}

// Count .gen files in /brains
static int countBrainFiles() {
  File dir = SD.open(BRAIN_DIR);
  if (!dir) return 0;

  int n = 0;
  while (true) {
    File e = dir.openNextFile();
    if (!e) break;

    const char* nm = e.name();
    int len = strlen(nm);
    if (len > 4 && strcasecmp(nm + len - 4, ".gen") == 0) n++;
    e.close();
  }
  dir.close();
  return n;
}

// ================= SPAWNING =================
static float lifespanFromParents(float a, float b) {
  float base = (a + b) * 0.5f;
  float spread = base * 0.35f;        // 35% either way
  float v = base + frand(-spread, spread);
  if (v < LIFESPAN_MIN) v = LIFESPAN_MIN;
  if (v > LIFESPAN_MAX) v = LIFESPAN_MAX;
  return v;
}

static void applyGenomeToBody(Creature& c) {
  // Size 0..255 -> 22..38 px tall. These are the visible norns on the
  // playfield, so they need to read clearly at a glance.
  c.sizePx = traitF(c.genome.trait[T_SIZE], 22.0f, 38.0f);
  c.health = 100.0f;
}

static void spawnCreature(int slot, const Genome& g, bool female,
                          float life) {
  Creature& c = creatures[slot];

  c.genome = g;
  c.female = female;
  c.alive  = true;
  c.ageTicks = 0;
  c.lifespan = life;
  c.x = frand(30, WORLD_W - 30);
  c.y = GROUND_Y - 2;
  c.vx = 0;
  c.facing = (esp_random() & 1) ? 1 : -1;

  for (int i = 0; i < 5; i++) c.learn[i] = 0.0f;

  c.drive.hunger = frand(20, 60);
  c.drive.tired  = frand(0, 20);
  c.drive.lonely = frand(10, 40);
  c.drive.fear   = 0;
  c.drive.mood   = frand(40, 70);

  c.act = ACT_WANDER;
  wanderTargetX[slot] = frand(20, WORLD_W - 20);
  c.walkPhase = frand(0, 6.28f);
  c.blinkTimer = irand(40, 200);

  applyGenomeToBody(c);
}

// Build the population from whatever genomes are on the card. If there
// are fewer files than creatures, the shortfall is filled with fresh
// random founders and each is written out, so the population persists.
static void seedWorld() {
  Genome g;
  int loaded = 0;

  // Start with a random population size between 2 and MAX_CREATURES, so
  // games do not always begin identically.
  int want = irand(2, MAX_CREATURES);
  Serial.printf("target population: %d\n", want);

  File dir = SD.open(BRAIN_DIR);
  if (dir) {
    while (loaded < want) {
      File e = dir.openNextFile();
      if (!e) break;

      const char* nm = e.name();
      int len = strlen(nm);

      if (len > 4 && strcasecmp(nm + len - 4, ".gen") == 0) {
        // Build the full path (openNextFile gives the bare name)
        char path[48];
        snprintf(path, sizeof(path), BRAIN_DIR "/%s", nm);

        if (loadBrain(path, g)) {
          float life = frand(LIFESPAN_MIN, LIFESPAN_MAX);
          spawnCreature(loaded, g, (esp_random() & 1), life);
          loaded++;
        }
      }
      e.close();
    }
    dir.close();
  }

  // Fill any shortfall with new founders
  for (int i = loaded; i < want; i++) {
    randomGenome(g, 0);

    char path[48];
    makeBrainPath(path, sizeof(path), 0);
    saveBrain(g, path);

    float life = frand(LIFESPAN_MIN, LIFESPAN_MAX);
    spawnCreature(i, g, (esp_random() & 1), life);
  }

  // Any slots beyond the target start empty
  for (int i = want; i < MAX_CREATURES; i++) {
    creatures[i].alive = false;
  }

  // ---- guarantee both sexes are present ----
  // A random draw can give an all-male or all-female group, which makes
  // breeding impossible and gives no clue why. With two or more living
  // creatures, force at least one of each.
  if (want >= 2) {
    int males = 0, females = 0;
    for (int i = 0; i < want; i++) {
      if (!creatures[i].alive) continue;
      if (creatures[i].female) females++; else males++;
    }

    if (males == 0) {
      creatures[0].female = false;          // make one male
      Serial.println("sex balance: forced a male");
    } else if (females == 0) {
      creatures[1].female = true;           // make one female
      Serial.println("sex balance: forced a female");
    }
  }

  // If nothing at all was on the card, make sure at least three exist
  if (loaded == 0) {
    Serial.printf("no genomes found, created %d founders\n", want);
  } else {
    Serial.printf("loaded %d genome(s), population %d\n", loaded, want);
  }

  for (int i = 0; i < MAX_EGGS; i++) eggs[i].alive = false;
  for (int i = 0; i < MAX_FOOD; i++) food[i].alive = false;

  buildScenery();
}

// ================= DRAWING: THE NORN =================
// Drawn to resemble the original: a pear-shaped body, a large domed
// head with two eyes and a small snout, stubby arms and two legs.
// Colours come from the pigment genes, so inheritance is visible.

// ================= DRAWING: THE NORN =================
// Shaped after the originals: a pear body that narrows at the waist,
// a domed head with a snout, tufted ears, a tail, stubby arms with paws,
// and two legs. Colours come from the pigment genes, so a child visibly
// inherits its hue from whichever parent's pigment genes it took.



static void drawNorn(const Creature& c, int x, int y, bool isSelected) {
  uint8_t pr = c.genome.trait[T_PIG_R];
  uint8_t pg = c.genome.trait[T_PIG_G];
  uint8_t pb = c.genome.trait[T_PIG_B];

  // Three shades from one pigment, so the body reads as rounded
  uint8_t lr = (uint8_t)(pr + (255 - pr) * 0.35f);
  uint8_t lg = (uint8_t)(pg + (255 - pg) * 0.35f);
  uint8_t lb = (uint8_t)(pb + (255 - pb) * 0.35f);

  uint16_t body   = tft.color565(pr, pg, pb);
  uint16_t hi     = tft.color565(lr, lg, lb);              // lit side
  uint16_t shade  = tft.color565(pr / 2, pg / 2, pb / 2);  // shadowed side
  uint16_t belly  = tft.color565(255, 245, 225);           // cream underside
  uint16_t outline= tft.color565(pr / 4, pg / 4, pb / 4);

  float h = c.sizePx;
  int bodyW = (int)(h * 0.66f);
  int headR = (int)(h * 0.36f);

  float bob = sinf(c.walkPhase) * (h * 0.045f);
  int by = y + (int)bob;

  int bodyH = (int)(h * 0.64f);
  int bodyTop = by - bodyH;

  // ---- tail: a curve trailing behind ----
  int tailDir = -c.facing;
  for (int s = 0; s < (int)(h * 0.5f); s++) {
    int tx = x + tailDir * (int)(bodyW * 0.45f) + tailDir * s / 2;
    int ty = by - (int)(bodyH * 0.25f) - s / 3;
    tft.drawPixel(tx, ty, body);
    tft.drawPixel(tx, ty + 1, shade);
  }

  // ---- legs ----
  int legLen = (int)(h * 0.28f);
  int swing = (int)(sinf(c.walkPhase) * (h * 0.12f));

  for (int leg = 0; leg < 2; leg++) {
    int lx = x + ((leg == 0) ? -1 : 1) * (bodyW / 3)
               + ((leg == 0) ? swing : -swing);
    // Thigh
    tft.fillRect(lx - 1, by - legLen / 3, 4, legLen / 3 + 2, body);
    // Shin
    tft.fillRect(lx, by, 3, legLen, shade);
    // Foot
    tft.fillRect(lx - 1 + c.facing * 2, by + legLen, 5, 2, outline);
  }

  // ---- body: a pear that narrows at the shoulders ----
  for (int i = 0; i < bodyH; i++) {
    float t = (float)i / bodyH;
    // Waist pinch around a third of the way down, widest at the base
    float wf = 0.58f + 0.52f * t;
    if (t < 0.35f) wf = 0.58f + t * 1.1f;
    int w = (int)(bodyW * wf);
    int px = x - w / 2;
    int rowY = bodyTop + i;

    // Base fill
    tft.fillRect(px, rowY, w, 1, body);

    // A soft gradient across the body, drawn as horizontal spans rather
    // than per-pixel. Each shadow band is two interleaved rows of the
    // neighbouring colour, which reads as a gradient but costs about five
    // fillRect calls per row instead of one drawPixel per pixel.
    {
      int bandHi   = w - (int)(w * 0.30f);          // lit portion
      int bandMid  = (int)(w * 0.30f);
      int bandLow  = (int)(w * 0.30f);

      if (c.facing > 0) {
        // Light falls on the right: highlight first, shadow last
        tft.fillRect(px, rowY, bandHi, 1, hi);
        if (bandMid > 0) {
          // Interleave with the previous rows to fake a blend
          tft.fillRect(px + bandHi, rowY, bandMid, 1,
                       ((rowY & 1) ? body : hi));
        }
        if (bandLow > 0) tft.fillRect(px + bandHi + bandMid, rowY, bandLow, 1, shade);
      } else {
        // Light falls on the left
        if (bandLow > 0) tft.fillRect(px, rowY, bandLow, 1, shade);
        if (bandMid > 0) {
          tft.fillRect(px + bandLow, rowY, bandMid, 1,
                       ((rowY & 1) ? body : hi));
        }
        tft.fillRect(px + bandLow + bandMid, rowY, bandHi, 1, hi);
      }
    }

    // Fur tufts along the back edge, every few rows
    if ((i & 3) == 1) {
      int backX = (c.facing > 0) ? px - 1 : px + w;
      tft.drawPixel(backX, rowY, body);
    }
  }

  // Cream belly patch, with a pattern from the genome so a child can
  // inherit its mother's markings.
  int bw2 = bodyW / 3;
  int bh2 = (int)(bodyH * 0.42f);
  int bx0 = x - bw2 / 2;
  int by0 = by - bh2 - (int)(bodyH * 0.08f);

  tft.fillRoundRect(bx0, by0, bw2, bh2, 3, belly);

  uint8_t pattern = c.genome.trait[T_PATTERN] % 3;

  if (pattern == 1) {
    // Spots
    for (int sy_ = by0 + 3; sy_ < by0 + bh2 - 2; sy_ += 5) {
      for (int sx_ = bx0 + 2; sx_ < bx0 + bw2 - 2; sx_ += 5) {
        tft.drawPixel(sx_, sy_, body);
      }
    }
  } else if (pattern == 2) {
    // Vertical stripe
    for (int sy_ = by0 + 1; sy_ < by0 + bh2 - 1; sy_ += 2) {
      tft.drawPixel(x, sy_, body);
      if (bw2 > 5) tft.drawPixel(x - 2, sy_, body);
    }
  }

  // Toes: three small darker marks on each foot
  {
    int legLen2 = (int)(h * 0.28f);
    for (int leg = 0; leg < 2; leg++) {
      int lx = x + ((leg == 0) ? -1 : 1) * (bodyW / 3);
      int fx0 = lx - 1 + c.facing * 2;
      int fy0 = by + legLen2 + 1;
      tft.drawPixel(fx0 + 1, fy0, outline);
      tft.drawPixel(fx0 + 3, fy0, outline);
    }
  }

  // ---- arms with paws ----
  int armY = bodyTop + (int)(bodyH * 0.28f);
  int armLen = (int)(h * 0.24f);
  int armSwing = (int)(cosf(c.walkPhase) * (h * 0.05f));

  for (int a = 0; a < 2; a++) {
    int side = (a == 0) ? -1 : 1;
    int ax = x + side * (bodyW / 2 - 1);
    int ay = armY + (a == 0 ? armSwing : -armSwing);

    tft.fillRect(ax - 1, ay, 3, armLen, body);
    tft.fillCircle(ax, ay + armLen, 2, hi);      // paw
  }

  // ---- head ----
  int headY = bodyTop - headR + 1;

  // Neck, so the head is not floating
  tft.fillRect(x - 3, headY + headR - (int)(h * 0.06f), 6,
               (int)(h * 0.12f), body);

  tft.fillCircle(x, headY, headR, body);

  // Dome highlight, upper-facing side
  tft.fillCircle(x - c.facing * headR / 3, headY - headR / 3,
                 headR / 2, hi);

  // Cheeks, slightly darker, giving the face some shape
  tft.fillCircle(x - (int)(headR * 0.5f), headY + headR / 3, headR / 4, shade);
  tft.fillCircle(x + (int)(headR * 0.5f), headY + headR / 3, headR / 4, shade);

  // Snout, pointing the way it faces
  int snX = x + (c.facing * headR * 3) / 4;
  int snY = headY + headR / 4;
  tft.fillCircle(snX, snY, headR / 3, belly);
  tft.drawPixel(snX + c.facing, snY - headR / 6, outline);   // nostril

  // Mouth, a small curve beneath the snout
  tft.drawPixel(snX, snY + headR / 3, outline);

  // Eyes with pupils
  int eyeY = headY - headR / 5;
  int eyeDX = (int)(headR * 0.45f);
  bool blinking = (c.blinkTimer < 4);

  for (int e = 0; e < 2; e++) {
    int ex = x + ((e == 0) ? -eyeDX : eyeDX);

    if (blinking) {
      tft.drawFastHLine(ex - 2, eyeY, 5, outline);       // closed
    } else {
      tft.fillCircle(ex, eyeY, 3, 0xFFFF);               // white
      tft.fillCircle(ex + c.facing, eyeY, 2, 0x0000);    // pupil
      tft.drawPixel(ex + c.facing, eyeY - 1, 0xFFFF);    // glint
    }

    // Brow, which lifts with mood
    int browY = eyeY - 5 - (int)(c.drive.mood / 40.0f);
    tft.drawFastHLine(ex - 3, browY, 7, outline);
  }

  // ---- tufted ears ----
  for (int e = 0; e < 2; e++) {
    int side = (e == 0) ? -1 : 1;
    int ex = x + side * (headR - 1);
    int ey = headY - headR / 2;
    tft.fillTriangle(ex, ey, ex + side * 5, ey - 7, ex + side * 2, ey + 2, body);
    tft.drawPixel(ex + side * 4, ey - 5, hi);
  }

  // ---- sleeping indicator ----
  if (c.act == ACT_SLEEP) {
    tft.setTextSize(1);
    tft.setTextColor(0xFFFFFF, COL_SKY);
    tft.setCursor(x + headR + 4, headY - headR - 8);
    tft.print("z");
    tft.setCursor(x + headR + 8, headY - headR - 14);
    tft.print("z");
  }

  // ---- mood line above a happy creature ----
  if (c.drive.mood > 82 && c.act != ACT_SLEEP) {
    tft.drawPixel(x - 3, bodyTop - (int)(h * 0.95f), 0xF81F);
    tft.drawPixel(x,     bodyTop - (int)(h * 1.0f),  0xF81F);
    tft.drawPixel(x + 3, bodyTop - (int)(h * 0.95f), 0xF81F);
  }

  if (isSelected) {
    tft.drawCircle(x, by - (int)(h * 0.35f), (int)(h * 0.95f), COL_SEL);
    tft.drawCircle(x, by - (int)(h * 0.35f), (int)(h * 0.95f) + 1,
                   tft.color565(120, 110, 0));
  }
}

static void drawEggAt(const Egg& e, int sx) {
  tft.fillCircle(sx, (int)e.y, 5, COL_EGG);
  tft.drawCircle(sx, (int)e.y, 6, 0x0000);
  tft.drawPixel(sx - 1, (int)e.y - 1, 0x0000);
}

static void drawFoodAt(const Food& f, int sx) {
  tft.fillCircle(sx, (int)f.y, 3, COL_FOOD);
  tft.drawPixel(sx, (int)f.y - 3, COL_GRASS);
}

// ================= WORLD RENDER =================
// Scenery is drawn from a fixed random seed-backed table so it does not
// shimmer between frames. Built once at boot into arrays.
#define MAX_HILLS   14
#define MAX_TREES  30
#define MAX_ROCKS  26
#define MAX_FLOWERS 42

struct Scene { int x, y, w, h; uint8_t kind; };
static Scene hills[MAX_HILLS];
static Scene trees[MAX_TREES];
static Scene rocks[MAX_ROCKS];
static Scene flowers[MAX_FLOWERS];

static void buildScenery() {
  // Rolling hills behind the horizon
  for (int i = 0; i < MAX_HILLS; i++) {
    hills[i].x = (i * WORLD_W) / (MAX_HILLS - 1) + irand(-20, 20);
    hills[i].w = irand(70, 130);
    hills[i].h = irand(22, 44);
    hills[i].y = GROUND_Y;
  }

  // Trees standing along the horizon
  for (int i = 0; i < MAX_TREES; i++) {
    trees[i].x = 14 + (i * (WORLD_W - 28)) / (MAX_TREES - 1) + irand(-8, 8);
    trees[i].h = irand(18, 34);
    trees[i].w = 3;
    trees[i].y = GROUND_Y;
    trees[i].kind = (uint8_t)irand(0, 1);   // 0 = round crown, 1 = conical
  }

  // Rocks and flowers on the ground plane
  for (int i = 0; i < MAX_ROCKS; i++) {
    rocks[i].x = irand(10, WORLD_W - 10);
    rocks[i].y = irand(GROUND_Y + 3, WORLD_H - 4);
    rocks[i].w = irand(6, 13);
  }
  for (int i = 0; i < MAX_FLOWERS; i++) {
    flowers[i].x = irand(8, WORLD_W - 8);
    flowers[i].y = irand(GROUND_Y + 2, WORLD_H - 4);
    flowers[i].kind = (uint8_t)irand(0, 2);  // colour choice
  }
}

// World-to-screen helper: everything in world coordinates is drawn
// shifted left by the camera position.
static inline int wx2sx(float worldX) { return (int)(worldX - cameraX); }

// Renders the static scenery at a given camera position into the
// current draw target (the sprite when caching, the panel otherwise).
static void renderBackdropAt(int cam) {

  // ---- sky, with a soft band gradient toward the horizon ----
  for (int y = 0; y < GROUND_Y; y++) {
    // Interpolate from a deeper blue at the top to a pale one at the horizon
    float t = (float)y / (float)GROUND_Y;
    uint8_t r = (uint8_t)(0x6C + (0xB8 - 0x6C) * t);
    uint8_t g = (uint8_t)(0x8C + (0xCF - 0x8C) * t);
    uint8_t b = (uint8_t)(0xC8 + (0xE8 - 0xC8) * t);
    tft.drawFastHLine(0, y, SCR_W, tft.color565(r, g, b));
  }

  // ---- distant hills ----
  for (int i = 0; i < MAX_HILLS; i++) {
    for (int dx = -hills[i].w / 2; dx <= hills[i].w / 2; dx++) {
      float t = (float)dx / (hills[i].w / 2.0f);
      int h = (int)(hills[i].h * (1.0f - t * t));
      if (h <= 0) continue;
      int px = wx2sx(hills[i].x + dx);
      if (px < 0 || px >= SCR_W) continue;
      tft.drawFastVLine(px, GROUND_Y - h, h, 0x4A65);
    }
  }

  // ---- ground ----
  tft.fillRect(0, GROUND_Y, SCR_W, WORLD_H - GROUND_Y, COL_GROUND);

  // Soil speckle, spaced across the whole world so it scrolls with it
  for (int i = 0; i < 300; i++) {
    int wx_ = (i * 37) % WORLD_W;
    int sx = wx2sx(wx_);
    if (sx < 0 || sx >= SCR_W) continue;
    int sy = GROUND_Y + 3 + ((i * 53) % (WORLD_H - GROUND_Y - 4));
    tft.drawPixel(sx, sy, 0x6203);
  }

  // Grass tufts along the horizon, in world space
  int firstTuft = (cam / 5) * 5;
  for (int wxg = firstTuft; wxg < cam + SCR_W + 5; wxg += 5) {
    int sx = wx2sx(wxg);
    int h = 2 + ((wxg / 5) % 3);
    if (sx < -2 || sx > SCR_W + 2) continue;
    tft.drawLine(sx, GROUND_Y, sx + 1, GROUND_Y - h, COL_GRASS);
  }

  // ---- trees ----
  for (int i = 0; i < MAX_TREES; i++) {
    Scene& t = trees[i];

    int tx = wx2sx(t.x);
    if (tx < -t.h || tx > SCR_W + t.h) continue;      // off screen

    // Trunk
    tft.fillRect(tx, t.y - t.h / 3, 3, t.h / 3, 0x4200);

    // Crown
    if (t.kind == 0) {
      tft.fillCircle(tx + 1, t.y - t.h / 3 - t.h / 4, t.h / 3, 0x24E4);
      tft.fillCircle(tx + 1, t.y - t.h / 3 - t.h / 2, t.h / 4, 0x2D45);
    } else {
      tft.fillTriangle(tx - t.h / 4, t.y - t.h / 3,
                       tx + 1 + t.h / 4, t.y - t.h / 3,
                       tx + 1, t.y - t.h, 0x24E4);
    }
  }

  // ---- rocks ----
  for (int i = 0; i < MAX_ROCKS; i++) {
    int rx = wx2sx(rocks[i].x);
    if (rx < -14 || rx > SCR_W + 2) continue;
    tft.fillRect(rx, rocks[i].y, rocks[i].w, 3, 0x7BCF);
    tft.drawPixel(rx + 1, rocks[i].y - 1, 0x9CF3);
  }

  // ---- flowers ----
  for (int i = 0; i < MAX_FLOWERS; i++) {
    int fx = wx2sx(flowers[i].x);
    if (fx < 0 || fx >= SCR_W) continue;
    uint16_t petal = (flowers[i].kind == 0) ? 0xF81F
                   : (flowers[i].kind == 1) ? 0xFFE0 : 0xFFFF;
    tft.drawPixel(fx, flowers[i].y, 0x24E4);        // stem
    tft.drawPixel(fx, flowers[i].y - 1, petal);     // bloom
  }
}

// The scenery is static, so it is rendered ONCE into a RAM canvas and
// then blitted each frame. That replaces a 126-line gradient plus
// hundreds of primitives per frame with a single pushSprite - which is
// what stops the flicker.
//
// The cache is rebuilt only when the camera has moved BG_REBUILD_PX.

// Draws the backdrop. If a canvas exists, the scenery is rendered into it
// and blitted; otherwise it goes straight to the panel. Either way
// something is drawn - the one rule this function must never break.
static void drawWorldBackdrop() {
  int cam = (int)cameraX;

  // No canvas: draw direct. Slower and it flickers, but it draws.
  if (!bgReady) {
    renderScenery(tft, cam);
    return;
  }

  // Canvas is current for this camera position - just blit it
  if (abs(cam - bgCamX) < BG_REBUILD_PX) {
    bgSprite.pushSprite(0, 0);
    return;
  }

  // Rebuild the cache at the new camera position
  bgSprite.fillSprite(COL_SKY);
  renderScenery(bgSprite, cam);
  bgCamX = cam;
  bgSprite.pushSprite(0, 0);
}
// Draws the scenery into whichever target is passed - the sprite when
// caching, or tft when there is no canvas. Both share the drawing API.
static void renderScenery(TFT_eSPI& tgt, int cam) {
  for (int y = 0; y < GROUND_Y; y++) {
    float t = (float)y / (float)GROUND_Y;
    uint8_t r = (uint8_t)(0x6C + (0xB8 - 0x6C) * t);
    uint8_t g = (uint8_t)(0x8C + (0xCF - 0x8C) * t);
    uint8_t b = (uint8_t)(0xC8 + (0xE8 - 0xC8) * t);
    tgt.drawFastHLine(0, y, SCR_W, tgt.color565(r, g, b));
  }

  for (int i = 0; i < MAX_HILLS; i++) {
    for (int dx = -hills[i].w / 2; dx <= hills[i].w / 2; dx++) {
      float t = (float)dx / (hills[i].w / 2.0f);
      int h = (int)(hills[i].h * (1.0f - t * t));
      if (h <= 0) continue;
      int px = (int)(hills[i].x + dx) - cam;
      if (px < 0 || px >= SCR_W) continue;
      tgt.drawFastVLine(px, GROUND_Y - h, h, 0x4A65);
    }
  }

  tgt.fillRect(0, GROUND_Y, SCR_W, WORLD_H - GROUND_Y, COL_GROUND);

  for (int i = 0; i < 300; i++) {
    int wxp = (i * 37) % WORLD_W;
    int sx = wxp - cam;
    if (sx < 0 || sx >= SCR_W) continue;
    int sy = GROUND_Y + 3 + ((i * 53) % (WORLD_H - GROUND_Y - 4));
    tgt.drawPixel(sx, sy, 0x6203);
  }

  int firstTuft = (cam / 5) * 5;
  for (int wxg = firstTuft; wxg < cam + SCR_W + 5; wxg += 5) {
    int sx = wxg - cam;
    int h = 2 + ((wxg / 5) % 3);
    if (sx < -2 || sx > SCR_W + 2) continue;
    tgt.drawLine(sx, GROUND_Y, sx + 1, GROUND_Y - h, COL_GRASS);
  }

  for (int i = 0; i < MAX_TREES; i++) {
    Scene& t = trees[i];
    int tx = t.x - cam;
    if (tx < -t.h || tx > SCR_W + t.h) continue;
    tgt.fillRect(tx, t.y - t.h / 3, 3, t.h / 3, 0x4200);
    if (t.kind == 0) {
      tgt.fillCircle(tx + 1, t.y - t.h / 3 - t.h / 4, t.h / 3, 0x24E4);
      tgt.fillCircle(tx + 1, t.y - t.h / 3 - t.h / 2, t.h / 4, 0x2D45);
    } else {
      tgt.fillTriangle(tx - t.h / 4, t.y - t.h / 3,
                       tx + 1 + t.h / 4, t.y - t.h / 3,
                       tx + 1, t.y - t.h, 0x24E4);
    }
  }

  for (int i = 0; i < MAX_ROCKS; i++) {
    int rx = rocks[i].x - cam;
    if (rx < -14 || rx > SCR_W + 2) continue;
    tgt.fillRect(rx, rocks[i].y, rocks[i].w, 3, 0x7BCF);
    tgt.drawPixel(rx + 1, rocks[i].y - 1, 0x9CF3);
  }

  for (int i = 0; i < MAX_FLOWERS; i++) {
    int fx = flowers[i].x - cam;
    if (fx < 0 || fx >= SCR_W) continue;
    uint16_t petal = (flowers[i].kind == 0) ? 0xF81F
                   : (flowers[i].kind == 1) ? 0xFFE0 : 0xFFFF;
    tgt.drawPixel(fx, flowers[i].y, 0x24E4);
    tgt.drawPixel(fx, flowers[i].y - 1, petal);
  }
}

// ================= HUD =================
// Buttons in a 3-wide grid. Seven of them, so three rows: 3 + 3 + 1.
// The HUD height below is derived from the row count.
#define HUD_BTN_W    100
#define HUD_BTN_H    20
#define HUD_BTN_GAPX 5
#define HUD_BTN_GAPY 4
#define HUD_BTN_X0   6
#define HUD_BTN_Y0   (WORLD_H + 24)         // below the two status lines

static int hudBtnRow(int i) { return i / 3; }         // 0 or 1
static int hudBtnCol(int i) { return i % 3; }         // 0,1,2

static int hudBtnX(int i) {
  return HUD_BTN_X0 + hudBtnCol(i) * (HUD_BTN_W + HUD_BTN_GAPX);
}
static int hudBtnY(int i) {
  return HUD_BTN_Y0 + hudBtnRow(i) * (HUD_BTN_H + HUD_BTN_GAPY);
}

static int hudButtonAt(int x, int y) {
  for (int i = 0; i < 9; i++) {
    int bx = hudBtnX(i), by = hudBtnY(i);
    if (x >= bx && x < bx + HUD_BTN_W &&
        y >= by && y < by + HUD_BTN_H) return i;
  }
  return -1;
}

// The HUD is split in two, because redrawing all of it every frame is
// what made it blink. The panel and buttons never change, so they are
// painted once; only the two status lines are redrawn, and only the
// 22px text band is cleared rather than the whole 96px strip.

#define HUD_TEXT_H 22

// ---- painted once, at startup and after any full-screen change ----
static void drawHudChrome() {
  tft.fillRect(0, WORLD_H, SCR_W, HUD_H, COL_PANEL);
  tft.drawFastHLine(0, WORLD_H, SCR_W, COL_DIM);

  static const char* labels[9] = { "TICKLE", "SLAP", "BREED",
                                   "LOAD", "FOOD", "SAVE",
                                   "NEXT >", "NEW GAME", "PAUSE" };

  for (int i = 0; i < 9; i++) {
    int bx = hudBtnX(i);
    int by = hudBtnY(i);

    uint16_t edge = (i == 5) ? COL_FRIENDLY
                  : (i == 6) ? COL_ACCENT
                  : (i == 7) ? COL_WARN
                  : (i == 8) ? COL_PAUSED
                  : COL_DIM;
    uint16_t fg   = (i == 5) ? COL_FRIENDLY
                  : (i == 6) ? COL_ACCENT
                  : (i == 7) ? COL_WARN
                  : (i == 8) ? COL_PAUSED
                  : COL_TEXT;

    tft.fillRoundRect(bx, by, HUD_BTN_W, HUD_BTN_H, 4, COL_BG);
    tft.drawRoundRect(bx, by, HUD_BTN_W, HUD_BTN_H, 4, edge);

    tft.setTextSize(2);
    tft.setTextColor(fg, COL_BG);
    int tw = strlen(labels[i]) * 12;
    tft.setCursor(bx + (HUD_BTN_W - tw) / 2, by + (HUD_BTN_H - 16) / 2);
    tft.print(labels[i]);
    tft.setTextSize(1);
  }
}

// ---- redrawn each frame: just the two text lines ----
static void drawHudStatus() {
  int y = WORLD_H;

  // Clear only the text band, leaving the buttons untouched
  tft.fillRect(0, y + 1, SCR_W, HUD_TEXT_H, COL_PANEL);

  tft.setTextSize(1);

  if (millis() < msgUntil) {
    tft.setTextColor(COL_ACCENT, COL_PANEL);
    tft.setCursor(6, y + 3);
    tft.print(statusMsg);
  } else if (selected >= 0 && creatures[selected].alive) {
    Creature& c = creatures[selected];
    tft.setTextColor(COL_TEXT, COL_PANEL);
    tft.setCursor(6, y + 3);
    tft.printf("%s  gen %d  %s",
               c.genome.name, c.genome.generation,
               c.female ? "F" : "M");

    tft.setTextColor(COL_TEXT, COL_PANEL);
    tft.setCursor(186, y + 3);
    tft.printf("age %d", (int)c.ageTicks);

    // Living population and its sex mix, so a missing partner is obvious
    int aliveM = 0, aliveF = 0;
    for (int k = 0; k < MAX_CREATURES; k++) {
      if (!creatures[k].alive) continue;
      if (creatures[k].female) aliveF++; else aliveM++;
    }
    tft.setCursor(258, y + 3);
    tft.printf("%dm %df", aliveM, aliveF);
  } else {
    tft.setTextColor(COL_TEXT, COL_PANEL);
    tft.setCursor(6, y + 3);
    tft.print("tap a creature to select it");
  }

  if (selected >= 0 && creatures[selected].alive &&
      millis() >= msgUntil) {
    Creature& c = creatures[selected];
    tft.setTextColor(COL_TEXT, COL_PANEL);
    tft.setCursor(6, y + 13);
    tft.printf("HUNGRY %d", (int)c.drive.hunger);
    tft.setCursor(90, y + 13);
    tft.printf("TIRED %d", (int)c.drive.tired);
    tft.setCursor(174, y + 13);
    tft.printf("LONE %d", (int)c.drive.lonely);
    tft.setCursor(252, y + 13);
    tft.printf("MOOD %d", (int)c.drive.mood);
  }
}

// What the render loop calls. The chrome is already on screen.
static void drawHud() {
  drawHudStatus();
}

// ================= BRAIN LIST SCREEN =================
static void drawBrainList() {
  tft.fillScreen(COL_BG);

  tft.fillRect(0, 0, SCR_W, 24, COL_PANEL);
  tft.setTextSize(2);
  tft.setTextColor(COL_ACCENT, COL_PANEL);
  tft.setCursor(8, 4);
  tft.print("GENOMES");

  tft.setTextSize(1);
  int y = 30;
  int shown = 0;

  File dir = SD.open("/brains");
  if (!dir) {
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setCursor(8, y);
    tft.print("no /brains folder on SD");
  } else {
    while (true) {
      File e = dir.openNextFile();
      if (!e) break;

      const char* nm = e.name();
      int len = strlen(nm);
      if (len > 4 && strcasecmp(nm + len - 4, ".gen") == 0) {
        // Read the genome so the list shows its generation and name,
        // not just a filename
        char path[48];
        snprintf(path, sizeof(path), BRAIN_DIR "/%s", nm);

        Genome g;
        tft.setTextColor(COL_TEXT, COL_BG);
        tft.setCursor(8, y);
        tft.print(nm);

        if (loadBrain(path, g)) {
          tft.setTextColor(COL_DIM, COL_BG);
          tft.setCursor(150, y);
          tft.printf("gen %d  %.6s", g.generation, g.name);
        }
        y += 12;
        shown++;
        if (y > SCR_H - 30) break;
      }
      e.close();
    }
    dir.close();

    if (shown == 0) {
      tft.setTextColor(COL_DIM, COL_BG);
      tft.setCursor(8, y);
      tft.print("no .gen files found");
    }
  }

  tft.setTextColor(COL_DIM, COL_BG);
  tft.setCursor(8, SCR_H - 16);
  tft.print("tap to return");
}

// ================= BEHAVIOUR =================
static float driveSpeed(const Creature& c) {
  return traitF(c.genome.trait[T_SPEED], 0.34f, 1.05f);
}

static int nearestCreature(int self, float& outDist) {
  int best = -1;
  float bd = 1e9;

  for (int i = 0; i < MAX_CREATURES; i++) {
    if (i == self || !creatures[i].alive) continue;
    float dx = creatures[i].x - creatures[self].x;
    float dy = creatures[i].y - creatures[self].y;
    float d = sqrtf(dx * dx + dy * dy);
    if (d < bd) { bd = d; best = i; }
  }
  outDist = bd;
  return best;
}

// ===============================================================
// Grendel
// ===============================================================
static void spawnGrendel() {
  grendel.alive      = true;
  grendel.hunting    = false;
  grendel.facing     = -1;
  grendel.vx         = 0;
  grendel.walkPhase  = 0;
  grendel.attackTimer = 0;

  // Start well away from wherever the population begins
  float lo = SCR_W * 1.2f;
  float hi = WORLD_W - SCR_W * 0.2f;
  if (hi < lo) hi = lo + 1;
  grendel.x = frand(lo, hi);
  grendel.y = GROUND_Y;

  Serial.printf("grendel spawned at %.0f (world %d)\n", grendel.x, WORLD_W);
}

static void updateGrendel() {
  if (!grendel.alive) return;

  grendel.walkPhase += 0.14f;
  if (grendel.attackTimer > 0) grendel.attackTimer -= 100.0f;   // called at 10 Hz

  // ---------------------------------------------------------------
  // The Grendel pays no attention to the Norns. It ambles along on
  // its own business, occasionally changing its mind, and if one
  // happens to be in its way it gives it a slap and carries on.
  // ---------------------------------------------------------------
  grendel.hunting = false;

  // Wander: keep going, but change direction now and then
  if ((esp_random() % 220) == 0) {
    grendel.vx = -grendel.vx;
  }
  if (grendel.vx == 0)
    grendel.vx = (esp_random() & 1) ? GRENDEL_SPEED_ROAM : -GRENDEL_SPEED_ROAM;

  // Turn at the world edges
  if (grendel.x < 40)           grendel.vx =  fabsf(grendel.vx);
  if (grendel.x > WORLD_W - 40) grendel.vx = -fabsf(grendel.vx);

  grendel.vx = (grendel.vx > 0) ? GRENDEL_SPEED_ROAM : -GRENDEL_SPEED_ROAM;
  grendel.facing = (grendel.vx > 0) ? 1 : -1;

  grendel.x += grendel.vx;
  if (grendel.x < 10) grendel.x = 10;
  if (grendel.x > WORLD_W - 10) grendel.x = WORLD_W - 10;

  // ---- a slap for anything it walks into, and only if it just met ----
  if (grendel.attackTimer <= 0) {
    for (int i = 0; i < MAX_CREATURES; i++) {
      if (!creatures[i].alive) continue;
      float d = fabsf(creatures[i].x - grendel.x);
      if (d < GRENDEL_REACH) {
        Creature& c = creatures[i];

        grendel.attackTimer = GRENDEL_ATTACK_MS;

        // A slap, not a mauling: it stings, it startles, it does not kill.
        c.health -= GRENDEL_DAMAGE;
        c.drive.mood -= 12;
        c.drive.fear += 40;
        if (c.drive.mood  < 0)   c.drive.mood  = 0;
        if (c.drive.fear  > 100) c.drive.fear  = 100;

        // Keep health above zero - the Grendel bruises Norns, it does
        // not kill them. Only old age ends a Norn in this world.
        if (c.health < 25.0f) c.health = 25.0f;

        say("the grendel slapped a norn", 2200);
        break;
      }
    }
  }
}

static void drawGrendelMarker() {
  if (!grendel.alive) return;

  int sx = wx2sx(grendel.x);
  if (sx >= 0 && sx < SCR_W) return;          // it is visible; no marker

  bool  right = (sx >= SCR_W);
  int   mx    = right ? (SCR_W - 10) : 10;
  int   my    = WORLD_H / 2;

  int dist = (int)(fabsf(grendel.x - (cameraX + SCR_W / 2.0f)) / 10.0f);

  uint16_t col = COL_WARN;         // it is not hunting anything

  // Small arrowhead at the screen edge
  if (right) {
    tft.fillTriangle(mx + 6, my, mx - 4, my - 7, mx - 4, my + 7, col);
  } else {
    tft.fillTriangle(mx - 6, my, mx + 4, my - 7, mx + 4, my + 7, col);
  }

  tft.setTextSize(1);
  tft.setTextColor(col, COL_SKY);
  char buf[8];
  snprintf(buf, sizeof(buf), "%dm", dist);
  int tw = strlen(buf) * 6;
  tft.setCursor(right ? (mx - tw - 4) : (mx + 8), my - 4);
  tft.print(buf);
}

static void drawGrendel(int sx) {
  if (!grendel.alive) return;
  if (sx < -80 || sx > SCR_W + 80) return;

  // ---------------------------------------------------------------
  // Shaped after the original Albian Grendel: a small dark hunched
  // body, a big round head that tapers to a pointed snout, two round
  // bulging eyes set wide, a wide grinning mouth, and a long thin
  // tail dragging behind. Roughly 44px tall - a head shorter than
  // the old one and much rounder.
  // ---------------------------------------------------------------
  const int bodyH = 18;         // squat torso
  const int bodyW = 26;
  const int legH  = 10;
  const int headR = 13;         // big round head

  int baseY   = GROUND_Y;
  int bodyTop = baseY - legH - bodyH;
  int f       = grendel.facing;

  // ---- tail: long, thin, drags behind and lifts slightly ----
  int tailDir = -f;
  for (int i = 0; i < 5; i++) {
    int tx = sx + tailDir * (10 + i * 5);
    int ty = baseY - legH - 3 - (int)(sinf(grendel.walkPhase * 0.5f + i * 0.7f) * 2.0f);
    int th = (i < 3) ? 3 : 2;
    tft.fillRect(tx, ty, 5, th, COL_GRENDEL_DK);
  }

  // ---- legs: short, splayed, walking ----
  int l1 = (int)(sinf(grendel.walkPhase) * 3.0f);
  int l2 = (int)(sinf(grendel.walkPhase + 3.14159f) * 3.0f);
  tft.fillRect(sx - 9 + l1, baseY - legH, 6, legH, COL_GRENDEL_DK);
  tft.fillRect(sx + 3 + l2, baseY - legH, 6, legH, COL_GRENDEL_DK);
  // feet
  tft.fillRect(sx - 11 + l1, baseY - 2, 9, 2, COL_GRENDEL_DK);
  tft.fillRect(sx + 1 + l2, baseY - 2, 9, 2, COL_GRENDEL_DK);

  // ---- body: rounded, humped at the shoulders ----
  for (int row = 0; row < bodyH; row++) {
    float t = (float)row / (float)(bodyH - 1);
    int w = (int)(bodyW * (0.80f + 0.20f * sinf(t * 3.14159f)));
    int yy = bodyTop + row;

    tft.fillRect(sx - w / 2, yy, w, 1, COL_GRENDEL);
    if (row < 2) tft.fillRect(sx - w / 2, yy, w, 1, COL_GRENDEL_LT);
    if (row > bodyH * 2 / 3)
      tft.fillRect(sx - w / 4, yy, w / 2, 1, COL_GRENDEL_DK);
  }

  // ---- head: a big round ball, set forward of the body ----
  int hx = sx + f * 5;
  int hy = bodyTop - headR + 5;
  tft.fillCircle(hx, hy, headR, COL_GRENDEL);
  // lit top-left of the head
  tft.fillCircle(hx - f * 3, hy - 4, headR - 5, COL_GRENDEL_LT);

  // ---- snout: tapers forward to a point ----
  tft.fillTriangle(hx + f * 4, hy - 5,
                   hx + f * 4, hy + 6,
                   hx + f * (headR + 9), hy + 2, COL_GRENDEL);

  // ---- the mouth: wide open, no teeth ----
  // A big dark cavity taking up the lower half of the head, with a
  // lighter rim so the opening reads clearly against the body.
  int mw = headR * 2 - 2;
  int mh = headR - 2;
  int mx0 = hx - mw / 2;
  int my  = hy + 1;

  tft.fillRect(mx0, my, mw, mh, COL_GRENDEL_DK);
  tft.drawRect(mx0, my, mw, mh, COL_GRENDEL_LT);

  // darker interior, so it looks like a hole rather than a patch
  if (mw > 6 && mh > 4)
    tft.fillRect(mx0 + 2, my + 2, mw - 4, mh - 4, COL_MOUTH);

  // ---- eyes: round, bulging, set wide on the head ----
  int ex1 = hx - f * 2;
  int ex2 = hx + f * 7;
  tft.fillCircle(ex1, hy - 5, 4, COL_TEXT);
  tft.fillCircle(ex2, hy - 5, 4, COL_TEXT);
  tft.fillCircle(ex1 + f, hy - 5, 2, COL_WARN);
  tft.fillCircle(ex2 + f, hy - 5, 2, COL_WARN);

  // ---- brow ridges, which give it the original's scowl ----
  tft.fillRect(hx - 7, hy - 10, 6, 2, COL_GRENDEL_DK);
  tft.fillRect(hx + 3, hy - 10, 6, 2, COL_GRENDEL_DK);

  // ---- a few spines down the back ----
  for (int i = 0; i < 3; i++) {
    int yy = bodyTop + 2 + i * 5;
    tft.fillTriangle(sx - 5, yy, sx + 1, yy, sx - 2, yy - 4, COL_GRENDEL_DK);
  }
}

// ===============================================================
// Population helpers
// ===============================================================
static int eggsAlive() {
  int n = 0;
  for (int i = 0; i < MAX_EGGS; i++)
    if (eggs[i].alive) n++;
  return n;
}

static int countLivingNorns() {
  int n = 0;
  for (int i = 0; i < MAX_CREATURES; i++)
    if (creatures[i].alive) n++;
  return n;
}

// ===============================================================
// End of game
// ===============================================================
static bool gameOver = false;

static void endGame() {
  gameOver  = true;
  selected  = -1;
  paused    = false;

  drawWorldBackdrop();
  drawHudChrome();

  // Dim panel across the middle of the playfield
  int boxH = 76;
  int boxY = (WORLD_H - boxH) / 2;
  tft.fillRect(0, boxY, SCR_W, boxH, COL_PANEL);
  tft.drawRect(4, boxY, SCR_W - 8, boxH, COL_WARN);
  tft.drawRect(5, boxY + 1, SCR_W - 10, boxH - 2, COL_WARN);

  tft.setTextSize(2);
  tft.setTextColor(COL_WARN, COL_PANEL);
  const char* l1 = "THE LAST NORN";
  const char* l2 = "HAS DIED";
  tft.setCursor((SCR_W - (int)strlen(l1) * 12) / 2, boxY + 12);
  tft.print(l1);
  tft.setCursor((SCR_W - (int)strlen(l2) * 12) / 2, boxY + 34);
  tft.print(l2);

  tft.setTextSize(1);
  tft.setTextColor(COL_TEXT, COL_PANEL);
  const char* l3 = "tap NEW GAME to start again";
  tft.setCursor((SCR_W - (int)strlen(l3) * 6) / 2, boxY + 56);
  tft.print(l3);

  Serial.println("game over: last norn died");
}

// ===============================================================
// Creature update
// ===============================================================
static void updateCreature(int idx) {
  Creature& c = creatures[idx];
  if (!c.alive) return;

  // ---- age ----
  c.ageTicks += 0.6f;
  if (c.ageTicks >= c.lifespan) {
    c.alive = false;
    if (selected == idx) selected = -1;

    if (countLivingNorns() == 0) {
      endGame();
    } else {
      say("a norn has died of old age", 3000);
    }
    return;
  }

  // ---- drives drift ----
  float appetite   = traitF(c.genome.trait[T_APPETITE], 0.5f, 2.2f);
  float metabolism = traitF(c.genome.trait[T_METABOLISM], 0.6f, 1.8f);

  c.drive.hunger += 0.018f * appetite;
  c.drive.tired  += (c.act == ACT_SLEEP) ? -0.45f : 0.013f * metabolism;
  c.drive.lonely += 0.011f;
  c.drive.fear   -= 0.6f;                    // fear ebbs when not threatened

  if (c.act == ACT_SLEEP) c.drive.hunger -= 0.008f;

  if (c.drive.hunger < 0)   c.drive.hunger = 0;
  if (c.drive.tired < 0)    c.drive.tired = 0;
  if (c.drive.lonely < 0)   c.drive.lonely = 0;
  if (c.drive.fear < 0)     c.drive.fear = 0;

  // Mood falls when needs are unmet
  float unmet = (c.drive.hunger + c.drive.tired + c.drive.lonely) / 300.0f;
  c.drive.mood += (0.5f - unmet);
  if (c.drive.mood < 0)   c.drive.mood = 0;
  if (c.drive.mood > 100) c.drive.mood = 100;

  // ---- pick an activity ----
  float nearDist;
  int nearIdx = nearestCreature(idx, nearDist);

  // Learning biases the choice: what you reward, it does more.
  float wEat    = 1.0f + c.learn[ACT_EAT];
  float wSleep  = 1.0f + c.learn[ACT_SLEEP];
  float wSocial = 1.0f + c.learn[ACT_SOCIAL];

  // Fear overrides everything: a frightened Norn runs, it does not eat.
  if (c.drive.fear > 60) {
    c.act = ACT_FLEE;
  } else if (c.drive.hunger > 55 && c.drive.hunger * wEat > 60) {
    c.act = ACT_EAT;
  } else if (c.drive.tired > 60 && c.drive.tired * wSleep > 65) {
    c.act = ACT_SLEEP;
  } else if (c.drive.lonely > 45 && nearIdx >= 0 && wSocial > 0.8f) {
    c.act = ACT_SOCIAL;
  } else if (c.act == ACT_EAT && c.drive.hunger > 30) {
    // keep eating until satisfied
  } else if (c.act == ACT_SLEEP && c.drive.tired > 20) {
    // keep sleeping
  } else {
    c.act = ACT_WANDER;
  }

  // ---- act ----
  float sp = driveSpeed(c);

  switch (c.act) {

    case ACT_FLEE: {
      // Run directly away from the Grendel, faster than usual.
      float away = (c.x < grendel.x) ? -1.0f : 1.0f;
      c.facing = (away > 0) ? 1 : -1;
      c.vx = away * sp * 1.9f;          // genuine flight speed

      // Fear decays once it is far enough away
      float gd = fabsf(c.x - grendel.x);
      if (gd > 180.0f) c.drive.fear -= 3.0f;
      else             c.drive.fear -= 0.4f;
      if (c.drive.fear < 0) c.drive.fear = 0;

      // Keep it inside the world
      if (c.x < 16) { c.x = 16; c.drive.fear -= 2.0f; }
      if (c.x > WORLD_W - 16) { c.x = WORLD_W - 16; c.drive.fear -= 2.0f; }
      break;
    }

    case ACT_SLEEP:
      c.vx *= 0.8f;
      break;

    case ACT_EAT: {
      // Walk to the nearest food, eat it on contact
      int best = -1;
      float bd = 1e9;
      for (int i = 0; i < MAX_FOOD; i++) {
        if (!food[i].alive) continue;
        float dx = food[i].x - c.x;
        float dy = food[i].y - c.y;
        float d = sqrtf(dx * dx + dy * dy);
        if (d < bd) { bd = d; best = i; }
      }

      if (best >= 0) {
        if (bd < 8) {
          food[best].alive = false;
          c.drive.hunger -= 60;
          if (c.drive.hunger < 0) c.drive.hunger = 0;
          c.drive.mood += 6;
          if (c.drive.mood > 100) c.drive.mood = 100;
          c.act = ACT_WANDER;
        } else {
          c.vx = (food[best].x > c.x) ? sp : -sp;
        }
      } else {
        c.vx *= 0.7f;
        if (c.drive.hunger > 80) c.act = ACT_WANDER;   // give up and roam
      }
      break;
    }

    case ACT_SOCIAL:
      if (nearIdx >= 0 && nearDist > 14) {
        c.vx = (creatures[nearIdx].x > c.x) ? sp : -sp;
      } else {
        c.vx *= 0.6f;
        c.drive.lonely -= 0.7f;
        c.drive.mood += 0.4f;
      }
      break;

    case ACT_WANDER:
    default: {
      // Head for a target somewhere else in the world
      float g = wanderTargetX[idx];
      float d = g - c.x;

      if (fabsf(d) < 12.0f || (esp_random() % 200) == 0) {
        // Arrived, or got bored: pick a new spot well away from here
        float off = frand(80.0f, 260.0f) * ((esp_random() & 1) ? 1 : -1);
        g = c.x + off;
        if (g < 20)           g = 20;
        if (g > WORLD_W - 20) g = WORLD_W - 20;
        wanderTargetX[idx] = g;
        d = g - c.x;
      }

      c.facing = (d > 0) ? 1 : -1;

      // Pause occasionally so the walk is not robotic
      if ((esp_random() % 120) == 0) {
        c.vx = 0.0f;
      } else {
        float pace = 0.85f + (esp_random() % 40) / 100.0f;
        c.vx = c.facing * sp * (pace + c.learn[ACT_WANDER] * 0.2f);
      }
      break;
    }
  }

  // ---- move ----
  c.x += c.vx;
  if (c.x < 14) { c.x = 14; c.facing = 1; }
  if (c.x > WORLD_W - 14) { c.x = WORLD_W - 14; c.facing = -1; }

  if (c.vx > 0.02f) c.facing = 1;
  if (c.vx < -0.02f) c.facing = -1;

  // Walk cycle advances with movement
  c.walkPhase += fabsf(c.vx) * 0.35f;
  if (c.walkPhase > 6.28f) c.walkPhase -= 6.28f;

  // Blink
  c.blinkTimer--;
  if (c.blinkTimer < 0) c.blinkTimer = irand(60, 260);

  // Starvation: hunger at max drains mood and eventually health
  if (c.drive.hunger > 95) {
    c.drive.mood -= 0.05f;
    if ((esp_random() % 400) == 0) {
      c.alive = false;
      if (selected == idx) selected = -1;

      if (countLivingNorns() == 0) endGame();
      else                         say("a norn has starved", 3000);
    }
  }
}

// ================= EGGS =================
static void updateEggs() {
  for (int i = 0; i < MAX_EGGS; i++) {
    Egg& e = eggs[i];
    if (!e.alive) continue;

    e.hatchTimer -= 1.0f;
    if (e.hatchTimer > 0) continue;

    // Find a free creature slot
    int slot = -1;
    for (int k = 0; k < MAX_CREATURES; k++)
      if (!creatures[k].alive) { slot = k; break; }

    if (slot >= 0) {
      float life = frand(LIFESPAN_MIN, LIFESPAN_MAX);
      spawnCreature(slot, e.genome, (esp_random() & 1), life);
      creatures[slot].x = e.x;
      creatures[slot].y = GROUND_Y - 2;

      // Write the newborn's own genome file. This is what makes the card
      // a population rather than an archive: the next boot loads it.
      char path[48];
      makeBrainPath(path, sizeof(path), e.genome.generation);
      saveBrain(e.genome, path);

      say("an egg has hatched!", 3000);
    }

    e.alive = false;
  }
}

static void tryBreed() {
  if (selected < 0 || !creatures[selected].alive) {
    say("select a creature first");
    return;
  }

  Creature& a = creatures[selected];

  if (a.ageTicks < LIFESPAN_MIN * 0.12f) {
    say("too young to breed");
    return;
  }

  // Find a willing partner of the opposite sex. Track why each candidate
  // was rejected so the message can say something useful.
  int partner = -1;
  float bd = 1e9;
  int liveOthers = 0;      // alive creatures other than the selected one
  int opposite   = 0;      // of those, how many are the other sex

  for (int i = 0; i < MAX_CREATURES; i++) {
    if (i == selected || !creatures[i].alive) continue;
    liveOthers++;
    if (creatures[i].female == a.female) continue;
    opposite++;

    if (creatures[i].drive.mood < 40) continue;
    if (creatures[i].drive.hunger > 80) continue;

    float dx = creatures[i].x - a.x;
    float d = fabsf(dx);
    if (d < bd) { bd = d; partner = i; }
  }

  if (partner < 0) {
    // Say which condition actually blocked it
    if (liveOthers == 0) {
      say("no other norns alive");
    } else if (opposite == 0) {
      say("no partner of the other sex");
    } else {
      say("partner unwilling - needs mood 40+", 2600);
    }
    return;
  }

  if (bd > 60) {
    say("partner too far away");
    return;
  }

  // Free egg slot?
  int slot = -1;
  for (int i = 0; i < MAX_EGGS; i++)
    if (!eggs[i].alive) { slot = i; break; }

  if (slot < 0) {
    say("no room for another egg");
    return;
  }

  Creature& b = creatures[partner];

  breedGenome(a.genome, b.genome, eggs[slot].genome, a.female);

  eggs[slot].x = (a.x + b.x) * 0.5f;
  eggs[slot].y = GROUND_Y - 4;
  eggs[slot].hatchTimer = 400;
  eggs[slot].alive = true;

  a.drive.mood += 10;
  b.drive.mood += 10;
  if (a.drive.mood > 100) a.drive.mood = 100;
  if (b.drive.mood > 100) b.drive.mood = 100;

  char buf[40];
  snprintf(buf, sizeof(buf), "egg laid - gen %d", eggs[slot].genome.generation);
  say(buf, 3000);
}

// ================= TICKLE / SLAP =================
// The player's whole influence on learning: reward or punish whatever the
// creature is doing right now, which biases its future choices.
static void tickleSelected() {
  if (selected < 0 || !creatures[selected].alive) {
    say("select a creature first");
    return;
  }
  Creature& c = creatures[selected];

  c.learn[c.act] += 0.35f;
  if (c.learn[c.act] > 1.5f) c.learn[c.act] = 1.5f;

  c.drive.mood += 12;
  if (c.drive.mood > 100) c.drive.mood = 100;

  if (c.act == ACT_EAT)         say("it liked that - eating rewarded");
  else if (c.act == ACT_SLEEP)  say("it liked that - sleeping rewarded");
  else if (c.act == ACT_SOCIAL) say("it liked that - company rewarded");
  else                          say("it liked that");
}

static void slapSelected() {
  if (selected < 0 || !creatures[selected].alive) {
    say("select a creature first");
    return;
  }
  Creature& c = creatures[selected];

  c.learn[c.act] -= 0.40f;
  if (c.learn[c.act] < -0.9f) c.learn[c.act] = -0.9f;

  c.drive.mood -= 15;
  if (c.drive.mood < 0) c.drive.mood = 0;
  c.drive.fear += 25;

  if (c.act == ACT_EAT)        say("it learned to avoid eating");
  else if (c.act == ACT_SLEEP) say("it learned to avoid sleeping");
  else                         say("it learned to avoid that");
}

static void dropFood() {
  // Drop the apple just in front of the selected norn, so it can actually
  // reach it. With a world three screens wide, random placement meant the
  // food usually landed off screen.
  float dropX;

  if (selected >= 0 && creatures[selected].alive) {
    Creature& c = creatures[selected];
    // Slightly ahead of the facing direction, and clamped to the world
    dropX = c.x + c.facing * frand(18.0f, 40.0f);
  } else {
    // Nothing selected: drop it in front of the camera instead
    dropX = cameraX + frand(60.0f, SCR_W - 60.0f);
  }

  if (dropX < 20)             dropX = 20;
  if (dropX > WORLD_W - 20)   dropX = WORLD_W - 20;

  for (int i = 0; i < MAX_FOOD; i++) {
    if (food[i].alive) continue;
    food[i].x = dropX;
    food[i].y = 24;
    food[i].vy = 0;
    food[i].alive = true;
    say("food dropped nearby");
    return;
  }
  say("the ground is already covered");
}

// ================= NEW GAME =================
// Wipe the population and start over. The genomes stay on the card, so
// this reseeds from whatever is there rather than forgetting the lineage.
static void startNewGame() {
  gameOver = false;
  for (int i = 0; i < MAX_CREATURES; i++) creatures[i].alive = false;
  for (int i = 0; i < MAX_EGGS; i++)      eggs[i].alive = false;
  for (int i = 0; i < MAX_FOOD; i++)      food[i].alive = false;

  selected = -1;

  cameraX = 0;
  cameraTarget = 0;

  seedWorld();
  spawnGrendel();
  drawWorldBackdrop();
  drawHudChrome();

  say("new game", 2500);
}

// ================= SELECTION =================
// Move the selection to the next living creature, wrapping around, and
// ease the camera across so the new norn is centred.
static void selectNextCreature() {
  int start = (selected >= 0) ? selected : -1;

  for (int step = 1; step <= MAX_CREATURES; step++) {
    int idx = (start + step) % MAX_CREATURES;
    if (idx < 0) idx += MAX_CREATURES;
    if (creatures[idx].alive) {
      selected = idx;
      say(creatures[idx].genome.name, 1500);
      return;
    }
  }
  say("no living norns", 1500);
}

// ================= TOUCH =================
static bool getTouch(int &x, int &y) {
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint();
  x = map(p.x, TS_MINX, TS_MAXX, 0, SCR_W) + TOUCH_OFFSET_X;
  y = map(p.y, TS_MINY, TS_MAXY, 0, SCR_H) + TOUCH_OFFSET_Y;

  // Report every tap so a mis-calibration is visible immediately
  Serial.printf("raw %d,%d -> %d,%d\n", p.x, p.y, x, y);
  x = constrain(x, 0, SCR_W - 1);
  y = constrain(y, 0, SCR_H - 1);
  return true;
}

static void handleTouch() {
  int x, y;
  if (!getTouch(x, y)) return;

  int btn = hudButtonAt(x, y);
  if (btn >= 0) {
    while (ts.touched()) { delay(5); yield(); }

    switch (btn) {
      case 0: tickleSelected(); break;
      case 1: slapSelected();   break;
      case 2: tryBreed();       break;
      case 3:
        // LOAD: rebuild the world from /save.dat. seedWorld() first so a
        // missing or rejected save still leaves a playable population.
        seedWorld();
        spawnGrendel();
        if (loadGame()) {
          drawWorldBackdrop();
          drawHudChrome();
          say("saved world restored", 4000);
        } else {
          drawWorldBackdrop();
          drawHudChrome();
          say("no usable save - new world", 4000);
        }
        return;
      case 4: dropFood();       break;
      case 5:
        if (saveGame()) say("world saved to /save.dat", 3000);
        else            say("save failed - check SD", 3000);
        break;
      case 6:
        selectNextCreature();
        break;
      case 7:
        startNewGame();
        break;
      case 8:
        paused = !paused;
        say(paused ? "paused" : "resumed", 1200);
        break;
    }
    return;
  }

  // World tap: select the nearest creature
  if (y < WORLD_H) {
    while (ts.touched()) { delay(5); yield(); }

    int best = -1;
    float bd = 26.0f;
    for (int i = 0; i < MAX_CREATURES; i++) {
      if (!creatures[i].alive) continue;
      float dx = creatures[i].x - (x + cameraX);      // x is screen space
      float dy = creatures[i].y - (y + 12);
      float d = sqrtf(dx * dx + dy * dy);
      if (d < bd) { bd = d; best = i; }
    }

    selected = best;
    if (best < 0) say("tap closer to a creature", 1200);
    return;
  }
}

// ================= SETUP / LOOP =================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("=== CYD Creatures ==="));

  pinMode(BL_PIN, OUTPUT);
  digitalWrite(BL_PIN, HIGH);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(COL_SKY);

  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  // ---- backdrop canvas ----
  // 16bpp first (about 90KB), then 8bpp (about 45KB) if that fails. The
  // scenery is dithered anyway, so 8bpp costs almost nothing visually.
  bgSprite.setColorDepth(16);
  void* ok = bgSprite.createSprite(SCR_W, WORLD_H);

  if (!ok) {
    bgSprite.deleteSprite();
    bgSprite.setColorDepth(8);
    ok = bgSprite.createSprite(SCR_W, WORLD_H);
    Serial.println("backdrop: 16bpp failed, retrying at 8bpp");
  }

  bgReady = (ok != nullptr);
  Serial.printf("backdrop canvas: %s (heap %u)\n",
                bgReady ? "ok" : "none - drawing direct", ESP.getFreeHeap());

  SPI.begin();
  bool sdOk = SD.begin(SD_CS, SPI, 4000000);
  Serial.printf("SD: %s\n", sdOk ? "mounted" : "not found");

  if (sdOk) {
    if (!SD.exists("/brains")) {
      bool made = SD.mkdir("/brains");
      Serial.printf("mkdir /brains: %s\n", made ? "ok" : "failed");
    }
  }

  // Seed a world so the title screen can draw a Norn, but do NOT show
  // the game yet - the start screen decides whether to keep it.
  seedWorld();
  spawnGrendel();

  lastTick  = millis();
  lastFrame = millis();

  // No start screen: every boot begins a fresh game. LOAD on the HUD
  // restores a save if the player wants one.
  drawWorldBackdrop();
  drawHudChrome();
  say("new game - tickle a norn to teach it", 4000);

  Serial.printf("hud buttons painted, world %d, %d living\n",
                WORLD_W, countLivingNorns());

  Serial.printf("start screen shown (save %s)\n",
                SD.exists(SAVE_PATH) ? "present" : "absent");
  Serial.println(F("ready"));
}

void loop() {
  handleTouch();

  // ---- game over: nothing moves, but NEW GAME stays tappable ----
  if (gameOver) { delay(30); yield(); return; }

  // ---- paused: hold the world still, keep the touch handler alive ----
  if (paused) {
    // Keep a small marker on screen so the state is obvious
    static bool shown = false;
    if (!shown) {
      shown = true;
      tft.setTextSize(2);
      tft.setTextColor(COL_PAUSED, COL_SKY);
      tft.setCursor(96, 12);
      tft.print("PAUSED");
    }
    delay(30);
    yield();
    return;
  }

  // Clear the PAUSED banner if it was showing
  {
    static bool wasPaused = false;
    if (wasPaused) {
      drawWorldBackdrop();
      drawHudChrome();
      wasPaused = false;
    }
  }

  // ---- camera follows the selected creature, or drifts slowly ----
  if (selected >= 0 && creatures[selected].alive)
    cameraTarget = creatures[selected].x - SCR_W / 2.0f;
  else
    cameraTarget = cameraX;            // hold still when nothing is selected

  if (cameraTarget < 0) cameraTarget = 0;
  if (cameraTarget > WORLD_W - SCR_W) cameraTarget = WORLD_W - SCR_W;

  // Ease toward the target so the view slides rather than snapping
  // Ease toward the target. Snappier when the selection has just changed,
  // so a NEXT tap finds the creature without a long slide.
  cameraX += (cameraTarget - cameraX) * 0.14f;

  // ---- simulation tick, 10 per second ----
  unsigned long now = millis();
  if (now - lastTick >= 100) {
    lastTick = now;

    for (int i = 0; i < MAX_CREATURES; i++) updateCreature(i);
    updateGrendel();
    updateEggs();

    // Safety net: whatever else may set a Norn dead, once none are left
    // the game is over. Checked here so no death path can slip past it.
    if (!gameOver && countLivingNorns() == 0 && eggsAlive() == 0) {
      endGame();
    }

    // Food falls to the ground
    for (int i = 0; i < MAX_FOOD; i++) {
      if (!food[i].alive) continue;
      food[i].vy += 0.4f;
      food[i].y += food[i].vy;
      if (food[i].y > GROUND_Y - 3) {
        food[i].y = GROUND_Y - 3;
        food[i].vy = 0;
      }
    }
  }

  // ---- render, ~20 fps ----
  if (now - lastFrame >= 50) {
    lastFrame = now;

    drawWorldBackdrop();

    for (int i = 0; i < MAX_FOOD; i++)
      if (food[i].alive) drawFoodAt(food[i], wx2sx(food[i].x));

    for (int i = 0; i < MAX_EGGS; i++)
      if (eggs[i].alive) drawEggAt(eggs[i], wx2sx(eggs[i].x));

    for (int i = 0; i < MAX_CREATURES; i++) {
      if (!creatures[i].alive) continue;

      int sx = wx2sx(creatures[i].x);

      // Skip anything fully off screen, but keep a margin so a norn
      // walking in from the edge is not popped into view
      if (sx < -60 || sx > SCR_W + 60) continue;

      drawNorn(creatures[i], sx, GROUND_Y, i == selected);
    }

    drawGrendel(wx2sx(grendel.x));
    drawGrendelMarker();

    drawHud();
  }

  delay(5);
  yield();
}
