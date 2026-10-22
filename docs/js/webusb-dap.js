// CMSIS-DAP v2 over WebUSB.
//
// Finds the probe's vendor-class CMSIS-DAP interface (bulk OUT + bulk IN),
// claims it and runs one command at a time, the way the QT Py's
// Adafruit_USBD_CMSISDAP expects (DAP_PACKET_COUNT = 1).

export const CMD = {
  INFO: 0x00,
  CONNECT: 0x02,
  DISCONNECT: 0x03,
  TRANSFER_CONFIGURE: 0x04,
  TRANSFER: 0x05,
  TRANSFER_BLOCK: 0x06,
  SWJ_PINS: 0x10,
  SWJ_CLOCK: 0x11,
  SWJ_SEQUENCE: 0x12,
};

export const INFO = {
  VENDOR: 0x01,
  PRODUCT: 0x02,
  SERIAL: 0x03,
  PROTOCOL_VERSION: 0x04,
  FIRMWARE_VERSION: 0x09,
  CAPABILITIES: 0xf0,
  PACKET_COUNT: 0xfe,
  PACKET_SIZE: 0xff,
};

// DAP_Transfer request bits
export const REQ = { APnDP: 0x01, RnW: 0x02 };

const PORT_SWD = 1;
const DAP_OK = 0x00;
const ACK_OK = 1, ACK_WAIT = 2, ACK_FAULT = 4;
const RESP_ERROR = 0x08, RESP_MISMATCH = 0x10;
const COMMAND_TIMEOUT_MS = 3000;

/** A probe, USB or SWD failure. code matches Adafruit_DAP_CH32::Error names. */
export class SwdError extends Error {
  constructor(code, message) {
    super(message || SwdError.text(code));
    this.name = 'SwdError';
    this.code = code;
  }

  static text(code) {
    switch (code) {
      case 'NO_ACK': return 'No answer from the target. Check the SWD wiring, GND and target power.';
      case 'WAIT': return 'The target kept answering WAIT.';
      case 'FAULT': return 'The target answered FAULT.';
      case 'PROTOCOL': return 'SWD parity or protocol error. Try a lower SWD clock or shorter wires.';
      case 'MISMATCH': return 'Value mismatch.';
      case 'TIMEOUT': return 'The operation timed out.';
      case 'DAP': return 'The probe rejected a CMSIS-DAP command.';
      case 'ALIGN': return 'Address or length is not a multiple of 4.';
      case 'USB': return 'USB communication with the probe failed.';
      default: return 'Unknown error.';
    }
  }
}

export function put32(arr, offset, v) {
  arr[offset] = v & 0xff;
  arr[offset + 1] = (v >>> 8) & 0xff;
  arr[offset + 2] = (v >>> 16) & 0xff;
  arr[offset + 3] = (v >>> 24) & 0xff;
}

export function get32(arr, offset) {
  return (arr[offset] | (arr[offset + 1] << 8) | (arr[offset + 2] << 16) |
          (arr[offset + 3] << 24)) >>> 0;
}

function transferError(resp) {
  if (resp & RESP_ERROR) return new SwdError('PROTOCOL');
  if (resp & RESP_MISMATCH) return new SwdError('MISMATCH');
  const ack = resp & 0x07;
  if (ack === ACK_WAIT) return new SwdError('WAIT');
  if (ack === ACK_FAULT) return new SwdError('FAULT');
  return new SwdError('NO_ACK');
}

const CMD_NAMES = {
  0x00: 'Info', 0x02: 'Connect', 0x03: 'Disconnect', 0x04: 'TransferConfigure',
  0x05: 'Transfer', 0x06: 'TransferBlock', 0x10: 'SWJ_Pins', 0x11: 'SWJ_Clock',
  0x12: 'SWJ_Sequence',
};
const HISTORY_LENGTH = 16;

const hexBytes = (b, max) => Array.from(b.slice(0, max), (x) => x.toString(16).padStart(2, '0')).join(' ') +
  (b.length > max ? ' …(' + b.length + ' bytes)' : '');

function withTimeout(promise, ms, onTimeout) {
  let timer;
  return Promise.race([
    promise,
    new Promise((_, reject) => {
      timer = setTimeout(() => reject(onTimeout()), ms);
    }),
  ]).finally(() => clearTimeout(timer));
}

/**
 * Find a CMSIS-DAP v2 interface: vendor class, a bulk OUT and a bulk IN
 * endpoint, preferably with "CMSIS-DAP" in its name (as OpenOCD and pyOCD
 * require).
 */
export function findDapInterface(device) {
  const config = device.configuration;
  if (!config) return null;
  let fallback = null;
  for (const iface of config.interfaces) {
    for (const alt of iface.alternates) {
      if (alt.interfaceClass !== 0xff) continue;
      const epOut = alt.endpoints.find((e) => e.direction === 'out' && e.type === 'bulk');
      const epIn = alt.endpoints.find((e) => e.direction === 'in' && e.type === 'bulk');
      if (!epOut || !epIn) continue;
      const found = { iface, alt, epOut, epIn };
      if (alt.interfaceName && alt.interfaceName.includes('CMSIS-DAP')) return found;
      if (!fallback) fallback = found;
    }
  }
  return fallback;
}

