// index.js —— 腾讯云函数（SCF）主入口
// 流程严格对齐 vd_asr.c（模式 A「在线视频转文字」）：
//   解析多平台分享链接 -> 下载视频(流式落盘) -> 手写提取 AAC(边读边写文件) ->
//   按 ADTS 帧边界分片 -> 腾讯云 ASR(CreateRecTask + 轮询 DescribeTaskStatus) -> 合并文字
// 前端只调用本函数的 API 网关地址，所有「脏活」在云端完成，不受前端域名白名单限制。

'use strict';

const https = require('https');
const http = require('http');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { tc3Sign } = require('./tc3.js');
const { extractAAC, forEachSegment, AAC_SR_TABLE } = require('./mp4aac.js');

const ASR_HOST = 'asr.tencentcloudapi.com';
const ASR_VERSION = '2019-06-14';
// 多平台聚合解析接口：自动识别 20+ 平台（抖音/快手/小红书/B站/油管/TikTok…），
// 与 vd_asr.c 的 PARSE_BASE 保持一致。无需手动选平台，直接把分享链接丢给它即可。
const PARSE_API = 'https://api.bugpk.com/api/short_videos?url=';

// 内存保护：云函数内存上限仅 384MB。任何一步把大文件整体缓冲进内存都会被平台 kill。
// 单响应体硬上限（解析接口/代理下载共用），超过即中止请求并给出明确错误，而不是 OOM。
const MAX_BODY = 256 * 1024 * 1024;
// 视频落盘上限：受 /tmp 磁盘空间约束。提取已改为「按帧随机读 + 写盘」，不把整段视频读进内存，
// 故内存不再是瓶颈；此处仅防 /tmp 被写满。超过此值的视频仍可下载/预览，但无法转文字。
const MAX_VIDEO_BYTES = 1500 * 1024 * 1024;
// 腾讯云录音文件识别（SourceType=1 直传 Data）单文件上限 5MB，分片留安全余量。
const SEG_MAX = 5 * 1024 * 1024 - 64 * 1024;

function sleep(ms) { return new Promise((r) => setTimeout(r, ms)); }

// ---------- 通用 HTTP GET（自动跟随重定向，带响应体上限保护） ----------
function pickModule(u) { return u.startsWith('https') ? https : http; }
function httpGet(url, redirects = 5) {
  return new Promise((resolve, reject) => {
    const mod = pickModule(url);
    const req = mod.get(url, { headers: { 'User-Agent': 'Mozilla/5.0 (compatible; dyasr/1.0)' } }, (res) => {
      const code = res.statusCode || 0;
      if ([301, 302, 303, 307, 308].includes(code) && res.headers.location && redirects > 0) {
        res.resume();
        const next = new URL(res.headers.location, url).toString();
        resolve(httpGet(next, redirects - 1));
        return;
      }
      const declared = parseInt(res.headers['content-length'] || '0', 10);
      if (declared > MAX_BODY) {
        res.resume();
        return resolve({ statusCode: 413, headers: res.headers, body: Buffer.alloc(0), tooLarge: true, declared });
      }
      const chunks = [];
      let total = 0;
      res.on('data', (c) => {
        total += c.length;
        if (total > MAX_BODY) {
          res.destroy(new Error('响应体超过 ' + (MAX_BODY / 1048576) + 'MB 上限，已中止'));
          return;
        }
        chunks.push(c);
      });
      res.on('error', (e) => { res.resume(); reject(e); });
      res.on('end', () => resolve({ statusCode: code, headers: res.headers, body: Buffer.concat(chunks) }));
    });
    req.on('error', reject);
    req.setTimeout(90000, () => req.destroy(new Error('请求超时')));
  });
}

// ---------- 流式下载到本地临时文件（视频绝不整体缓冲进内存，防 384MB 上限被撞爆） ----------
function downloadToFile(url, filepath, redirects = 5) {
  return new Promise((resolve, reject) => {
    const mod = pickModule(url);
    const req = mod.get(url, { headers: { 'User-Agent': 'Mozilla/5.0 (compatible; dyasr/1.0)' } }, (res) => {
      const code = res.statusCode || 0;
      if ([301, 302, 303, 307, 308].includes(code) && res.headers.location && redirects > 0) {
        res.resume();
        const next = new URL(res.headers.location, url).toString();
        resolve(downloadToFile(next, filepath, redirects - 1));
        return;
      }
      if (code !== 200) { res.resume(); return resolve({ statusCode: code, headers: res.headers }); }
      const declared = parseInt(res.headers['content-length'] || '0', 10);
      if (declared > MAX_VIDEO_BYTES) {
        res.resume();
        return resolve({ statusCode: 413, headers: res.headers, tooLarge: true, declared });
      }
      let out;
      try { out = fs.createWriteStream(filepath); } catch (e) { return reject(e); }
      let written = 0, aborted = false;
      const cleanup = () => { try { fs.unlinkSync(filepath); } catch (_) {} };
      res.on('data', (c) => {
        if (aborted) return;
        written += c.length;
        if (written > MAX_VIDEO_BYTES) {
          aborted = true;
          res.destroy(new Error('视频超过 ' + (MAX_VIDEO_BYTES / 1048576) + 'MB 上限，已中止下载'));
          out.destroy();
          return;
        }
        out.write(c);
      });
      res.on('error', (e) => { res.resume(); cleanup(); reject(e); });
      out.on('error', (e) => { cleanup(); reject(e); });
      res.on('end', () => {
        if (aborted) { cleanup(); return; }
        out.end(() => resolve({ statusCode: 200, headers: res.headers, size: written }));
      });
    });
    req.on('error', reject);
    req.setTimeout(120000, () => req.destroy(new Error('下载超时')));
  });
}

