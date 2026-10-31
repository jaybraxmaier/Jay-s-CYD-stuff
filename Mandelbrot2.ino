#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>
#include <math.h>

// ============================================================
// ESP32 CYD 2.8" 320x240
// ESP32-2432S028R
// ============================================================

#define SCREEN_W       320
#define SCREEN_H       240
#define FRACTAL_H      210
#define STATUS_H       30

// Backlight
#define BACKLIGHT_PIN  21

// XPT2046 touch
#define TOUCH_CS       33
#define TOUCH_IRQ      36
#define TOUCH_SCLK     25
#define TOUCH_MISO     39
#define TOUCH_MOSI     32

// Touch calibration
#define TOUCH_MIN_X    200
#define TOUCH_MAX_X    3800
#define TOUCH_MIN_Y    240
#define TOUCH_MAX_Y    3800

// ============================================================
// OBJECTS
// ============================================================

TFT_eSPI tft = TFT_eSPI();

SPIClass touchSPI(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);

// ============================================================
// SCREEN STATES
// ============================================================

enum ScreenMode {
  SCREEN_MENU,
  SCREEN_EXPLORE,
  SCREEN_COLORS,
  SCREEN_ITERATIONS,
  SCREEN_FAMOUS,
  SCREEN_JULIA
};

ScreenMode screenMode = SCREEN_MENU;

// ============================================================
// FRACTAL MODES
// ============================================================

#define MODE_MANDELBROT 0
#define MODE_JULIA      1

int fractalMode = MODE_MANDELBROT;

// ============================================================
// FRACTAL SETTINGS
// ============================================================

double centerX = -0.5;
double centerY = 0.0;

double viewWidth = 3.2;

int maxIterations = 120;

// Julia constant
double juliaCX = -0.8;
double juliaCY = 0.156;

// ============================================================
// PALETTES
// ============================================================

#define PALETTE_COUNT 10

int palette = 0;

const char *paletteNames[PALETTE_COUNT] = {
  "Classic",
  "Fire",
  "Ocean",
  "Rainbow",
  "Electric",
  "Green",
  "Purple",
  "Gold",
  "Ice",
  "Mono"
};

// ============================================================
// FAMOUS MANDELBROT LOCATIONS
// ============================================================

#define FAMOUS_COUNT 8

const char *famousNames[FAMOUS_COUNT] = {
  "Main Set",
  "Seahorse",
  "Elephant",
  "Double Spiral",
  "Triple Spiral",
  "Mini Mandel",
  "Valley",
  "Lightning"
};

double famousX[FAMOUS_COUNT] = {
  -0.5,
  -0.743643887037151,
  0.285,
  -0.745,
  -0.7453,
  -0.101096,
  -0.16,
  -1.25066
};

double famousY[FAMOUS_COUNT] = {
  0.0,
  0.13182590420533,
  0.01,
  0.1127,
  0.1127,
  0.956,
  1.0405,
  0.02012
};

double famousZoom[FAMOUS_COUNT] = {
  3.2,
  0.02,
  0.025,
  0.015,
  0.004,
  0.05,
  0.02,
  0.01
};

// ============================================================
// JULIA PRESETS
// ============================================================

#define JULIA_COUNT 8

const char *juliaNames[JULIA_COUNT] = {
  "Dendrite",
  "Rabbit",
  "Spiral",
  "Galaxy",
  "Snowflake",
  "Dragon",
  "Lightning",
  "Classic"
};

double juliaPresetsX[JULIA_COUNT] = {
  0.0,
  -0.123,
  -0.745,
  -0.4,
  0.355,
  -0.8,
  -0.835,
  -0.7269
};

double juliaPresetsY[JULIA_COUNT] = {
  1.0,
  0.745,
  0.113,
  0.6,
  0.355,
  0.156,
  0.2321,
  0.1889
};

// ============================================================
// TOUCH STATE
// ============================================================

bool wasTouched = false;

int touchStartX = 0;
int touchStartY = 0;

int touchLastX = 0;
int touchLastY = 0;

bool touchMoved = false;

