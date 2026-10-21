// Runs the web programmer's JavaScript against the simulated probe.
//   node test/run.mjs

import assert from 'node:assert/strict';
import { FakeDapDevice } from './sim.mjs';
import { WebUsbDap, SwdError } from '../js/webusb-dap.js';
import { SwdTarget } from '../js/swd.js';
import { Stm32F4, FlashError, FLASH_BASE } from '../js/stm32f4.js';
import { parseHex, parseElf, alignSegments, crc32, looksLikeVectorTable } from '../js/firmware.js';

let failures = 0;
async function test(name, fn) {
  try {
    await fn();
    console.log('ok   ' + name);
  } catch (e) {
    failures++;
    console.log('FAIL ' + name + '\n     ' + (e.stack || e));
  }
}

async function setup(opts) {
  const dev = new FakeDapDevice(opts);
  const dap = new WebUsbDap(dev);
  await dap.open();
  const swd = new SwdTarget(dap);
  const f4 = new Stm32F4(swd);
  return { dev, dap, swd, f4 };
}

function randomImage(len, seed = 1) {
  const d = new Uint8Array(len);
  let x = seed;
  for (let i = 0; i < len; i++) {
    x = (x * 1103515245 + 12345) >>> 0;
    d[i] = x >>> 24;
  }
  // a plausible vector table
  d.set([0x00, 0x00, 0x02, 0x20, 0xc1, 0x01, 0x00, 0x08]);
  return d;
}

function toHex(segments) {
  const lines = [];
  const rec = (type, addr, bytes) => {
    const r = [bytes.length, (addr >> 8) & 255, addr & 255, type, ...bytes];
    const sum = (256 - (r.reduce((a, b) => a + b, 0) & 255)) & 255;
    lines.push(':' + [...r, sum].map((b) => b.toString(16).padStart(2, '0').toUpperCase()).join(''));
  };
  for (const s of segments) {
    let upper = -1;
    for (let i = 0; i < s.data.length; i += 16) {
      const a = s.addr + i;
      if (a >>> 16 !== upper) {
        upper = a >>> 16;
        rec(4, 0, [upper >> 8, upper & 255]);
      }
      rec(0, a & 0xffff, [...s.data.subarray(i, i + 16)]);
    }
  }
  rec(1, 0, []);
  return lines.join('\n') + '\n';
}

function toElf(segments) {
  const phoff = 52, phentsize = 32;
  let off = phoff + phentsize * segments.length;
  const total = off + segments.reduce((n, s) => n + s.data.length, 0);
  const b = new Uint8Array(total);
  const dv = new DataView(b.buffer);
  b.set([0x7f, 0x45, 0x4c, 0x46, 1, 1, 1]);
  dv.setUint16(16, 2, true);
  dv.setUint16(18, 40, true);
  dv.setUint32(28, phoff, true);
  dv.setUint16(42, phentsize, true);
  dv.setUint16(44, segments.length, true);
  segments.forEach((s, i) => {
    const p = phoff + i * phentsize;
    dv.setUint32(p, 1, true);
    dv.setUint32(p + 4, off, true);
    dv.setUint32(p + 8, 0x20000000, true); // vaddr (RAM for .data) differs from paddr
    dv.setUint32(p + 12, s.addr, true);
    dv.setUint32(p + 16, s.data.length, true);
    dv.setUint32(p + 20, s.data.length + 64, true);
    b.set(s.data, off);
    off += s.data.length;
  });
  return b;
}

await test('open drains a stale response and reads probe info', async () => {
  const { dap } = await setup({ staleResponse: true });
  assert.equal(dap.packetSize, 64);
  assert.equal(dap.packetCount, 1);
  assert.equal(await dap.infoString(0x02), 'Adafruit_DAP_CH32 CMSIS-DAP');
  assert.equal(dap.interfaceName, 'Adafruit CMSIS-DAP v2');
});

await test('busy probe gives a clear message', async () => {
  const dap = new WebUsbDap(new FakeDapDevice({ busy: true }));
  await assert.rejects(dap.open(), (e) => e instanceof SwdError && /OpenOCD/.test(e.message));
});

await test('identify an STM32F405', async () => {
  const { f4, swd } = await setup();
  await f4.identify();
  assert.equal(f4.name, 'STM32F405/407/415/417');
  assert.equal(f4.flashKB, 1024);
  assert.equal(swd.dpidr, 0x2ba01477);
});