// 剩余可用磁盘空间（字节）；不可用时返回 -1
function freeBytes(tmpDir) {
  try {
    const st = fs.statfsSync(tmpDir);
    return (st.bavail || 0) * (st.bsize || 1);
  } catch (_) { return -1; }
}

// 清理 /tmp 中本程序遗留的旧临时文件。
// SCF 容器会被复用，崩溃/OOM 的运行可能残留大视频文件占满 /tmp，导致后续连很小的下载都 ENOSPC。
// 本函数在每次请求开头先清一遍，自愈磁盘占满。
// 策略：删除所有 name != 当前请求 token 的 vd_* 条目。SCF 单实例单线程，不存在真正的并发竞争；
// 即使有，误删并发临时文件最多导致该次请求报「文件不存在」而非 ENOSPC 写爆盘，是更安全的取舍。
function purgeStaleTemp(tmpDir, currentToken) {
  try {
    const cur = 'vd_' + currentToken;
    let entries;
    try { entries = fs.readdirSync(tmpDir); } catch (_) { return; }
    for (const name of entries) {
      if (!/^vd_/.test(name) || name === cur) continue;
      try {
        const p = path.join(tmpDir, name);
        const st = fs.statSync(p);
        if (st.isDirectory()) { try { fs.rmdirSync(p, { recursive: true, force: true }); } catch (_) {} }
        else { try { fs.unlinkSync(p); } catch (_) {} }
      } catch (_) {}
    }
  } catch (_) {}
}

// ---------- 会话工作目录（按前端传入的稳定 token 复用，跨步骤保留中间文件） ----------
// 前端逐步驱动「解析→下载→提取→提交」，每步用同一个 token，工作目录里已下载/提取的文件可复用，
// 避免重复下载；若云函数实例被回收（冷启动）导致文件丢失，ensure* 会自动重做对应子步骤（幂等）。
function safeToken(t) {
  t = String(t || '').replace(/[^a-zA-Z0-9_\-]/g, '');
  return t.slice(0, 64);
}
function workDirOf(token) {
  const tmpDir = process.env.TMPDIR || os.tmpdir();
  return path.join(tmpDir, 'vd_' + token);
}
// 确保工作目录里有 video.mp4：已存在则跳过，否则流式下载。返回 { videoPath, size, reused } 或 { warn }
async function ensureVideo(token, videoUrl, title) {
  const workDir = workDirOf(token);
  fs.mkdirSync(workDir, { recursive: true });
  const videoPath = path.join(workDir, 'video.mp4');
  let fsize = 0; try { fsize = fs.statSync(videoPath).size; } catch (_) {}
  if (fsize > 0) return { videoPath, size: fsize, reused: true };

  const dl = await downloadToFile(videoUrl, videoPath);
  if (dl.tooLarge || dl.statusCode === 413) {
    try { fs.unlinkSync(videoPath); } catch (_) {}
    const approx = dl.declared ? (dl.declared / 1048576).toFixed(0) + 'MB' : '过大';
    return { warn: '该视频约 ' + approx + '，超过当前云函数处理上限（' + (MAX_VIDEO_BYTES / 1048576) + 'MB）。视频可下载/预览，但暂无法转文字。' };
  }
  if (dl.statusCode !== 200) { try { fs.unlinkSync(videoPath); } catch (_) {} throw new Error('视频下载失败 HTTP ' + dl.statusCode); }
  fsize = fs.statSync(videoPath).size;
  if (!fsize) { try { fs.unlinkSync(videoPath); } catch (_) {} throw new Error('视频下载为空'); }
  return { videoPath, size: fsize, declared: dl.declared || fsize, reused: false };
}
// 确保工作目录里有 audio.aac：已存在则跳过，否则从 video.mp4 提取。返回 { aacPath, info, reused }
async function ensureAAC(token) {
  const workDir = workDirOf(token);
  fs.mkdirSync(workDir, { recursive: true });
  const videoPath = path.join(workDir, 'video.mp4');
  const aacPath = path.join(workDir, 'audio.aac');
  let asize = 0; try { asize = fs.statSync(aacPath).size; } catch (_) {}
  if (asize > 0) return { aacPath, info: null, reused: true };
  if (!fs.existsSync(videoPath)) throw new Error('未找到 video.mp4，请先执行下载步骤');
  const info = await extractAAC(videoPath, aacPath);
  let outSize = 0; try { outSize = fs.statSync(aacPath).size; } catch (_) {}
  if (!outSize) throw new Error('音频提取失败：未生成 audio.aac（视频封装可能非标准 MP4/AAC）。建议改用 ffmpeg。');
  if (outSize > fs.statSync(videoPath).size * 2) throw new Error('音频提取异常：输出远超输入，封装结构可能非标准。建议改用 ffmpeg。');
  return { aacPath, info, reused: false };
}

