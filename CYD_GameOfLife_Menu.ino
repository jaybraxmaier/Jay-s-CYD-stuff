/*
 * CYD Conway's Game of Life  (ESP32-2432S028R / Cheap Yellow Display)
 *
 * A cellular automaton on a logical grid, drawn as scaled blocks, with an
 * on-screen settings menu. Double-buffered so no cell reads a
 * half-updated field.
 *
 * ---------------------------------------------------------------------
 * CONTROLS
 *   Tap grid          - stamp the current pattern at that point
 *   STEP              - advance one generation
 *   RUN / PAUSE       - toggle automatic stepping
 *   SEED              - randomise the grid, reset the generation counter
 *   MENU              - open the settings panel (pauses the simulation)
 *
 * MENU SETTINGS
 *   Speed             - ms per generation, 20..400
 *   Cell size         - 2 / 3 / 4 px. Applied on the NEXT RESET (the grid
 *                       arrays are sized at compile time from CELL_START,
 *                       so this is a boot-time setting, stored in NVS).
 *   Seed density      - percent alive when seeding, 10..60
 *   Rule              - Conway / HighLife / Seeds
 *
 * ---------------------------------------------------------------------
 * BOARD: single USB-C CYD => ST7789_2_DRIVER in User_Setup.h
 *        Micro-USB CYD     => ILI9341_DRIVER
 *
 * LIBRARIES: TFT_eSPI (CYD User_Setup.h), XPT2046_Touchscreen
 * ---------------------------------------------------------------------
 */

#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>

// ================= HARDWARE =================
#define BL_PIN    21              // backlight; 27 on some revisions

#define TOUCH_CS   33
#define TOUCH_IRQ  36
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK  25

#define TS_MINX 200
#define TS_MAXX 3700
#define TS_MINY 200
#define TS_MAXY 3700

// ================= SCREEN =================
#define SCR_W 320
#define SCR_H 240

// ================= GRID GEOMETRY =================
// The grid arrays are sized at COMPILE TIME, so cell size is a boot-time
// setting. CELL_START is the value used unless NVS holds another, and a
// change from the menu takes effect on the next reset.
#define CELL_START 3
#define MAX_CELL   4
#define MIN_CELL   2

// Size the arrays for the FINEST grid (smallest cell) so any runtime cell
// size fits. At CELL=2 that is 160x120 = 19200 cells; three byte-arrays
// of that size come to about 58KB, which the board can hold.
#define GRID_W_MAX (SCR_W / MIN_CELL)      // 160
#define GRID_H_MAX (SCR_H / MIN_CELL)      // 120

static uint8_t gridA[GRID_W_MAX * GRID_H_MAX];
static uint8_t gridB[GRID_W_MAX * GRID_H_MAX];
static uint8_t prevShown[GRID_W_MAX * GRID_H_MAX];

static uint8_t* cur  = gridA;
static uint8_t* next = gridB;

// Active grid dimensions, derived from the cell size in use
static int cellPx = CELL_START;
static int gridW  = SCR_W / CELL_START;
static int gridH  = SCR_H / CELL_START;

// ================= SETTINGS =================
struct Settings {
  int  stepDelayMs;
  int  cellSize;
  int  seedDensity;
  int  rule;          // 0 Conway, 1 HighLife, 2 Seeds
};

static Settings cfg;
static Preferences prefs;

#define RULE_CONWAY   0
#define RULE_HIGHLIFE 1
#define RULE_SEEDS    2
#define RULE_COUNT    3

static const char* RULE_NAMES[RULE_COUNT] = { "Conway", "HighLife", "Seeds" };

static void loadSettings() {
  prefs.begin("life", true);
  cfg.stepDelayMs = prefs.getInt("speed", 80);
  cfg.cellSize    = prefs.getInt("cell", CELL_START);
  cfg.seedDensity = prefs.getInt("density", 28);
  cfg.rule        = prefs.getInt("rule", RULE_CONWAY);
  prefs.end();

  cfg.stepDelayMs = constrain(cfg.stepDelayMs, 20, 400);
  cfg.cellSize    = constrain(cfg.cellSize, MIN_CELL, MAX_CELL);
  cfg.seedDensity = constrain(cfg.seedDensity, 10, 60);
  cfg.rule        = constrain(cfg.rule, 0, RULE_COUNT - 1);
}

