// Page logic: connect the QT Py over WebUSB, identify the target, load a
// firmware file and program it.

import { WebUsbDap, SwdError, INFO, findDapInterface } from './webusb-dap.js';
import { SwdTarget } from './swd.js';
import { Stm32F4, FlashError, FLASH_BASE, hex32 } from './stm32f4.js';
import { loadFirmware, looksLikeVectorTable, FirmwareError } from './firmware.js';

const $ = (id) => document.getElementById(id);

class CancelledError extends Error {
  constructor() {
    super('Cancelled.');
    this.name = 'CancelledError';
  }
}

const state = {
  dap: null,
  swd: null,
  f4: null,
  targetOk: false,
  file: null,   // File, kept so a new .bin address can re-parse it
  image: null,  // result of loadFirmware()
  busy: false,
  cancel: false,
  cancellable: false,
};

// ---------------------------------------------------------------- log

const t0 = performance.now();
function log(text, kind = '') {
  const el = $('log');
  const line = document.createElement('div');
  const t = document.createElement('span');
  t.className = 't';
  t.textContent = ((performance.now() - t0) / 1000).toFixed(3).padStart(8) + '  ';
  const msg = document.createElement('span');
  if (kind) msg.className = kind;
  msg.textContent = text;
  line.append(t, msg);
  el.append(line);
  while (el.childElementCount > 2000) el.firstElementChild.remove();
  el.scrollTop = el.scrollHeight;
}

// ---------------------------------------------------------------- view helpers

function setNode(which, nodeState, status) {
  $('node-' + which).dataset.state = nodeState;
  if (status !== undefined) $('node-' + which + '-status').textContent = status;
}

function setLink(which, linkState) {
  $('link-' + which).dataset.state = linkState;
}

function setMessage(id, text, kind = '') {
  const el = $(id);
  el.textContent = text || '';
  el.className = 'message' + (kind ? ' ' + kind : '');
}

function setFacts(id, rows) {
  const dl = $(id);
  dl.replaceChildren();
  for (const [k, v, num] of rows) {
    const dt = document.createElement('dt');
    dt.textContent = k;
    const dd = document.createElement('dd');
    dd.textContent = v;
    if (num) dd.className = 'num';
    dl.append(dt, dd);
  }
  dl.hidden = rows.length === 0;
}

const kb = (n) => (n / 1024).toFixed(n < 10240 ? 1 : 0) + ' KB';

function refreshControls() {
  const probe = !!(state.dap && state.dap.opened);
  const idle = !state.busy;
  $('btn-connect').hidden = probe;
  $('btn-disconnect').hidden = !probe;
  $('btn-connect').disabled = !idle || !WebUsbDap.supported;
  $('btn-disconnect').disabled = !idle;
  $('btn-detect').disabled = !probe || !idle;
  $('sel-clock').disabled = !idle;
  $('step-target').dataset.locked = String(!probe);
  $('step-firmware').dataset.locked = String(!(probe && state.targetOk));
  $('btn-program').disabled = !(probe && state.targetOk && state.image && idle);
  $('btn-cancel').hidden = !(state.busy && state.cancellable);
  for (const id of ['btn-backup', 'btn-reset', 'btn-mass-erase']) {
    $(id).disabled = !(probe && state.targetOk && idle);
  }
  $('file').disabled = !idle;
}

// ---------------------------------------------------------------- errors

function describe(e) {
  if (e instanceof SwdError || e instanceof FlashError || e instanceof FirmwareError ||
      e instanceof CancelledError) return e.message;
  if (e && e.name === 'NotFoundError') return null; // chooser closed
  if (e && e.name === 'SecurityError') return 'The browser blocked USB access: ' + e.message;
  console.error(e);
  return 'Unexpected error: ' + (e && e.message ? e.message : e);
}

function probeLost(reason) {
  if (state.dap) state.dap.close();
  state.dap = state.swd = state.f4 = null;
  state.targetOk = false;
  setNode('probe', 'idle', 'Not connected');
  setNode('target', 'idle', 'Unknown');
  setLink('usb', 'idle');
  setLink('swd', 'idle');
  $('probe-facts').hidden = true;
  $('target-facts').hidden = true;
  setMessage('target-message', '');
  if (reason) {
    setMessage('probe-message', reason, 'error');
    log(reason, 'error');
  }
  refreshControls();
}