// ---------- 腾讯云 API 请求（TC3 签名） ----------
function tcRequest(opts) {
  return new Promise((resolve, reject) => {
    const payloadStr = JSON.stringify(opts.payloadObj);
    const ts = Math.floor(Date.now() / 1000);
    const { authorization, host } = tc3Sign({
      secretId: opts.secretId,
      secretKey: opts.secretKey,
      service: 'asr',
      action: opts.action,
      payloadStr,
      timestamp: ts
    });
    const data = Buffer.from(payloadStr, 'utf8');
    const req = https.request({
      hostname: host,
      path: '/',
      method: 'POST',
      headers: {
        'Content-Type': 'application/json; charset=utf-8',
        'Content-Length': data.length,
        'Authorization': authorization,
        'X-TC-Action': opts.action,
        'X-TC-Version': ASR_VERSION,
        'X-TC-Timestamp': String(ts),
        'X-TC-Region': opts.region || 'ap-guangzhou'
      }
    }, (res) => {
      const chunks = [];
      res.on('data', (c) => chunks.push(c));
      res.on('end', () => resolve({ statusCode: res.statusCode, body: Buffer.concat(chunks).toString('utf8') }));
    });
    req.on('error', reject);
    req.setTimeout(30000, () => req.destroy(new Error('腾讯云请求超时')));
    req.write(data);
    req.end();
  });
}

// 去掉识别结果行首的时间戳前缀 [x:x.xxx,x:x.xxx]（对应 vd_asr.c 的 clean_transcript）
function cleanTranscript(text) {
  if (!text) return '';
  return text.split('\n')
    .map((line) => line.replace(/^\s*\[[\d:.,]+\]\s*/, ''))
    .join('\n').trim();
}

// ---------- 配置解析：前端传入优先，否则用云函数环境变量 ----------
function resolveConfig(body) {
  return {
    secretId: (body.secretId || '').trim() || process.env.SECRET_ID || '',
    secretKey: (body.secretKey || '').trim() || process.env.SECRET_KEY || '',
    region: (body.region || '').trim() || process.env.REGION || 'ap-guangzhou',
    engine: body.engine || process.env.ENGINE || '16k_zh'
  };
}

function parseEvent(event) {
  let b = event && event.body;
  if (typeof b === 'string') {
    try { b = JSON.parse(b); } catch (e) { b = {}; }
  } else if (!b) {
    b = {};
  }
  if (event && event.queryString && typeof event.queryString === 'object') {
    Object.assign(b, event.queryString);
  }
  return b;
}

// ---------- 判断是否为直链媒体文件（对应 vd_asr.c 的 is_direct_media） ----------
function isDirectMedia(url) {
  const low = url.split('?')[0].toLowerCase();
  return /\.(mp4|m3u8|webm|mov|m4v|flv)(\?|$)/i.test(low);
}

