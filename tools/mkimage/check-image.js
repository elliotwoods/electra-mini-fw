#!/usr/bin/env node
/**
 * Pre-flash gate for Electra One Mini application images.
 *
 * The bootloader will happily program whatever S-record it finds at /boot/update.srec.
 * It is not a validator, so this is. Everything checked here is something that would
 * otherwise be discovered as a device that does not boot.
 *
 *   node check-image.js <image.srec> [--expect-base 0xNNNNNNNN] [--stage <ELECTRA-drive>]
 *
 * --stage copies the image to <drive>/boot/update.srec, but only if every check passed
 * and only if the destination is not named blupdate.srec. See RECOVERY.md.
 */

'use strict';

const fs = require('fs');
const path = require('path');

// ---- device facts (see ../../README.md, src/startup/memory.ld) -------------
//   0x00000000  stock Electra bootloader — never ours to write
//   0x00100000  our bootloader (EMB), 128 KB
//   0x00120000  application, 2944 KB
const STOCK_BOOT_END = 0x00100000; // nothing of ours may land below this, ever
const FLASH_END = 0x00400000;      // 4 MB part
const SRAM_LO = 0x1ffe0000;
const SRAM_HI = 0x20080000;

// Which image is being gated. Defaults to the bootloader base, so a bare
// `check-image.js foo.srec` still does the right thing for the stock-shaped case.
const baseArgIdx = process.argv.indexOf('--expect-base');
const APP_BASE = baseArgIdx >= 0 ? Number(process.argv[baseArgIdx + 1]) : 0x00100000;
const BOOT_END = STOCK_BOOT_END;

const ADDR_BYTES = { 0: 2, 1: 2, 2: 3, 3: 4, 5: 2, 6: 3, 7: 4, 8: 3, 9: 2 };

let failures = 0;
let warnings = 0;

function pass(msg) { console.log(`  ok    ${msg}`); }
function fail(msg) { console.log(`  FAIL  ${msg}`); failures++; }
function warn(msg) { console.log(`  warn  ${msg}`); warnings++; }
function hex(n) { return `0x${(n >>> 0).toString(16).padStart(8, '0')}`; }

/** Parse S-record into coalesced segments + entry point. Throws on malformed input. */
function parseSrec(text) {
  const chunks = [];
  let entry = null;
  let header = null;
  const lines = text.split(/\r?\n/);

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i].trim();
    if (!line) continue;
    if (line[0] !== 'S') throw new Error(`line ${i + 1}: not an S-record`);

    const type = Number(line[1]);
    const count = parseInt(line.slice(2, 4), 16);
    const payload = Buffer.from(line.slice(4), 'hex');
    if (payload.length !== count) {
      throw new Error(`line ${i + 1}: byte count ${count} but ${payload.length} bytes present`);
    }

    let sum = count;
    for (let b = 0; b < payload.length - 1; b++) sum += payload[b];
    if (((~sum) & 0xff) !== payload[payload.length - 1]) {
      throw new Error(`line ${i + 1}: checksum mismatch`);
    }

    const nAddr = ADDR_BYTES[type];
    if (nAddr === undefined) throw new Error(`line ${i + 1}: unknown record type S${type}`);

    let addr = 0;
    for (let b = 0; b < nAddr; b++) addr = addr * 256 + payload[b];
    const data = payload.slice(nAddr, payload.length - 1);

    if (type === 0) header = data.toString('latin1');
    else if (type >= 1 && type <= 3) chunks.push({ addr, buf: data });
    else if (type >= 7 && type <= 9) entry = addr;
  }

  chunks.sort((a, b) => a.addr - b.addr);
  const segs = [];
  for (const c of chunks) {
    const last = segs[segs.length - 1];
    if (last && last.addr + last.len === c.addr) {
      last.parts.push(c.buf);
      last.len += c.buf.length;
    } else {
      segs.push({ addr: c.addr, len: c.buf.length, parts: [c.buf] });
    }
  }
  return { header, entry, segs, recordCount: chunks.length };
}

