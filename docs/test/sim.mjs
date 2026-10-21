// A fake WebUSB USBDevice that answers CMSIS-DAP commands the way the QT Py
// does, connected to a simulated STM32F405 (debug port, MEM-AP with 1 KB
// auto-increment wrap, Cortex-M debug registers, F4 flash controller).
// Lets the web app's JavaScript be tested in Node without hardware.

const FLASH_BASE = 0x08000000;
const FLASH_SIZE = 1024 * 1024;
const RAM_BASE = 0x20000000;
const RAM_SIZE = 128 * 1024;

const get32 = (a, o) => (a[o] | (a[o + 1] << 8) | (a[o + 2] << 16) | (a[o + 3] << 24)) >>> 0;
const put32 = (a, o, v) => { a[o] = v & 255; a[o + 1] = (v >>> 8) & 255; a[o + 2] = (v >>> 16) & 255; a[o + 3] = v >>> 24; };

class Stm32F405 {
  constructor(opts = {}) {
    this.opts = opts;
    this.flash = new Uint8Array(FLASH_SIZE).fill(0x00); // old firmware: all zeros
    this.ram = new Uint8Array(RAM_SIZE);
    this.reset(true);
    this.linkUp = false;
    this.select = 0;
    this.ctrlStat = 0;
    this.csw = 0;
    this.tar = 0;
    this.sticky = false;
    this.stats = { flashWrites: 0, erases: [], massErase: 0 };
  }

  reset(power) {
    this.flashCr = 0x80000000;
    this.flashSr = 0;
    this.keyStage = 0;
    this.busyReads = 0;
    if (power) {
      this.dhcsr = 0;
      this.demcr = 0;
    }
    // Vector catch: halt at the first instruction.
    this.halted = !!((this.dhcsr & 1) && (this.demcr & 1));
  }

  // --- memory map ---
  read32(addr) {
    addr >>>= 0;
    if (addr >= FLASH_BASE && addr < FLASH_BASE + FLASH_SIZE) return get32(this.flash, addr - FLASH_BASE);
    if (addr >= RAM_BASE && addr < RAM_BASE + RAM_SIZE) return get32(this.ram, addr - RAM_BASE);
    switch (addr) {
      case 0xe0042000: return 0x10076413; // DBGMCU_IDCODE: rev Z, DEV_ID 0x413
      case 0x1fff7a20: return (1024 << 16) | 0xffff;
      case 0x40023c0c: // FLASH_SR
        if (this.busyReads > 0) { this.busyReads--; return this.flashSr | (1 << 16); }
        return this.flashSr;
      case 0x40023c10: return this.flashCr;
      case 0xe000edf0: return (this.halted ? 1 << 17 : 0) | (1 << 16) | (this.dhcsr & 0xffff);
      case 0xe000edfc: return this.demcr;
      default: return 0;
    }
  }

  write32(addr, v) {
    addr >>>= 0;
    v >>>= 0;
    if (addr >= FLASH_BASE && addr < FLASH_BASE + FLASH_SIZE) {
      if ((this.flashCr & 0x80000000) || !(this.flashCr & 1)) { this.flashSr |= 1 << 7; return; } // PGSERR
      const o = addr - FLASH_BASE;
      put32(this.flash, o, get32(this.flash, o) & v); // flash can only clear bits
      this.stats.flashWrites++;
      return;
    }
    if (addr >= RAM_BASE && addr < RAM_BASE + RAM_SIZE) { put32(this.ram, addr - RAM_BASE, v); return; }
    switch (addr) {
      case 0x40023c04: // KEYR
        if (this.keyStage === 0 && v === 0x45670123) this.keyStage = 1;
        else if (this.keyStage === 1 && v === 0xcdef89ab) { this.flashCr &= ~0x80000000; this.keyStage = 0; }
        else this.keyStage = 0;
        return;
      case 0x40023c0c: this.flashSr &= ~(v & 0xf3); return; // write 1 to clear
      case 0x40023c10: {
        if (this.flashCr & 0x80000000) return; // locked
        this.flashCr = v >>> 0;
        if (v & (1 << 16)) { // STRT
          if (v & 4) { this.flash.fill(0xff); this.stats.massErase++; this.busyReads = 30; }
          else if (v & 2) {
            const n = (v >> 3) & 0xf;
            const start = n < 4 ? n * 0x4000 : n === 4 ? 0x10000 : 0x20000 + (n - 5) * 0x20000;
            const size = n < 4 ? 0x4000 : n === 4 ? 0x10000 : 0x20000;
            this.flash.fill(0xff, start, start + size);
            this.stats.erases.push(n);
            this.busyReads = 5;
          }
          this.flashCr &= ~(1 << 16);
        }
        return;
      }
      case 0xe000edf0:
        if ((v & 0xffff0000) >>> 0 !== 0xa05f0000) return;
        this.dhcsr = v & 0xffff;
        if (v & 2) this.halted = true;
        else if (!(v & 2)) this.halted = false;
        return;
      case 0xe000edfc: this.demcr = v; return;
      case 0xe000ed0c:
        if (v === 0x05fa0004) {
          this.reset(false);
          if (this.opts.resetDropsLink) this.linkUp = false;
        }
        return;
      default:
    }
  }

