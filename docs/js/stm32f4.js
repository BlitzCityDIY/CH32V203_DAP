// STM32F4 flash programming over SWD: a JavaScript port of
// Adafruit_DAP_CH32_STM32F4 (same register sequences and error checks),
// plus writeImage(), which erases, programs and verifies a whole image.

import { SwdError } from './webusb-dap.js';
import { sleep } from './swd.js';

export const FLASH_BASE = 0x08000000;

const DBGMCU_IDCODE = 0xe0042000;
const FLASH_SIZE_REG = 0x1fff7a20; // KB in bits 31:16

const FLASH_KEYR = 0x40023c04;
const FLASH_SR = 0x40023c0c;
const FLASH_CR = 0x40023c10;

const SR_EOP = 1 << 0;
const SR_OPERR = 1 << 1;
const SR_WRPERR = 1 << 4;
const SR_PGAERR = 1 << 5;
const SR_PGPERR = 1 << 6;
const SR_PGSERR = 1 << 7;
const SR_BSY = 1 << 16;
const SR_ERRORS = SR_OPERR | SR_WRPERR | SR_PGAERR | SR_PGPERR | SR_PGSERR;

const CR_PG = 1 << 0;
const CR_SER = 1 << 1;
const CR_MER = 1 << 2;
const CR_SNB_POS = 3;
const CR_PSIZE_X32 = 2 << 8;
const CR_STRT = 1 << 16;
const CR_LOCK = 0x80000000;

const KEY1 = 0x45670123;
const KEY2 = 0xcdef89ab;

const DEVICES = {
  0x413: 'STM32F405/407/415/417',
  0x419: 'STM32F42x/43x',
  0x423: 'STM32F401xB/C',
  0x433: 'STM32F401xD/E',
  0x431: 'STM32F411',
  0x441: 'STM32F412',
  0x421: 'STM32F446',
};

/** A flash controller or verify failure. */
export class FlashError extends Error {
  constructor(message, address) {
    super(message);
    this.name = 'FlashError';
    this.address = address;
  }
}

export const hex32 = (v) => '0x' + (v >>> 0).toString(16).toUpperCase().padStart(8, '0');

export function nameForId(idcode) {
  return DEVICES[idcode & 0xfff] || null;
}

/** Flash sector containing an offset from FLASH_BASE (first 1 MB). */
export function sectorAt(offset) {
  if (offset < 0x10000) {
    const n = Math.floor(offset / 0x4000);
    return { sector: n, start: n * 0x4000, size: 0x4000 };
  }
  if (offset < 0x20000) return { sector: 4, start: 0x10000, size: 0x10000 };
  if (offset < 0x100000) {
    const n = 5 + Math.floor((offset - 0x20000) / 0x20000);
    return { sector: n, start: 0x20000 + (n - 5) * 0x20000, size: 0x20000 };
  }
  return null;
}

function isBlank(data, start, end) {
  for (let i = start; i < end; i++) if (data[i] !== 0xff) return false;
  return true;
}

export class Stm32F4 {
  /** @param {import('./swd.js').SwdTarget} swd */
  constructor(swd) {
    this.swd = swd;
    this.devId = 0;
    this.flashKB = 0;
  }

  get name() {
    return nameForId(this.devId);
  }

  /** Bytes of flash this tool can program (first bank, 1 MB at most). */
  get flashLimit() {
    return Math.min(this.flashKB * 1024, 0x100000);
  }

  async _readIds() {
    this.devId = await this.swd.readMem32(DBGMCU_IDCODE);
    if (!nameForId(this.devId)) {
      throw new FlashError('Not a supported STM32F4 (DEV_ID ' +
        hex32(this.devId & 0xfff) + '). Only STM32F4 targets are supported so far.');
    }
    this.flashKB = (await this.swd.readMem32(FLASH_SIZE_REG)) >>> 16;
  }

  /** Identify the target without halting or resetting it. */
  async identify() {
    await this.swd.connect();
    await this.swd.powerUp();
    await this._readIds();
  }

