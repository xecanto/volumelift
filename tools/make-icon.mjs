// Draws the VolumeLift icon (speaker + sound waves on a gradient tile) and writes
// assets/VolumeLift.ico (PNG-compressed entries) and assets/icon-256.png.
// Usage: node tools/make-icon.mjs
import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";
import { fileURLToPath } from "node:url";

const root = path.join(path.dirname(fileURLToPath(import.meta.url)), "..");
const SIZES = [16, 20, 24, 32, 40, 48, 64, 256];
const TOP = [79, 70, 229]; // indigo
const BOTTOM = [6, 182, 212]; // cyan

const SPEAKER = [
  [0.17, 0.4],
  [0.3, 0.4],
  [0.47, 0.25],
  [0.47, 0.75],
  [0.3, 0.6],
  [0.17, 0.6],
];

function insidePolygon(x, y, points) {
  let inside = false;
  for (let i = 0, j = points.length - 1; i < points.length; j = i++) {
    const [xi, yi] = points[i];
    const [xj, yj] = points[j];
    if (yi > y !== yj > y && x < ((xj - xi) * (y - yi)) / (yj - yi) + xi) inside = !inside;
  }
  return inside;
}

function insideTile(x, y) {
  const margin = 0.03;
  const radius = 0.22;
  const dx = Math.max(margin + radius - x, 0, x - (1 - margin - radius));
  const dy = Math.max(margin + radius - y, 0, y - (1 - margin - radius));
  return x >= margin && x <= 1 - margin && y >= margin && y <= 1 - margin && dx * dx + dy * dy <= radius * radius;
}

function insideWave(x, y, radii) {
  const dx = x - 0.47;
  const dy = y - 0.5;
  if (dx <= 0 || Math.abs(Math.atan2(dy, dx)) > Math.PI / 4) return false;
  const distance = Math.hypot(dx, dy);
  return radii.some((r) => Math.abs(distance - r) <= 0.03);
}

function render(size) {
  const waves = size <= 24 ? [0.15, 0.28] : [0.13, 0.23, 0.33];
  const ss = 8; // supersampling per axis
  const rgba = Buffer.alloc(size * size * 4);
  for (let py = 0; py < size; py++) {
    for (let px = 0; px < size; px++) {
      let tile = 0;
      let white = 0;
      for (let sy = 0; sy < ss; sy++) {
        for (let sx = 0; sx < ss; sx++) {
          const x = (px + (sx + 0.5) / ss) / size;
          const y = (py + (sy + 0.5) / ss) / size;
          if (!insideTile(x, y)) continue;
          tile++;
          if (insidePolygon(x, y, SPEAKER) || insideWave(x, y, waves)) white++;
        }
      }
      const n = ss * ss;
      const t = (px + py) / (2 * size);
      const o = (py * size + px) * 4;
      for (let c = 0; c < 3; c++) {
        const bg = TOP[c] + (BOTTOM[c] - TOP[c]) * t;
        rgba[o + c] = Math.round(tile ? (bg * (tile - white) + 255 * white) / tile : 0);
      }
      rgba[o + 3] = Math.round((255 * tile) / n);
    }
  }
  return rgba;
}

const CRC_TABLE = Array.from({ length: 256 }, (_, n) => {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});

function crc32(buffer) {
  let c = 0xffffffff;
  for (const byte of buffer) c = CRC_TABLE[(c ^ byte) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const length = Buffer.alloc(4);
  length.writeUInt32BE(data.length);
  const body = Buffer.concat([Buffer.from(type, "ascii"), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(body));
  return Buffer.concat([length, body, crc]);
}

function png(size, rgba) {
  const header = Buffer.alloc(13);
  header.writeUInt32BE(size, 0);
  header.writeUInt32BE(size, 4);
  header[8] = 8; // bit depth
  header[9] = 6; // RGBA
  const rows = Buffer.alloc(size * (size * 4 + 1));
  for (let y = 0; y < size; y++) {
    rows[y * (size * 4 + 1)] = 0; // no filter
    rgba.copy(rows, y * (size * 4 + 1) + 1, y * size * 4, (y + 1) * size * 4);
  }
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk("IHDR", header),
    chunk("IDAT", zlib.deflateSync(rows, { level: 9 })),
    chunk("IEND", Buffer.alloc(0)),
  ]);
}

const images = SIZES.map((size) => ({ size, data: png(size, render(size)) }));
const directory = Buffer.alloc(6 + 16 * images.length);
directory.writeUInt16LE(0, 0);
directory.writeUInt16LE(1, 2); // icon
directory.writeUInt16LE(images.length, 4);
let offset = directory.length;
images.forEach(({ size, data }, i) => {
  const e = 6 + i * 16;
  directory[e] = size >= 256 ? 0 : size;
  directory[e + 1] = size >= 256 ? 0 : size;
  directory.writeUInt16LE(1, e + 4); // planes
  directory.writeUInt16LE(32, e + 6); // bits per pixel
  directory.writeUInt32LE(data.length, e + 8);
  directory.writeUInt32LE(offset, e + 12);
  offset += data.length;
});

fs.mkdirSync(path.join(root, "assets"), { recursive: true });
fs.writeFileSync(path.join(root, "assets", "VolumeLift.ico"), Buffer.concat([directory, ...images.map((i) => i.data)]));
fs.writeFileSync(path.join(root, "assets", "icon-256.png"), images.at(-1).data);
console.log(`wrote assets/VolumeLift.ico (${SIZES.join(", ")} px) and assets/icon-256.png`);