/**
 * Run an operation that uses the target. Releases the SWD pins afterwards,
 * reports errors in the step's message line.
 */
async function run(messageId, fn) {
  if (state.busy) return;
  state.busy = true;
  state.cancel = false;
  refreshControls();
  try {
    await fn();
  } catch (e) {
    const text = describe(e);
    if (text) {
      setMessage(messageId, text, e instanceof CancelledError ? 'warn' : 'error');
      log(text, e instanceof CancelledError ? 'warn' : 'error');
    }
    if (e instanceof SwdError && e.code === 'USB' && state.dap && !state.dap.opened) {
      probeLost('Lost contact with the QT Py. Unplug it, plug it back in and connect again.');
    } else if (state.dap && !(e instanceof FirmwareError) && !(e instanceof CancelledError)) {
      setLink('swd', 'error');
    }
  } finally {
    if (state.swd && state.dap && state.dap.opened) {
      await state.swd.disconnect().catch(() => {});
    }
    state.busy = false;
    state.cancellable = false;
    refreshControls();
  }
}

// ---------------------------------------------------------------- probe

async function connectProbe(device, silent = false) {
  if (state.busy) return;
  state.busy = true;
  refreshControls();
  setMessage('probe-message', '');
  setNode('probe', 'busy', 'Connecting…');
  setLink('usb', 'busy');
  const dap = new WebUsbDap(device);
  try {
    await dap.open();
    const product = await dap.infoString(INFO.PRODUCT).catch(() => '');
    const version = await dap.infoString(INFO.FIRMWARE_VERSION).catch(() => '');
    const serial = await dap.infoString(INFO.SERIAL).catch(() => device.serialNumber || '');
    state.dap = dap;
    state.swd = new SwdTarget(dap);
    state.swd.clockHz = Number($('sel-clock').value);
    state.f4 = new Stm32F4(state.swd);
    setNode('probe', 'ok', device.productName || product || 'CMSIS-DAP probe');
    setLink('usb', 'ok');
    setFacts('probe-facts', [
      ['Device', device.productName || '(no name)'],
      ['Probe firmware', [product, version].filter(Boolean).join(' ') || 'unknown'],
      ['Serial number', serial || 'none', true],
      ['Packets', dap.packetSize + ' bytes, ' + dap.packetCount + ' buffered'],
    ]);
    log('Connected to ' + (device.productName || 'probe') + ' (' + (product || 'CMSIS-DAP') + ' ' + version + ')', 'ok');
  } catch (e) {
    await dap.close();
    setNode('probe', 'error', 'Could not connect');
    setLink('usb', 'error');
    const text = describe(e);
    if (text && !silent) {
      setMessage('probe-message', text, 'error');
      log(text, 'error');
    } else if (text) {
      setNode('probe', 'idle', 'Not connected');
      setLink('usb', 'idle');
      log('Could not reconnect automatically: ' + text, 'warn');
    }
    return;
  } finally {
    state.busy = false;
    refreshControls();
  }
  await detectTarget();
}

async function chooseProbe() {
  try {
    const device = await WebUsbDap.request();
    await connectProbe(device);
  } catch (e) {
    const text = describe(e);
    if (text) {
      setMessage('probe-message', text, 'error');
      log(text, 'error');
    }
  }
}

async function disconnectProbe() {
  if (state.busy) return;
  if (state.swd) await state.swd.disconnect().catch(() => {});
  log('Disconnected from the QT Py.');
  probeLost(null);
  setMessage('probe-message', '');
}

// ---------------------------------------------------------------- target

function showTarget() {
  const f4 = state.f4;
  setNode('target', 'ok', f4.name + ', ' + f4.flashKB + ' KB');
  setLink('swd', 'ok');
  setFacts('target-facts', [
    ['Chip', f4.name],
    ['DEV_ID', hex32(f4.devId & 0xfff).replace('0x00000', '0x'), true],
    ['Flash', f4.flashKB + ' KB' + (f4.flashKB > 1024 ? ' (first 1 MB programmable here)' : '')],
    ['SWD DPIDR', hex32(state.swd.dpidr), true],
  ]);
  checkImageFits();
}