// ============================================================
// COLOR HELPERS
// ============================================================

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) |
         ((g & 0xFC) << 3) |
         (b >> 3);
}

uint16_t paletteColor(int iteration) {

  if (iteration >= maxIterations) {
    return TFT_BLACK;
  }

  double t = (double)iteration / (double)maxIterations;

  int r;
  int g;
  int b;

  switch (palette) {

    // --------------------------------------------------------
    // CLASSIC
    // --------------------------------------------------------
    case 0:
      r = (int)(9 * (1 - t) * t * t * t * 255);
      g = (int)(15 * (1 - t) * (1 - t) * t * t * 255);
      b = (int)(8.5 * (1 - t) * (1 - t) * (1 - t) * t * 255);
      break;

    // --------------------------------------------------------
    // FIRE
    // --------------------------------------------------------
    case 1:
      r = (int)(255 * t);
      g = (int)(255 * t * t);
      b = (int)(80 * t * t * t);
      break;

    // --------------------------------------------------------
    // OCEAN
    // --------------------------------------------------------
    case 2:
      r = (int)(20 * t);
      g = (int)(150 * t);
      b = (int)(255 * t);
      break;

    // --------------------------------------------------------
    // RAINBOW
    // --------------------------------------------------------
    case 3: {
      double h = t * 360.0;

      if (h < 60) {
        r = 255;
        g = (int)(h * 4.25);
        b = 0;
      }
      else if (h < 120) {
        r = (int)((120 - h) * 4.25);
        g = 255;
        b = 0;
      }
      else if (h < 180) {
        r = 0;
        g = 255;
        b = (int)((h - 120) * 4.25);
      }
      else if (h < 240) {
        r = 0;
        g = (int)((240 - h) * 4.25);
        b = 255;
      }
      else if (h < 300) {
        r = (int)((h - 240) * 4.25);
        g = 0;
        b = 255;
      }
      else {
        r = 255;
        g = 0;
        b = (int)((360 - h) * 4.25);
      }

      break;
    }

    // --------------------------------------------------------
    // ELECTRIC
    // --------------------------------------------------------
    case 4:
      r = (int)(100 * t);
      g = (int)(200 * t);
      b = 255;
      break;

    // --------------------------------------------------------
    // GREEN
    // --------------------------------------------------------
    case 5:
      r = (int)(40 * t);
      g = (int)(255 * t);
      b = (int)(80 * t);
      break;

    // --------------------------------------------------------
    // PURPLE
    // --------------------------------------------------------
    case 6:
      r = (int)(220 * t);
      g = (int)(50 * t);
      b = (int)(255 * t);
      break;

    // --------------------------------------------------------
    // GOLD
    // --------------------------------------------------------
    case 7:
      r = 255;
      g = (int)(220 * t);
      b = (int)(60 * t);
      break;

    // --------------------------------------------------------
    // ICE
    // --------------------------------------------------------
    case 8:
      r = (int)(100 * t);
      g = (int)(220 * t);
      b = 255;
      break;

    // --------------------------------------------------------
    // MONO
    // --------------------------------------------------------
    default:
      r = (int)(255 * t);
      g = r;
      b = r;
      break;
  }

  r = constrain(r, 0, 255);
  g = constrain(g, 0, 255);
  b = constrain(b, 0, 255);

  return rgb565(r, g, b);
}

// ============================================================
// RESET VIEW
// ============================================================

void resetView() {

  if (fractalMode == MODE_MANDELBROT) {
    centerX = -0.5;
    centerY = 0.0;
    viewWidth = 3.2;
  }
  else {
    centerX = 0.0;
    centerY = 0.0;
    viewWidth = 3.2;
  }
}

// ============================================================
// DRAW MAIN MENU
// ============================================================

