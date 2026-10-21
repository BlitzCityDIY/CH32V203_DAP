/*!
 * @file Adafruit_USBD_DragDrop.h
 *
 * Drag-and-drop programming drive: copy a firmware .bin onto the drive and
 * the target (STM32F4) is programmed, verified and reset to run.
 * DETAILS.TXT shows the probe, target and last result; FAIL.TXT appears
 * after a failed drop
 *
 * Requires Tools > USB support > Adafruit TinyUSB with USBD
 *
 */

#ifndef ADAFRUIT_USBD_DRAGDROP_H
#define ADAFRUIT_USBD_DRAGDROP_H

#include <Arduino.h>

#if defined(USE_TINYUSB)

#include <Adafruit_TinyUSB.h>

#include "Adafruit_DAP_CH32.h"
#include "msc/dragdrop_engine.h"

/*!
 * @brief USB drive for drag-and-drop programming.
 */
class Adafruit_USBD_DragDrop {
public:
  bool begin(Adafruit_DAP_CH32 *dap, const char *label = "QTPY-DAP");
  void task(void);

  /*! @return True while a drop is being programmed. */
  bool programming(void) const {
    return _engine.state() == DragDropEngine::PROGRAMMING;
  }
  /*! @return Result of the last drop. */
  DragDropEngine::Result lastResult(void) const { return _engine.lastResult(); }
  /*! @return Error text of the last failed drop. */
  const char *lastError(void) const { return _engine.lastError(); }

private:
  static int32_t readCb(uint32_t lba, void *buffer, uint32_t bufsize);
  static int32_t writeCb(uint32_t lba, uint8_t *buffer, uint32_t bufsize);
  static void flushCb(void);
  static bool readyCb(void);

  static Adafruit_USBD_DragDrop *_instance;
  Adafruit_USBD_MSC _msc;
  DragDropEngine _engine;
  uint32_t _pending_bytes = 0;
};

#endif // USE_TINYUSB
#endif