async function detectTarget() {
  await run('target-message', async () => {
    setMessage('target-message', '');
    setNode('target', 'busy', 'Looking…');
    setLink('swd', 'busy');
    state.targetOk = false;
    try {
      await state.f4.identify();
    } catch (e) {
      if (e instanceof FlashError && state.swd.dpidr) {
        setNode('target', 'error', 'Unsupported chip');
        setFacts('target-facts', [['SWD DPIDR', hex32(state.swd.dpidr), true],
          ['DBGMCU_IDCODE', hex32(state.f4.devId), true]]);
      } else {
        setNode('target', 'error', 'No answer');
        $('target-facts').hidden = true;
      }
      throw e;
    }
    state.targetOk = true;
    showTarget();
    log('Found ' + state.f4.name + ' with ' + state.f4.flashKB + ' KB flash (DPIDR ' + hex32(state.swd.dpidr) + ')', 'ok');
  });
}

// ---------------------------------------------------------------- firmware file

function parseAddress(text) {
  const t = text.trim().replace(/_/g, '');
  if (!/^(0x[0-9a-f]+|\d+)$/i.test(t)) return null;
  const v = Number(t);
  return Number.isFinite(v) && v >= 0 && v <= 0xffffffff ? v : null;
}

function checkImageFits() {
  const img = state.image;
  const warn = $('file-warning');
  const notes = [];
  if (img) {
    if (!looksLikeVectorTable(img.segments, FLASH_BASE)) {
      notes.push('No STM32 vector table at ' + hex32(FLASH_BASE) +
        '. The chip will not start from this image unless a bootloader there jumps to it.');
    }
    if (state.f4 && state.targetOk) {
      const limit = FLASH_BASE + state.f4.flashLimit;
      if (img.start < FLASH_BASE || img.end > limit) {
        notes.push('The image (' + hex32(img.start) + '..' + hex32(img.end) +
          ') does not fit in this chip\'s flash (' + hex32(FLASH_BASE) + '..' + hex32(limit) + ').');
      }
    }
  }
  warn.textContent = notes.join(' ');
  warn.hidden = notes.length === 0;
}

async function useFile(file) {
  state.file = file;
  const addrInput = $('bin-address');
  const addr = parseAddress(addrInput.value);
  addrInput.setAttribute('aria-invalid', String(addr === null));
  setMessage('program-message', '');
  try {
    state.image = await loadFirmware(file, addr === null ? FLASH_BASE : addr);
  } catch (e) {
    state.image = null;
    $('file-card').hidden = true;
    const text = describe(e);
    setMessage('program-message', text, 'error');
    log(file.name + ': ' + text, 'error');
    refreshControls();
    return;
  }
  const img = state.image;
  $('bin-address-row').hidden = img.format !== 'binary';
  setFacts('file-facts', [
    ['File', img.name],
    ['Format', img.format + ', ' + img.fileSize.toLocaleString() + ' bytes'],
    ['Address', hex32(img.start) + ' to ' + hex32(img.end), true],
    ['To program', kb(img.bytes) + (img.segments.length > 1 ? ' in ' + img.segments.length + ' regions' : '')],
    ['File CRC-32', hex32(img.fileCrc), true],
  ]);
  $('file-card').hidden = false;
  checkImageFits();
  log('Loaded ' + img.name + ': ' + img.format + ', ' + kb(img.bytes) + ' at ' + hex32(img.start));
  refreshControls();
}

// ---------------------------------------------------------------- progress

const STAGE_TEXT = { erase: 'Erasing', program: 'Programming', verify: 'Verifying', read: 'Reading' };

