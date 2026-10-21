// Firmware files: raw .bin, Intel HEX and ELF, turned into the 4-byte
// aligned flash segments Stm32F4.writeImage() programs.

export class FirmwareError extends Error {
  constructor(message) {
    super(message);
    this.name = 'FirmwareError';
  }
}

/** CRC-32 (zlib polynomial), same as Adafruit_DAP_CH32::crc32(). */
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

export function crc32(data, crc = 0) {
  crc = ~crc >>> 0;
  for (let i = 0; i < data.length; i++) crc = CRC_TABLE[(crc ^ data[i]) & 0xff] ^ (crc >>> 8);
  return ~crc >>> 0;
}

export function parseBin(bytes, baseAddr) {
  if (!bytes.length) throw new FirmwareError('The file is empty.');
  return [{ addr: baseAddr >>> 0, data: bytes }];
}

/** Intel HEX: data, EOF, extended segment/linear address records. */
export function parseHex(text) {
  const segments = [];
  let upper = 0;
  let cur = null;
  let sawEof = false;
  const lines = text.split(/\r?\n/);
  for (let ln = 0; ln < lines.length; ln++) {
    const line = lines[ln].trim();
    if (!line) continue;
    const where = ' (line ' + (ln + 1) + ')';
    if (line[0] !== ':' || line.length < 11 || (line.length - 1) % 2) {
      throw new FirmwareError('Not a valid Intel HEX record' + where + '.');
    }
    const rec = new Uint8Array((line.length - 1) / 2);
    for (let i = 0; i < rec.length; i++) {
      const b = parseInt(line.substr(1 + i * 2, 2), 16);
      if (Number.isNaN(b)) throw new FirmwareError('Invalid hex digits' + where + '.');
      rec[i] = b;
    }
    const count = rec[0];
    if (rec.length !== count + 5) throw new FirmwareError('Record length mismatch' + where + '.');
    let sum = 0;
    for (const b of rec) sum = (sum + b) & 0xff;
    if (sum !== 0) throw new FirmwareError('Checksum error' + where + '.');
    const offset = (rec[1] << 8) | rec[2];
    const type = rec[3];
    const payload = rec.subarray(4, 4 + count);
    if (type === 0x00) {
      const addr = (upper + offset) >>> 0;
      if (cur && cur.addr + cur.len === addr) {
        cur.chunks.push(payload.slice());
        cur.len += count;
      } else {
        cur = { addr, len: count, chunks: [payload.slice()] };
        segments.push(cur);
      }
    } else if (type === 0x01) {
      sawEof = true;
      break;
    } else if (type === 0x02) {
      upper = ((payload[0] << 8) | payload[1]) << 4;
    } else if (type === 0x04) {
      upper = (((payload[0] << 8) | payload[1]) << 16) >>> 0;
    } else if (type !== 0x03 && type !== 0x05) {
      throw new FirmwareError('Unknown record type ' + type + where + '.');
    }
  }
  if (!sawEof) throw new FirmwareError('The HEX file has no end-of-file record (truncated?).');
  if (!segments.length) throw new FirmwareError('The HEX file contains no data.');
  return segments.map((s) => {
    const data = new Uint8Array(s.len);
    let p = 0;
    for (const c of s.chunks) {
      data.set(c, p);
      p += c.length;
    }
    return { addr: s.addr, data };
  });
}

