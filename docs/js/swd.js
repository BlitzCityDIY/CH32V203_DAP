// SWD target access: Debug Port, MEM-AP 0 and Cortex-M core control.
// A JavaScript port of Adafruit_DAP_CH32 (connect, powerUp, readMem32,
// writeBlock, resetAndHalt, ...). The difference: every round trip now
// crosses USB, so related transfers are batched into one DAP_Transfer
// packet, and SELECT and CSW are cached.

import { REQ, SwdError } from './webusb-dap.js';

const DP_DPIDR = 0x00, DP_ABORT = 0x00, DP_CTRL_STAT = 0x04, DP_SELECT = 0x08;
const AP_CSW = 0x00, AP_TAR = 0x04, AP_DRW = 0x0c;

// MEM-AP CSW: 32-bit transfers, HPROT = 0x23, with/without auto-increment
const CSW_WORD = 0x23000002;
const CSW_WORD_INC = 0x23000012;

// Cortex-M debug registers
const DHCSR = 0xe000edf0;
const DEMCR = 0xe000edfc;
const AIRCR = 0xe000ed0c;
const DBGKEY = 0xa05f0000;
const C_DEBUGEN = 1 << 0;
const C_HALT = 1 << 1;
const S_HALT = 1 << 17;
const VC_CORERESET = 1 << 0;
const AIRCR_SYSRESETREQ = 0x05fa0004;

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const dpRd = (reg) => ({ req: REQ.RnW | (reg & 0x0c) });
const dpWr = (reg, value) => ({ req: reg & 0x0c, value });
const apRd = (reg) => ({ req: REQ.APnDP | REQ.RnW | (reg & 0x0c) });
const apWr = (reg, value) => ({ req: REQ.APnDP | (reg & 0x0c), value });

export class SwdTarget {
  /** @param {import('./webusb-dap.js').WebUsbDap} dap */
  constructor(dap) {
    this.dap = dap;
    this.clockHz = 1000000; // the QT Py caps host requests at 1 MHz by default
    this.idleCycles = 2;    // an STM32F4 needs at least 2 (see the library README)
    this.dpidr = 0;
    this._invalidate();
  }

  _invalidate() {
    this._select = null;
    this._csw = null;
  }

  /** Run a batch; on any error the cached SELECT/CSW may be stale. */
  async _xfer(ops) {
    try {
      return await this.dap.transfer(ops);
    } catch (e) {
      this._invalidate();
      throw e;
    }
  }

  _selectOps(ap, reg) {
    const want = ((ap << 24) | (reg & 0xf0)) >>> 0;
    if (this._select === want) return [];
    this._select = want;
    return [dpWr(DP_SELECT, want)];
  }

  _cswOps(csw) {
    if (this._csw === csw) return [];
    this._csw = csw;
    return [apWr(AP_CSW, csw)];
  }

  /** Line reset, JTAG-to-SWD switch, read DPIDR, clear sticky errors. */
  async connect() {
    const d = this.dap;
    this._invalidate();
    await d.swjClock(this.clockHz);
    await d.connectSwd();
    await d.transferConfigure(this.idleCycles, 100, 0);
    const ones = new Uint8Array(7).fill(0xff);
    await d.swjSequence(56, ones);
    await d.swjSequence(16, Uint8Array.of(0x9e, 0xe7));
    await d.swjSequence(56, ones);
    await d.swjSequence(8, Uint8Array.of(0x00));
    const [dpidr] = await this._xfer([dpRd(DP_DPIDR), dpWr(DP_ABORT, 0x1e)]);
    this.dpidr = dpidr;
    return dpidr;
  }

  /** Release the SWD pins. */
  async disconnect() {
    this._invalidate();
    await this.dap.disconnect();
  }

  /** Power up the debug and system domains. */
  async powerUp() {
    const REQ_PWR = 0x50000000, ACKS = 0xa0000000;
    this._select = 0;
    let [stat] = await this._xfer([dpWr(DP_SELECT, 0), dpWr(DP_CTRL_STAT, REQ_PWR), dpRd(DP_CTRL_STAT)]);
    for (let i = 0; i < 100; i++) {
      if (((stat & ACKS) >>> 0) === ACKS) return;
      await sleep(1);
      [stat] = await this._xfer([dpRd(DP_CTRL_STAT)]);
    }
    throw new SwdError('TIMEOUT', 'The target did not power up its debug port.');
  }