void drawMainMenu() {

  tft.fillScreen(TFT_BLACK);

  tft.setTextDatum(MC_DATUM);

  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(2);

  tft.drawString(
    "FRACTAL EXPLORER",
    SCREEN_W / 2,
    15
  );

  tft.setTextSize(1);

  // Mandelbrot
  tft.fillRoundRect(
    10, 40,
    145, 35,
    5,
    TFT_BLUE
  );

  tft.setTextColor(TFT_WHITE);
  tft.drawString(
    "MANDELBROT",
    82,
    57
  );

  // Julia
  tft.fillRoundRect(
    165, 40,
    145, 35,
    5,
    TFT_PURPLE
  );

  tft.drawString(
    "JULIA SET",
    237,
    57
  );

  // Colors
  tft.fillRoundRect(
    10, 85,
    145, 35,
    5,
    TFT_DARKGREY
  );

  tft.drawString(
    "COLORS",
    82,
    102
  );

  // Iterations
  tft.fillRoundRect(
    165, 85,
    145, 35,
    5,
    TFT_DARKGREY
  );

  tft.drawString(
    "ITERATIONS",
    237,
    102
  );

  // Famous
  tft.fillRoundRect(
    10, 130,
    300, 35,
    5,
    TFT_GREEN
  );

  tft.setTextColor(TFT_BLACK);
  tft.drawString(
    "FAMOUS LOCATIONS",
    160,
    147
  );

  // Reset
  tft.fillRoundRect(
    10, 175,
    145, 35,
    5,
    TFT_RED
  );

  tft.setTextColor(TFT_WHITE);
  tft.drawString(
    "RESET",
    82,
    192
  );

  // Explore
  tft.fillRoundRect(
    165, 175,
    145, 35,
    5,
    TFT_ORANGE
  );

  tft.drawString(
    "EXPLORE",
    237,
    192
  );

  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY);

  tft.drawString(
    "Tap a button",
    160,
    228
  );

  tft.setTextDatum(TL_DATUM);
}

// ============================================================
// COLOR MENU
// ============================================================

void drawColorMenu() {

  tft.fillScreen(TFT_BLACK);

  tft.setTextDatum(MC_DATUM);

  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(2);
  tft.drawString(
    "COLOR PALETTE",
    160,
    15
  );

  tft.setTextSize(1);

  for (int i = 0; i < PALETTE_COUNT; i++) {

    int col = i % 2;
    int row = i / 2;

    int x = (col == 0) ? 10 : 165;
    int y = 35 + row * 34;

    uint16_t c;

    if (i == palette) {
      c = TFT_BLUE;
    }
    else {
      c = TFT_DARKGREY;
    }

    tft.fillRoundRect(
      x, y,
      145, 28,
      4,
      c
    );

    tft.setTextColor(TFT_WHITE);

    tft.drawString(
      paletteNames[i],
      x + 72,
      y + 14
    );
  }

  tft.fillRoundRect(
    10, 205,
    300, 28,
    4,
    TFT_RED
  );

  tft.drawString(
    "BACK",
    160,
    219
  );

  tft.setTextDatum(TL_DATUM);
}

// ============================================================
// ITERATION MENU
// ============================================================

void drawIterationMenu() {

  tft.fillScreen(TFT_BLACK);

  tft.setTextDatum(MC_DATUM);

  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(2);

  tft.drawString(
    "ITERATIONS",
    160,
    15
  );

  int values[6] = {
    40, 80, 120,
    200, 400, 800
  };

  tft.setTextSize(1);

  for (int i = 0; i < 6; i++) {

    int col = i % 2;
    int row = i / 2;

    int x = (col == 0) ? 10 : 165;
    int y = 40 + row * 50;

    uint16_t c =
      (values[i] == maxIterations)
      ? TFT_BLUE
      : TFT_DARKGREY;

    tft.fillRoundRect(
      x, y,
      145, 38,
      4,
      c
    );

    tft.setTextColor(TFT_WHITE);

    char buffer[20];

    sprintf(
      buffer,
      "%d",
      values[i]
    );

    tft.drawString(
      buffer,
      x + 72,
      y + 19
    );
  }

  tft.fillRoundRect(
    10, 205,
    300, 28,
    4,
    TFT_RED
  );

  tft.drawString(
    "BACK",
    160,
    219
  );

  tft.setTextDatum(TL_DATUM);
}

// ============================================================
// FAMOUS MENU
// ============================================================