static void saveSettings() {
  prefs.begin("life", false);
  prefs.putInt("speed", cfg.stepDelayMs);
  prefs.putInt("cell", cfg.cellSize);
  prefs.putInt("density", cfg.seedDensity);
  prefs.putInt("rule", cfg.rule);
  prefs.end();
}

// ================= UI =================
#define BAR_H 24
static const int BAR_Y = SCR_H - BAR_H;

// Four buttons across the bar
static const int BTN_W    = 74;
static const int BTN_H    = 20;
static const int BTN_Y    = BAR_Y + 2;
static const int BTN_GAP  = 6;
static const int BTN_X0   = 6;

// Button indices
enum { B_STEP = 0, B_PAUSE, B_SEED, B_MENU, B_COUNT };

static const char* BTN_LABELS[B_COUNT] = { "STEP", "RUN", "SEED", "MENU" };

static int btnX(int i) { return BTN_X0 + i * (BTN_W + BTN_GAP); }

static bool running   = true;
static int  generation = 0;
static unsigned long lastStepMs = 0;

// ================= MENU =================
static bool menuOpen = false;

// Menu rows
enum { M_SPEED = 0, M_CELL, M_DENSITY, M_RULE, M_DONE, M_RESEED, M_COUNT };

static const int MENU_ROW_H   = 32;
static const int MENU_TOP     = 34;
static const int MENU_BTN_W   = 40;
static const int MENU_BTN_H   = 24;
static const int MENU_MINUS_X = 8;
static const int MENU_PLUS_X  = SCR_W - 8 - MENU_BTN_W;

static const char* MENU_ROW_LABELS[4] = { "Speed", "Cell size", "Density", "Rule" };

TFT_eSPI tft = TFT_eSPI();
SPIClass touchSPI(HSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

// ================= COLOURS =================
#define COL_BG      0x0000
#define COL_CELL    0x07E0
#define COL_BAR     0x10A2
#define COL_TEXT    0xFFFF
#define COL_DIM     0x7BEF
#define COL_ACCENT  0x07FF
#define COL_PANEL   0x0861

// ================= PATTERNS =================
struct Pattern {
  const char* name;
  uint8_t w, h;
  const char* rows[8];
};

static const Pattern PATTERNS[] = {
  { "Glider",  3, 3, { ".X.", "..X", "XXX" } },
  { "Block",   2, 2, { "XX", "XX" } },
  { "Blinker", 3, 1, { "XXX" } }
};
static const int PATTERN_COUNT = sizeof(PATTERNS) / sizeof(PATTERNS[0]);
static int currentPattern = 0;

// ================= GRID =================
static inline int idxOf(int x, int y) { return y * gridW + x; }

static int neighbours(const uint8_t* g, int x, int y) {
  int count = 0;
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      if (dx == 0 && dy == 0) continue;
      int nx = x + dx;
      int ny = y + dy;
      if (nx < 0) nx = gridW - 1;
      else if (nx >= gridW) nx = 0;
      if (ny < 0) ny = gridH - 1;
      else if (ny >= gridH) ny = 0;
      count += g[ny * gridW + nx];
    }
  }
  return count;
}

static void clearGrid(uint8_t* g) {
  memset(g, 0, (size_t)gridW * gridH);
}

static void randomSeedGrid(uint8_t* g, int densityPercent) {
  for (int i = 0; i < gridW * gridH; i++) {
    g[i] = (random(100) < densityPercent) ? 1 : 0;
  }
}

// ================= RULES =================
// Conway   B3/S23 : born on 3, survives on 2 or 3
// HighLife B36/S23: born on 3 or 6, survives on 2 or 3
// Seeds    B2/S   : born on 2, never survives
static uint8_t nextState(uint8_t alive, int n) {
  switch (cfg.rule) {
    case RULE_HIGHLIFE:
      if (alive) return (n == 2 || n == 3) ? 1 : 0;
      return (n == 3 || n == 6) ? 1 : 0;

    case RULE_SEEDS:
      if (alive) return 0;
      return (n == 2) ? 1 : 0;

    case RULE_CONWAY:
    default:
      if (alive) return (n == 2 || n == 3) ? 1 : 0;
      return (n == 3) ? 1 : 0;
  }
}