  // --- debug port ---
  // returns { ack, value }
  access(req, value) {
    const apndp = req & 1, rnw = (req >> 1) & 1, a = req & 0x0c;
    if (!this.linkUp) return { ack: 7 };
    if (!apndp) {
      if (rnw) {
        if (a === 0x00) return { ack: 1, value: 0x2ba01477 };
        if (a === 0x04) {
          const req2 = this.ctrlStat & 0x50000000;
          return { ack: 1, value: (this.ctrlStat | (req2 << 1)) >>> 0 };
        }
        return { ack: 1, value: 0 };
      }
      if (a === 0x00) { this.sticky = false; return { ack: 1 }; }
      if (a === 0x04) { this.ctrlStat = value; return { ack: 1 }; }
      if (a === 0x08) { this.select = value; return { ack: 1 }; }
      return { ack: 1 };
    }
    if (this.sticky) return { ack: 4 };
    const bank = this.select & 0xf0;
    if ((this.select >>> 24) !== 0) return { ack: 1, value: 0 };
    const reg = bank | a;
    const inc = (this.csw & 0x30) === 0x10;
    const step = () => { if (inc) this.tar = ((this.tar & ~0x3ff) | ((this.tar + 4) & 0x3ff)) >>> 0; };
    if (rnw) {
      if (reg === 0x00) return { ack: 1, value: this.csw };
      if (reg === 0x04) return { ack: 1, value: this.tar };
      if (reg === 0x0c) { const v = this.read32(this.tar); step(); return { ack: 1, value: v }; }
      if (reg === 0xfc) return { ack: 1, value: 0x24770011 };
      return { ack: 1, value: 0 };
    }
    if (reg === 0x00) this.csw = value;
    else if (reg === 0x04) this.tar = value >>> 0;
    else if (reg === 0x0c) { this.write32(this.tar, value); step(); }
    return { ack: 1 };
  }

  swjSequence(bits, data) {
    if (this.opts.deadTarget) return; // unpowered / unwired target never answers
    // A line reset (>= 50 ones) followed by the DPIDR read brings the link up.
    let ones = 0;
    for (let i = 0; i < bits; i++) {
      const b = (data[i >> 3] >> (i & 7)) & 1;
      ones = b ? ones + 1 : 0;
      if (ones >= 50) this.linkUp = true;
    }
  }
}

export class FakeDapDevice {
  constructor(opts = {}) {
    this.target = new Stm32F405(opts);
    this.opts = opts;
    this.opened = false;
    this.configuration = null;
    this.inQueue = [];
    this.commands = 0;
    this.productName = 'QT Py CH32V203 CMSIS-DAP';
    if (opts.staleResponse) this.inQueue.push(Uint8Array.of(0x05, 1, 1, 0, 0, 0, 0));
    this.portOn = false;
    this.clockHz = 1000000;
  }