await test('write, verify and read back an image with a blank hole', async () => {
  const { f4, dev } = await setup();
  const img = randomImage(70001);
  img.fill(0xff, 8192, 10240); // 2 KB of 0xFF: skipped when programming
  const segments = alignSegments([{ addr: FLASH_BASE, data: img }]);
  assert.equal(segments[0].data.length, 70004);
  await f4.select();
  const stages = new Set();
  const before = dev.commands;
  await f4.writeImage(segments, { onProgress: (s) => stages.add(s) });
  const used = dev.commands - before;
  await f4.deselect(true);
  assert.deepEqual([...stages], ['erase', 'program', 'verify']);
  assert.deepEqual(dev.target.stats.erases, [0, 1, 2, 3, 4]);
  assert.deepEqual(dev.target.flash.subarray(0, img.length), img);
  assert.equal(dev.target.flash[70001], 0xff); // padding
  assert.equal(dev.target.flash[0x1ffff], 0xff); // rest of sector 4 erased
  assert.equal(dev.target.flash[0x20000], 0x00); // sector 5 untouched
  assert.equal(dev.target.stats.flashWrites, (70004 - 2048) / 4);
  assert.ok(dev.target.flashCr & 0x80000000, 'flash locked afterwards');
  assert.ok(!dev.target.halted, 'target running afterwards');
  const back = await f4.read(FLASH_BASE, img.length);
  assert.deepEqual(back, img);
  console.log('     ' + used + ' USB round trips for 68 KB erase+program+verify (' +
    (used / 68.4).toFixed(1) + ' per KB)');
});

await test('reset that drops the SWD link is recovered', async () => {
  const { f4, dev } = await setup({ resetDropsLink: true });
  await f4.select();
  assert.ok(dev.target.halted);
  const img = alignSegments([{ addr: FLASH_BASE, data: randomImage(1000) }]);
  await f4.writeImage(img, {});
  await f4.deselect(true);
  assert.ok(!dev.target.halted);
});

await test('verify catches flash that was not erased', async () => {
  const { f4 } = await setup();
  await f4.select();
  const data = randomImage(64); // flash still holds zeros: AND gives zeros
  await f4.program(FLASH_BASE + 0x40000, data);
  await assert.rejects(f4.verify(FLASH_BASE + 0x40000, data),
    (e) => e instanceof FlashError && e.address === FLASH_BASE + 0x40000);
});

await test('mass erase', async () => {
  const { f4, dev } = await setup();
  await f4.select();
  await f4.writeImage(alignSegments([{ addr: FLASH_BASE, data: randomImage(256) }]), { eraseAll: true });
  assert.equal(dev.target.stats.massErase, 1);
  assert.equal(dev.target.flash[0xfffff], 0xff);
});

await test('out-of-range image is rejected before erasing', async () => {
  const { f4, dev } = await setup();
  await f4.select();
  await assert.rejects(f4.writeImage([{ addr: FLASH_BASE + 0xfff00, data: new Uint8Array(0x200) }], {}),
    FlashError);
  assert.equal(dev.target.stats.erases.length, 0);
});

await test('Intel HEX with two regions round-trips', async () => {
  const a = randomImage(300, 7), b = randomImage(50, 9);
  const segs = [{ addr: FLASH_BASE, data: a }, { addr: FLASH_BASE + 0x20010, data: b }];
  const parsed = parseHex(toHex(segs));
  assert.equal(parsed.length, 2);
  assert.deepEqual(parsed[0].data, a);
  assert.equal(parsed[1].addr, FLASH_BASE + 0x20010);
  assert.deepEqual(parsed[1].data, b);
  assert.throws(() => parseHex(':0100000041BE\n'), /end-of-file/);
  assert.throws(() => parseHex(':0100000041BF\n:00000001FF\n'), /Checksum/);
});

await test('ELF uses physical (load) addresses', async () => {
  const text = randomImage(200, 3), data = randomImage(12, 4);
  const segs = parseElf(toElf([{ addr: FLASH_BASE, data: text }, { addr: FLASH_BASE + 200, data }]));
  assert.equal(segs[1].addr, FLASH_BASE + 200);
  const merged = alignSegments(segs);
  assert.equal(merged.length, 1);
  assert.equal(merged[0].data.length, 212);
  assert.ok(looksLikeVectorTable(merged, FLASH_BASE));
});

await test('alignSegments pads unaligned edges with 0xFF and rejects overlaps', async () => {
  const s = alignSegments([{ addr: FLASH_BASE + 2, data: Uint8Array.of(1, 2, 3) }]);
  assert.equal(s[0].addr, FLASH_BASE);
  assert.deepEqual(s[0].data, Uint8Array.of(0xff, 0xff, 1, 2, 3, 0xff, 0xff, 0xff));
  assert.throws(() => alignSegments([
    { addr: FLASH_BASE, data: new Uint8Array(8) }, { addr: FLASH_BASE + 4, data: new Uint8Array(8) }]));
});

await test('crc32 matches zlib', async () => {
  assert.equal(crc32(new TextEncoder().encode('123456789')), 0xcbf43926);
});

console.log(failures ? failures + ' test(s) failed' : 'all tests passed');
process.exit(failures ? 1 : 0);