static void stepGeneration() {
  for (int y = 0; y < gridH; y++) {
    for (int x = 0; x < gridW; x++) {
      int i = y * gridW + x;
      next[i] = nextState(cur[i], neighbours(cur, x, y));
    }
  }
  uint8_t* t = cur; cur = next; next = t;
  generation++;
}

// ================= DRAWING =================
static void drawCell(int x, int y, bool alive) {
  int px = x * cellPx;
  int py = y * cellPx;
  if (py + cellPx > BAR_Y) return;              // don't paint under the bar
  tft.fillRect(px, py, cellPx, cellPx, alive ? COL_CELL : COL_BG);
}

static void renderDiff() {
  for (int y = 0; y < gridH; y++) {
    for (int x = 0; x < gridW; x++) {
      int i = y * gridW + x;
      if (cur[i] != prevShown[i]) {
        drawCell(x, y, cur[i] != 0);
        prevShown[i] = cur[i];
      }
    }
  }
}

static void renderAll() {
  for (int y = 0; y < gridH; y++)
    for (int x = 0; x < gridW; x++) {
      int i = y * gridW + x;
      drawCell(x, y, cur[i] != 0);
      prevShown[i] = cur[i];
    }
}

// ================= BAR =================
static void drawBar() {
  tft.fillRect(0, BAR_Y, SCR_W, BAR_H, COL_BAR);
  tft.drawFastHLine(0, BAR_Y, SCR_W, COL_DIM);
  tft.setTextSize(1);

  for (int i = 0; i < B_COUNT; i++) {
    int x = btnX(i);
    bool active = false;

    if (i == B_PAUSE) {
      tft.fillRoundRect(x, BTN_Y, BTN_W, BTN_H, 4, COL_BG);
      tft.drawRoundRect(x, BTN_Y, BTN_W, BTN_H, 4, running ? COL_ACCENT : COL_DIM);
      tft.setTextColor(running ? COL_ACCENT : COL_TEXT, COL_BG);
      tft.setCursor(x + 8, BTN_Y + 6);
      tft.print(running ? "PAUSE" : "RUN");
      continue;
    }

    tft.fillRoundRect(x, BTN_Y, BTN_W, BTN_H, 4, COL_BG);
    tft.drawRoundRect(x, BTN_Y, BTN_W, BTN_H, 4, i == B_MENU ? COL_ACCENT : COL_DIM);
    tft.setTextColor(i == B_MENU ? COL_ACCENT : COL_TEXT, COL_BG);
    tft.setCursor(x + 8, BTN_Y + 6);
    tft.print(BTN_LABELS[i]);
  }

  // Generation counter, right-aligned in the PAUSE button
  char gen[12];
  snprintf(gen, sizeof(gen), "%d", generation);
  tft.setTextColor(COL_DIM, COL_BG);
  int gw = (int)strlen(gen) * 6;
  tft.setCursor(btnX(B_PAUSE) + BTN_W - gw - 6, BTN_Y + 6);
  tft.print(gen);
}

static void updateGenCounter() {
  char gen[12];
  snprintf(gen, sizeof(gen), "%d", generation);
  tft.setTextSize(1);
  tft.fillRect(btnX(B_PAUSE) + BTN_W - 60, BTN_Y + 6, 60, 8, COL_BG);
  tft.setTextColor(COL_DIM, COL_BG);
  int gw = (int)strlen(gen) * 6;
  tft.setCursor(btnX(B_PAUSE) + BTN_W - gw - 6, BTN_Y + 6);
  tft.print(gen);
}

// ================= MENU DRAW =================
static String valueForRow(int row) {
  char buf[24];
  switch (row) {
    case M_SPEED:
      snprintf(buf, sizeof(buf), "%d ms", cfg.stepDelayMs);
      break;
    case M_CELL:
      snprintf(buf, sizeof(buf), "%d px%s", cfg.cellSize,
               (cfg.cellSize != cellPx) ? " *" : "");
      break;
    case M_DENSITY:
      snprintf(buf, sizeof(buf), "%d %%", cfg.seedDensity);
      break;
    case M_RULE:
      snprintf(buf, sizeof(buf), "%s", RULE_NAMES[cfg.rule]);
      break;
    default:
      buf[0] = '\0';
  }
  return String(buf);
}

