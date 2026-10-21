// Adafruit_DAP_CH32 - USB CDC echo and platform diagnostics
//   - Every byte received on the USB serial port is echoed back unchanged.
//   - When a terminal opens the port a diagnostics banner is
//     printed, ending with the line "--- echo ready ---".
//   - NeoPixel heartbeat:
//       blinking blue  = running, no terminal connected
//       blinking green = running, terminal connected
//       frozen         = firmware is hung
//
// Arduino IDE settings:
//   Board:         CH32V20x  ->  Board Select: Adafruit QTPy CH32V203G6
//   Clock Select:  144MHz Internal
//   USB support:   Adafruit TinyUSB with USBD
//   Upload method: WCH-ISP

#include <Adafruit_NeoPixel.h>
#include <Adafruit_TinyUSB.h>

#if !defined(USE_TINYUSB)
#error "Select Tools > USB support > 'Adafruit TinyUSB with USBD' (or USBFS)"
#endif

#if !(F_CPU == 144000000 || F_CPU == 96000000 || F_CPU == 48000000)
#error "USB needs a 48, 96 or 144 MHz system clock (Tools > Clock Select)"
#endif

#define NEOPIXEL_PIN PA4 // QT Py CH32V203 on-board NeoPixel (D6)

Adafruit_NeoPixel pixel(1, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// Linker symbols
extern "C" {
extern uint32_t _data_vma;  // start of .data (bottom of used RAM)
extern uint32_t _ebss;      // end of .bss (top of static RAM)
extern uint32_t _heap_end;  // heap may grow from _ebss up to here
extern uint32_t _susrstack; // lowest stack address
extern uint32_t _eusrstack; // initial stack pointer (top of RAM)
}

static const uint32_t STACK_PAINT = 0xDEADBEEF;

static void paintStack(void) {
  uint32_t sp;
  asm volatile("mv %0, sp" : "=r"(sp));
  for (uint32_t *p = &_susrstack; (uint32_t)p < sp - 64; p++) {
    *p = STACK_PAINT;
  }
}

static uint32_t stackHighWater(void) {
  uint32_t *p = &_susrstack;
  while (p < &_eusrstack && *p == STACK_PAINT) {
    p++;
  }
  return (uint32_t)&_eusrstack - (uint32_t)p;
}

// Print wrapper that records how many banner bytes the USB stack accepted.
class CountingPrint : public Print {
public:
  uint32_t attempted = 0, accepted = 0;
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *buf, size_t len) override {
    size_t n = Serial.write(buf, len);
    attempted += len;
    accepted += n;
    return n;
  }
};

static void printHex32(Print &out, uint32_t v) {
  static const char digits[] = "0123456789ABCDEF";
  char s[11] = {'0', 'x'};
  for (int i = 0; i < 8; i++) {
    s[2 + i] = digits[(v >> (28 - 4 * i)) & 0xF];
  }
  s[10] = '\0';
  out.print(s);
}

static uint32_t session_rx = 0, session_tx = 0;
static uint32_t last_rx = 0, last_tx = 0;
static uint32_t last_banner_attempted = 0, last_banner_accepted = 0;
static uint32_t sessions = 0;
static bool was_connected = false;

static void printBanner(void) {
  CountingPrint out;
  const uint32_t ram_start = (uint32_t)&_data_vma;
  const uint32_t ram_top = (uint32_t)&_eusrstack;
  const uint32_t static_ram = (uint32_t)&_ebss - ram_start;
  const uint32_t stack_size = ram_top - (uint32_t)&_susrstack;
  const uint32_t free_gap = (uint32_t)&_heap_end - (uint32_t)&_ebss;
  const volatile uint32_t *uid = (const volatile uint32_t *)0x1FFFF7E8;

  out.println();
  out.println("=== Adafruit_DAP_CH32: CDC echo ===");
  out.print("SystemCoreClock : ");
  out.print(SystemCoreClock / 1000000);
  out.println(" MHz");
  out.print("USB peripheral  : ");
#if defined(CFG_TUD_WCH_USBIP_FSDEV) && CFG_TUD_WCH_USBIP_FSDEV
  out.println("USBD (fsdev)");
#elif defined(CFG_TUD_WCH_USBIP_USBFS) && CFG_TUD_WCH_USBIP_USBFS
  out.println("USBFS (otg)");
#else
  out.println("unknown");
#endif
  out.print("TinyUSB stack   : ");
  out.print(TUSB_VERSION_MAJOR);
  out.print('.');
  out.print(TUSB_VERSION_MINOR);
  out.print('.');
  out.println(TUSB_VERSION_REVISION);
  out.print("Chip ID         : ");
  printHex32(out, *(const volatile uint32_t *)0x1FFFF704);
  out.println();
  out.print("ESIG flash size : ");
  out.print(*(const volatile uint16_t *)0x1FFFF7E0);
  out.println(" KB");
  out.print("Unique ID       : ");
  printHex32(out, uid[0]);
  out.print(' ');
  printHex32(out, uid[1]);
  out.print(' ');
  printHex32(out, uid[2]);
  out.println();
  out.print("RAM total       : ");
  out.print(ram_top - ram_start);
  out.println(" bytes");
  out.print("  static (.data+.bss) : ");
  out.println(static_ram);
  out.print("  free (heap gap)     : ");
  out.println(free_gap);
  out.print("  stack reserved      : ");
  out.println(stack_size);
  out.print("  stack high-water    : ");
  out.println(stackHighWater());
  out.print("CDC buffers     : rx ");
  out.print(CFG_TUD_CDC_RX_BUFSIZE);
  out.print(" / tx ");
  out.println(CFG_TUD_CDC_TX_BUFSIZE);
  out.print("Session #       : ");
  out.println(sessions);
  out.print("Previous session: rx ");
  out.print(last_rx);
  out.print(" / tx ");
  out.print(last_tx);
  out.println(" bytes");
  out.print("Previous banner : ");
  out.print(last_banner_accepted);
  out.print(" of ");
  out.print(last_banner_attempted);
  out.println(" bytes accepted by USB");
  out.println("--- echo ready ---");
  Serial.flush();

  last_banner_attempted = out.attempted;
  last_banner_accepted = out.accepted;
}

static void heartbeat(bool connected) {
  static uint32_t last_ms = 0;
  static bool on = false;
  uint32_t now = millis();
  if (now - last_ms >= 250) {
    last_ms = now;
    on = !on;
    uint32_t color = connected ? pixel.Color(0, 20, 0) : pixel.Color(0, 0, 20);
    pixel.setPixelColor(0, on ? color : 0);
    pixel.show();
  }
}

void setup() {
  paintStack();

  pixel.begin();
  pixel.setPixelColor(0, pixel.Color(20, 0, 0));
  pixel.show();

  TinyUSBDevice.setManufacturerDescriptor("Adafruit");
  TinyUSBDevice.setProductDescriptor("DAP_CH32 Echo");

  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }

  Serial.begin(115200);
}

void loop() {
  const bool connected = Serial.dtr();
  heartbeat(connected);

  if (connected && !was_connected) {
    sessions++;
    last_rx = session_rx;
    last_tx = session_tx;
    session_rx = session_tx = 0;
    printBanner();
  }
  was_connected = connected;

  uint8_t buf[64];
  int avail = Serial.available();
  if (avail > 0) {
    size_t n = Serial.read(buf, min((size_t)avail, sizeof(buf)));
    session_rx += n;
    session_tx += Serial.write(buf, n);
  }
}
