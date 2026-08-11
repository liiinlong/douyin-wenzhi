// mp4aac.js —— 手写 MP4 解复用（Node 版，无 ffmpeg）
// 忠实移植 vd_asr.c 的 mp4_extract_aac 算法：解析 moov→trak→mdia→hdlr(soun)→minf→stbl，
// 读 stsz/stsc/stco 样本表，逐帧拼 ADTS+AAC 写出。
//
// 性能/内存关键点（修复 720p 卡死）：
//   .c 把整个文件 mmap/malloc 进内存，stsc/stsz/stco 全是用「内存指针」随机读，1.6 亿次也飞快。
//   本实现只把「moov 结构盒子」（通常几百 KB ~ 几 MB，远小于视频本身）读进内存做解析，
//   与 .c 的内存指针等价且快；真正的 AAC 样本字节（mdat 里的大头）仍用 fd 从磁盘随机读，
//   因此 1.2GB 视频也不会把 384MB 内存撑爆 —— 这是用户要求的「放磁盘、不放内存」方案。

'use strict';

const fs = require('fs');

// ---------- 基础读取 ----------

function rd_be32(b, off) {
  return ((b[off] << 24) | (b[off + 1] << 16) | (b[off + 2] << 8) | b[off + 3]) >>> 0;
}
function rd_be64(b, off) {
  let v = 0;
  for (let i = 0; i < 8; i++) v = v * 256 + b[off + i];
  return v;
}

// 本地 fd 读取器（绝对偏移，从磁盘随机读）—— 用于扫描顶层定位 moov，以及读取样本字节
class FdReader {
  constructor(fd) { this.fd = fd; }
  read(off, len) {
    const buf = Buffer.alloc(len);
    let pos = off, got = 0;
    while (got < len) {
      const n = fs.readSync(this.fd, buf, got, len - got, pos);
      if (n === 0) break;
      got += n; pos += n;
    }
    return buf;
  }
}

// 内存读取器：把一段连续缓冲（如 moov）以「绝对文件偏移」方式暴露。
// 解析 box / stsz / stsc / stco 全走它 = 内存随机读，与 .c 的内存指针等价且飞快。
class MemReader {
  constructor(buf, origin) { this.buf = buf; this.origin = origin; }
  read(off, len) {
    const lo = off - this.origin;
    if (lo < 0) return Buffer.alloc(len);
    return this.buf.subarray(lo, lo + len);
  }
}

// 在 [base, base+size) 区域内查找名为 type 的顶层 box（兼容 64 位 size 与 size=0 兜底）
function findBox(r, base, size, type) {
  let off = 0;
  while (off + 8 <= size) {
    const head = r.read(base + off, 8);
    if (head.length < 8) break;
    let bsize = rd_be32(head, 0);
    const btype = head.toString('latin1', 4, 8);
    let hdr = 8;
    if (bsize === 1) {
      if (off + 16 > size) break;
      bsize = rd_be64(r.read(base + off + 8, 8), 0);
      hdr = 16;
    } else if (bsize === 0) {
      bsize = size - off;
    }
    if (bsize < hdr || off + bsize > size) break;
    if (btype === type) return { off: base + off + hdr, len: bsize - hdr };
    off += bsize;
  }
  return null;
}

// 遍历 [base, base+size) 内的子 box（生成器）
function* eachChild(r, base, size) {
  let off = 0;
  while (off + 8 <= size) {
    const head = r.read(base + off, 8);
    if (head.length < 8) break;
    let bsize = rd_be32(head, 0);
    const btype = head.toString('latin1', 4, 8);
    let hdr = 8;
    if (bsize === 1) {
      if (off + 16 > size) break;
      bsize = rd_be64(r.read(base + off + 8, 8), 0);
      hdr = 16;
    } else if (bsize === 0) {
      bsize = size - off;
    }
    if (bsize < hdr || off + bsize > size) break;
    yield { type: btype, off: base + off + hdr, len: bsize - hdr };
    off += bsize;
  }
}

const AAC_SR_TABLE = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
  16000, 12000, 11025, 8000, 7350, 0, 0, 0];