function makeProgress(stages) {
  const box = $('progress');
  box.hidden = false;
  box.dataset.done = 'false';
  box.dataset.failed = 'false';
  for (const li of $('stages').children) {
    li.hidden = !stages.includes(li.dataset.stage);
    li.dataset.status = '';
  }
  $('stages').hidden = stages.length === 1;
  let stage = null, stageStart = 0, lastDraw = 0;
  const update = (s, done, total) => {
    if (state.cancel) throw new CancelledError();
    const now = performance.now();
    if (s !== stage) {
      for (const li of $('stages').children) {
        if (li.dataset.stage === stage) li.dataset.status = 'done';
        if (li.dataset.stage === s) li.dataset.status = 'active';
      }
      stage = s;
      stageStart = now;
    }
    if (now - lastDraw < 50 && done < total) return;
    lastDraw = now;
    const pct = total ? (100 * done) / total : 0;
    $('bar-fill').style.width = pct.toFixed(1) + '%';
    $('bar').setAttribute('aria-valuenow', pct.toFixed(0));
    let text;
    if (s === 'erase') {
      text = $('opt-erase-all').checked ? 'Erasing the whole chip'
        : done < total ? 'Erasing sector ' + (done + 1) + ' of ' + total
        : 'Erased ' + total + (total === 1 ? ' sector' : ' sectors');
    } else {
      const secs = (now - stageStart) / 1000;
      const rate = secs > 0.2 ? ', ' + (done / 1024 / secs).toFixed(1) + ' KB/s' : '';
      text = STAGE_TEXT[s] + ' ' + kb(done) + ' of ' + kb(total) + rate;
    }
    $('progress-text').textContent = text;
  };
  update.finish = (ok, text) => {
    for (const li of $('stages').children) {
      if (ok && li.dataset.status === 'active') li.dataset.status = 'done';
    }
    box.dataset.done = String(ok);
    box.dataset.failed = String(!ok);
    if (ok) $('bar-fill').style.width = '100%';
    $('progress-text').textContent = text;
  };
  return update;
}

// ---------------------------------------------------------------- program

async function programTarget() {
  const img = state.image;
  if (!img) return;
  const opts = {
    verify: $('opt-verify').checked,
    eraseAll: $('opt-erase-all').checked,
    run: $('opt-run').checked,
  };
  setMessage('program-message', '');
  const stages = ['erase', 'program'].concat(opts.verify ? ['verify'] : []);
  const progress = makeProgress(stages);
  const start = performance.now();
  log('Programming ' + img.name + ' (' + kb(img.bytes) + ')' + (opts.eraseAll ? ', erasing the whole chip' : ''));
  state.cancellable = true;
  await run('program-message', async () => {
    setNode('target', 'busy', 'Programming…');
    setLink('swd', 'busy');
    let ok = false;
    try {
      await state.f4.select();
      await state.f4.writeImage(img.segments, { ...opts, onProgress: progress });
      ok = true;
    } catch (e) {
      progress.finish(false, e instanceof CancelledError ? 'Cancelled' : 'Failed');
      throw e;
    } finally {
      // On failure: lock the flash and leave the chip halted, so a half
      // written image does not run.
      await state.f4.deselect(ok && opts.run).catch((e) => log('Could not reset the target: ' + describe(e), 'warn'));
      if (state.targetOk) showTarget();
      if (!ok) setNode('target', 'error', 'Programming failed');
    }
    const secs = ((performance.now() - start) / 1000).toFixed(1);
    const text = 'Programmed ' + kb(img.bytes) + ' in ' + secs + ' s' +
      (opts.verify ? ', verified' : '') + (opts.run ? '. The target is running the new firmware.' : '. The target is halted.');
    progress.finish(true, 'Done in ' + secs + ' s');
    setMessage('program-message', text, 'ok');
    log(text, 'ok');
  });
}

// ---------------------------------------------------------------- tools