static void drawMenu() {
  tft.fillScreen(COL_BG);

  // Header
  tft.fillRect(0, 0, SCR_W, 30, COL_PANEL);
  tft.setTextSize(2);
  tft.setTextColor(COL_ACCENT, COL_PANEL);
  tft.setCursor(8, 7);
  tft.print("SETTINGS");

  tft.setTextSize(1);

  for (int row = 0; row < 4; row++) {
    int y = MENU_TOP + row * (MENU_ROW_H + 4);

    // Row background
    tft.fillRoundRect(4, y, SCR_W - 8, MENU_ROW_H, 5, COL_PANEL);

    // Label
    tft.setTextColor(COL_DIM, COL_PANEL);
    tft.setCursor(12, y + 4);
    tft.print(MENU_ROW_LABELS[row]);

    // Value
    String v = valueForRow(row);
    tft.setTextColor(COL_TEXT, COL_PANEL);
    int vw = v.length() * 6;
    tft.setCursor((SCR_W - vw) / 2, y + 16);
    tft.print(v);

    // Minus
    tft.fillRoundRect(MENU_MINUS_X, y + 4, MENU_BTN_W, MENU_BTN_H, 4, COL_BG);
    tft.drawRoundRect(MENU_MINUS_X, y + 4, MENU_BTN_W, MENU_BTN_H, 4, COL_DIM);
    tft.setTextSize(2);
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.setCursor(MENU_MINUS_X + 14, y + 8);
    tft.print("-");

    // Plus
    tft.fillRoundRect(MENU_PLUS_X, y + 4, MENU_BTN_W, MENU_BTN_H, 4, COL_BG);
    tft.drawRoundRect(MENU_PLUS_X, y + 4, MENU_BTN_W, MENU_BTN_H, 4, COL_DIM);
    tft.setCursor(MENU_PLUS_X + 12, y + 8);
    tft.print("+");

    tft.setTextSize(1);
  }

  // Footer buttons
  int fy = MENU_TOP + 4 * (MENU_ROW_H + 4) + 6;
  int bw = (SCR_W - 8 - 2 * 8) / 2;
  int doneX = 4;
  int seedX = 4 + bw + 8;

  tft.fillRoundRect(doneX, fy, bw, 26, 5, COL_BG);
  tft.drawRoundRect(doneX, fy, bw, 26, 5, COL_ACCENT);
  tft.setTextSize(1);
  tft.setTextColor(COL_ACCENT, COL_BG);
  tft.setCursor(doneX + (bw - 24) / 2, fy + 10);
  tft.print("DONE");

  tft.fillRoundRect(seedX, fy, bw, 26, 5, COL_BG);
  tft.drawRoundRect(seedX, fy, bw, 26, 5, COL_DIM);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setCursor(seedX + (bw - 40) / 2, fy + 10);
  tft.print("RESEED");

  // Hint about the cell-size restart
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setCursor(6, fy + 32);
  tft.print("* cell size applies after a reset");
}

// ================= MENU ACTIONS =================
static void reseedNow() {
  randomSeedGrid(cur, cfg.seedDensity);
  generation = 0;
  clearGrid(next);
  memset(prevShown, 0xFF, (size_t)gridW * gridH);
  renderAll();
}

static void menuAdjust(int row, int delta) {
  switch (row) {
    case M_SPEED:
      cfg.stepDelayMs = constrain(cfg.stepDelayMs + delta * 20, 20, 400);
      break;
    case M_CELL:
      cfg.cellSize = constrain(cfg.cellSize + delta, MIN_CELL, MAX_CELL);
      break;
    case M_DENSITY:
      cfg.seedDensity = constrain(cfg.seedDensity + delta * 5, 10, 60);
      break;
    case M_RULE:
      cfg.rule = (cfg.rule + delta + RULE_COUNT) % RULE_COUNT;
      break;
  }
  saveSettings();
  drawMenu();
}

// ================= TOUCH =================
static bool getTouch(int &x, int &y) {
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint();
  x = map(p.x, TS_MINX, TS_MAXX, 0, SCR_W);
  y = map(p.y, TS_MINY, TS_MAXY, 0, SCR_H);
  x = constrain(x, 0, SCR_W - 1);
  y = constrain(y, 0, SCR_H - 1);
  return true;
}

