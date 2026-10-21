// Adafruit_DAP_CH32 - SWD wiring and signal diagnostics
//
// For when swd_identify_stm32 reports "no ACK" or unreliable results:
//   1. Line check   - levels the target's pulls give with probe pins floating
//   2. Drive check  - can the probe drive SWCLK / SWDIO / nRESET?
//   3. SWCLK        - requested vs. measured clock
//   4. Reliability  - 20 CPUID reads per SWD clock and pin edge speed
//
// NeoPixel: blue = idle, yellow = running, green = done.

#include <Adafruit_NeoPixel.h>
#include <Adafruit_DAP_CH32.h>
#include <Adafruit_TinyUSB.h>

#define PIN_SWCLK A2
#define PIN_SWDIO A3
#define PIN_NRESET A0

// CMSIS-DAP command IDs
static const uint8_t CMD_CONNECT = 0x02;
static const uint8_t CMD_SWJ_PINS = 0x10;

Adafruit_DAP_CH32 dap;
Adafruit_NeoPixel pixel(1, PA4, NEO_GRB + NEO_KHZ800);
uint8_t req[64], resp[64];

static void lineCheck(void) {
  dap.disconnect(); // probe pins floating
  delay(2);
  int dio = 0, clk = 0;
  for (int i = 0; i < 100; i++) {
    dio += digitalRead(PIN_SWDIO) ? 1 : 0;
    clk += digitalRead(PIN_SWCLK) ? 1 : 0;
    delayMicroseconds(10);
  }
  Serial.print("1. Line check: SWDIO high ");
  Serial.print(dio);
  Serial.print("/100, SWCLK high ");
  Serial.print(clk);
  Serial.println("/100  (an STM32 pulls SWDIO up and SWCLK down)");
}

// DAP_SWJ_Pins: drive the selected pins, return the levels read back.
// Bits: 0 = SWCLK, 1 = SWDIO, 7 = nRESET.
static uint8_t swjPins(uint8_t value, uint8_t select, uint32_t wait_us) {
  req[0] = CMD_SWJ_PINS;
  req[1] = value;
  req[2] = select;
  req[3] = (uint8_t)wait_us;
  req[4] = (uint8_t)(wait_us >> 8);
  req[5] = (uint8_t)(wait_us >> 16);
  req[6] = (uint8_t)(wait_us >> 24);
  dap.execute(req, resp);
  return resp[1];
}

static void driveCheck(void) {
  req[0] = CMD_CONNECT;
  req[1] = 1; // SWD: pins become outputs
  dap.execute(req, resp);
  Serial.println("2. Drive check (drive a level, read the pin back):");
  const struct {
    const char *name;
    uint8_t mask;
  } pins[] = {{"SWCLK", 0x01}, {"SWDIO", 0x02}, {"nRESET", 0x80}};
  for (auto &p : pins) {
    // nRESET is open drain: rising relies on the target's pull-up.
    uint32_t wait_us = (p.mask == 0x80) ? 50000 : 100;
    bool low = swjPins(0x00, p.mask, wait_us) & p.mask;
    bool high = swjPins(0xFF, p.mask, wait_us) & p.mask;
    Serial.print("   ");
    Serial.print(p.name);
    Serial.print(": 0 -> ");
    Serial.print(low);
    Serial.print(", 1 -> ");
    Serial.print(high);
    Serial.print((!low && high) ? "   OK" : "   PROBLEM");
    Serial.println(p.mask == 0x80 ? "  (only if nRESET is wired)" : "");
  }
  swjPins(0xFF, 0x83, 50000); // leave all pins high / released
}

static void clockCheck(void) {
  Serial.println("3. SWCLK (requested -> measured):");
  const uint32_t clocks[] = {100000, 400000, 1000000};
  for (uint32_t hz : clocks) {
    dap.setClock(hz);
    Serial.print("   ");
    Serial.print(hz / 1000);
    Serial.print(" kHz -> ");
    Serial.print(dap.measureClock() / 1000);
    Serial.println(" kHz");
  }
}

// CPUID reads (CSW write, TAR write, DRW read) per clock and edge speed.
static void reliabilityMatrix(void) {
  const uint32_t clocks[] = {100000, 400000, 1000000, 2000000};
  const uint8_t edges[] = {10, 2};
  const int tries = 20;
  Serial.println("4. Reliability: CPUID reads per SWD clock / edge speed:");
  for (uint8_t mhz : edges) {
    for (uint32_t hz : clocks) {
      dap.setPinSpeed(mhz);
      dap.setClock(hz);
      Serial.print("   ");
      Serial.print(hz / 1000);
      Serial.print(" kHz, ");
      Serial.print(mhz);
      Serial.print(" MHz edges: ");
      if (!dap.connect() || !dap.powerUp()) {
        Serial.print("connect failed: ");
        Serial.println(dap.errorString());
        continue;
      }
      int good = 0;
      const char *first = nullptr;
      for (int i = 0; i < tries; i++) {
        uint32_t v = 0;
        const char *step = "CSW write";
        bool ok = dap.apWrite(0, Adafruit_DAP_CH32::AP_REG_CSW, 0x23000002);
        if (ok) {
          step = "TAR write";
          ok = dap.apWrite(0, Adafruit_DAP_CH32::AP_REG_TAR, 0xE000ED00);
        }
        if (ok) {
          step = "DRW read";
          ok = dap.apRead(0, Adafruit_DAP_CH32::AP_REG_DRW, &v);
        }
        if (ok && (v >> 24) != 0x41) { // CPUID implementer: ARM
          step = "wrong value";
          ok = false;
        }
        if (ok) {
          good++;
        } else {
          if (!first) {
            first = step;
          }
          dap.connect(); // resync
          dap.powerUp();
        }
      }
      Serial.print(good);
      Serial.print('/');
      Serial.print(tries);
      if (first) {
        Serial.print(", first failure: ");
        Serial.print(first);
      }
      Serial.println();
      dap.disconnect();
    }
  }
  dap.setPinSpeed(2); // library defaults
  dap.setClock(1000000);
}

static void runDiagnostics(void) {
  pixel.setPixelColor(0, pixel.Color(20, 15, 0));
  pixel.show();
  Serial.println();
  Serial.println("=== SWD diagnostics ===");
  dap.setClock(100000);
  lineCheck();
  driveCheck();
  clockCheck();
  reliabilityMatrix();
  dap.disconnect();
  Serial.println("(press Enter to run again)");
  pixel.setPixelColor(0, pixel.Color(0, 20, 0));
  pixel.show();
}

void setup() {
  pixel.begin();
  pixel.setPixelColor(0, pixel.Color(0, 0, 20));
  pixel.show();
  Serial.begin(115200);
  dap.begin(PIN_SWCLK, PIN_SWDIO, PIN_NRESET);
}

void loop() {
  static bool was_connected = false;
  bool connected = Serial.dtr();
  bool run = connected && !was_connected;
  was_connected = connected;
  while (Serial.available()) {
    Serial.read();
    run = true;
  }
  if (run) {
    runDiagnostics();
  }
}