// ---------- 从解析结果里挑一条「体积友好、码率合理」的直链（对齐手机快捷指令 ~92MB 1080p） ----------
// 实测 bugpk 对抖音链接的返回：
//   data.url        —— 主直链/原画（可能 1.2GB，一定有音轨，但费流量，仅作最后兜底）
//   data.video_url  —— 部分链接才有：无水印压缩版（约 92MB），有就优先用
//   data.video_backup —— 多清晰度备链（已验证 720p 备链有音轨，体积小更省流量）
// 取值优先级：video_url > 备链里最小分辨率(优先 720p) > 主直链 url(最后兜底) > video
// 返回 { url, res, fmt, label } —— res=分辨率数字(无则9999)，fmt=封装优先级(0 mp4 /1 其他 /2 dash)
function pickVideoUrl(pd, pj) {
  const resOf = (label) => { const m = /(\d{3,4})/i.exec(label || ''); return m ? parseInt(m[1], 10) : 9999; };
  const fmtOf = (label) => { const l = String(label || '').toLowerCase(); return /mp4/.test(l) ? 0 : (/dash/.test(l) ? 2 : 1); };
  const norm = (url, label) => ({ url, res: resOf(label), fmt: fmtOf(label), label: String(label || '') });

  // ① 无水印压缩版（约 92MB，部分链接才有）
  const vu = pd.video_url || pj.video_url;
  if (vu) return norm(vu, 'video_url');

  // ② 备链：mp4 封装优先、分辨率小优先、优先 ≤720p（已验证 720p 备链有音轨）
  const backup = pd.video_backup || pj.video_backup;
  if (Array.isArray(backup) && backup.length) {
    const items = backup.map((it) => {
      if (typeof it === 'string') return norm(it, '');
      return norm(it.url || it.src || '', String(it.label || it.quality || it.type || ''));
    }).filter((x) => x.url);
    if (items.length) {
      const sorted = items.slice().sort((a, b) => (a.fmt - b.fmt) || (a.res - b.res));
      const small = sorted.filter((x) => x.res <= 720);
      return small[0] || sorted[0];
    }
  }

  // ③ 主直链（最可靠、一定有音轨，但可能 1.2GB 原画——最后兜底）
  const u = pd.url || pj.url;
  if (u) return norm(u, 'url(原画)');

  const v = pd.video || pj.video;
  if (v) return norm(v, 'video');
  return null;
}

// ---------- 解析聚合接口返回（对应 vd_asr.c 的解析分支） ----------
// 返回 { ok:true, videoUrl, title } 或 { ok:false, msg, retry?:true }
function classifyParse(text) {
  const body = text.trim();
  if (!body) return { ok: false, msg: '解析接口返回空（上游偶发抽风）', retry: true };
  let pj;
  try { pj = JSON.parse(body); } catch (e) { return { ok: false, msg: '解析接口返回非 JSON', retry: true }; }
  const code = Number(pj.code !== undefined ? pj.code : 200);
  if (code !== 200 && code !== 0) {
    return { ok: false, msg: '解析失败：' + (pj.msg || pj.message || JSON.stringify(pj).slice(0, 120)) };
  }
  const pd = pj.data || {};
  const pv = pickVideoUrl(pd, pj);
  const title = pd.title || pj.title || pd.desc || pj.desc || 'video';
  if (!pv || !pv.url) {
    return { ok: false, msg: '未返回视频直链（可能是图集/直播，或平台暂不支持）' };
  }
  return { ok: true, videoUrl: pv.url, title, meta: { res: pv.res, fmt: pv.fmt, label: pv.label } };
}

// 带重试的解析（吸收 bugpk 偶发空响应）。对应 vd_asr.c「解析失败：网络错误/接口不可用」的前置兜底。
async function parseShortVideo(link) {
  let lastMsg = '';
  for (let attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) await sleep(1000);
    const pr = await httpGet(PARSE_API + encodeURIComponent(link));
    if (pr.statusCode !== 200) { lastMsg = '解析接口 HTTP ' + pr.statusCode; continue; }
    const r = classifyParse(pr.body.toString('utf8'));
    if (r.ok) return r;
    lastMsg = r.msg;
    if (!r.retry) { r.attempts = attempt + 1; return r; } // 接口明确失败，不重试
  }
  return { ok: false, msg: lastMsg || '解析失败（重试 3 次后仍失败）' };
}