async function backupFlash() {
  setMessage('tools-message', '');
  const progress = makeProgress(['read']);
  state.cancellable = true;
  await run('tools-message', async () => {
    setLink('swd', 'busy');
    await state.f4.identify();
    const len = state.f4.flashLimit;
    log('Reading ' + kb(len) + ' of flash');
    let data;
    try {
      data = await state.f4.read(FLASH_BASE, len, (d, t) => progress('read', d, t));
    } catch (e) {
      progress.finish(false, e instanceof CancelledError ? 'Cancelled' : 'Failed');
      throw e;
    }
    const blob = new Blob([data], { type: 'application/octet-stream' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'stm32-' + (state.f4.devId & 0xfff).toString(16) + '-flash-' + new Date().toISOString().slice(0, 10) + '.bin';
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
    progress.finish(true, 'Saved ' + a.download);
    setLink('swd', 'ok');
    setMessage('tools-message', 'Saved ' + kb(len) + ' as ' + a.download + '.', 'ok');
    log('Saved flash as ' + a.download, 'ok');
  });
}

async function resetTarget() {
  setMessage('tools-message', '');
  await run('tools-message', async () => {
    setLink('swd', 'busy');
    await state.swd.connect();
    await state.swd.powerUp();
    await state.swd.resetAndRun();
    setLink('swd', 'ok');
    setMessage('tools-message', 'Target reset; its firmware is running.', 'ok');
    log('Target reset', 'ok');
  });
}

async function massErase() {
  if (!confirm('Erase all flash on the ' + state.f4.name + '? Its firmware will be gone.')) return;
  setMessage('tools-message', '');
  await run('tools-message', async () => {
    setNode('target', 'busy', 'Erasing…');
    setLink('swd', 'busy');
    log('Erasing the whole chip');
    const start = performance.now();
    try {
      await state.f4.select();
      await state.f4.massErase();
    } finally {
      await state.f4.deselect(false).catch(() => {});
      showTarget();
    }
    const text = 'Chip erased in ' + ((performance.now() - start) / 1000).toFixed(1) + ' s.';
    setMessage('tools-message', text, 'ok');
    log(text, 'ok');
  });
}

// ---------------------------------------------------------------- startup

function wireEvents() {
  $('btn-connect').addEventListener('click', chooseProbe);
  $('btn-disconnect').addEventListener('click', disconnectProbe);
  $('btn-detect').addEventListener('click', detectTarget);
  $('btn-program').addEventListener('click', programTarget);
  $('btn-cancel').addEventListener('click', () => {
    state.cancel = true;
    log('Cancelling after the current step…', 'warn');
  });
  $('btn-backup').addEventListener('click', backupFlash);
  $('btn-reset').addEventListener('click', resetTarget);
  $('btn-mass-erase').addEventListener('click', massErase);
  $('btn-clear-log').addEventListener('click', () => $('log').replaceChildren());

  $('sel-clock').addEventListener('change', () => {
    if (state.swd) state.swd.clockHz = Number($('sel-clock').value);
    log('SWD clock set to ' + $('sel-clock').selectedOptions[0].textContent);
  });

  $('file').addEventListener('change', () => {
    const f = $('file').files[0];
    if (f) useFile(f);
    $('file').value = '';
  });
  $('bin-address').addEventListener('change', () => {
    if (state.file) useFile(state.file);
  });

  const drop = $('drop');
  drop.addEventListener('dragover', (e) => {
    e.preventDefault();
    if (!state.busy) drop.classList.add('over');
  });
  drop.addEventListener('dragleave', () => drop.classList.remove('over'));
  drop.addEventListener('drop', (e) => {
    e.preventDefault();
    drop.classList.remove('over');
    const f = e.dataTransfer.files[0];
    if (f && !state.busy) useFile(f);
  });
  // Dropping a file elsewhere should not navigate away from the page.
  window.addEventListener('dragover', (e) => e.preventDefault());
  window.addEventListener('drop', (e) => e.preventDefault());

  navigator.usb.addEventListener('disconnect', (e) => {
    if (state.dap && e.device === state.dap.device) {
      state.busy = false;
      probeLost('The QT Py was unplugged.');
    }
  });
  navigator.usb.addEventListener('connect', (e) => {
    if (!state.dap && !state.busy) {
      log('QT Py plugged in, connecting…');
      connectProbe(e.device, true);
    }
  });
}

async function start() {
  refreshControls();
  if (!window.isSecureContext) {
    setNode('pc', 'error', 'Page not served securely');
    const n = $('unsupported');
    n.textContent = 'WebUSB only works on https:// pages or http://localhost. Open this page from GitHub Pages or a local web server.';
    n.hidden = false;
    $('btn-connect').disabled = true;
    return;
  }
  if (!WebUsbDap.supported) {
    setNode('pc', 'error', 'No WebUSB in this browser');
    const n = $('unsupported');
    n.textContent = 'This browser has no WebUSB. Use Chrome, Edge or another Chromium-based browser on a desktop computer.';
    n.hidden = false;
    $('btn-connect').disabled = true;
    return;
  }
  setNode('pc', 'ok', 'WebUSB ready');
  wireEvents();
  log('Ready. Connect the QT Py to begin.');

  // Reconnect to a probe this page was allowed to use before.
  const known = (await WebUsbDap.permittedDevices()).filter(
    (d) => !d.configuration || findDapInterface(d));
  if (known.length === 1) {
    log('Reconnecting to ' + (known[0].productName || 'the QT Py') + '…');
    await connectProbe(known[0], true);
  }
}

start();