void drawFamousMenu() {

  tft.fillScreen(TFT_BLACK);

  tft.setTextDatum(MC_DATUM);

  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(2);

  tft.drawString(
    "FAMOUS LOCATIONS",
    160,
    15
  );

  tft.setTextSize(1);

  for (int i = 0; i < FAMOUS_COUNT; i++) {

    int y = 35 + i * 21;

    uint16_t c =
      (i == 0)
      ? TFT_BLUE
      : TFT_DARKGREY;

    tft.fillRoundRect(
      10, y,
      300, 18,
      3,
      c
    );

    tft.setTextColor(TFT_WHITE);

    tft.drawString(
      famousNames[i],
      160,
      y + 9
    );
  }

  tft.fillRoundRect(
    10, 205,
    300, 28,
    4,
    TFT_RED
  );

  tft.drawString(
    "BACK",
    160,
    219
  );

  tft.setTextDatum(TL_DATUM);
}

// ============================================================
// JULIA MENU
// ============================================================

void drawJuliaMenu() {

  tft.fillScreen(TFT_BLACK);

  tft.setTextDatum(MC_DATUM);

  tft.setTextColor(TFT_CYAN);
  tft.setTextSize(2);

  tft.drawString(
    "JULIA PRESETS",
    160,
    15
  );

  tft.setTextSize(1);

  for (int i = 0; i < JULIA_COUNT; i++) {

    int col = i % 2;
    int row = i / 2;

    int x = (col == 0) ? 10 : 165;
    int y = 35 + row * 39;

    tft.fillRoundRect(
      x, y,
      145, 32,
      4,
      TFT_DARKGREY
    );

    tft.setTextColor(TFT_WHITE);

    tft.drawString(
      juliaNames[i],
      x + 72,
      y + 16
    );
  }

  tft.fillRoundRect(
    10, 205,
    300, 28,
    4,
    TFT_RED
  );

  tft.drawString(
    "BACK",
    160,
    219
  );

  tft.setTextDatum(TL_DATUM);
}

// ============================================================
// STATUS BAR
// ============================================================

void drawStatusBar() {

  tft.fillRect(
    0,
    FRACTAL_H,
    SCREEN_W,
    STATUS_H,
    TFT_DARKGREY
  );

  tft.setTextDatum(ML_DATUM);
  tft.setTextSize(1);

  tft.setTextColor(TFT_WHITE);

  if (fractalMode == MODE_MANDELBROT) {
    tft.drawString(
      "MANDELBROT",
      5,
      FRACTAL_H + 15
    );
  }
  else {
    tft.drawString(
      "JULIA",
      5,
      FRACTAL_H + 15
    );
  }

  // Menu button
  tft.fillRoundRect(
    265,
    FRACTAL_H + 4,
    50,
    22,
    3,
    TFT_BLUE
  );

  tft.setTextDatum(MC_DATUM);

  tft.setTextColor(TFT_WHITE);

  tft.drawString(
    "MENU",
    290,
    FRACTAL_H + 15
  );

  tft.setTextDatum(TL_DATUM);
}

// ============================================================
// MANDELBROT CALCULATION
// ============================================================

int mandelbrot(double cx, double cy) {

  double zx = 0.0;
  double zy = 0.0;

  int i;

  for (i = 0; i < maxIterations; i++) {

    double zx2 = zx * zx;
    double zy2 = zy * zy;

    if (zx2 + zy2 > 4.0) {
      break;
    }

    zy = 2.0 * zx * zy + cy;
    zx = zx2 - zy2 + cx;
  }

  return i;
}

// ============================================================
// JULIA CALCULATION
// ============================================================

int julia(double zx, double zy) {

  int i;

  for (i = 0; i < maxIterations; i++) {

    double zx2 = zx * zx;
    double zy2 = zy * zy;

    if (zx2 + zy2 > 4.0) {
      break;
    }

    zy = 2.0 * zx * zy + juliaCY;
    zx = zx2 - zy2 + juliaCX;
  }

  return i;
}

// ============================================================
// RENDER FRACTAL
// ============================================================