// ---------- AudioSpecificConfig 解析（兼容 HE-AAC / 显式采样率） ----------
// 把 esds 里 tag=0x05 的 DecoderSpecificInfo 负载按 ISO 14496-3 逐位解析。
// 兼容点：① AOT 扩展(>31)；② HE-AAC(AOT 5/29，含 SBR/PS 扩展块)；
//        ③ samplingFrequencyIndex==15 时的 24bit 显式采样率。
function parseAudioSpecificConfig(asc) {
  const len = asc.length;
  let bitPos = 0;
  const readBits = (n) => {
    let v = 0;
    for (let k = 0; k < n; k++) {
      const byteIdx = bitPos >> 3;
      if (byteIdx >= len) return 0;
      const bit = (asc[byteIdx] >> (7 - (bitPos & 7))) & 1;
      v = (v << 1) | bit;
      bitPos += 1;
    }
    return v;
  };
  let aot = readBits(5);
  if (aot === 31) aot = 32 + readBits(6);
  let freqIdx = readBits(4);
  let explicitRate = 0;
  if (freqIdx === 15) explicitRate = readBits(24);
  const chan = readBits(4);
  let isHeAac = false, isHeAacv2 = false, extRate = 0;
  if (aot === 5 || aot === 29) {
    isHeAac = true;
    if (aot === 29) isHeAacv2 = true;
    const extAot = readBits(5);
    if (extAot === 5 || extAot === 29) {
      let efIdx = readBits(4);
      if (efIdx === 15) extRate = readBits(24);
    }
  }
  return { objType: aot, freqIdx, explicitRate, chan, isHeAac, isHeAacv2, extRate };
}

// ADTS profile(2bit) 映射：HE-AAC 系列在 ADTS 里统一标为 LC(2)，SBR/PS 由码流内信令携带；
// 其余按 objType-1 取，并夹紧到 [0,3]，避免非法 profile 写出损坏的 ADTS 头。
function adtsProfileFor(objType) {
  if (objType === 5 || objType === 29) return 2;
  let p = objType - 1;
  if (p < 0) p = 1;
  if (p > 3) p = 3;
  return p;
}

// 24bit 显式采样率 → 最接近的 AAC_SR_TABLE 标准索引（ADTS 只接受 4bit 索引）
function freqIndexFor(explicitRate) {
  let best = 4, bestDiff = Infinity;
  for (let i = 0; i < AAC_SR_TABLE.length; i++) {
    const r = AAC_SR_TABLE[i];
    if (!r) continue;
    const d = Math.abs(r - explicitRate);
    if (d < bestDiff) { bestDiff = d; best = i; }
  }
  return best;
}

// 解析 esds 里的 AudioSpecificConfig（DecoderSpecificInfo, tag 0x05）
function parse_esds(buf, start, len) {
  let i = 4;
  while (i + 1 < len) {
    const tag = buf[start + i];
    if (tag === 0x05) {
      let j = i + 1, dlen = 0, cnt = 0;
      while (j < len && cnt < 4) {
        const by = buf[start + j++]; dlen = (dlen << 7) | (by & 0x7f); cnt++;
        if (!(by & 0x80)) break;
      }
      if (j + 2 <= len && dlen >= 2) {
        const asc = buf.slice(start + j, start + j + dlen);
        return parseAudioSpecificConfig(asc);
      }
      return null;
    }
    i++;
  }
  return null;
}

function write_adts(profile, freqIdx, chan, frameLen) {
  const aac_frame_length = frameLen + 7;
  const out = Buffer.alloc(7);
  out[0] = 0xff;
  out[1] = 0xf1;
  out[2] = ((profile & 0x3) << 6) | ((freqIdx & 0xf) << 2) | ((chan >> 2) & 0x1);
  out[3] = ((chan & 0x3) << 6) | ((aac_frame_length >> 11) & 0x3);
  out[4] = (aac_frame_length >> 3) & 0xff;
  out[5] = ((aac_frame_length & 0x7) << 5) | 0x1f;
  out[6] = 0xfc;
  return out;
}