  /** Connect, reset and halt before the firmware runs, and identify. */
  async select() {
    await this.swd.connect();
    await this.swd.powerUp();
    await this.swd.resetAndHalt();
    await this._readIds();
  }

  /** Lock the flash; optionally reset the chip to run its firmware. */
  async deselect(run = true) {
    await this.swd.writeMem32(FLASH_CR, CR_LOCK).catch(() => {});
    if (run) await this.swd.resetAndRun();
  }

  checkRange(addr, len) {
    const limit = this.flashLimit;
    if (((addr | len) & 3) || addr < FLASH_BASE || addr - FLASH_BASE > limit ||
        len > limit - (addr - FLASH_BASE)) {
      throw new FlashError(hex32(addr) + '..' + hex32(addr + len) +
        ' is outside the target\'s flash (' + hex32(FLASH_BASE) + ', ' +
        (limit / 1024) + ' KB) or not 4-byte aligned.');
    }
  }

  /** Wait while BSY is set, then check and clear the error flags. */
  async _waitReady(timeoutMs, pollMs = 1) {
    const t0 = performance.now();
    for (;;) {
      const sr = await this.swd.readMem32(FLASH_SR);
      if (!(sr & SR_BSY)) {
        if (sr & SR_ERRORS) {
          await this.swd.writeMem32(FLASH_SR, SR_ERRORS | SR_EOP).catch(() => {});
          if (sr & SR_WRPERR) throw new FlashError('Flash write protection error (WRPERR). Remove write protection in the option bytes.');
          if (sr & SR_PGAERR) throw new FlashError('Flash programming alignment error (PGAERR).');
          if (sr & SR_PGPERR) throw new FlashError('Flash programming parallelism error (PGPERR).');
          if (sr & SR_PGSERR) throw new FlashError('Flash programming sequence error (PGSERR).');
          throw new FlashError('Flash operation error (OPERR).');
        }
        return;
      }
      if (performance.now() - t0 > timeoutMs) {
        throw new FlashError('The flash stayed busy for more than ' + (timeoutMs / 1000) + ' s.');
      }
      await sleep(pollMs);
    }
  }

  /** Unlock, clear stale errors, wait until idle and write CR. */
  async _start(cr) {
    const s = this.swd;
    let v = await s.readMem32(FLASH_CR);
    if (v & CR_LOCK) {
      await s.writeMem32(FLASH_KEYR, KEY1);
      await s.writeMem32(FLASH_KEYR, KEY2);
      v = await s.readMem32(FLASH_CR);
      if (v & CR_LOCK) throw new FlashError('The flash rejected the unlock keys.');
    }
    await s.writeMem32(FLASH_SR, SR_ERRORS | SR_EOP);
    await this._waitReady(100);
    await s.writeMem32(FLASH_CR, cr);
  }

  /** Clear CR and lock the flash, keeping the first error. */
  async _finish() {
    await this.swd.writeMem32(FLASH_CR, 0).catch(() => {});
    await this.swd.writeMem32(FLASH_CR, CR_LOCK).catch(() => {});
  }

  /** Erase the whole flash (up to ~16 s on a 1 MB part). */
  async massErase() {
    try {
      await this._start(CR_MER | CR_PSIZE_X32);
      await this.swd.writeMem32(FLASH_CR, CR_MER | CR_PSIZE_X32 | CR_STRT);
      await this._waitReady(40000, 50);
    } finally {
      await this._finish();
    }
  }

  /**
   * Erase sectors.
   * @param {number[]} sectors sector numbers
   * @param {(done:number,total:number)=>void} [onProgress] in sectors
   */
  async eraseSectors(sectors, onProgress) {
    try {
      await this._start(0);
      let done = 0;
      for (const n of sectors) {
        const cr = CR_SER | (n << CR_SNB_POS) | CR_PSIZE_X32;
        await this.swd.writeMem32(FLASH_CR, cr);
        await this.swd.writeMem32(FLASH_CR, cr | CR_STRT);
        // 128 KB sectors take up to ~4 s, 16 KB sectors up to ~0.5 s.
        await this._waitReady(n >= 5 ? 6000 : 2000, 10);
        if (onProgress) onProgress(++done, sectors.length);
      }
    } finally {
      await this._finish();
    }
  }

