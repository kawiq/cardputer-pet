// Cardputer ADV mini pet - ходит по экрану, реагирует на звуки (микрофон) и клавиши.
#include <M5Cardputer.h>
#include <Preferences.h>
#include <math.h>

static constexpr int W = 240, H = 135;
static constexpr int GROUND = 108;          // линия "пола"
static constexpr int MIC_SR = 16000;
static constexpr int MIC_LEN = 256;
static constexpr int MIC_BUFS = 4;

M5Canvas cv(&M5Cardputer.Display);
Preferences prefs;

int16_t micBuf[MIC_BUFS][MIC_LEN];
int micIdx = 0;

enum State { IDLE, WALK, SLEEP, HAPPY, SCARED, EAT, PLAY, DANCE };
State state = IDLE;
uint32_t stateUntil = 0;

// --- питомец ---
float px = 120, py = 0, vy = 0;   // py - высота над полом (0 = на полу)
int dir = 1;                      // 1 вправо, -1 влево
float food = 70, joy = 70, energy = 90;   // 0..100
uint32_t lastStat = 0, lastSave = 0, lastInteract = 0;
uint32_t nextBlink = 0, blinkUntil = 0;
uint32_t frame = 0;

// --- звук ---
float level = 0, noiseFloor = 300;
int loudFrames = 0;
uint32_t lastSpike = 0;

static const char* bubble = nullptr;
static uint32_t bubbleUntil = 0;

void say(const char* s, uint32_t ms = 1500) { bubble = s; bubbleUntil = millis() + ms; }

void setState(State s, uint32_t dur) {
  state = s;
  stateUntil = millis() + dur;
}

void jump(float power) { if (py <= 0.5f) vy = power; }

void saveStats() {
  prefs.putUChar("food", (uint8_t)food);
  prefs.putUChar("joy", (uint8_t)joy);
  prefs.putUChar("energy", (uint8_t)energy);
}

void pickNext() {
  if (energy < 20) { setState(SLEEP, 20000); say("zZz", 3000); return; }
  int r = random(100);
  if (r < 65) {
    dir = random(2) ? 1 : -1;
    setState(WALK, 1500 + random(3500));
  } else {
    setState(IDLE, 1000 + random(3000));
  }
}

// ---------- микрофон ----------
void updateMic() {
  if (!M5Cardputer.Mic.isEnabled()) return;
  int16_t* b = micBuf[micIdx];
  if (!M5Cardputer.Mic.record(b, MIC_LEN, MIC_SR)) return;
  micIdx = (micIdx + 1) % MIC_BUFS;

  int64_t sum = 0;
  for (int i = 0; i < MIC_LEN; i++) sum += b[i];
  float mean = (float)sum / MIC_LEN;
  float acc = 0;
  for (int i = 0; i < MIC_LEN; i++) { float d = b[i] - mean; acc += d * d; }
  float rms = sqrtf(acc / MIC_LEN);

  level = level * 0.6f + rms * 0.4f;
  // медленно подстраиваем уровень фонового шума (только когда тихо)
  if (level < noiseFloor * 1.6f) noiseFloor = noiseFloor * 0.995f + level * 0.005f;
  if (noiseFloor < 30) noiseFloor = 30;
}

void reactToSound() {
  uint32_t now = millis();
  float spikeTh = noiseFloor * 3.0f + 400;
  float bigTh   = noiseFloor * 7.0f + 1800;
  float musicTh = noiseFloor * 1.8f + 150;

  // длительный шум / музыка -> танец
  if (level > musicTh) loudFrames++; else if (loudFrames > 0) loudFrames--;

  if (level > bigTh && now - lastSpike > 600) {
    lastSpike = now; lastInteract = now;
    setState(SCARED, 1800); say("!!", 1500);
    jump(5.5f); dir = -dir; joy -= 3;
  } else if (level > spikeTh && now - lastSpike > 400) {
    lastSpike = now; lastInteract = now;
    if (state == SLEEP) { setState(SCARED, 1200); say("?!", 1000); jump(4.0f); }
    else { setState(HAPPY, 1800); say("<3", 1500); jump(4.5f); joy += 4; }
  } else if (loudFrames > 45 && state != DANCE && state != SLEEP) {
    setState(DANCE, 4000); say("~", 3000);
    joy += 3;
  }
  if (state == DANCE && loudFrames < 5 && now > stateUntil) pickNext();
}

// ---------- ввод ----------
void handleKeys() {
  if (!(M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())) return;
  auto st = M5Cardputer.Keyboard.keysState();
  for (char c : st.word) {
    lastInteract = millis();
    switch (c) {
      case 'f': case 'F':
        setState(EAT, 2500); say("nom", 2000); food = fminf(100, food + 25); joy += 2; break;
      case 'p': case 'P':
        setState(PLAY, 3000); say("yay", 2000); joy = fminf(100, joy + 15); energy -= 5; jump(5.0f); break;
      case 's': case 'S':
        if (state == SLEEP) { setState(IDLE, 1000); say("hi", 1000); }
        else { setState(SLEEP, 60000); say("zZz", 3000); }
        break;
      default:
        if (state == SLEEP) { setState(IDLE, 800); say("hm?", 1000); }
        break;
    }
  }
}

