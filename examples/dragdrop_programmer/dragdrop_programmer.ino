// Adafruit_DAP_CH32 - drag-and-drop programmer
//
// The QT Py CH32V203 appears as a USB drive "QTPY-DAP"
// Copy a firmware .bin onto the drive: the STM32F4 is erased,
// programmed, verified and reset to run the new firmware.
// The drive then disappears briefly and comes back:
// DETAILS.TXT shows the result of the last drop, and FAIL.TXT appears if it
// failed. (.hex files are not supported yet.)
//
// Arduino IDE settings:
//   Board:         CH32V20x  ->  Board Select: Adafruit QTPy CH32V203G6
//   Clock Select:  144MHz Internal
//   USB support:   Adafruit TinyUSB with USBD
//   Upload method: WCH-ISP
//
// Wiring (QT Py -> target):
//   A3 (PA0) -> SWDIO          A2 (PA1) -> SWCLK
//   A0 (PB1) -> nRESET (opt.)  GND      -> GND
//   TX (PA2) -> target RX      RX (PA3) -> target TX
//
// NeoPixel: blue = waiting for the PC, green = ready (or last drop OK),
// yellow = programming, red = last drop failed (see FAIL.TXT),
// purple flicker = UART traffic, flashing red = drive could not start.

#include <Adafruit_NeoPixel.h>
#include <Adafruit_CDC_UART_Bridge.h>
#include <Adafruit_DAP_CH32.h>
#include <Adafruit_TinyUSB.h>
#include <Adafruit_USBD_DragDrop.h>

#if !defined(CFG_TUD_WCH_USBIP_FSDEV) || !CFG_TUD_WCH_USBIP_FSDEV
#error "Select Tools > USB support > 'Adafruit TinyUSB with USBD' (the QT Py's CH32V203G6 has no USBFS pins)"
#endif

Adafruit_DAP_CH32 dap;
Adafruit_USBD_DragDrop drive;
Adafruit_CDC_UART_Bridge bridge;
Adafruit_NeoPixel pixel(1, PA4, NEO_GRB + NEO_KHZ800);

static bool drive_ok = false;

void setup() {
  pixel.begin();
  pixel.setPixelColor(0, pixel.Color(0, 0, 20));
  pixel.show();

  TinyUSBDevice.setManufacturerDescriptor("Adafruit");
  TinyUSBDevice.setProductDescriptor("QT Py CH32V203 Drag-and-Drop");

  Serial.begin(115200); // USB serial port: carries the target's UART
  bridge.begin(Serial);

  dap.begin();                   // SWCLK = A2, SWDIO = A3, nRESET = A0
  drive_ok = drive.begin(&dap);  // identifies the target, adds the drive
}

void loop() {
  bridge.task();
  drive.task();

  static uint32_t last_led = 0;
  uint32_t now = millis();
  if (now - last_led >= 20) {
    last_led = now;
    bool blink = (now / 40) & 1;
    uint32_t color = pixel.Color(0, 20, 0); // green: ready / last drop OK
    if (!drive_ok) {
      color = blink ? pixel.Color(20, 0, 0) : 0; // drive could not start
    } else if (drive.programming()) {
      color = pixel.Color(20, 15, 0); // yellow: programming
    } else if (!TinyUSBDevice.mounted()) {
      color = pixel.Color(0, 0, 20); // blue: waiting for the PC
    } else if (drive.lastResult() == DragDropEngine::FAILED) {
      color = pixel.Color(20, 0, 0); // red: last drop failed
    } else if ((bridge.bytesToTarget() || bridge.bytesFromTarget()) &&
               now - bridge.lastActivityMillis() < 50) {
      color = blink ? pixel.Color(15, 0, 20) : pixel.Color(0, 20, 0);
    }
    pixel.setPixelColor(0, color);
    pixel.show();
  }
}
