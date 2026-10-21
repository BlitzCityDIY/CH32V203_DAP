/*!
 * @file dragdrop_engine.h
 *
 * Drag-and-drop programming logic, independent of USB.
 *
 * Interprets the sectors a host writes to the virtual drive:
 *  - Root directory writes give each file's first cluster and size.
 *  - The first data sector starting with a valid STM32 vector table starts
 *    a session; later sectors map to flash by their offset from it. Other
 *    writes (OS metadata files) are ignored.
 *  - process() - reset and halt the target, erase each flash sector on
 *    first use, program, verify. The newest sector is held back by one
 *    so the last one is programmed only up to the file size, which hosts
 *    often write after the data.
 *  - After QUIET_MS without writes the session ends: the target is reset to
 *    run, DETAILS.TXT is updated (FAIL.TXT added on failure) and the drive
 *    reports "not ready" for REMOUNT_MS so the host re-reads it.
 *
 */

#ifndef ADAFRUIT_DAP_CH32_DRAGDROP_ENGINE_H
#define ADAFRUIT_DAP_CH32_DRAGDROP_ENGINE_H

#include <stdint.h>

#include "../Adafruit_DAP_CH32.h"
#include "../Adafruit_DAP_CH32_STM32F4.h"
#include "virtual_fat.h"

/*!
 * @brief Drag-and-drop programming state machine.
 */
class DragDropEngine {
public:
  static const uint32_t QUIET_MS = 500;         //!< write gap ending a drop
  static const uint32_t REMOUNT_MS = 1500;      //!< "not ready" after a drop
  static const uint32_t UNRECOGNISED_MS = 1500; //!< wait for a session

  /*! Engine state. */
  enum State : uint8_t { IDLE, PROGRAMMING, REMOUNT };
  /*! Result of the last drop. */
  enum Result : uint8_t { NONE, OK, FAILED };

  void begin(Adafruit_DAP_CH32 *dap, const char *label);

  /*!
   * @brief Read a sector of the drive.
   * @param lba Sector number.
   * @param buf Receives 512 bytes.
   */
  void read(uint32_t lba, uint8_t *buf) { _fat.readSector(lba, buf); }
  bool write(uint32_t lba, const uint8_t *buf);
  void process(void);
  void poll(uint32_t now_ms);

  /*! @return True if write() handed over a sector for process(). */
  bool pending(void) const { return _pending; }
  /*! @return False while the drive should report "not ready". */
  bool unitReady(void) const { return _state != REMOUNT; }
  /*! @return Current state. */
  State state(void) const { return _state; }
  /*! @return Result of the last drop. */
  Result lastResult(void) const { return _result; }
  /*! @return Error text of the last failed drop. */
  const char *lastError(void) const { return _res_error; }
  /*! @return Bytes programmed by the last drop. */
  uint32_t lastBytes(void) const { return _res_bytes; }
  /*! @return True if a target was identified. */
  bool targetFound(void) const { return _found; }
  /*! @return Target name, or "none". */
  const char *targetName(void) const;
  /*! @return The virtual drive. */
  VirtualFat &fat(void) { return _fat; }

private:
  struct Seen {    // file entry from a root directory write
    uint32_t lba;  // first data sector
    uint32_t size; // bytes (0 until the host writes it)
    char name[11];
  };

  void rebuildFiles(void);
  void parseDir(const uint8_t *buf);
  const Seen *seenAt(uint32_t lba) const;
  static bool looksLikeImage(const uint8_t *buf);
  void fail(const char *error);
  void programSector(uint32_t lba, const uint8_t *buf);
  void finish(uint32_t now_ms, bool unrecognised_hex);
  static uint32_t detailsText(void *context, char *buf, uint32_t max);
  static uint32_t failText(void *context, char *buf, uint32_t max);

  Adafruit_DAP_CH32 *_dap = nullptr;
  Adafruit_DAP_CH32_STM32F4 _f4;
  VirtualFat _fat;

  bool _found = false;
  uint32_t _dpidr = 0;

  State _state = IDLE;
  bool _pending = false, _selected = false, _failed = false, _held = false;
  const uint8_t *_p_buf = nullptr;
  uint32_t _p_lba = 0, _start_lba = 0, _erased = 0, _bytes = 0;
  uint32_t _t_start = 0, _t_last = 0, _t_dir = 0, _t_remount = 0;
  uint32_t _hold_lba = 0;
  const char *_error = nullptr;
  Seen _seen[4];
  uint8_t _seen_n = 0;
  uint8_t _hold[VirtualFat::SECTOR_SIZE];

  Result _result = NONE;
  const char *_res_error = nullptr;
  uint32_t _res_bytes = 0, _res_ms = 0;
  char _res_name[13] = {0};
};

#endif