  async readMem32(addr) {
    const [v] = await this._xfer([
      ...this._selectOps(0, AP_CSW), ...this._cswOps(CSW_WORD),
      apWr(AP_TAR, addr), apRd(AP_DRW)]);
    return v;
  }

  async writeMem32(addr, value) {
    await this._xfer([
      ...this._selectOps(0, AP_CSW), ...this._cswOps(CSW_WORD),
      apWr(AP_TAR, addr), apWr(AP_DRW, value)]);
  }

  /**
   * Block transfer with address auto-increment. TAR is reloaded at every
   * 1 KB boundary, where MEM-AP auto-increment wraps.
   */
  async _block(addr, len, data, write) {
    if ((addr | len) & 3) throw new SwdError('ALIGN');
    const out = write ? null : new Uint8Array(len);
    const max = (write ? this.dap.blockWriteWords : this.dap.blockReadWords) * 4;
    const reqByte = REQ.APnDP | AP_DRW | (write ? 0 : REQ.RnW);
    let pos = 0;
    let tarValid = false;
    while (pos < len) {
      const a = addr + pos;
      if (!tarValid || (a & 0x3ff) === 0) {
        await this._xfer([...this._selectOps(0, AP_CSW), ...this._cswOps(CSW_WORD_INC), apWr(AP_TAR, a)]);
        tarValid = true;
      }
      const n = Math.min(0x400 - (a & 0x3ff), max, len - pos);
      try {
        if (write) {
          await this.dap.transferBlock(reqByte, data.subarray(pos, pos + n));
        } else {
          out.set(await this.dap.transferBlock(reqByte, n / 4), pos);
        }
      } catch (e) {
        this._invalidate();
        throw e;
      }
      pos += n;
    }
    return out;
  }

  /** @returns {Promise<Uint8Array>} */
  readBlock(addr, len) {
    return this._block(addr, len, null, false);
  }

  /** @param {Uint8Array} data length a multiple of 4 */
  writeBlock(addr, data) {
    return this._block(addr, data.length, data, true);
  }

  async isHalted() {
    return ((await this.readMem32(DHCSR)) & S_HALT) !== 0;
  }

  async haltCore() {
    await this.writeMem32(DHCSR, DBGKEY | C_DEBUGEN | C_HALT);
    for (let i = 0; i < 100; i++) {
      if (await this.isHalted()) return;
      await sleep(1);
    }
    throw new SwdError('TIMEOUT', 'The core did not halt.');
  }

  async resumeCore(keepDebug = false) {
    await this.writeMem32(DHCSR, DBGKEY | C_DEBUGEN);
    if (!keepDebug) await this.writeMem32(DHCSR, DBGKEY);
  }

  /**
   * Reset the chip and halt it at its first instruction (DHCSR halt, DEMCR
   * reset vector catch, AIRCR system reset). The reset may cut off the
   * AIRCR write and disturb the link, which is re-synced.
   */
  async resetAndHalt() {
    await this.writeMem32(DHCSR, DBGKEY | C_DEBUGEN | C_HALT);
    const demcr = await this.readMem32(DEMCR);
    await this.writeMem32(DEMCR, demcr | VC_CORERESET);
    await this.writeMem32(AIRCR, AIRCR_SYSRESETREQ).catch(() => {});
    await sleep(10);
    for (let i = 0; i < 100; i++) {
      let halted;
      try {
        halted = await this.isHalted();
      } catch (e) {
        await this.connect().then(() => this.powerUp()).catch(() => {});
        continue;
      }
      if (halted) {
        const d = await this.readMem32(DEMCR);
        await this.writeMem32(DEMCR, d & ~VC_CORERESET);
        return;
      }
      await sleep(1);
    }
    throw new SwdError('TIMEOUT', 'The target did not halt after reset.');
  }

  /** Reset the chip and let it run, leaving debug mode. */
  async resetAndRun() {
    const demcr = await this.readMem32(DEMCR);
    await this.writeMem32(DEMCR, demcr & ~VC_CORERESET);
    await this.writeMem32(DHCSR, DBGKEY | C_DEBUGEN);
    await this.writeMem32(AIRCR, AIRCR_SYSRESETREQ).catch(() => {});
    await sleep(10);
    try {
      await this.writeMem32(DHCSR, DBGKEY);
    } catch (e) {
      await this.connect();
      await this.powerUp();
      await this.writeMem32(DHCSR, DBGKEY);
    }
  }
}