// ---------- 解析 MP4 结构，得到音频样本清单（与来源无关） ----------
// reader: 任意带 .read(off,len)->Buffer 的对象（本地 fd 或 moov 内存缓冲）
// moovOff/moovLen: moov 盒子「内容」在 reader 中的偏移与长度（不含 8 字节头）。
//   直接遍历 moov 的子 box（与 .c 的 mp4_iter_init(moov) 一致，无需在 moov 内再搜 moov）。
function buildAudioPlan(reader, moovOff, moovLen, fileSize) {
  // 遍历 trak，找到 handler_type == 'soun' 的音频轨（与 .c 一致）
  let audioStbl = null;
  for (const box of eachChild(reader, moovOff, moovLen)) {
    if (box.type !== 'trak') continue;
    const mdia = findBox(reader, box.off, box.len, 'mdia');
    if (!mdia) continue;
    const hdlr = findBox(reader, mdia.off, mdia.len, 'hdlr');
    if (!hdlr || hdlr.len < 12) continue;
    const handlerType = reader.read(hdlr.off + 8, 4).toString('latin1', 0, 4);
    if (handlerType !== 'soun') continue;
    const minf = findBox(reader, mdia.off, mdia.len, 'minf');
    if (!minf) continue;
    const stbl = findBox(reader, minf.off, minf.len, 'stbl');
    if (!stbl) continue;
    audioStbl = stbl;
    break;
  }
  if (!audioStbl) throw new Error('未找到音频轨(soun)，视频可能无音轨');

  // stsd -> esds（找不到就扫描原始字节）
  const stsd = findBox(reader, audioStbl.off, audioStbl.len, 'stsd');
  if (!stsd || stsd.len < 8) throw new Error('缺 stsd');
  let esds = findBox(reader, stsd.off + 8, stsd.len - 8, 'esds');
  if (!esds) {
    const region = reader.read(stsd.off, stsd.len);
    for (let k = 0; k + 4 <= stsd.len; k++) {
      if (region.toString('latin1', k, k + 4) === 'esds') {
        esds = { off: stsd.off + k + 4, len: stsd.len - (k + 4) };
        break;
      }
    }
  }
  let objType = 2, freqIdx = 4, chan = 2, isHeAac = false, isHeAacv2 = false, explicitRate = 0;
  if (esds) {
    const asc = parse_esds(reader.read(esds.off, esds.len), 0, esds.len);
    if (asc) {
      objType = asc.objType;
      freqIdx = asc.freqIdx;
      chan = asc.chan;
      isHeAac = asc.isHeAac;
      isHeAacv2 = asc.isHeAacv2;
      explicitRate = asc.explicitRate;
    }
  }
  if (freqIdx === 15 && explicitRate) freqIdx = freqIndexFor(explicitRate);
  else if (freqIdx < 0 || freqIdx > 12) freqIdx = 4;
  const profile = adtsProfileFor(objType);
  const sampleRate = AAC_SR_TABLE[freqIdx];

  // 读取样本大小表：优先 stsz(每样本 32bit)，缺失时回退 stz2(紧凑型 8/16/32bit 打包)
  const stsz = findBox(reader, audioStbl.off, audioStbl.len, 'stsz');
  let fixedSize = 0, sampleCount = 0, stszTable = 0, stszBytesPerEntry = 4;
  if (stsz && stsz.len >= 12) {
    const stszBuf = reader.read(stsz.off, stsz.len);
    fixedSize = rd_be32(stszBuf, 4);
    sampleCount = rd_be32(stszBuf, 8);
    stszTable = stsz.off + 12;
    stszBytesPerEntry = 4;
  } else {
    const stz2 = findBox(reader, audioStbl.off, audioStbl.len, 'stz2');
    if (!stz2 || stz2.len < 12) throw new Error('缺 stsz/stz2（无样本大小表）');
    const stz2Buf = reader.read(stz2.off, stz2.len);
    const fieldSize = stz2Buf[5]; // version(1)+flags(3)+reserved(1)+field_size(1)
    if (![1, 2, 4].includes(fieldSize)) throw new Error('stz2 field_size=' + fieldSize + ' 暂不支持');
    sampleCount = rd_be32(stz2Buf, 6);
    stszTable = stz2.off + 10;
    stszBytesPerEntry = fieldSize;
    fixedSize = 0; // stz2 必须为逐样本大小
  }
  if (sampleCount === 0) throw new Error('音频样本数为 0（无音轨或非标准封装）');

  const stsc = findBox(reader, audioStbl.off, audioStbl.len, 'stsc');
  if (!stsc || stsc.len < 8) throw new Error('缺 stsc');
  const stscCount = rd_be32(reader.read(stsc.off, stsc.len), 4);
  const stscTable = stsc.off + 8;

  let stco = findBox(reader, audioStbl.off, audioStbl.len, 'stco');
  let is64 = false;
  if (!stco) { stco = findBox(reader, audioStbl.off, audioStbl.len, 'co64'); is64 = true; }
  if (!stco || stco.len < 8) throw new Error('缺 stco/co64');
  const chunkCount = rd_be32(reader.read(stco.off, stco.len), 4);
  const stcoTable = stco.off + 8;

  // 一次性把样本大小表读进内存（与 .c 的内存指针等价）；兼容 stsz(4字节/样本) 与 stz2(1/2/4字节/样本)
  const stszFull = reader.read(stszTable, sampleCount * stszBytesPerEntry);
  const readSampleSize = (idx) => {
    if (fixedSize) return fixedSize;
    let v = 0;
    const base = idx * stszBytesPerEntry;
    for (let k = 0; k < stszBytesPerEntry; k++) v = (v << 8) | stszFull[base + k];
    return v >>> 0;
  };
  const stscEntries = [];
  {
    const raw = reader.read(stscTable, stscCount * 12);
    for (let e = 0; e < stscCount; e++) stscEntries.push([rd_be32(raw, e * 12), rd_be32(raw, e * 12 + 4)]);
  }
  const chunkEntries = [];
  {
    const raw = reader.read(stcoTable, chunkCount * (is64 ? 8 : 4));
    for (let c = 0; c < chunkCount; c++) {
      chunkEntries.push(is64 ? rd_be64(raw, c * 8) : rd_be32(raw, c * 4));
    }
  }

  // 逐 chunk -> 逐 sample（与 .c 的 mp4_extract_aac 主循环逐字对应）
  const samples = [];
  let sampleIdx = 0;
  for (let c = 0; c < chunkCount && sampleIdx < sampleCount; c++) {
    let spc = 1;
    for (let e = 0; e < stscCount; e++) {
      const firstChunk = stscEntries[e][0];
      const samplesPer = stscEntries[e][1];
      if (firstChunk <= c + 1) spc = samplesPer; else break;
    }
    const chunkOff = chunkEntries[c];
    let pos = chunkOff;
    for (let s = 0; s < spc && sampleIdx < sampleCount; s++) {
      const ssize = readSampleSize(sampleIdx);
      if (pos + ssize > fileSize) break;
      samples.push({ off: pos, size: ssize });
      pos += ssize;
      sampleIdx++;
    }
  }
  if (!samples.length) throw new Error('未提取到任何 AAC 样本（视频可能无音轨或非标准封装）');

  // 不合并样本：MP4 中每个 audio 样本 = 一个独立 AAC 帧，必须各自带一个 ADTS 帧头。
  // 旧逻辑把「首尾相接的相邻样本」合并成一个大块、只写 1 个 ADTS 帧头，导致一个帧头包裹 8~15 个真实帧，
  // 解码器按帧长跳过时边界全部错位 → 声音杂乱、时长变短。这里改为逐帧独立封装（每样本一个帧头）。
  let totalBytes = 0;
  for (const s of samples) totalBytes += 7 + s.size;

  return { samples, profile, freqIdx, chan, sampleRate, totalBytes, frames: samples.length, isHeAac, isHeAacv2 };
}