// ---------- 步骤一：解析多平台分享链接 + 下载 + 提音频 + 提交 ASR（对齐 vd_asr.c 模式 A） ----------
async function handleProcess(body, reqToken) {
  const cfg = resolveConfig(body);
  if (!cfg.secretId || !cfg.secretKey) return { ok: false, msg: '缺少腾讯云密钥（前端未填且云端未配置环境变量）' };

  const link = (body.link || '').trim();
  if (!link) return { ok: false, msg: '缺少视频分享链接(link)' };

  // ① 解析直链（直链媒体直接下载，跳过解析；否则走多平台聚合接口 + 重试）
  let videoUrl, title;
  if (isDirectMedia(link)) {
    videoUrl = link;
    title = 'video';
  } else {
    const pr = await parseShortVideo(link);
    if (!pr.ok) return { ok: false, msg: pr.msg };
    videoUrl = pr.videoUrl;
    title = pr.title;
  }

  // HLS(m3u8) 流：当前后端仅能处理 MP4/AAC 直链提取音频，故可下载/预览但无法转文字
  if (/\.m3u8(\?|$)/i.test(videoUrl)) {
    return {
      ok: true,
      title,
      videoUrl,
      warn: '该链接返回的是 HLS(m3u8) 流。当前后端只支持 MP4/AAC 直链提取音频，因此只能下载/预览视频，无法转文字（需引入 ffmpeg 才能处理 HLS）。'
    };
  }

  // ② 下载视频（流式写入 /tmp 专属工作目录，绝不把整段视频缓冲进函数内存，防止被平台 kill）
  const tmpDir = process.env.TMPDIR || os.tmpdir();
  // 预检磁盘空间：确保至少有 50MB 可用，否则提前报错而不是下载到一半才 ENOSPC
  const freeBefore = freeBytes(tmpDir);
  if (freeBefore >= 0 && freeBefore < 50 * 1024 * 1024) {
    return { ok: false, msg: '云函数 /tmp 剩余空间仅 ' + (freeBefore / 1048576).toFixed(1) + 'MB，不足以处理视频。请改一下函数配置（如内存大小）触发容器重建以重置 /tmp。' };
  }
  const workDir = path.join(tmpDir, 'vd_' + reqToken);
  fs.mkdirSync(workDir);
  const videoPath = path.join(workDir, 'video.mp4');
  const aacPath = path.join(workDir, 'audio.aac');
  try {
  const dl = await downloadToFile(videoUrl, videoPath);
  if (dl.tooLarge || dl.statusCode === 413) {
    try { fs.unlinkSync(videoPath); } catch (_) {}
    const approx = dl.declared ? (dl.declared / 1048576).toFixed(0) + 'MB' : '过大';
    return {
      ok: true,
      title,
      videoUrl,
      warn: '该视频约 ' + approx + '，超过当前云函数处理上限（' + (MAX_VIDEO_BYTES / 1048576) + 'MB）。视频本身可下载/预览，但暂无法转文字。请在云函数控制台把“内存/磁盘”调大后重试，或改用 ffmpeg 方案。'
    };
  }
  if (dl.statusCode !== 200) { try { fs.unlinkSync(videoPath); } catch (_) {} return { ok: false, msg: '视频下载失败 HTTP ' + dl.statusCode }; }
  let fsize = 0;
  try { fsize = fs.statSync(videoPath).size; } catch (e) {}
  if (!fsize) { try { fs.unlinkSync(videoPath); } catch (_) {} return { ok: false, msg: '视频下载为空' }; }

  // ③ 手写提取 AAC：按帧随机读 MP4 + 边写 .aac 文件（内存只占用一帧，对齐 vd_asr.c 的 mp4_extract_aac）
  let aac;
  try {
    const diagVideoSize = fsize ? (fsize / 1048576).toFixed(1) + 'MB' : '未知';
    aac = await extractAAC(videoPath, aacPath);
    // 提取完成后立即检查输出文件大小，若异常膨胀说明 MP4 结构导致解析出错
    let aacOutSize = 0;
    try { aacOutSize = fs.statSync(aacPath).size; } catch (_) {}
    if (!aacOutSize) {
      // 文件没写出来（提取函数在某些非标准封装下可能静默产出 0 字节），提前报错而不是到 forEachSegment 才 ENOENT
      try { fs.unlinkSync(videoPath); } catch (_) {}
      return { ok: false, msg: '音频提取失败：未生成 audio.aac（视频封装可能非标准 MP4/AAC，手写解析无法处理）。建议改用 ffmpeg 方式。' };
    }
    if (aacOutSize > fsize * 2) {
      // 音频不可能超过视频本身两倍，出现则说明封装结构异常
      try { fs.unlinkSync(aacPath); } catch (_) {}
      return { ok: false, msg: '音频提取异常：输出(' + (aacOutSize / 1048576).toFixed(1) + 'MB)远超输入视频(' + diagVideoSize + ')，该视频的 MP4 封装结构可能非标准（如多音轨/特殊编码），手写解析无法处理。建议改用 ffmpeg 方式。' };
    }
  } catch (e) {
    try { fs.unlinkSync(videoPath); } catch (_) {}
    try { fs.unlinkSync(aacPath); } catch (_) {}
    return { ok: false, msg: '音频提取失败：' + e.message };
  }
  try { fs.unlinkSync(videoPath); } catch (_) {} // 视频文件已无用，立即释放磁盘

  // ④ 按 ADTS 帧边界流式分片，每段单独提交识别（腾讯云“内嵌音频”限制 ≤5MB）
  let aacBytes = 0;
  try { aacBytes = fs.statSync(aacPath).size; } catch (e) {}
  const taskIds = [];
  let segCount = 0;
  try {
    await forEachSegment(aacPath, SEG_MAX, async (seg) => {
      segCount++;
      const r = await tcRequest({
        ...cfg,
        action: 'CreateRecTask',
        payloadObj: {
          EngineModelType: cfg.engine,
          ChannelNum: 1,
          ResTextFormat: 0,
          SourceType: 1,
          DataLen: seg.length,
          Data: seg.toString('base64')
        }
      });
      let resp;
      try { resp = JSON.parse(r.body); } catch (e) { throw new Error('ASR 返回非 JSON'); }
      const response = resp.Response || resp;
      if (response.Error) throw new Error('腾讯云错误：' + response.Error.Code + ' ' + response.Error.Message);
      const data = response.Data || response;
      if (!data.TaskId) throw new Error('第 ' + segCount + ' 段未返回 TaskId：' + JSON.stringify(resp).slice(0, 400));
      taskIds.push(data.TaskId);
    });
  } catch (e) {
    try { fs.unlinkSync(aacPath); } catch (_) {}
    return { ok: false, msg: '提交识别失败：' + e.message };
  }
  try { fs.unlinkSync(aacPath); } catch (_) {} // 音频已送识别，释放磁盘

  if (!taskIds.length) return { ok: false, msg: '音频为空或分片数为 0' };

  return {
    ok: true,
    title,
    videoUrl,
    taskIds,
    segCount,
    frames: aac.frames,
    sampleRate: aac.sampleRate,
    audioBytes: aacBytes
  };
  } finally {
    // 无论成功/失败/异常，都删除本请求的工作目录，避免 /tmp 累积占满磁盘
    try { fs.rmdirSync(workDir, { recursive: true, force: true }); } catch (_) {}
  }
}

