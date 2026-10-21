// Adafruit_DAP_CH32 - SWD wiring and signal diagnostics
//
//   1. Line check   - levels the target's pulls give with probe pins floating
//   2. Drive check  - can the probe drive SWCLK / SWDIO / nRESET?
//   3. Raw capture  - a hand-built DPIDR read showing every bit received,
//                     normally and with nRESET held low
//   4. SWCLK        - requested vs. measured clock (write and read paths)
//   5. Reliability  - 20 CPUID reads per SWD clock and pin edge speed
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
static const uint8_t CMD_SWJ_SEQUENCE = 0x12;
static const uint8_t CMD_SWD_SEQUENCE = 0x1D;

Adafruit_DAP_CH32 dap;
Adafruit_NeoPixel pixel(1, PA4, NEO_GRB + NEO_KHZ800);
uint8_t req[64], resp[64];

static void printHex32(uint32_t v) {
  static const char hex[] = "0123456789ABCDEF";
  Serial.print("0x");
  for (int i = 28; i >= 0; i -= 4) {
    Serial.print(hex[(v >> i) & 0xF]);
  }
}

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

static void rawDpidrRead(const char *label) {
  const uint8_t con[] = {CMD_CONNECT, 1};
  const uint8_t line_reset[] = {CMD_SWJ_SEQUENCE, 56, 0xFF, 0xFF, 0xFF,
                                0xFF, 0xFF, 0xFF, 0xFF};
  const uint8_t to_swd[] = {CMD_SWJ_SEQUENCE, 16, 0x9E, 0xE7};
  const uint8_t idle[] = {CMD_SWJ_SEQUENCE, 8, 0x00};
  dap.execute(con, resp);
  dap.execute(line_reset, resp);
  dap.execute(to_swd, resp);
  dap.execute(line_reset, resp);
  dap.execute(idle, resp);

  const uint8_t raw[] = {CMD_SWD_SEQUENCE, 3, 8, 0xA5, 0x80 | 37, 8, 0x00};
  dap.execute(raw, resp);

  auto capBit = [](int i) { return (resp[2 + i / 8] >> (i % 8)) & 1; };
  uint32_t v = 0;
  int ones = 0;
  for (int i = 0; i < 32; i++) {
    v |= (uint32_t)capBit(4 + i) << i;
    ones += capBit(4 + i);
  }
  int ack = capBit(1) | (capBit(2) << 1) | (capBit(3) << 2);
  Serial.print(label);
  Serial.print(" ack=");
  Serial.print(capBit(1));
  Serial.print(capBit(2));
  Serial.print(capBit(3));
  Serial.print(" data=");
  printHex32(v);
  Serial.print(" parity=");
  Serial.print(capBit(36));
  if (ack == 1 && (ones & 1) == capBit(36)) {
    Serial.println("  -> OK, valid parity");
  } else if (ack == 7 && v == 0xFFFFFFFF) {
    Serial.println("  -> all ones: the target never drove SWDIO");
  } else if (ack == 0 && v == 0) {
    Serial.println("  -> all zeros: SWDIO held low (short to GND?)");
  } else {
    Serial.println("  -> partial response: timing or signal problem");
  }
}

static void clockCheck(void) {
  Serial.println("4. SWCLK (requested -> measured write / read path):");
  const uint32_t clocks[] = {100000, 400000, 1000000};
  for (uint32_t hz : clocks) {
    dap.setClock(hz);
    uint32_t w = dap.measureClock(false);
    uint32_t r = dap.measureClock(true);
    Serial.print("   ");
    Serial.print(hz / 1000);
    Serial.print(" kHz -> ");
    Serial.print(w / 1000);
    Serial.print(" / ");
    Serial.print(r / 1000);
    Serial.println(" kHz");
  }
}

static void reliabilityMatrix(void) {
  const uint32_t clocks[] = {100000, 400000, 1000000, 2000000};
  const uint8_t edges[] = {10, 2};
  const int tries = 20;
  Serial.println("5. Reliability: CPUID reads per SWD clock / edge speed:");
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
  Serial.println("3. Raw DPIDR read at 100 kHz:");
  rawDpidrRead("   normal:       ");
  swjPins(0x00, 0x80, 0); // hold nRESET low
  delay(20);
  rawDpidrRead("   under reset:  ");
  swjPins(0x80, 0x80, 50000);
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