/** 32-bit little-endian ARM ELF: PT_LOAD segments at their load (physical) address. */
export function parseElf(bytes) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (bytes[4] !== 1) throw new FirmwareError('Only 32-bit ELF files are supported.');
  if (bytes[5] !== 1) throw new FirmwareError('Only little-endian ELF files are supported.');
  const machine = dv.getUint16(18, true);
  if (machine !== 40) throw new FirmwareError('This ELF is not for an ARM processor (e_machine ' + machine + ').');
  const phoff = dv.getUint32(28, true);
  const phentsize = dv.getUint16(42, true);
  const phnum = dv.getUint16(44, true);
  const segments = [];
  for (let i = 0; i < phnum; i++) {
    const p = phoff + i * phentsize;
    if (p + 32 > bytes.length) throw new FirmwareError('The ELF program headers are truncated.');
    const type = dv.getUint32(p, true);
    const offset = dv.getUint32(p + 4, true);
    const paddr = dv.getUint32(p + 12, true);
    const filesz = dv.getUint32(p + 16, true);
    if (type !== 1 || filesz === 0) continue; // PT_LOAD with file data only
    if (offset + filesz > bytes.length) throw new FirmwareError('An ELF segment is truncated.');
    segments.push({ addr: paddr, data: bytes.slice(offset, offset + filesz) });
  }
  if (!segments.length) throw new FirmwareError('The ELF file has no loadable data.');
  return segments;
}

/**
 * Sort, pad to 4-byte boundaries with 0xFF and merge segments that touch
 * after padding.
 */
export function alignSegments(segments) {
  const sorted = [...segments].sort((a, b) => a.addr - b.addr);
  const ranges = [];
  for (const s of sorted) {
    const start = (s.addr & ~3) >>> 0;
    const end = ((s.addr + s.data.length + 3) & ~3) >>> 0;
    const last = ranges[ranges.length - 1];
    if (last && start <= last.end) {
      if (s.addr < last.dataEnd) {
        throw new FirmwareError('The image has overlapping data at 0x' + s.addr.toString(16) + '.');
      }
      last.end = Math.max(last.end, end);
      last.dataEnd = s.addr + s.data.length;
      last.parts.push(s);
    } else {
      ranges.push({ start, end, dataEnd: s.addr + s.data.length, parts: [s] });
    }
  }
  return ranges.map((r) => {
    const data = new Uint8Array(r.end - r.start).fill(0xff);
    for (const p of r.parts) data.set(p.data, p.addr - r.start);
    return { addr: r.start, data };
  });
}

/** STM32 vector table check from the drag-and-drop engine. */
export function looksLikeVectorTable(segments, flashBase) {
  const first = segments.find((s) => s.addr === flashBase);
  if (!first || first.data.length < 8) return false;
  const d = first.data;
  const sp = (d[0] | (d[1] << 8) | (d[2] << 16) | (d[3] << 24)) >>> 0;
  const reset = (d[4] | (d[5] << 8) | (d[6] << 16) | (d[7] << 24)) >>> 0;
  const spOk = !(sp & 3) && ((sp >= 0x20000000 && sp <= 0x20080000) ||
                             (sp >= 0x10000000 && sp <= 0x10010000));
  const resetOk = (reset & 1) === 1 && reset >= flashBase && reset < flashBase + 0x100000;
  return spOk && resetOk;
}

/**
 * Load a firmware file chosen by the user.
 * @param {File} file
 * @param {number} binBase load address for .bin files
 */
export async function loadFirmware(file, binBase) {
  const bytes = new Uint8Array(await file.arrayBuffer());
  const name = file.name.toLowerCase();
  let format, raw;
  if (bytes.length >= 4 && bytes[0] === 0x7f && bytes[1] === 0x45 && bytes[2] === 0x4c && bytes[3] === 0x46) {
    format = 'ELF';
    raw = parseElf(bytes);
  } else if (name.endsWith('.hex') || name.endsWith('.ihex') || name.endsWith('.ihx')) {
    format = 'Intel HEX';
    raw = parseHex(new TextDecoder().decode(bytes));
  } else if (name.endsWith('.uf2')) {
    throw new FirmwareError('UF2 files are not supported yet. Use the .bin, .hex or .elf from your build.');
  } else {
    format = 'binary';
    raw = parseBin(bytes, binBase);
  }
  const segments = alignSegments(raw);
  const start = segments[0].addr;
  const last = segments[segments.length - 1];
  return {
    name: file.name,
    format,
    fileSize: bytes.length,
    fileCrc: crc32(bytes),
    segments,
    start,
    end: last.addr + last.data.length,
    bytes: segments.reduce((n, s) => n + s.data.length, 0),
  };
}