// ---------- логика ----------
void updateStats() {
  uint32_t now = millis();
  if (now - lastStat >= 1000) {
    lastStat = now;
    food -= 0.04f;
    joy  -= (food < 25 ? 0.06f : 0.02f);
    if (state == SLEEP) energy += 0.5f; else energy -= 0.015f;
    food = constrain(food, 0, 100); joy = constrain(joy, 0, 100); energy = constrain(energy, 0, 100);
    if (state == SLEEP && energy >= 98) { setState(IDLE, 500); say("morning", 1500); }
  }
  joy = constrain(joy, 0, 100); energy = constrain(energy, 0, 100);
  if (now - lastSave > 60000) { lastSave = now; saveStats(); }
}

void updatePet() {
  uint32_t now = millis();

  // физика прыжка
  if (py > 0 || vy > 0) {
    py += vy; vy -= 0.45f;
    if (py <= 0) { py = 0; vy = 0; }
  }

  if (now > stateUntil && state != SLEEP) {
    if (state == DANCE && loudFrames > 20) setState(DANCE, 2000);
    else pickNext();
  }
  if (state == SLEEP && now > stateUntil) { setState(IDLE, 500); say("*yawn*", 1500); }

  // голод - просит еды, иногда
  if (food < 25 && state == IDLE && random(400) == 0) say("food?", 1800);

  switch (state) {
    case WALK:  px += dir * 0.9f; break;
    case SCARED: px += dir * 2.2f; break;
    case DANCE: if ((frame / 8) % 2 == 0) jump(2.2f); px += sinf(frame * 0.2f) * 0.8f; break;
    case PLAY:  px += dir * 1.6f; if (frame % 18 == 0) jump(3.5f); break;
    default: break;
  }
  if (px < 28)  { px = 28;  dir = 1; }
  if (px > W - 28) { px = W - 28; dir = -1; }

  if (now > nextBlink) { blinkUntil = now + 140; nextBlink = now + 2000 + random(3000); }
}

// ---------- рисование ----------
void bar(int x, int y, int w, const char* label, float v, uint16_t col) {
  cv.setTextSize(1); cv.setTextColor(0x8410);
  cv.setCursor(x, y); cv.print(label);
  int bx = x + 8;
  cv.drawRect(bx, y - 1, w, 9, 0x8410);
  uint16_t c = v < 25 ? TFT_RED : col;
  cv.fillRect(bx + 1, y, (int)((w - 2) * v / 100.0f), 7, c);
}