export class WebUsbDap {
  /** @param {USBDevice} device */
  constructor(device) {
    this.device = device;
    this.packetSize = 64;
    this.packetCount = 1;
    this.opened = false;
    this._queue = Promise.resolve();
    this._ifaceNumber = -1;
    this.history = []; // last commands and responses, for error reports
  }

  /** The last commands and responses, oldest first, one line each. */
  describeHistory() {
    return this.history.map((h, i) => {
      const dt = i ? h.t - this.history[i - 1].t : 0;
      return ('+' + dt.toFixed(1) + ' ms').padStart(10) + '  ' +
        (CMD_NAMES[h.req[0]] || '0x' + h.req[0].toString(16)).padEnd(17) +
        hexBytes(h.req, 20) + '  ->  ' + (h.resp ? hexBytes(h.resp, 12) : '(no response)');
    });
  }

  _record(req, resp) {
    this.history.push({ t: performance.now(), req, resp });
    if (this.history.length > HISTORY_LENGTH) this.history.shift();
  }

  static get supported() {
    return typeof navigator !== 'undefined' && 'usb' in navigator;
  }

  /** Show Chrome's device chooser (needs a user gesture). */
  static async request() {
    return navigator.usb.requestDevice({ filters: [{ classCode: 0xff }] });
  }

  /** Probes this site may already open without asking. */
  static async permittedDevices() {
    return navigator.usb.getDevices();
  }

  async open() {
    const d = this.device;
    try {
      if (!d.opened) await d.open();
    } catch (e) {
      throw new SwdError('USB', 'Could not open the probe (' + e.message + '). ' +
        'On Linux, add a udev rule that gives your user access to it.');
    }
    if (!d.configuration) await d.selectConfiguration(1);
    const found = findDapInterface(d);
    if (!found) {
      await d.close().catch(() => {});
      throw new SwdError('USB', 'This device has no CMSIS-DAP v2 interface. ' +
        'Upload the cmsis_dap_probe sketch to the QT Py.');
    }
    try {
      await d.claimInterface(found.iface.interfaceNumber);
    } catch (e) {
      await d.close().catch(() => {});
      throw new SwdError('USB', 'Could not claim the CMSIS-DAP interface. ' +
        'Close OpenOCD, pyOCD or any other tab using the probe, then try again.');
    }
    if (found.alt.alternateSetting !== 0) {
      await d.selectAlternateInterface(found.iface.interfaceNumber, found.alt.alternateSetting);
    }
    this._ifaceNumber = found.iface.interfaceNumber;
    this.epOut = found.epOut.endpointNumber;
    this.epIn = found.epIn.endpointNumber;
    this.inSize = Math.max(found.epIn.packetSize, 64);
    this.interfaceName = found.alt.interfaceName || '';
    this.opened = true;

    // The first command also drains a response left over from an earlier
    // session (see command()).
    this.packetSize = (await this.infoU16(INFO.PACKET_SIZE)) || 64;
    this.packetCount = (await this.infoU8(INFO.PACKET_COUNT)) || 1;
    this.inSize = Math.max(this.inSize, this.packetSize);
  }

  async close() {
    const d = this.device;
    this.opened = false;
    try {
      if (d.opened) {
        if (this._ifaceNumber >= 0) await d.releaseInterface(this._ifaceNumber).catch(() => {});
        await d.close();
      }
    } catch (e) {
      // already gone (unplugged)
    }
  }

  /**
   * Send one command and return its response. Commands run one at a time.
   * A response whose command ID does not match is a leftover from an
   * interrupted earlier session and is skipped.
   */
  command(request, timeoutMs = COMMAND_TIMEOUT_MS) {
    const run = async () => {
      if (!this.opened) throw new SwdError('USB', 'The probe is not connected.');
      const out = request instanceof Uint8Array ? request : Uint8Array.from(request);
      const timeout = () => {
        this.opened = false; // a transfer may still be pending: start over
        return new SwdError('USB', 'The probe stopped responding. Unplug the QT Py, plug it back in and connect again.');
      };
      let result;
      try {
        result = await withTimeout(this.device.transferOut(this.epOut, out), timeoutMs, timeout);
      } catch (e) {
        throw e instanceof SwdError ? e : new SwdError('USB', 'USB write failed: ' + e.message);
      }
      if (result.status !== 'ok') throw new SwdError('USB', 'USB write status: ' + result.status);
      for (let attempt = 0; attempt < 4; attempt++) {
        try {
          result = await withTimeout(this.device.transferIn(this.epIn, this.inSize), timeoutMs, timeout);
        } catch (e) {
          throw e instanceof SwdError ? e : new SwdError('USB', 'USB read failed: ' + e.message);
        }
        if (result.status !== 'ok') throw new SwdError('USB', 'USB read status: ' + result.status);
        const data = new Uint8Array(result.data.buffer, result.data.byteOffset, result.data.byteLength);
        if (data.length && data[0] === out[0]) {
          this._record(out, data.slice());
          return data;
        }
      }
      throw new SwdError('USB', 'Probe responses are out of sync. Unplug the QT Py and plug it back in.');
    };
    const p = this._queue.then(run, run);
    this._queue = p.catch(() => {});
    return p;
  }

