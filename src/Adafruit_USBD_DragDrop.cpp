/*!
 * @file Adafruit_USBD_DragDrop.cpp
 *
 * Drag-and-drop programming drive 
 *
 */

#include "Adafruit_USBD_DragDrop.h"

#if defined(USE_TINYUSB)

#include "usb/usb_functions.h"

Adafruit_USBD_DragDrop *Adafruit_USBD_DragDrop::_instance = nullptr;

/*!
 * @brief Identify the target (no halt or reset), add the drive and
 *        re-enumerate.
 * @param dap Probe; begin() must have been called on it.
 * @param label Volume label, up to 11 characters.
 * @return False if the CMSIS-DAP interface was already added (USBD) or the
 *         drive could not be added.
 */
bool Adafruit_USBD_DragDrop::begin(Adafruit_DAP_CH32 *dap, const char *label) {
#if DAP_CH32_USB_SMALL_PMA
  if (dap_ch32_usb_dap_added) {
    return false;
  }
#endif
  _instance = this;
  _engine.begin(dap, label);

  _msc.setID("Adafruit", "QT Py DAP Drive", "1.0");
  _msc.setCapacity(VirtualFat::TOTAL_SECTORS, VirtualFat::SECTOR_SIZE);
  _msc.setReadWriteCallback(readCb, writeCb, flushCb);
  _msc.setReadyCallback(readyCb);
  _msc.setUnitReady(true);
  if (!_msc.begin()) {
    return false;
  }
  dap_ch32_usb_drive_added = true;
  dap_ch32_usb_reenumerate();
  return true;
}

/*!
 * @brief Program received sectors and end drops. Call from loop().
 *
 * Slow work runs here, outside the USB stack: write callbacks hand sectors
 * over with TUD_MSC_RET_ASYNC, completed by tud_msc_async_io_done().
 */
void Adafruit_USBD_DragDrop::task(void) {
  if (_engine.pending()) {
    _engine.process();
    tud_msc_async_io_done((int32_t)_pending_bytes, false);
  }
  _engine.poll(millis());
}

int32_t Adafruit_USBD_DragDrop::readCb(uint32_t lba, void *buffer,
                                       uint32_t bufsize) {
  uint8_t *p = (uint8_t *)buffer;
  for (uint32_t done = 0; done + VirtualFat::SECTOR_SIZE <= bufsize;
       done += VirtualFat::SECTOR_SIZE) {
    _instance->_engine.read(lba++, p + done);
  }
  return (int32_t)bufsize;
}

// Must not block: delay() runs the USB stack, which would re-enter TinyUSB.
int32_t Adafruit_USBD_DragDrop::writeCb(uint32_t lba, uint8_t *buffer,
                                        uint32_t bufsize) {
  if (_instance->_engine.write(lba, buffer)) {
    _instance->_pending_bytes = bufsize;
    return TUD_MSC_RET_ASYNC; // buffer stays valid until io_done
  }
  return (int32_t)bufsize;
}

void Adafruit_USBD_DragDrop::flushCb(void) {}

bool Adafruit_USBD_DragDrop::readyCb(void) {
  return _instance->_engine.unitReady();
}

#endif // USE_TINYUSB