void drawPet() {
  const uint16_t BODY = cv.color565(255, 176, 70);
  const uint16_t DARK = cv.color565(200, 110, 30);
  const uint16_t CHEEK = cv.color565(255, 110, 120);

  int x = (int)px;
  int base = GROUND - (int)py;
  bool walking = (state == WALK || state == SCARED || state == PLAY);
  float bob = walking ? sinf(frame * 0.5f) * 2 : (state == SLEEP ? sinf(frame * 0.08f) * 1.2f : sinf(frame * 0.12f) * 0.8f);

  int bw = 44, bh = 34;
  if (state == SLEEP) { bh -= 4; bw += 4; }
  int top = base - 8 - bh + (int)bob;

  // тень
  int sh = 18 - (int)(py * 0.3f); if (sh < 8) sh = 8;
  cv.fillEllipse(x, GROUND + 2, sh, 3, 0x2104);

  // хвост
  cv.fillCircle(x - dir * (bw / 2), base - 16, 5, DARK);
  // ножки
  int fo = walking ? (int)(sinf(frame * 0.5f) * 3) : 0;
  cv.fillRoundRect(x - 14 + fo, base - 9, 9, 9, 3, DARK);
  cv.fillRoundRect(x + 5 - fo, base - 9, 9, 9, 3, DARK);
  // ушки
  cv.fillTriangle(x - bw / 2 + 2, top + 8, x - bw / 2 + 4, top - 8, x - bw / 2 + 16, top + 2, BODY);
  cv.fillTriangle(x + bw / 2 - 2, top + 8, x + bw / 2 - 4, top - 8, x + bw / 2 - 16, top + 2, BODY);
  // тело
  cv.fillRoundRect(x - bw / 2, top, bw, bh + 4, 14, BODY);
  // щёки
  cv.fillCircle(x - 15, top + bh - 10, 3, CHEEK);
  cv.fillCircle(x + 15, top + bh - 10, 3, CHEEK);

  int ey = top + 13, ex = dir * 3;
  bool blink = millis() < blinkUntil;

  // глаза
  if (state == SLEEP) {
    cv.drawLine(x - 12, ey + 2, x - 5, ey + 2, TFT_BLACK);
    cv.drawLine(x + 5, ey + 2, x + 12, ey + 2, TFT_BLACK);
  } else if (state == HAPPY || state == DANCE || state == PLAY) {
    for (int s = -1; s <= 1; s += 2) {
      int cx = x + s * 9 + ex;
      cv.drawLine(cx - 4, ey + 3, cx, ey - 2, TFT_BLACK);
      cv.drawLine(cx, ey - 2, cx + 4, ey + 3, TFT_BLACK);
    }
  } else if (state == SCARED) {
    for (int s = -1; s <= 1; s += 2) {
      int cx = x + s * 9 + ex;
      cv.fillCircle(cx, ey, 6, TFT_WHITE); cv.fillCircle(cx, ey, 2, TFT_BLACK);
    }
  } else if (blink) {
    cv.drawLine(x - 12 + ex, ey, x - 5 + ex, ey, TFT_BLACK);
    cv.drawLine(x + 5 + ex, ey, x + 12 + ex, ey, TFT_BLACK);
  } else {
    for (int s = -1; s <= 1; s += 2) {
      int cx = x + s * 9 + ex;
      cv.fillCircle(cx, ey, 5, TFT_WHITE); cv.fillCircle(cx + dir * 1, ey + 1, 2, TFT_BLACK);
    }
  }

  // рот
  int my = ey + 12;
  if (state == SCARED) cv.drawCircle(x + ex, my + 2, 3, TFT_BLACK);
  else if (state == EAT) {
    int o = (frame / 5) % 2 ? 4 : 1;
    cv.fillEllipse(x + ex, my + 1, 4, o, TFT_BLACK);
  } else if (state == HAPPY || state == DANCE || state == PLAY) {
    cv.fillCircle(x + ex, my, 4, TFT_BLACK);
    cv.fillRect(x + ex - 5, my - 5, 11, 5, BODY);
  } else if (food < 25 || joy < 25) {
    cv.drawLine(x - 4 + ex, my + 3, x + ex, my, TFT_BLACK);   // грустный
    cv.drawLine(x + ex, my, x + 4 + ex, my + 3, TFT_BLACK);
  } else if (state == SLEEP) {
    cv.drawCircle(x + ex, my + 1, 2, TFT_BLACK);
  } else {
    cv.drawLine(x - 4 + ex, my, x + ex, my + 3, TFT_BLACK);   // улыбка
    cv.drawLine(x + ex, my + 3, x + 4 + ex, my, TFT_BLACK);
  }

  // еда / мячик
  if (state == EAT) {
    cv.fillCircle(x + dir * 30, GROUND - 5, 5, TFT_RED);
    cv.drawLine(x + dir * 30, GROUND - 10, x + dir * 31, GROUND - 13, TFT_GREEN);
  }
  if (state == PLAY) {
    int by = GROUND - 6 - (int)fabsf(sinf(frame * 0.25f) * 22);
    cv.fillCircle(x + dir * 32, by, 5, TFT_CYAN);
  }

  // облачко с текстом
  if (bubble && millis() < bubbleUntil) {
    int tw = cv.textWidth(bubble);
    int bx = x + 18, by = top - 20;
    if (bx + tw + 8 > W) bx = x - 18 - tw - 8;
    cv.fillRoundRect(bx, by, tw + 8, 14, 4, TFT_WHITE);
    cv.setTextColor(TFT_BLACK); cv.setTextSize(1);
    cv.setCursor(bx + 4, by + 3); cv.print(bubble);
  }
}

void draw() {
  cv.fillSprite(cv.color565(24, 28, 48));
  // пол
  cv.fillRect(0, GROUND + 5, W, H - GROUND - 5, cv.color565(48, 80, 52));
  cv.drawFastHLine(0, GROUND + 5, W, cv.color565(90, 140, 90));

  // HUD
  bar(2, 3, 44, "F", food, TFT_ORANGE);
  bar(66, 3, 44, "J", joy, TFT_MAGENTA);
  bar(130, 3, 44, "E", energy, TFT_CYAN);
  // индикатор звука
  int lv = constrain((int)(level / (noiseFloor * 8.0f + 2000) * 40), 0, 40);
  cv.drawRect(192, 2, 44, 9, 0x8410);
  cv.fillRect(194, 4, lv, 5, TFT_GREEN);

  drawPet();

  cv.setTextSize(1); cv.setTextColor(0xBDF7);
  cv.setCursor(4, 125); cv.print("F:feed  P:play  S:sleep");
  cv.pushSprite(0, 0);
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(120);
  cv.createSprite(W, H);
  cv.setFont(&fonts::Font0);

  // микрофон и динамик делят один I2S -> динамик выключаем
  M5Cardputer.Speaker.end();
  auto mc = M5Cardputer.Mic.config();
  mc.sample_rate = MIC_SR;
  M5Cardputer.Mic.config(mc);
  M5Cardputer.Mic.begin();

  prefs.begin("pet", false);
  food   = prefs.getUChar("food", 70);
  joy    = prefs.getUChar("joy", 70);
  energy = prefs.getUChar("energy", 90);
  if (food < 20) food = 20;   // после выключения питомец не должен быть совсем голодным

  randomSeed(esp_random());
  lastInteract = millis();
  say("hello!", 2000);
  setState(IDLE, 1000);
}

void loop() {
  M5Cardputer.update();
  frame++;
  handleKeys();
  updateMic();
  reactToSound();
  updateStats();
  updatePet();
  draw();
  delay(25);
}