  async open() { this.opened = true; }
  async close() { this.opened = false; }
  async selectConfiguration() {
    const ep = (n, d) => ({ endpointNumber: n, direction: d, type: 'bulk', packetSize: 64 });
    this.configuration = {
      interfaces: [
        { interfaceNumber: 0, alternates: [{ alternateSetting: 0, interfaceClass: 2, interfaceName: 'CDC', endpoints: [] }] },
        { interfaceNumber: 1, alternates: [{ alternateSetting: 0, interfaceClass: 10, interfaceName: null, endpoints: [] }] },
        { interfaceNumber: 2, alternates: [{ alternateSetting: 0, interfaceClass: 0xff,
          interfaceName: 'Adafruit CMSIS-DAP v2', endpoints: [ep(3, 'out'), ep(3, 'in')] }] },
      ],
    };
  }
  async claimInterface(n) { if (this.opts.busy) throw new Error('Unable to claim interface.'); this.claimed = n; }
  async releaseInterface() {}
  async selectAlternateInterface() {}

  async transferOut(ep, data) {
    this.commands++;
    this.inQueue.push(this.execute(Uint8Array.from(data)));
    return { status: 'ok', bytesWritten: data.length };
  }

  async transferIn(ep, len) {
    const r = this.inQueue.shift();
    if (!r) throw new Error('nothing to read (test bug: probe would hang)');
    const out = r.slice(0, len);
    return { status: 'ok', data: new DataView(out.buffer) };
  }

  execute(req) {
    const t = this.target;
    const cmd = req[0];
    const str = (s) => { const b = new TextEncoder().encode(s + '\0'); return Uint8Array.of(0, b.length, ...b); };
    switch (cmd) {
      case 0x00: {
        const id = req[1];
        if (id === 0xff) return Uint8Array.of(0, 2, 64, 0);
        if (id === 0xfe) return Uint8Array.of(0, 1, 1);
        if (id === 0x01) return str('Adafruit');
        if (id === 0x02) return str('Adafruit_DAP_CH32 CMSIS-DAP');
        if (id === 0x03) return str('ABCDEF0123456789ABCDEF01');
        if (id === 0x04) return str('2.1.2');
        if (id === 0x09) return str('0.1.0');
        if (id === 0xf0) return Uint8Array.of(0, 1, 0x01);
        return Uint8Array.of(0, 0);
      }
      case 0x02: this.portOn = true; return Uint8Array.of(0x02, req[1] === 1 || req[1] === 0 ? 1 : 0);
      case 0x03: this.portOn = false; t.linkUp = false; return Uint8Array.of(0x03, 0);
      case 0x04: this.idle = req[1]; return Uint8Array.of(0x04, 0);
      case 0x10: return Uint8Array.of(0x10, 0xff);
      case 0x11: this.clockHz = Math.min(get32(req, 1), 1000000); return Uint8Array.of(0x11, 0);
      case 0x12: {
        const bits = req[1] || 256;
        t.swjSequence(bits, req.subarray(2));
        return Uint8Array.of(0x12, 0);
      }
      case 0x05: {
        const count = req[2];
        let p = 3, done = 0, ack = 1;
        const reads = [];
        for (let i = 0; i < count; i++) {
          const r = req[p++];
          let v = 0;
          if (!(r & 2)) { v = get32(req, p); p += 4; }
          const res = t.access(r, v);
          ack = res.ack;
          if (ack !== 1) break;
          if (r & 2) reads.push(res.value >>> 0);
          done++;
        }
        const out = new Uint8Array(3 + reads.length * 4);
        out[0] = 0x05; out[1] = done; out[2] = ack;
        reads.forEach((v, i) => put32(out, 3 + i * 4, v));
        if (out.length > 64) throw new Error('response too long');
        return out;
      }
      case 0x06: {
        if (req.length > 64) throw new Error('request too long: ' + req.length);
        const count = req[2] | (req[3] << 8);
        const r = req[4];
        const write = !(r & 2);
        const reads = [];
        let done = 0, ack = 1;
        for (let i = 0; i < count; i++) {
          const res = t.access(r, write ? get32(req, 5 + i * 4) : 0);
          ack = res.ack;
          if (ack !== 1) break;
          if (!write) reads.push(res.value);
          done++;
        }
        const out = new Uint8Array(4 + reads.length * 4);
        out[0] = 0x06; out[1] = done & 255; out[2] = done >> 8; out[3] = ack;
        reads.forEach((v, i) => put32(out, 4 + i * 4, v));
        if (out.length > 64) throw new Error('response too long');
        return out;
      }
      default:
        return Uint8Array.of(0xff);
    }
  }
}