function main() {
  const args = process.argv.slice(2);
  const file = args[0];
  const stageIdx = args.indexOf('--stage');
  const stageTo = stageIdx >= 0 ? args[stageIdx + 1] : null;

  if (!file) {
    console.error('usage: node check-image.js <image.srec> [--expect-base 0xNNNNNNNN] [--stage <ELECTRA-drive>]');
    process.exit(2);
  }

  console.log(`\nchecking ${path.basename(file)}\n`);

  let img;
  try {
    img = parseSrec(fs.readFileSync(file, 'latin1'));
  } catch (e) {
    console.log(`  FAIL  ${e.message}`);
    console.log('\nrefusing to continue: the bootloader would reject or mis-apply this.\n');
    process.exit(1);
  }
  pass(`S-record parses, ${img.recordCount} data records, all checksums valid`);

  // ---- layout -------------------------------------------------------------
  if (img.segs.length === 0) {
    fail('no data records at all');
  } else if (img.segs.length > 1) {
    // Name the gaps, with addresses and sizes. This used to report only "N segments", which is
    // not enough to distinguish a harmless four-byte alignment hole between .vectors and .text
    // from a genuinely broken image. The flasher meanwhile REJECTED any gap outright, so the
    // two tools disagreed about what a valid image was — and the disagreement only surfaced
    // when a routine linker change happened to introduce a hole.
    warn(`${img.segs.length} segments — the flasher fills inter-section gaps with 0xFF`);
    for (const s of img.segs) console.log(`          ${hex(s.addr)}–${hex(s.addr + s.len)}`);
    for (let i = 1; i < img.segs.length; i++) {
      const end = img.segs[i - 1].addr + img.segs[i - 1].len;
      const gap = img.segs[i].addr - end;
      console.log(`          gap ${hex(end)}–${hex(img.segs[i].addr)} (${gap} bytes)`);
      if (gap > 4096) fail(`gap of ${gap} bytes is too large to be alignment padding`);
    }
  }

  const lo = img.segs.length ? img.segs[0].addr : 0;
  const hi = img.segs.length ? img.segs[img.segs.length - 1].addr + img.segs[img.segs.length - 1].len : 0;

  if (lo === APP_BASE) {
    pass(`loads at ${hex(APP_BASE)}`);
  } else {
    fail(`loads at ${hex(lo)}, expected ${hex(APP_BASE)} — wrong linker script?`);
  }

  if (lo < BOOT_END) {
    fail(`image starts below ${hex(BOOT_END)} — this would overwrite the STOCK BOOTLOADER. Refusing.`);
  } else {
    pass('does not intrude on the bootloader region');
  }

  if (hi > FLASH_END) {
    fail(`image ends at ${hex(hi)}, past the ${hex(FLASH_END)} end of flash`);
  } else {
    pass(`fits in flash (${((hi - lo) / 1024).toFixed(1)} KiB, ends ${hex(hi)})`);
  }

  // ---- vector table -------------------------------------------------------
  if (img.segs.length && lo === APP_BASE && img.segs[0].len >= 8) {
    const head = Buffer.concat(img.segs[0].parts).slice(0, 8);
    const sp = head.readUInt32LE(0);
    const pc = head.readUInt32LE(4);

    if (sp > SRAM_LO && sp <= SRAM_HI) pass(`initial SP ${hex(sp)} is in SRAM`);
    else fail(`initial SP ${hex(sp)} is not in SRAM (${hex(SRAM_LO)}–${hex(SRAM_HI)})`);

    if (pc & 1) pass(`reset vector ${hex(pc)} has the Thumb bit set`);
    else fail(`reset vector ${hex(pc)} is missing the Thumb bit — the core will fault immediately`);

    const target = pc & ~1;
    if (target >= lo && target < hi) pass('reset vector points inside the image');
    else fail(`reset vector ${hex(target)} points outside the image`);
  }

  if (img.entry !== null) pass(`entry record present (${hex(img.entry)})`);
  else warn('no S7/S8/S9 entry record');

  // ---- verdict ------------------------------------------------------------
  console.log('');
  if (failures) {
    console.log(`${failures} failure(s). DO NOT FLASH.\n`);
    process.exit(1);
  }
  console.log(warnings ? `passed with ${warnings} warning(s).\n` : 'all checks passed.\n');

  if (stageTo) {
    const dest = path.join(stageTo, 'boot', 'update.srec');
    if (/blupdate/i.test(dest)) {
      console.log('refusing to write blupdate.srec — that is the bootloader. See RECOVERY.md.\n');
      process.exit(1);
    }
    fs.mkdirSync(path.dirname(dest), { recursive: true });
    fs.copyFileSync(file, dest);
    console.log(`staged -> ${dest}`);
    console.log('eject the drive, then reconnect USB. Flash takes 2-3 min; do not interrupt.\n');
  }
}

main();