  /**
   * Program erased flash in 1 KB chunks. Chunks of only 0xFF are skipped
   * (erased flash already reads 0xFF).
   * @param {(done:number,total:number)=>void} [onProgress] in bytes
   */
  async program(addr, data, onProgress) {
    this.checkRange(addr, data.length);
    try {
      await this._start(CR_PG | CR_PSIZE_X32);
      let pos = 0;
      while (pos < data.length) {
        const n = Math.min(0x400 - ((addr + pos) & 0x3ff), data.length - pos);
        if (!isBlank(data, pos, pos + n)) {
          await this.swd.writeBlock(addr + pos, data.subarray(pos, pos + n));
          await this._waitReady(100);
        }
        pos += n;
        if (onProgress) onProgress(pos, data.length);
      }
    } finally {
      await this._finish();
    }
  }

  /** Read flash back and compare; throws FlashError with the first bad address. */
  async verify(addr, data, onProgress) {
    this.checkRange(addr, data.length);
    let pos = 0;
    while (pos < data.length) {
      const n = Math.min(0x400 - ((addr + pos) & 0x3ff), data.length - pos);
      const got = await this.swd.readBlock(addr + pos, n);
      for (let i = 0; i < n; i++) {
        if (got[i] !== data[pos + i]) {
          const bad = (addr + pos + i) & ~3;
          throw new FlashError('Verify failed at ' + hex32(bad) + ': flash differs from the image.', bad);
        }
      }
      pos += n;
      if (onProgress) onProgress(pos, data.length);
    }
  }

  /** Read a flash range (for a backup). addr must be 4-byte aligned. */
  async read(addr, len, onProgress) {
    const padded = (len + 3) & ~3;
    const out = new Uint8Array(padded);
    let pos = 0;
    while (pos < padded) {
      const n = Math.min(0x400 - ((addr + pos) & 0x3ff), padded - pos);
      out.set(await this.swd.readBlock(addr + pos, n), pos);
      pos += n;
      if (onProgress) onProgress(Math.min(pos, len), len);
    }
    return out.subarray(0, len);
  }

  /**
   * Erase, program and verify an image. The target must be selected.
   * @param {{addr:number, data:Uint8Array}[]} segments 4-byte aligned
   * @param {{verify?:boolean, eraseAll?:boolean,
   *          onProgress?:(stage:string, done:number, total:number)=>void}} opts
   */
  async writeImage(segments, opts = {}) {
    const progress = opts.onProgress || (() => {});
    for (const s of segments) this.checkRange(s.addr, s.data.length);

    if (opts.eraseAll) {
      progress('erase', 0, 1);
      await this.massErase();
      progress('erase', 1, 1);
    } else {
      const sectors = new Set();
      for (const s of segments) {
        const first = sectorAt(s.addr - FLASH_BASE);
        const last = sectorAt(s.addr - FLASH_BASE + s.data.length - 1);
        for (let n = first.sector; n <= last.sector; n++) sectors.add(n);
      }
      const list = [...sectors].sort((a, b) => a - b);
      progress('erase', 0, list.length);
      await this.eraseSectors(list, (d, t) => progress('erase', d, t));
    }

    const total = segments.reduce((sum, s) => sum + s.data.length, 0);
    let base = 0;
    progress('program', 0, total);
    for (const s of segments) {
      await this.program(s.addr, s.data, (d) => progress('program', base + d, total));
      base += s.data.length;
    }

    if (opts.verify !== false) {
      base = 0;
      progress('verify', 0, total);
      for (const s of segments) {
        await this.verify(s.addr, s.data, (d) => progress('verify', base + d, total));
        base += s.data.length;
      }
    }
  }
}

export { SwdError };