static void handleMenuTouch(int x, int y) {
  // Rows
  for (int row = 0; row < 4; row++) {
    int ry = MENU_TOP + row * (MENU_ROW_H + 4);
    if (y < ry || y > ry + MENU_ROW_H) continue;

    if (x >= MENU_MINUS_X && x < MENU_MINUS_X + MENU_BTN_W) {
      menuAdjust(row, -1);
      return;
    }
    if (x >= MENU_PLUS_X && x < MENU_PLUS_X + MENU_BTN_W) {
      menuAdjust(row, +1);
      return;
    }
    return;
  }

  // Footer
  int fy = MENU_TOP + 4 * (MENU_ROW_H + 4) + 6;
  if (y < fy || y > fy + 26) return;

  int bw = (SCR_W - 8 - 2 * 8) / 2;
  int doneX = 4;
  int seedX = 4 + bw + 8;

  if (x >= doneX && x < doneX + bw) {
    menuOpen = false;
    lastStepMs = millis();
    renderAll();
    drawBar();
    return;
  }
  if (x >= seedX && x < seedX + bw) {
    reseedNow();
    drawMenu();
    return;
  }
}

static void stampPattern(int cx, int cy) {
  const Pattern& p = PATTERNS[currentPattern];
  for (int y = 0; y < p.h; y++)
    for (int x = 0; x < p.w; x++) {
      int gx = cx + x, gy = cy + y;
      if (gx < 0 || gx >= gridW || gy < 0 || gy >= gridH) continue;
      cur[gy * gridW + gx] = (p.rows[y][x] == 'X') ? 1 : 0;
    }
  renderDiff();
}

static void handleTouch() {
  static unsigned long lastTouch = 0;

  int x, y;
  if (!getTouch(x, y)) return;

  unsigned long now = millis();
  if (now - lastTouch < 200) {
    while (ts.touched()) { delay(5); yield(); }
    return;
  }
  lastTouch = now;
  while (ts.touched()) { delay(5); yield(); }

  if (menuOpen) {
    handleMenuTouch(x, y);
    return;
  }

  // Bar buttons
  if (y >= BTN_Y && y <= BTN_Y + BTN_H) {
    for (int i = 0; i < B_COUNT; i++) {
      int bx = btnX(i);
      if (x < bx || x >= bx + BTN_W) continue;

      if (i == B_STEP) {
        stepGeneration();
        renderDiff();
        updateGenCounter();
      } else if (i == B_PAUSE) {
        running = !running;
        drawBar();
      } else if (i == B_SEED) {
        reseedNow();
        drawBar();
      } else if (i == B_MENU) {
        menuOpen = true;
        drawMenu();
      }
      return;
    }
    return;
  }

  // Grid tap: stamp the pattern centred on the point
  int gx = x / cellPx;
  int gy = y / cellPx;
  stampPattern(gx - PATTERNS[currentPattern].w / 2,
               gy - PATTERNS[currentPattern].h / 2);
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("=== CYD Game of Life ==="));

  pinMode(BL_PIN, OUTPUT);
  digitalWrite(BL_PIN, HIGH);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(COL_BG);

  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  randomSeed(analogRead(34));

  loadSettings();

  // Apply the stored cell size. If it differs from the compiled default,
  // the grid dimensions change here - which is why it is a boot setting.
  cellPx = cfg.cellSize;
  gridW  = SCR_W / cellPx;
  gridH  = SCR_H / cellPx;

  clearGrid(gridA);
  clearGrid(gridB);
  memset(prevShown, 0xFF, (size_t)gridW * gridH);

  randomSeedGrid(cur, cfg.seedDensity);

  renderAll();
  drawBar();

  lastStepMs = millis();

  Serial.printf("grid %dx%d, cell %dpx, speed %dms, rule %s\n",
                gridW, gridH, cellPx, cfg.stepDelayMs, RULE_NAMES[cfg.rule]);
  Serial.println(F("running"));
}

// ================= LOOP =================
void loop() {
  handleTouch();

  if (!menuOpen && running &&
      millis() - lastStepMs >= (unsigned long)cfg.stepDelayMs) {
    lastStepMs = millis();
    stepGeneration();
    renderDiff();
    updateGenCounter();
  }

  delay(2);
  yield();
}