// ---------- 按样本清单把音频写入 .aac（readAt 从磁盘 fd 随机读样本字节） ----------
async function writePlanToAAC(plan, readAt, aacPath) {
  const outFd = fs.openSync(aacPath, 'w');
  let written = 0;
  try {
    for (const s of plan.samples) {
      if (written + 7 + s.size > plan.totalBytes * 1.5 + 1024) {
        throw new Error('音频提取异常：输出体积远超样本清单总和，疑似 MP4 封装结构异常（手写解析无法处理）。请改用 ffmpeg 方案。');
      }
      const adts = write_adts(plan.profile, plan.freqIdx, plan.chan, s.size);
      const data = await readAt(s.off, s.size);
      fs.writeSync(outFd, adts);
      fs.writeSync(outFd, data);
      written += 7 + s.size;
    }
  } finally {
    fs.closeSync(outFd);
  }
  if (written === 0) throw new Error('音频写入为空');
  return written;
}

// ---------- 本地提取：moov 读内存解析 + 样本字节从磁盘读 ----------
async function extractAAC(mp4Path, aacPath) {
  const fd = fs.openSync(mp4Path, 'r');
  try {
    const fsize = fs.fstatSync(fd).size;
    const r = new FdReader(fd);
    // 1) 定位 moov（仅读 8 字节头、按 box 跳跃，极快）
    const moov = findBox(r, 0, fsize, 'moov');
    if (!moov) throw new Error('未找到 moov box（可能不是标准 MP4/AAC 封装）');
    // 2) 把 moov 整块读进内存（几百 KB~几 MB，安全），后续解析全在内存里（等同 .c 内存指针）
    const moovBuf = Buffer.alloc(moov.len);
    fs.readSync(fd, moovBuf, 0, moov.len, moov.off);
    const mem = new MemReader(moovBuf, moov.off);
    const plan = buildAudioPlan(mem, moov.off, moov.len, fsize);
    // 3) 样本字节从磁盘随机读（大头留在磁盘，不进内存）
    const fdRead = (off, len) => {
      const b = Buffer.alloc(len);
      let pos = off, got = 0;
      while (got < len) {
        const n = fs.readSync(fd, b, got, len - got, pos);
        if (n === 0) break;
        got += n; pos += n;
      }
      return b;
    };
    const written = await writePlanToAAC(plan, fdRead, aacPath);
    if (written > fsize * 2) {
      try { fs.unlinkSync(aacPath); } catch (_) {}
      throw new Error('音频提取异常：输出(' + (written / 1048576).toFixed(1) + 'MB)远超输入视频(' + (fsize / 1048576).toFixed(1) + 'MB)，封装结构可能非标准。建议改用 ffmpeg 方式。');
    }
    return { frames: plan.frames, sampleRate: plan.sampleRate, chan: plan.chan, totalBytes: plan.totalBytes, isHeAac: plan.isHeAac, isHeAacv2: plan.isHeAacv2 };
  } finally {
    fs.closeSync(fd);
  }
}