void renderFractal() {

  tft.fillScreen(TFT_BLACK);

  double aspect =
    (double)SCREEN_W /
    (double)FRACTAL_H;

  double viewHeight =
    viewWidth / aspect;

  double minX =
    centerX - viewWidth / 2.0;

  double maxX =
    centerX + viewWidth / 2.0;

  double minY =
    centerY - viewHeight / 2.0;

  double maxY =
    centerY + viewHeight / 2.0;

  for (int py = 0; py < FRACTAL_H; py++) {

    double cy =
      minY +
      ((double)py / (double)(FRACTAL_H - 1)) *
      (maxY - minY);

    for (int px = 0; px < SCREEN_W; px++) {

      double cx =
        minX +
        ((double)px / (double)(SCREEN_W - 1)) *
        (maxX - minX);

      int iteration;

      if (fractalMode == MODE_MANDELBROT) {
        iteration = mandelbrot(cx, cy);
      }
      else {
        iteration = julia(cx, cy);
      }

      uint16_t color =
        paletteColor(iteration);

      tft.drawPixel(
        px,
        py,
        color
      );
    }

    // Keep ESP32 responsive
    if ((py & 7) == 0) {
      yield();
    }
  }

  drawStatusBar();
}

// ============================================================
// ZOOM AT SCREEN LOCATION
// ============================================================

void zoomAt(
  int screenX,
  int screenY,
  double zoomFactor
) {

  if (screenY >= FRACTAL_H) {
    return;
  }

  double aspect =
    (double)SCREEN_W /
    (double)FRACTAL_H;

  double oldViewHeight =
    viewWidth / aspect;

  double oldMinX =
    centerX - viewWidth / 2.0;

  double oldMinY =
    centerY - oldViewHeight / 2.0;

  double oldCX =
    oldMinX +
    ((double)screenX / SCREEN_W) *
    viewWidth;

  double oldCY =
    oldMinY +
    ((double)screenY / FRACTAL_H) *
    oldViewHeight;

  // Zoom
  viewWidth *= zoomFactor;

  // Don't go too far
  if (viewWidth < 0.000000000001) {
    viewWidth = 0.000000000001;
  }

  double newViewHeight =
    viewWidth / aspect;

  double newMinX =
    centerX - viewWidth / 2.0;

  double newMinY =
    centerY - newViewHeight / 2.0;

  double newCX =
    newMinX +
    ((double)screenX / SCREEN_W) *
    viewWidth;

  double newCY =
    newMinY +
    ((double)screenY / FRACTAL_H) *
    newViewHeight;

  // Keep the point under the finger
  centerX += oldCX - newCX;
  centerY += oldCY - newCY;
}

// ============================================================
// PAN
// ============================================================

void panFractal(
  int dx,
  int dy
) {

  double aspect =
    (double)SCREEN_W /
    (double)FRACTAL_H;

  double viewHeight =
    viewWidth / aspect;

  double moveX =
    ((double)dx / SCREEN_W) *
    viewWidth;

  double moveY =
    ((double)dy / FRACTAL_H) *
    viewHeight;

  centerX -= moveX;
  centerY -= moveY;
}

// ============================================================
// TOUCH COORDINATES
// ============================================================

void getTouchXY(
  int &x,
  int &y
) {

  TS_Point p =
    touch.getPoint();

  x = map(
    p.x,
    TOUCH_MIN_X,
    TOUCH_MAX_X,
    0,
    SCREEN_W - 1
  );

  y = map(
    p.y,
    TOUCH_MIN_Y,
    TOUCH_MAX_Y,
    0,
    SCREEN_H - 1
  );

  x = constrain(
    x,
    0,
    SCREEN_W - 1
  );

  y = constrain(
    y,
    0,
    SCREEN_H - 1
  );
}

// ============================================================
// HANDLE MENU TOUCH
// ============================================================

