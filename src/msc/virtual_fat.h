/*!
 * @file virtual_fat.h
 *
 * FAT12 drive (no storage): 8 MB, 512-byte sectors,
 * 4 KB clusters. Sector 0 boot, 1-6 FAT, 7-8 root directory (32 entries),
 * data from sector 9 (cluster 2). Text files of up to 512 bytes are written
 * by generator callbacks straight into the sector being read.
 *
 */

#ifndef ADAFRUIT_DAP_CH32_VIRTUAL_FAT_H
#define ADAFRUIT_DAP_CH32_VIRTUAL_FAT_H

#include <stdint.h>
#include <string.h>

/*!
 * @brief FAT12 volume whose sectors are generated when read.
 */
class VirtualFat {
public:
  static const uint32_t SECTOR_SIZE = 512;       //!< bytes per sector
  static const uint32_t TOTAL_SECTORS = 16384;   //!< 8 MB volume
  static const uint32_t SECTORS_PER_CLUSTER = 8; //!< 4 KB clusters
  static const uint32_t FAT_START = 1;           //!< first FAT sector
  static const uint32_t FAT_SECTORS = 6;         //!< sectors per FAT
  static const uint32_t ROOT_START = FAT_START + FAT_SECTORS; //!< root dir
  static const uint32_t ROOT_ENTRIES = 32; //!< root directory entries
  static const uint32_t ROOT_SECTORS =
      ROOT_ENTRIES * 32 / SECTOR_SIZE; //!< root directory sectors
  static const uint32_t DATA_START =
      ROOT_START + ROOT_SECTORS; //!< first data sector (cluster 2)
  static const uint32_t CLUSTERS =
      (TOTAL_SECTORS - DATA_START) / SECTORS_PER_CLUSTER; //!< data clusters
  static const uint8_t MAX_FILES = 4; //!< generated files at most

  /*!
   * @brief File content generator.
   * @param context Context given to addFile().
   * @param buf Receives the content.
   * @param max Buffer size (512).
   * @return Content length. Must not change until the drive is remounted.
   */
  typedef uint32_t (*Generator)(void *context, char *buf, uint32_t max);

  /*!
   * @brief Set the volume label.
   * @param label Up to 11 characters (upper-cased, padded with spaces).
   */
  void setVolumeLabel(const char *label);

  /*!
   * @brief Add a generated file (one cluster, at most 512 bytes).
   * @param name83 8.3 name as 11 characters without the dot, e.g.
   *        "DETAILS TXT".
   * @param gen Content generator.
   * @param context Passed to gen.
   * @return False if MAX_FILES files exist.
   */
  bool addFile(const char *name83, Generator gen, void *context);
  /*! @brief Remove all generated files. */
  void clearFiles(void) { _count = 0; }

  /*!
   * @brief Generate a sector.
   * @param lba Sector number.
   * @param buf Receives 512 bytes.
   */
  void readSector(uint32_t lba, uint8_t *buf);

  /*! @return First data sector after the generated files: where a host
   *          normally starts writing a newly copied file. */
  uint32_t firstFreeSector(void) const {
    return DATA_START + _count * SECTORS_PER_CLUSTER;
  }

private:
  void bootSector(uint8_t *buf);
  void fatSector(uint32_t index, uint8_t *buf);
  void rootSector(uint32_t index, uint8_t *buf);

  struct File {
    char name[11];
    Generator gen;
    void *context;
  };
  char _label[11] = {'D', 'A', 'P', 'L', 'I', 'N', 'K', ' ', ' ', ' ', ' '};
  File _files[MAX_FILES];
  uint8_t _count = 0;
};

#endif