// ---------- 按 ADTS 帧边界流式分片（对应 vd_asr.c 的 split_audio，AF_AAC_ADTS） ----------
async function forEachSegment(aacPath, maxChunk, cb) {
  const fd = fs.openSync(aacPath, 'r');
  try {
    const size = fs.fstatSync(fd).size;
    const readAt = (off, len) => {
      const b = Buffer.alloc(len);
      let pos = off, got = 0;
      while (got < len) {
        const n = fs.readSync(fd, b, got, len - got, pos);
        if (n === 0) break;
        got += n; pos += n;
      }
      return b;
    };

    let pos = 0, segStart = 0, segLen = 0;
    while (pos < size) {
      if (pos + 7 > size) break;
      const h = readAt(pos, 7);
      if (!(h[0] === 0xff && (h[1] & 0xf0) === 0xf0)) {
        await cb(readAt(pos, size - pos));
        return;
      }
      let fl = ((h[3] & 0x03) << 11) | (h[4] << 3) | ((h[5] >> 5) & 0x07);
      if (fl < 7) fl = 7;
      const fend = pos + fl;
      if (fend > size) { await cb(readAt(pos, size - pos)); return; }
      if (segLen > 0 && segLen + fl > maxChunk) {
        await cb(readAt(segStart, segLen));
        segStart = pos; segLen = 0;
      }
      if (segLen === 0) segStart = pos;
      segLen += fl;
      pos = fend;
    }
    if (segLen > 0) await cb(readAt(segStart, segLen));
  } finally {
    fs.closeSync(fd);
  }
}

module.exports = { buildAudioPlan, writePlanToAAC, extractAAC, forEachSegment, FdReader, MemReader, AAC_SR_TABLE, rd_be32, rd_be64, parse_esds, parseAudioSpecificConfig, adtsProfileFor, freqIndexFor };