// ---------- 代理下载视频（前端跨域 <a download> 会被浏览器忽略，故走云函数返回附件） ----------
async function handleVideo(body) {
  const videoUrl = (body.videoUrl || '').trim();
  const title = (body.title || 'video').replace(/[\\/:"*?<>|]/g, '_').slice(0, 80);
  if (!videoUrl) return { ok: false, msg: '缺少 videoUrl' };

  const video = await httpGet(videoUrl);
  if (video.statusCode !== 200 || video.body.length === 0) {
    return { ok: false, msg: '视频下载失败 HTTP ' + video.statusCode };
  }
  // API 网关对响应包体有上限，过大时回退到“原始链接/长按预览”，避免代理失败
  if (video.body.length > 30 * 1024 * 1024) {
    return { ok: false, msg: '视频约 ' + (video.body.length / 1048576).toFixed(1) + 'MB 过大，代理下载受限；请使用下方“原始链接”或长按预览保存。' };
  }

  const fname = (title || 'video') + '.mp4';
  const enc = encodeURIComponent(fname).replace(/%20/g, ' ');
  return {
    isBase64Encoded: true,
    statusCode: 200,
    headers: {
      'Content-Type': 'video/mp4',
      'Access-Control-Allow-Origin': '*',
      'Content-Disposition': 'attachment; filename="' + fname + '"; filename*=UTF-8\'\'' + enc
    },
    body: video.body.toString('base64')
  };
}

// ---------- 步骤二/三：轮询识别结果（单个 / 合并全部） ----------
async function handleQuery(body) {
  const cfg = resolveConfig(body);
  const taskId = body.taskId;
  if (!taskId) return { ok: false, msg: '缺少 taskId' };

  const r = await tcRequest({ ...cfg, action: 'DescribeTaskStatus', payloadObj: { TaskId: taskId } });
  let resp;
  try { resp = JSON.parse(r.body); } catch (e) { return { ok: false, msg: 'ASR 返回非 JSON' }; }
  const response = resp.Response || resp;
  if (response.Error) return { done: true, ok: false, msg: '腾讯云错误：' + response.Error.Code + ' ' + response.Error.Message };
  const data = response.Data || response;

  if (data.Status === 2 || data.StatusStr === 'success') {
    const text = cleanTranscript(data.Result || '');
    return { done: true, ok: true, status: data.StatusStr || data.Status, transcript: text };
  } else if (data.Status === 3 || data.StatusStr === 'failed') {
    return { done: true, ok: false, msg: '识别失败：' + (data.ErrorMsg || JSON.stringify(data).slice(0, 160)) };
  }
  return { done: false, ok: true, status: data.StatusStr || data.Status };
}

async function handleQueryAll(body) {
  const cfg = resolveConfig(body);
  const taskIds = Array.isArray(body.taskIds) ? body.taskIds : [];
  if (!taskIds.length) return { done: true, ok: false, msg: '缺少 taskIds' };

  const results = new Array(taskIds.length);
  let doneCount = 0;
  for (let i = 0; i < taskIds.length; i++) {
    const r = await tcRequest({ ...cfg, action: 'DescribeTaskStatus', payloadObj: { TaskId: taskIds[i] } });
    let resp;
    try { resp = JSON.parse(r.body); } catch (e) { return { done: true, ok: false, msg: 'ASR 返回非 JSON' }; }
    const response = resp.Response || resp;
    if (response.Error) return { done: true, ok: false, msg: '腾讯云错误：' + response.Error.Code + ' ' + response.Error.Message };
    const data = response.Data || response;
    if (data.Status === 2 || data.StatusStr === 'success') {
      results[i] = cleanTranscript(data.Result || '');
      doneCount++;
    } else if (data.Status === 3 || data.StatusStr === 'failed') {
      return { done: true, ok: false, msg: '第 ' + (i + 1) + ' 段识别失败：' + (data.ErrorMsg || JSON.stringify(data).slice(0, 160)) };
    }
  }
  if (doneCount < taskIds.length) {
    return { done: false, ok: true, progress: doneCount + '/' + taskIds.length };
  }
  return { done: true, ok: true, status: 'success', transcript: results.join('\n\n') };
}

// ---------- 步骤拆分：解析 / 下载 / 提取 / 提交，前端逐步驱动，后端每步返回进度 ----------
// 前端用同一个 token 依次调用，进度与后端真实执行位置一一对应，不再出现「前端卡在①、后端已跑到④」。
async function handleParse(body) {
  const link = (body.link || '').trim();
  if (!link) return { ok: false, msg: '缺少视频分享链接(link)' };

  if (isDirectMedia(link)) {
    const ext = link.split('?')[0].toLowerCase().match(/\.(\w+)(\?|$)/);
    const meta = { res: '直链', fmt: ext ? ext[1] : '' };
    if (/\.m3u8(\?|$)/i.test(link)) {
      return { ok: true, videoUrl: link, title: 'video', meta, warn: '该链接是 HLS(m3u8) 流，当前仅支持 MP4/AAC 直链提取音频，只能下载/预览、无法转文字。' };
    }
    return { ok: true, videoUrl: link, title: 'video', meta };
  }

  const pr = await parseShortVideo(link);
  if (!pr.ok) return { ok: false, msg: pr.msg };
  return { ok: true, videoUrl: pr.videoUrl, title: pr.title, meta: pr.meta || {} };
}

async function handleDownload(body, token) {
  const videoUrl = (body.videoUrl || '').trim();
  const title = body.title || 'video';
  if (!videoUrl) return { ok: false, msg: '缺少 videoUrl' };
  let r;
  try { r = await ensureVideo(token, videoUrl, title); }
  catch (e) { return { ok: false, msg: '下载失败：' + e.message }; }
  if (r.warn) return { ok: true, videoUrl, title, warn: r.warn };
  return { ok: true, videoUrl, title, size: r.size, declared: r.declared, reused: !!r.reused };
}

async function handleExtract(body, token) {
  const videoUrl = (body.videoUrl || '').trim();
  if (videoUrl) { try { await ensureVideo(token, videoUrl, body.title || 'video'); } catch (e) { return { ok: false, msg: '下载失败：' + e.message }; } }
  let r;
  try { r = await ensureAAC(token); }
  catch (e) { return { ok: false, msg: '音频提取失败：' + e.message }; }
  const aacBytes = fs.statSync(r.aacPath).size;
  const info = r.info || {};
  return { ok: true, frames: info.frames, sampleRate: info.sampleRate, audioBytes: aacBytes, reused: !!r.reused };
}

async function handleSubmit(body, token) {
  const cfg = resolveConfig(body);
  if (!cfg.secretId || !cfg.secretKey) return { ok: false, msg: '缺少腾讯云密钥（前端未填且云端未配置环境变量）' };
  const videoUrl = (body.videoUrl || '').trim();
  if (videoUrl) { try { await ensureVideo(token, videoUrl, body.title || 'video'); } catch (e) { return { ok: false, msg: '下载失败：' + e.message }; } }
  let r;
  try { r = await ensureAAC(token); }
  catch (e) { return { ok: false, msg: '音频提取失败：' + e.message }; }
  const aacPath = r.aacPath;
  let audioBytes = 0; try { audioBytes = fs.statSync(aacPath).size; } catch (e) {}

  const taskIds = []; let segCount = 0;
  try {
    await forEachSegment(aacPath, SEG_MAX, async (seg) => {
      segCount++;
      const rr = await tcRequest({
        ...cfg, action: 'CreateRecTask',
        payloadObj: {
          EngineModelType: cfg.engine, ChannelNum: 1, ResTextFormat: 0,
          SourceType: 1, DataLen: seg.length, Data: seg.toString('base64')
        }
      });
      let resp; try { resp = JSON.parse(rr.body); } catch (e) { throw new Error('ASR 返回非 JSON'); }
      const response = resp.Response || resp;
      if (response.Error) throw new Error('腾讯云错误：' + response.Error.Code + ' ' + response.Error.Message);
      const data = response.Data || response;
      if (!data.TaskId) throw new Error('第 ' + segCount + ' 段未返回 TaskId：' + JSON.stringify(resp).slice(0, 400));
      taskIds.push(data.TaskId);
    });
  } catch (e) { return { ok: false, msg: '提交识别失败：' + e.message }; }
  if (!taskIds.length) return { ok: false, msg: '音频为空或分片数为 0' };

  // 提交完成即释放工作目录（识别结果已存于腾讯云，不再需要本地文件）
  try { fs.rmdirSync(workDirOf(token), { recursive: true, force: true }); } catch (_) {}
  const info = r.info || {};
  return { ok: true, taskIds, segCount, audioBytes, frames: info.frames, sampleRate: info.sampleRate };
}

// ---------- 统一响应格式（适配函数 URL / API 网关协议） ----------
function httpResponse(obj, statusCode = 200) {
  if (obj && obj.isBase64Encoded) {
    const headers = { ...(obj.headers || {}) };
    headers['Access-Control-Allow-Origin'] = headers['Access-Control-Allow-Origin'] || '*';
    return { ...obj, headers };
  }
  return {
    isBase64Encoded: false,
    statusCode,
    headers: {
      'Content-Type': 'application/json; charset=utf-8',
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Headers': 'Content-Type',
      'Access-Control-Allow-Methods': 'POST, OPTIONS',
      'Cache-Control': 'no-store'
    },
    body: JSON.stringify(obj)
  };
}

function servePage() {
  try {
    const html = fs.readFileSync(path.join(__dirname, 'index.html'), 'utf8');
    return {
      statusCode: 200,
      headers: {
        'Content-Type': 'text/html; charset=utf-8',
        'Access-Control-Allow-Origin': '*',
        'Cache-Control': 'no-store'
      },
      body: html
    };
  } catch (e) {
    return httpResponse({ ok: false, msg: '未找到网页文件：' + e.message }, 500);
  }
}

// ---------- 入口 ----------
exports.main_handler = async (event, context) => {
  try {
    const tmpDir = process.env.TMPDIR || os.tmpdir();
    // 先清理 /tmp 里本程序遗留的旧临时文件（按会话 token 精准删除，不依赖时间阈值），
    // 防止磁盘被之前崩溃/失败的运行残留的大文件占满，导致后续连很小的下载都 ENOSPC。
    // 会话 token：前端分步调用时传入同一个 token，purge 会保留它对应的工作目录，跨步骤复用中间文件。
    const reqToken = Date.now() + '_' + Math.random().toString(36).slice(2, 8);
    if (event && event.httpMethod === 'OPTIONS') {
      return httpResponse({ ok: true });
    }
    const body = parseEvent(event);
    // 会话 token：前端分步调用时传入同一个 token，purge 会保留它对应的工作目录，跨步骤复用中间文件。
    const sessionToken = safeToken(body.token) || reqToken;
    purgeStaleTemp(tmpDir, sessionToken);
    // 浏览器直接打开函数 URL（GET 且无 action）→ 返回网页本体，实现「一个网址既是网页又是接口」
    if ((event && event.httpMethod === 'GET') && (!body.action || body.action === 'page' || body.action === 'index')) {
      return servePage();
    }
    const action = body.action;
    if (action === 'process') return httpResponse(await handleProcess(body, sessionToken));
    if (action === 'parse') return httpResponse(await handleParse(body));
    if (action === 'download') return httpResponse(await handleDownload(body, sessionToken));
    if (action === 'extract') return httpResponse(await handleExtract(body, sessionToken));
    if (action === 'submit') return httpResponse(await handleSubmit(body, sessionToken));
    if (action === 'query') return httpResponse(await handleQuery(body));
    if (action === 'queryAll') return httpResponse(await handleQueryAll(body));
    if (action === 'video') return httpResponse(await handleVideo(body));
    return httpResponse({ ok: false, msg: '未知 action：' + action + '（应为 process / query / queryAll / video）' }, 400);
  } catch (e) {
    if (e && e.code === 'ENOSPC') {
      return httpResponse({ ok: false, msg: '云函数 /tmp 磁盘写满（ENOSPC）。本请求开头已清理掉上次运行遗留的所有临时文件；若仍报此错，说明云函数「临时磁盘」配额本身过小（/tmp 大小通常等于你设置的内存大小）。请：①在控制台把“内存/临时磁盘”调大；②或直接改一下任意配置（如内存大小）触发容器重建以重置 /tmp；③实在不行可临时把单视频上限调小。' }, 500);
    }
    return httpResponse({ ok: false, msg: e.message || String(e) }, 500);
  }
};

// 供本地测试复用（SCF 入口仍是 main_handler，以下导出不影响线上逻辑）
exports.classifyParse = classifyParse;
exports.pickVideoUrl = pickVideoUrl;
exports.downloadToFile = downloadToFile;
