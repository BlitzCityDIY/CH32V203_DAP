// Adafruit_DAP_CH32 - USB CMSIS-DAP probe and USB-serial bridge
//
// Turns the QT Py CH32V203 into a debug probe for an SWD target (pyOCD,
// OpenOCD) and a USB-serial bridge to the target's UART, on one USB cable.
//
// Wiring (QT Py -> target):
//   A3 (PA0) -> SWDIO          A2 (PA1) -> SWCLK
//   A0 (PB1) -> nRESET (opt.)  GND      -> GND
//   TX (PA2) -> target RX      RX (PA3) -> target TX
// Feather STM32F405: SWD on the 2x5 pads; RX = PB11 (D0), TX = PB10 (D1).
//
// Arduino IDE settings:
//   Board:         CH32V20x  ->  Board Select: Adafruit QTPy CH32V203G6
//   Clock Select:  144MHz Internal
//   USB support:   Adafruit TinyUSB with USBD
//   Upload method: WCH-ISP
//
// NeoPixel: blue = waiting for the PC, green = ready,
// white flicker = debug commands, purple flicker = UART traffic.
//

#include <Adafruit_NeoPixel.h>
#include <Adafruit_CDC_UART_Bridge.h>
#include <Adafruit_DAP_CH32.h>
#include <Adafruit_TinyUSB.h>
#include <Adafruit_USBD_CMSISDAP.h>

#if !defined(USE_TINYUSB)
#error "Select Tools > USB support > 'Adafruit TinyUSB with USBD'"
#endif

Adafruit_DAP_CH32 dap;
Adafruit_USBD_CMSISDAP usb_dap;
Adafruit_CDC_UART_Bridge bridge;
Adafruit_NeoPixel pixel(1, PA4, NEO_GRB + NEO_KHZ800);

void setup() {
  pixel.begin();
  pixel.setPixelColor(0, pixel.Color(0, 0, 20));
  pixel.show();

  // "CMSIS-DAP" in the product string helps tools that search for it there.
  TinyUSBDevice.setManufacturerDescriptor("Adafruit");
  TinyUSBDevice.setProductDescriptor("QT Py CH32V203 CMSIS-DAP");

  Serial.begin(115200);  // USB serial port: carries the target's UART
  bridge.begin(Serial);  // UART starts at 115200 8N1, then follows the PC

  dap.begin();           // SWCLK = A2, SWDIO = A3, nRESET = A0
  usb_dap.begin(&dap);   // adds the CMSIS-DAP interface and re-enumerates
}

void loop() {
  usb_dap.task();
  bridge.task();

  // Status LED, updated at most every 20 ms
  static uint32_t last_led = 0;
  uint32_t now = millis();
  if (now - last_led >= 20) {
    last_led = now;
    bool blink = (now / 40) & 1;
    uint32_t color = pixel.Color(0, 20, 0); // green: ready
    if (!usb_dap.mounted()) {
      color = pixel.Color(0, 0, 20); // blue: waiting for the PC
    } else if (usb_dap.commandCount() &&
               now - usb_dap.lastCommandMillis() < 50) {
      color = blink ? pixel.Color(20, 20, 20) : pixel.Color(0, 20, 0);
    } else if ((bridge.bytesToTarget() || bridge.bytesFromTarget()) &&
               now - bridge.lastActivityMillis() < 50) {
      color = blink ? pixel.Color(15, 0, 20) : pixel.Color(0, 20, 0);
    }
    pixel.setPixelColor(0, color);
    pixel.show();
  }
}