  async _status(request, name) {
    const r = await this.command(request);
    if (r[1] !== DAP_OK) throw new SwdError('DAP', 'The probe rejected ' + name + '.');
    return r;
  }

  async info(id) {
    const r = await this.command([CMD.INFO, id]);
    return r.slice(2, 2 + r[1]);
  }

  async infoString(id) {
    const b = await this.info(id);
    let n = b.length;
    while (n && b[n - 1] === 0) n--;
    return new TextDecoder().decode(b.slice(0, n));
  }

  async infoU8(id) {
    const b = await this.info(id);
    return b.length >= 1 ? b[0] : 0;
  }

  async infoU16(id) {
    const b = await this.info(id);
    return b.length >= 2 ? b[0] | (b[1] << 8) : 0;
  }

  async connectSwd() {
    const r = await this.command([CMD.CONNECT, PORT_SWD]);
    if (r[1] !== PORT_SWD) throw new SwdError('DAP', 'The probe could not switch to SWD mode.');
  }

  async disconnect() {
    await this.command([CMD.DISCONNECT]);
  }

  async transferConfigure(idleCycles, waitRetry, matchRetry) {
    const req = [CMD.TRANSFER_CONFIGURE, idleCycles,
      waitRetry & 0xff, waitRetry >> 8, matchRetry & 0xff, matchRetry >> 8];
    await this._status(req, 'DAP_TransferConfigure');
  }

  async swjClock(hz) {
    const req = new Uint8Array(5);
    req[0] = CMD.SWJ_CLOCK;
    put32(req, 1, hz);
    await this._status(req, 'DAP_SWJ_Clock');
  }

  /** Clock out bitCount bits (1..256), LSB of data[0] first. */
  async swjSequence(bitCount, data) {
    const req = new Uint8Array(2 + Math.ceil(bitCount / 8));
    req[0] = CMD.SWJ_SEQUENCE;
    req[1] = bitCount & 0xff; // 0 = 256
    req.set(data.slice(0, req.length - 2), 2);
    await this._status(req, 'DAP_SWJ_Sequence');
  }

  /** Drive pins (bit 7 = nRESET) and return the pin input states. */
  async swjPins(value, select, waitUs = 0) {
    const req = new Uint8Array(7);
    req[0] = CMD.SWJ_PINS;
    req[1] = value;
    req[2] = select;
    put32(req, 3, waitUs);
    const r = await this.command(req);
    return r[1];
  }

  /**
   * Several DP/AP transfers in one packet.
   * @param {{req:number, value?:number}[]} ops req: REQ bits | A[3:2]
   * @returns {Promise<number[]>} the values read, in order
   */
  async transfer(ops) {
    const req = [CMD.TRANSFER, 0, ops.length];
    let reads = 0;
    for (const op of ops) {
      req.push(op.req);
      if (op.req & REQ.RnW) {
        reads++;
      } else {
        const v = op.value >>> 0;
        req.push(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, v >>> 24);
      }
    }
    if (req.length > this.packetSize || 3 + reads * 4 > this.packetSize) {
      throw new Error('internal: DAP_Transfer batch too large');
    }
    const r = await this.command(Uint8Array.from(req));
    const done = r[1], resp = r[2];
    if (done !== ops.length || (resp & 0x07) !== ACK_OK || (resp & (RESP_ERROR | RESP_MISMATCH))) {
      const e = transferError(resp);
      e.history = this.describeHistory();
      throw e;
    }
    const values = [];
    for (let i = 0; i < reads; i++) values.push(get32(r, 3 + i * 4));
    return values;
  }

  /** Words per DAP_TransferBlock packet. */
  get blockWriteWords() { return Math.floor((this.packetSize - 5) / 4); }
  get blockReadWords() { return Math.floor((this.packetSize - 4) / 4); }

  /**
   * DAP_TransferBlock: many words to or from one register.
   * @param {number} reqByte transfer request
   * @param {Uint8Array|number} dataOrCount bytes to write, or words to read
   * @returns {Promise<Uint8Array|null>} the bytes read
   */
  async transferBlock(reqByte, dataOrCount) {
    const write = !(reqByte & REQ.RnW);
    const words = write ? dataOrCount.length / 4 : dataOrCount;
    const req = new Uint8Array(5 + (write ? words * 4 : 0));
    req[0] = CMD.TRANSFER_BLOCK;
    req[1] = 0;
    req[2] = words & 0xff;
    req[3] = words >> 8;
    req[4] = reqByte;
    if (write) req.set(dataOrCount, 5);
    const r = await this.command(req);
    const done = r[1] | (r[2] << 8), resp = r[3];
    if (done !== words || (resp & 0x07) !== ACK_OK || (resp & RESP_ERROR)) {
      const e = transferError(resp);
      e.history = this.describeHistory();
      throw e;
    }
    return write ? null : r.slice(4, 4 + words * 4);
  }
}