void handleMenuTouch(
  int x,
  int y
) {

  // ----------------------------------------------------------
  // MANDELBROT
  // ----------------------------------------------------------

  if (
    x >= 10 &&
    x <= 155 &&
    y >= 40 &&
    y <= 75
  ) {

    fractalMode = MODE_MANDELBROT;

    resetView();

    screenMode = SCREEN_EXPLORE;

    renderFractal();

    return;
  }

  // ----------------------------------------------------------
  // JULIA
  // ----------------------------------------------------------

  if (
    x >= 165 &&
    x <= 310 &&
    y >= 40 &&
    y <= 75
  ) {

    screenMode = SCREEN_JULIA;

    drawJuliaMenu();

    return;
  }

  // ----------------------------------------------------------
  // COLORS
  // ----------------------------------------------------------

  if (
    x >= 10 &&
    x <= 155 &&
    y >= 85 &&
    y <= 120
  ) {

    screenMode = SCREEN_COLORS;

    drawColorMenu();

    return;
  }

  // ----------------------------------------------------------
  // ITERATIONS
  // ----------------------------------------------------------

  if (
    x >= 165 &&
    x <= 310 &&
    y >= 85 &&
    y <= 120
  ) {

    screenMode = SCREEN_ITERATIONS;

    drawIterationMenu();

    return;
  }

  // ----------------------------------------------------------
  // FAMOUS
  // ----------------------------------------------------------

  if (
    x >= 10 &&
    x <= 310 &&
    y >= 130 &&
    y <= 165
  ) {

    screenMode = SCREEN_FAMOUS;

    drawFamousMenu();

    return;
  }

  // ----------------------------------------------------------
  // RESET
  // ----------------------------------------------------------

  if (
    x >= 10 &&
    x <= 155 &&
    y >= 175 &&
    y <= 210
  ) {

    resetView();

    renderFractal();

    return;
  }

  // ----------------------------------------------------------
  // EXPLORE
  // ----------------------------------------------------------

  if (
    x >= 165 &&
    x <= 310 &&
    y >= 175 &&
    y <= 210
  ) {

    screenMode = SCREEN_EXPLORE;

    renderFractal();

    return;
  }
}

// ============================================================
// HANDLE COLOR TOUCH
// ============================================================

void handleColorTouch(
  int x,
  int y
) {

  if (y >= 205) {

    screenMode = SCREEN_MENU;

    drawMainMenu();

    return;
  }

  if (y < 35) {
    return;
  }

  int col =
    (x < 160) ? 0 : 1;

  int row =
    (y - 35) / 34;

  int index =
    row * 2 + col;

  if (
    index >= 0 &&
    index < PALETTE_COUNT
  ) {

    palette = index;

    screenMode = SCREEN_EXPLORE;

    renderFractal();
  }
}

// ============================================================
// HANDLE ITERATION TOUCH
// ============================================================

void handleIterationTouch(
  int x,
  int y
) {

  if (y >= 205) {

    screenMode = SCREEN_MENU;

    drawMainMenu();

    return;
  }

  int col =
    (x < 160) ? 0 : 1;

  int row =
    (y - 40) / 50;

  int index =
    row * 2 + col;

  if (
    index >= 0 &&
    index < 6
  ) {

    int values[6] = {
      40,
      80,
      120,
      200,
      400,
      800
    };

    maxIterations =
      values[index];

    screenMode = SCREEN_EXPLORE;

    renderFractal();
  }
}

// ============================================================
// HANDLE FAMOUS TOUCH
// ============================================================

void handleFamousTouch(
  int x,
  int y
) {

  if (y >= 205) {

    screenMode = SCREEN_MENU;

    drawMainMenu();

    return;
  }

  int index =
    (y - 35) / 21;

  if (
    index >= 0 &&
    index < FAMOUS_COUNT
  ) {

    fractalMode =
      MODE_MANDELBROT;

    centerX =
      famousX[index];

    centerY =
      famousY[index];

    viewWidth =
      famousZoom[index];

    screenMode =
      SCREEN_EXPLORE;

    renderFractal();
  }
}

// ============================================================
// HANDLE JULIA TOUCH
// ============================================================

void handleJuliaTouch(
  int x,
  int y
) {

  if (y >= 205) {

    screenMode = SCREEN_MENU;

    drawMainMenu();

    return;
  }

  int col =
    (x < 160) ? 0 : 1;

  int row =
    (y - 35) / 39;

  int index =
    row * 2 + col;

  if (
    index >= 0 &&
    index < JULIA_COUNT
  ) {

    fractalMode =
      MODE_JULIA;

    juliaCX =
      juliaPresetsX[index];

    juliaCY =
      juliaPresetsY[index];

    resetView();

    screenMode =
      SCREEN_EXPLORE;

    renderFractal();
  }
}

// ============================================================
// HANDLE EXPLORER TOUCH
// ============================================================

void handleExploreRelease(
  int x,
  int y
) {

  // MENU button
  if (
    y >= FRACTAL_H &&
    x >= 260
  ) {

    screenMode =
      SCREEN_MENU;

    drawMainMenu();

    return;
  }

  // Ignore status bar
  if (y >= FRACTAL_H) {
    return;
  }

  // ----------------------------------------------------------
  // TAP = ZOOM
  // ----------------------------------------------------------

  if (!touchMoved) {

    zoomAt(
      touchStartX,
      touchStartY,
      0.5
    );

    renderFractal();

    return;
  }

  // ----------------------------------------------------------
  // DRAG = PAN
  // ----------------------------------------------------------

  renderFractal();
}

// ============================================================
// CENTRAL TOUCH HANDLER
// ============================================================

void handleTouch() {

  bool touched =
    touch.touched();

  // ==========================================================
  // TOUCH START
  // ==========================================================

  if (
    touched &&
    !wasTouched
  ) {

    getTouchXY(
      touchStartX,
      touchStartY
    );

    touchLastX =
      touchStartX;

    touchLastY =
      touchStartY;

    touchMoved =
      false;

    wasTouched =
      true;

    return;
  }

  // ==========================================================
  // TOUCH MOVE
  // ==========================================================

  if (
    touched &&
    wasTouched
  ) {

    int x;
    int y;

    getTouchXY(
      x,
      y
    );

    int dx =
      x - touchLastX;

    int dy =
      y - touchLastY;

    if (
      abs(x - touchStartX) > 8 ||
      abs(y - touchStartY) > 8
    ) {

      touchMoved =
        true;
    }

    // Only pan in explorer
    if (
      screenMode == SCREEN_EXPLORE &&
      touchMoved
    ) {

      panFractal(
        dx,
        dy
      );

      touchLastX =
        x;

      touchLastY =
        y;
    }

    return;
  }

  // ==========================================================
  // TOUCH RELEASE
  // ==========================================================

  if (
    !touched &&
    wasTouched
  ) {

    wasTouched =
      false;

    int x =
      touchLastX;

    int y =
      touchLastY;

    switch (screenMode) {

      case SCREEN_MENU:
        handleMenuTouch(x, y);
        break;

      case SCREEN_COLORS:
        handleColorTouch(x, y);
        break;

      case SCREEN_ITERATIONS:
        handleIterationTouch(x, y);
        break;

      case SCREEN_FAMOUS:
        handleFamousTouch(x, y);
        break;

      case SCREEN_JULIA:
        handleJuliaTouch(x, y);
        break;

      case SCREEN_EXPLORE:
        handleExploreRelease(x, y);
        break;
    }
  }
}

// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  delay(500);

  Serial.println();
  Serial.println(
    "ESP32 CYD Fractal Explorer"
  );

  // Backlight
  pinMode(
    BACKLIGHT_PIN,
    OUTPUT
  );

  digitalWrite(
    BACKLIGHT_PIN,
    HIGH
  );

  // TFT
  tft.init();

  tft.setRotation(1);

  tft.fillScreen(
    TFT_BLACK
  );

  // Touch SPI
  touchSPI.begin(
    TOUCH_SCLK,
    TOUCH_MISO,
    TOUCH_MOSI,
    TOUCH_CS
  );

  touch.begin(
    touchSPI
  );

  touch.setRotation(1);

  delay(300);

  // Initial screen
  screenMode =
    SCREEN_MENU;

  fractalMode =
    MODE_MANDELBROT;

  resetView();

  drawMainMenu();

  Serial.println(
    "Ready."
  );
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  handleTouch();

  delay(10);
}
