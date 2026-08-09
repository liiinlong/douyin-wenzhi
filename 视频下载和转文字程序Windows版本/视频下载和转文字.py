# -*- coding: utf-8 -*-
"""
多平台视频 -> 音频 -> 腾讯云录音转文字（ASR）  图形界面版  纯 Python 标准库

三大功能（与 C 版一一对应）：
  模式 A「在线视频转文字」：调用第三方「聚合解析」接口，自动识别 20+ 平台
       （抖音/快手/小红书/B站/油管/TikTok/西瓜/微博/知乎/A站…），无需手动选平台 ->
       解析直链 -> 下载 -> 提取 AAC -> 腾讯云 ASR -> 结果。
  模式 B「本地文件转文字」：选择本机视频(MP4)或音频(mp3/wav/m4a/aac…)文件，
       直接调用腾讯云 ASR 转成文字。
  模式 C「仅下载视频」：只把在线视频下载到本地，不做转文字。

依赖：仅 Python 3 标准库（tkinter / urllib / hashlib / hmac / base64 / json），
     无第三方包、无 ffmpeg。MP4 解复用与 ADTS 打包为纯字节实现。

运行：
  python 视频下载和转文字.py

打包为单文件 exe（体积约 10MB 级，无第三方依赖）：
  pyinstaller --onefile --noconsole --name 视频下载和转文字 视频下载和转文字.py
"""

import base64
import hashlib
import hmac
import json
import os
import queue
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

# ============================ 常量 ============================

BUF_SIZE = 65536

MODE_ONLINE_TEXT = 1   # 在线视频转文字
MODE_LOCAL_TEXT = 2    # 本地文件转文字
MODE_DOWNLOAD = 3      # 仅下载视频

# 第三方「聚合解析」接口：自动识别 20+ 平台，无需手动选择平台，直接把分享链接丢给它即可。
# 可按需替换为自建/其它多平台解析服务。
PARSE_BASE = "https://api.bugpk.com/api/short_videos?url="

# 腾讯云录音文件识别（TC3 签名）
ASR_HOST = "asr.tencentcloudapi.com"
ASR_SERVICE = "asr"
ASR_VERSION = "2019-06-14"

# 腾讯云录音文件识别（SourceType=1 直传 Data）单文件上限 5MB，超出需分片。
# 分片必须在音频帧边界切，否则解码失败；这里按 ADTS AAC / MP3 帧、或 WAV 重建头。
ASR_MAX_CHUNK = 4 * 1024 * 1024    # 4MB，留安全余量（< 5MB）
ASR_MAX_CHUNKS = 64

USER_AGENT = "vd_asr/1.0"


# ============================ 文本 / 文件名 工具 ============================

def extract_url(text):
    """从一段文字中提取 http 开头的链接；没有 http 则返回原文（去首尾空白）。"""
    i = text.find("http")
    if i == -1:
        return text.strip()
    out = []
    for ch in text[i:]:
        if ch in ' \r\n\t"\'':
            break
        out.append(ch)
    return "".join(out)


def clean_transcript(raw):
    """去掉识别结果中的 [xxx] 区间标记（如 [00:00:01]），并吞掉其后的空格。"""
    out = []
    i, n = 0, len(raw)
    while i < n:
        if raw[i] == "[":
            j = raw.find("]", i)
            if j != -1:
                i = j + 1
                while i < n and raw[i] == " ":
                    i += 1
                continue
        out.append(raw[i])
        i += 1
    return "".join(out)


def is_direct_media(url):
    """判断是否为直链媒体文件（可直接下载，自动判断，无需手动选平台）。"""
    low = url.lower().split("?")[0]
    return any(ext in low for ext in (".mp4", ".m3u8", ".webm", ".mov", ".m4v", ".flv"))


_BAD_FN_CHARS = set('\\/:*?"<>|\r\n\t')


def sanitize_filename(s):
    """清理文件名中的非法字符（只删 Windows 禁用字符与控制字符）。"""
    s = "".join(ch for ch in s if ch not in _BAD_FN_CHARS and ord(ch) >= 0x20)
    return s.rstrip(" .")


def make_filename_from_url(url):
    """从直链推导文件名。"""
    path = url.split("?")[0]
    name = path.rsplit("/", 1)[-1] or "video"
    name = sanitize_filename(name) or "video"
    if "." not in name:
        name += ".mp4"
    return name


def is_audio_ext(ext):
    """是否为常见可直接送 ASR 的音频扩展名。"""
    return ext.lower() in ("mp3", "wav", "m4a", "aac", "amr", "flac", "wma", "ogg")


def is_mp4_video_ext(ext):
    """是否为可用 MP4 解复用提取音轨的视频扩展名。"""
    return ext.lower() in ("mp4", "m4v", "mov")


# ============================ 配置（读/写 ini） ============================

def app_dir():
    """程序所在目录（兼容 PyInstaller 打包后的 exe）。"""
    if getattr(sys, "frozen", False):
        return os.path.dirname(os.path.abspath(sys.executable))
    return os.path.dirname(os.path.abspath(__file__))


def load_config(ini_path):
    cfg = {"secret_id": "", "secret_key": "", "region": "ap-guangzhou", "engine": "16k_zh"}
    try:
        with open(ini_path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or line.startswith(";") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                k, v = k.strip().lower(), v.strip()
                if k == "secretid":
                    cfg["secret_id"] = v
                elif k == "secretkey":
                    cfg["secret_key"] = v
                elif k == "region":
                    cfg["region"] = v
                elif k == "enginemodeltype":
                    cfg["engine"] = v
    except OSError:
        pass
    return cfg


def save_config(ini_path, cfg):
    """保存配置到 ini（UTF-8）。成功返回 True。"""
    content = ("# 腾讯云语音识别（ASR）配置——由程序自动保存\r\n"
               "SecretId=%s\r\n"
               "SecretKey=%s\r\n"
               "Region=%s\r\n"
               "EngineModelType=%s\r\n"
               % (cfg["secret_id"], cfg["secret_key"], cfg["region"], cfg["engine"]))
    try:
        with open(ini_path, "w", encoding="utf-8", newline="") as f:
            f.write(content)
        return True
    except OSError:
        return False


# ============================ HTTP（urllib） ============================

def http_get(url, timeout=30):
    """GET 请求，返回响应字节；失败抛异常。"""
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def http_download_file(url, save_path, timeout=30):
    """下载文件到本地，返回已下载字节数（0 表示失败）。"""
    total = 0
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp, \
             open(save_path, "wb") as f:
            while True:
                buf = resp.read(BUF_SIZE)
                if not buf:
                    break
                f.write(buf)
                total += len(buf)
    except Exception:
        pass
    return total


def https_post_json(host, path, headers, body_bytes, timeout=60):
    """POST JSON，返回响应字节；失败抛异常。"""
    req = urllib.request.Request("https://%s%s" % (host, path), data=body_bytes,
                                 headers=headers, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


# ============================ 腾讯云 TC3 签名调用 ============================

def tc3_call(cfg, action, payload):
    """发起一次 TC3-HMAC-SHA256 签名调用，payload 为 JSON 字符串，返回响应文本。"""
    body = payload.encode("utf-8")
    ts = int(time.time())
    date = time.strftime("%Y-%m-%d", time.gmtime(ts))

    hashed_payload = hashlib.sha256(body).hexdigest()
    canonical = ("POST\n/\n\ncontent-type:application/json; charset=utf-8\n"
                 "host:%s\n\ncontent-type;host\n%s" % (ASR_HOST, hashed_payload))
    hashed_canonical = hashlib.sha256(canonical.encode("utf-8")).hexdigest()

    cred_scope = "%s/%s/tc3_request" % (date, ASR_SERVICE)
    string_to_sign = "TC3-HMAC-SHA256\n%d\n%s\n%s" % (ts, cred_scope, hashed_canonical)

    def _hm(key, msg):
        return hmac.new(key, msg.encode("utf-8"), hashlib.sha256).digest()

    k_date = _hm(("TC3" + cfg["secret_key"]).encode("utf-8"), date)
    k_service = _hm(k_date, ASR_SERVICE)
    k_signing = _hm(k_service, "tc3_request")
    signature = hmac.new(k_signing, string_to_sign.encode("utf-8"),
                         hashlib.sha256).hexdigest()

    authorization = ("TC3-HMAC-SHA256 Credential=%s/%s, "
                     "SignedHeaders=content-type;host, Signature=%s"
                     % (cfg["secret_id"], cred_scope, signature))
    headers = {
        "Content-Type": "application/json; charset=utf-8",
        "Authorization": authorization,
        "X-TC-Action": action,
        "X-TC-Timestamp": str(ts),
        "X-TC-Version": ASR_VERSION,
        "X-TC-Region": cfg["region"],
    }
    resp = https_post_json(ASR_HOST, "/", headers, body, timeout=60)
    return resp.decode("utf-8", "replace")


def _find_key(obj, key):
    """在嵌套 dict/list 中递归查找第一个同名 key 的值。"""
    if isinstance(obj, dict):
        if key in obj:
            return obj[key]
        for v in obj.values():
            r = _find_key(v, key)
            if r is not None:
                return r
    elif isinstance(obj, list):
        for v in obj:
            r = _find_key(v, key)
            if r is not None:
                return r
    return None


# ============================ MP4 解复用：提取 AAC 音轨 -> ADTS(.aac) ============================

AAC_SR_TABLE = (96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
                16000, 12000, 11025, 8000, 7350, 0, 0, 0)


def _rd_be32(b, o):
    return int.from_bytes(b[o:o + 4], "big")


def _rd_be64(b, o):
    return int.from_bytes(b[o:o + 8], "big")


def _iter_boxes(data, start, size):
    """迭代 data[start:start+size] 范围内的 MP4 box，产出 (type4_bytes, content_off, content_len)。"""
    off = start
    end = start + size
    while off + 8 <= end:
        bsize = _rd_be32(data, off)
        btype = data[off + 4:off + 8]
        hdr = 8
        if bsize == 1:
            if off + 16 > end:
                break
            bsize = _rd_be64(data, off + 8)
            hdr = 16
        elif bsize == 0:
            bsize = end - off
        if bsize < hdr or off + bsize > end:
            break
        yield btype, off + hdr, bsize - hdr
        off += bsize


def _find_box(data, start, size, type4):
    """在范围内找第一个指定类型的 box，返回 (content_off, content_len) 或 None。"""
    for btype, coff, clen in _iter_boxes(data, start, size):
        if btype == type4:
            return coff, clen
    return None


def _parse_esds(esds):
    """解析 esds 中的 AudioSpecificConfig，返回 (objType, freqIdx, chan) 或 None。"""
    i, n = 4, len(esds)
    while i + 1 < n:
        tag = esds[i]
        if tag == 0x05:
            j = i + 1
            dlen = 0
            cnt = 0
            while j < n and cnt < 4:
                b = esds[j]
                j += 1
                dlen = (dlen << 7) | (b & 0x7F)
                cnt += 1
                if not (b & 0x80):
                    break
            if j + 2 <= n and dlen >= 2:
                a0, a1 = esds[j], esds[j + 1]
                ot = (a0 >> 3) & 0x1F
                fi = ((a0 & 0x07) << 1) | ((a1 >> 7) & 0x01)
                ch = (a1 >> 3) & 0x0F
                return ot, fi, ch
            return None
        i += 1
    return None


def _adts_header(profile, freq_idx, chan, frame_len):
    """生成 7 字节 ADTS 头。"""
    aac_frame_length = frame_len + 7
    return bytes((
        0xFF,
        0xF1,
        ((profile & 0x3) << 6) | ((freq_idx & 0xF) << 2) | ((chan >> 2) & 0x1),
        ((chan & 0x3) << 6) | ((aac_frame_length >> 11) & 0x3),
        (aac_frame_length >> 3) & 0xFF,
        ((aac_frame_length & 0x7) << 5) | 0x1F,
        0xFC,
    ))


def mp4_extract_aac(data, out_path, log):
    """从 MP4 字节中提取 AAC 音轨写成 ADTS 文件。成功返回采样率（>0），失败返回 0。"""
    r = _find_box(data, 0, len(data), b"moov")
    if not r:
        log("  [MP4] 未找到 moov box")
        return 0
    moov_off, moov_len = r

    # 找音频轨（hdlr 为 soun）的 stbl
    audio_stbl = None
    for btype, coff, clen in _iter_boxes(data, moov_off, moov_len):
        if btype != b"trak":
            continue
        r = _find_box(data, coff, clen, b"mdia")
        if not r:
            continue
        mdia_off, mdia_len = r
        r = _find_box(data, mdia_off, mdia_len, b"hdlr")
        if not r or r[1] < 12:
            continue
        if data[r[0] + 8:r[0] + 12] != b"soun":
            continue
        r = _find_box(data, mdia_off, mdia_len, b"minf")
        if not r:
            continue
        r = _find_box(data, r[0], r[1], b"stbl")
        if not r:
            continue
        audio_stbl = r
        break
    if not audio_stbl:
        log("  [MP4] 未找到音频轨(soun)")
        return 0
    stbl_off, stbl_len = audio_stbl

    # stsd / esds：AAC 参数（对象类型、采样率索引、声道数）
    r = _find_box(data, stbl_off, stbl_len, b"stsd")
    if not r or r[1] < 8:
        log("  [MP4] 缺 stsd")
        return 0
    stsd_off, stsd_len = r
    esds = _find_box(data, stsd_off + 8, stsd_len - 8, b"esds")
    if not esds:
        seg = data[stsd_off:stsd_off + stsd_len]
        k = seg.find(b"esds")
        if k != -1:
            esds = (stsd_off + k + 4, stsd_len - (k + 4))

    obj_type, freq_idx, chan = 2, 4, 2
    parsed = _parse_esds(data[esds[0]:esds[0] + esds[1]]) if esds else None
    if parsed:
        obj_type, freq_idx, chan = parsed
    else:
        log("  [MP4] 未解析到 esds/ASC，使用默认 AAC-LC 参数")
    if freq_idx < 0 or freq_idx > 12:
        freq_idx = 4
    profile = obj_type - 1
    if profile < 0:
        profile = 1

    # stsz：sample 尺寸表
    r = _find_box(data, stbl_off, stbl_len, b"stsz")
    if not r or r[1] < 12:
        log("  [MP4] 缺 stsz")
        return 0
    stsz_off = r[0]
    fixed_size = _rd_be32(data, stsz_off + 4)
    sample_count = _rd_be32(data, stsz_off + 8)
    stsz_table = stsz_off + 12

    # stsc：sample -> chunk 映射
    r = _find_box(data, stbl_off, stbl_len, b"stsc")
    if not r or r[1] < 8:
        log("  [MP4] 缺 stsc")
        return 0
    stsc_off = r[0]
    stsc_count = _rd_be32(data, stsc_off + 4)
    stsc_table = stsc_off + 8

    # stco / co64：chunk 偏移表
    r = _find_box(data, stbl_off, stbl_len, b"stco")
    is64 = False
    if not r:
        r = _find_box(data, stbl_off, stbl_len, b"co64")
        is64 = True
    if not r or r[1] < 8:
        log("  [MP4] 缺 stco/co64")
        return 0
    stco_off = r[0]
    chunk_count = _rd_be32(data, stco_off + 4)
    stco_table = stco_off + 8

    try:
        f = open(out_path, "wb")
    except OSError:
        log("  [MP4] 无法创建输出音频文件")
        return 0

    sample_idx = 0
    wrote = 0
    fsize = len(data)
    with f:
        for c in range(chunk_count):
            if sample_idx >= sample_count:
                break
            spc = 1
            for e in range(stsc_count):
                first_chunk = _rd_be32(data, stsc_table + e * 12)
                samples_per = _rd_be32(data, stsc_table + e * 12 + 4)
                if first_chunk <= c + 1:
                    spc = samples_per
                else:
                    break
            chunk_off = (_rd_be64(data, stco_table + c * 8) if is64
                         else _rd_be32(data, stco_table + c * 4))
            pos = chunk_off
            for _s in range(spc):
                if sample_idx >= sample_count:
                    break
                ssize = fixed_size if fixed_size else _rd_be32(data, stsz_table + sample_idx * 4)
                if pos + ssize > fsize:
                    sample_idx = sample_count
                    break
                f.write(_adts_header(profile, freq_idx, chan, ssize))
                f.write(data[pos:pos + ssize])
                wrote += 7 + ssize
                pos += ssize
                sample_idx += 1

    log("  [MP4] 提取 %d 帧 AAC，%dHz，%d 声道，%.2f MB"
        % (sample_idx, AAC_SR_TABLE[freq_idx], chan, wrote / 1048576.0))
    return AAC_SR_TABLE[freq_idx] if sample_idx > 0 else 0


# ============================ 大音频自动分片 ============================

def detect_audio_fmt(d):
    """由音频字节特征判断格式：'aac'(ADTS) / 'mp3' / 'wav' / 'unknown'。"""
    if len(d) >= 2 and d[0] == 0xFF and (d[1] & 0xF0) == 0xF0:
        # 同步字 0xFFF：ADTS 的 layer 位(比特2:1)==00，MP3 的 layer 位非 00
        return "aac" if (d[1] & 0x06) == 0x00 else "mp3"
    if len(d) >= 12 and d[:4] == b"RIFF" and d[8:12] == b"WAVE":
        return "wav"
    return "unknown"


def _find_next_mp3_sync(d, pos, length):
    """在 [pos+1, length) 内找下一个 MP3 帧同步字偏移；找不到返回 length。"""
    for i in range(pos + 1, length - 1):
        if d[i] == 0xFF and (d[i + 1] & 0xE0) == 0xE0 and (d[i + 1] & 0x06) != 0x00:
            return i
    return length


def split_audio(data, fmt):
    """把音频切成每块 <= ASR_MAX_CHUNK 的分片（list[bytes]）。
    WAV 会为每个分片重建 WAV 头；无法识别格式则整文件作为 1 个分片。"""
    n = len(data)
    chunks = []
    if fmt == "wav":
        p = 12
        data_off = 0
        pcm_len = 0
        while p + 8 <= n:
            ck_size = int.from_bytes(data[p + 4:p + 8], "little")
            if data[p:p + 4] == b"data":
                data_off = p + 8
                pcm_len = ck_size
                break
            p += 8 + ck_size + (ck_size & 1)
        if data_off == 0 or pcm_len == 0 or data_off + pcm_len > n:
            return [data]                      # 退化：整文件
        per = ASR_MAX_CHUNK
        nchunks = min((pcm_len + per - 1) // per, ASR_MAX_CHUNKS)
        for i in range(nchunks):
            start = i * per
            clen = pcm_len - start if i == nchunks - 1 else per
            hdr = bytearray(data[:data_off])   # 复用原始 WAV 头（含 fmt）
            riff = 36 + clen                   # 修正 RIFF 大小与 data 大小
            hdr[4:8] = riff.to_bytes(4, "little")
            hdr[data_off - 4:data_off] = clen.to_bytes(4, "little")
            chunks.append(bytes(hdr) + data[data_off + start:data_off + start + clen])
        return chunks

    # 帧格式（ADTS / MP3 / 未知整文件）：按帧边界累积成 <= ASR_MAX_CHUNK 的分片
    pos = 0
    seg_start = 0
    seg_len = 0
    while pos < n and len(chunks) < ASR_MAX_CHUNKS:
        if fmt == "aac":
            if pos + 7 > n:
                fend = n
            else:
                fl = (((data[pos + 3] & 0x03) << 11)
                      | (data[pos + 4] << 3)
                      | ((data[pos + 5] >> 5) & 0x07))
                if fl < 7:
                    fl = 7
                fend = pos + fl
        elif fmt == "mp3":
            fend = _find_next_mp3_sync(data, pos, n)
        else:
            fend = n                           # 未知：整文件一帧
        if fend <= pos or fend > n:
            fend = n
        frame_len = fend - pos
        if seg_len > 0 and seg_len + frame_len > ASR_MAX_CHUNK:
            chunks.append(data[seg_start:seg_start + seg_len])
            seg_start = pos
            seg_len = 0
        if seg_len == 0:
            seg_start = pos
        seg_len += frame_len
        pos = fend
    if seg_len > 0 and len(chunks) < ASR_MAX_CHUNKS:
        chunks.append(data[seg_start:seg_start + seg_len])
    if not chunks:                             # 兜底
        chunks.append(data)
    return chunks


# ============================ ASR 公共管线（数据 -> 文字） ============================

def do_asr(cfg, data, log):
    """提交一段音频到腾讯云录音文件识别并轮询结果。成功返回文字，失败返回 None。"""
    log("  提交音频到腾讯云（%d 字节）…" % len(data))
    b64 = base64.b64encode(data).decode("ascii")
    payload = json.dumps({
        "EngineModelType": cfg["engine"],
        "ChannelNum": 1,
        "ResTextFormat": 0,
        "SourceType": 1,
        "DataLen": len(data),
        "Data": b64,
    }, separators=(",", ":"))

    try:
        r1 = tc3_call(cfg, "CreateRecTask", payload)
    except Exception as e:
        log("提交识别任务失败：网络或签名错误（%s）。" % e)
        return None
    try:
        j1 = json.loads(r1)
    except ValueError:
        log("  接口返回非 JSON：%.400s" % r1)
        return None

    err = _find_key(j1, "Error")
    if isinstance(err, dict) and err.get("Code"):
        log("  接口返回错误：%s - %s" % (err.get("Code"), err.get("Message") or ""))
        return None
    task_id = _find_key(j1, "TaskId")
    if not task_id:
        log("  未取得 TaskId。返回：%.400s" % r1)
        return None
    log("  任务已提交，TaskId=%s，等待识别结果…" % task_id)

    for attempt in range(1, 61):
        time.sleep(3)
        try:
            r2 = tc3_call(cfg, "DescribeTaskStatus",
                          json.dumps({"TaskId": task_id}, separators=(",", ":")))
            j2 = json.loads(r2)
        except Exception:
            log("  第 %d 次查询失败，重试…" % attempt)
            continue
        status = _find_key(j2, "Status")
        if status == 2:
            raw = _find_key(j2, "Result") or ""
            log("  识别完成！")
            return clean_transcript(raw)
        if status == 3:
            em = _find_key(j2, "ErrorMsg") or "(无详情)"
            log("  识别失败：%s" % em)
            return None
        log("  识别中…（第 %d 次轮询，状态=%s）" % (attempt, status))
    log("  识别超时（3 分钟未完成）。")
    return None


def do_asr_auto(cfg, data, log):
    """入口：自动处理 > 5MB 的大音频 —— 切片分别识别后合并文字。"""
    if len(data) <= ASR_MAX_CHUNK:
        return do_asr(cfg, data, log)
    log("  音频 %.2fMB 超过 5MB 限制，将自动分片识别后合并…" % (len(data) / 1048576.0))
    fmt = detect_audio_fmt(data)
    if fmt == "unknown":
        log("  当前音频格式暂不支持自动分片（仅 ADTS AAC / MP3 / WAV）。"
            "尝试整段提交；若腾讯云拒绝，请先转码为 mp3 或 aac 再试。")
        return do_asr(cfg, data, log)
    chunks = split_audio(data, fmt)
    log("  共分为 %d 个分片（每块 < 5MB），逐片识别并合并。" % len(chunks))
    parts = []
    for i, ck in enumerate(chunks):
        log("  [分片 %d/%d] 大小 %d 字节" % (i + 1, len(chunks), len(ck)))
        t = do_asr(cfg, ck, log)
        if t is None:
            log("  分片 %d 识别失败，终止合并。" % (i + 1))
            return None
        parts.append(t)
    log("  分片识别完成，已合并为完整文字。")
    return "\n".join(parts)


def save_transcript_file(txt_path, transcript, log):
    """把 transcript 存为带 BOM 的 UTF-8 文本。"""
    try:
        with open(txt_path, "w", encoding="utf-8-sig", newline="") as f:
            f.write(transcript)
        log("")
        log("已保存文字 -> %s" % txt_path)
    except OSError as e:
        log("保存文字文件失败：%s" % e)


# ============================ 模式 B：本地文件 -> 文字 ============================

def local_asr_pipeline(wp, log, set_result):
    cfg = wp["cfg"]
    log("本地文件 → 腾讯云录音转文字")
    log("引擎模型：%s   地域：%s" % (cfg["engine"], cfg["region"]))
    log("本地文件：%s" % wp["local_path"])

    ext = os.path.splitext(wp["local_path"])[1].lstrip(".").lower()
    base = os.path.splitext(os.path.basename(wp["local_path"]))[0]
    audio_path = os.path.join(wp["save_dir"], base + "_audio.aac")
    txt_path = os.path.join(wp["save_dir"], base + "_transcript.txt")

    if is_audio_ext(ext):
        log("")
        log("[1/2] 读取本地音频文件…")
        try:
            with open(wp["local_path"], "rb") as f:
                data = f.read()
        except OSError:
            log("读取文件失败。")
            return
        if not data:
            log("读取文件失败。")
            return
        log("  音频 %d 字节，扩展名 .%s" % (len(data), ext))
    elif is_mp4_video_ext(ext):
        log("")
        log("[1/2] 读取本地视频并提取 AAC 音频…")
        try:
            with open(wp["local_path"], "rb") as f:
                mp4 = f.read()
        except OSError:
            log("读取视频文件失败。")
            return
        if not mp4_extract_aac(mp4, audio_path, log):
            log("音频提取失败（可能不是标准 MP4/AAC 封装）。")
            return
        try:
            with open(audio_path, "rb") as f:
                data = f.read()
        except OSError:
            log("读取提取出的音频失败。")
            return
        if not data:
            log("读取提取出的音频失败。")
            return
    else:
        log("")
        log("不支持的格式：. %s" % (ext or "(未知)"))
        log("本地转文字仅支持：MP4/M4V/MOV 视频，以及 mp3/wav/m4a/aac/amr/flac/wma/ogg 音频。")
        log("其它格式请先用工具转码后再试。")
        return

    log("")
    log("[2/2] 调用腾讯云录音转文字…")
    transcript = do_asr_auto(cfg, data, log)
    if transcript is not None:
        set_result(transcript if transcript else "(空)")
        save_transcript_file(txt_path, transcript or "", log)
        log("本地转文字完成。")
    else:
        log("识别失败。")


# ============================ 模式 A / C：在线视频（转文字 / 仅下载） ============================

def online_pipeline(wp, log, set_result):
    cfg = wp["cfg"]
    is_download = (wp["mode"] == MODE_DOWNLOAD)

    log("在线视频 → 仅下载" if is_download else "在线视频 → 音频 → 腾讯云录音转文字")
    log("引擎模型：%s   地域：%s" % (cfg["engine"], cfg["region"]))

    link = wp["link"]
    direct = is_direct_media(link)   # 直链媒体文件：自动直接下载，无需解析

    # ---- 1. 解析直链 ----
    log("")
    log("[1/2] 解析链接…" if is_download else "[1/4] 解析链接…")
    log("  链接：%s" % link)

    video_url = ""
    title = ""
    if direct:
        video_url = link
        log("  直链媒体文件，跳过解析，直接下载。")
    else:
        api = PARSE_BASE + urllib.parse.quote(link, safe="")
        try:
            resp = http_get(api)
        except Exception as e:
            log("解析失败：网络错误或接口不可用（%s）。" % e)
            return
        if not resp:
            log("解析失败：网络错误或接口不可用。")
            return
        # 聚合接口返回 {"code":200,"msg":"...","data":{...,"url":"视频直链","title":"...","desc":"..."}}
        try:
            j = json.loads(resp.decode("utf-8", "replace"))
        except ValueError:
            log("解析失败：接口返回非 JSON。")
            log("接口返回：%.400s" % resp.decode("utf-8", "replace"))
            return
        code = j.get("code") if isinstance(j, dict) else None
        if code is not None:
            try:
                code_ok = int(code) == 200
            except (TypeError, ValueError):
                code_ok = True
            if not code_ok:
                msg = j.get("msg") or "(无详情)"
                log("解析失败（接口返回 %s）：%s" % (code, msg))
                return
        u = _find_key(j, "url")
        video_url = u if isinstance(u, str) else ""
        t = _find_key(j, "title") or _find_key(j, "desc")
        title = t if isinstance(t, str) else ""
        if not video_url:
            log("未解析到视频直链（该链接可能是图集/直播，无法作为视频下载）。")
            log("接口返回：%.400s" % resp.decode("utf-8", "replace"))
            return
        if title:
            log("  标题：%s" % title)
        log("  视频直链：%.80s..." % video_url)

    # ---- 文件名（视频） ----
    fname = sanitize_filename(title) if title else ""
    if not fname:
        fname = make_filename_from_url(video_url)
    if not fname:
        fname = "video.mp4"
    if "." not in fname:
        fname += ".mp4"
    base_noext = fname.rsplit(".", 1)[0] if "." in fname else fname

    video_path = os.path.join(wp["save_dir"], fname)
    audio_path = os.path.join(wp["save_dir"], base_noext + "_audio.aac")
    txt_path = os.path.join(wp["save_dir"], base_noext + "_transcript.txt")

    # ---- 2. 下载视频 ----
    log("")
    log("[2/2] 下载视频…" if is_download else "[2/4] 下载视频…")
    vbytes = http_download_file(video_url, video_path)
    if vbytes == 0:
        log("视频下载失败。")
        return
    log("  已下载 %.2f MB -> %s" % (vbytes / 1048576.0, video_path))

    # ---- 模式 C：仅下载，结束 ----
    if is_download:
        log("")
        log("下载完成（仅视频，未做转文字）。")
        return

    # ---- 3. 提取音频 ----
    log("")
    log("[3/4] 从 MP4 提取 AAC 音频…")
    try:
        with open(video_path, "rb") as f:
            mp4data = f.read()
    except OSError:
        log("读取视频文件失败。")
        return
    if not mp4_extract_aac(mp4data, audio_path, log):
        log("音频提取失败（可能不是标准 MP4/AAC 封装，或视频无音轨）。")
        return
    log("  已保存音频 -> %s" % audio_path)

    # ---- 4. 腾讯云录音文件识别 ----
    log("")
    log("[4/4] 调用腾讯云录音转文字…")
    try:
        with open(audio_path, "rb") as f:
            aac_data = f.read()
    except OSError:
        log("读取音频失败。")
        return
    if not aac_data:
        log("读取音频失败。")
        return

    transcript = do_asr_auto(cfg, aac_data, log)
    if transcript is None:
        return
    set_result(transcript if transcript else "(空)")
    save_transcript_file(txt_path, transcript or "", log)
    log("")
    log("全部完成。")


# ============================ GUI（ttk 现代风格） ============================

_REGION_PRESETS = ("ap-guangzhou", "ap-beijing", "ap-shanghai", "ap-hongkong",
                   "ap-singapore", "ap-seoul", "ap-sydney")
_ENGINE_PRESETS = ("16k_zh", "16k_en", "16k_zh-PY", "16k_zh_medical",
                   "16k_zh_dialect", "16k_zh-video", "8k_zh")


class App(object):
    def __init__(self, root):
        self.root = root
        self.q = queue.Queue()
        self.ini_path = os.path.join(app_dir(), "vd_asr.ini")

        root.title("多平台视频转文字工具 · 腾讯云 ASR")
        root.geometry("640x800")
        root.minsize(640, 760)
        try:
            root.iconbitmap(default="")
        except Exception:
            pass

        # ---------- 主容器 ----------
        self.frm = ttk.Frame(root, padding=14)
        self.frm.pack(fill="both", expand=True)

        self._build_header()       # 顶部标题
        self._build_cred()         # 腾讯云凭证
        self._build_task()         # 任务设置
        self._build_buttons()      # 操作按钮
        self._build_outputs()      # 日志 + 结果

        # ---------- 加载配置 ----------
        cfg = load_config(self.ini_path)
        self.ent_id.insert(0, cfg["secret_id"])
        self.ent_key.insert(0, cfg["secret_key"])
        self.ent_region.set(cfg["region"])
        self.ent_engine.set(cfg["engine"])

        self.update_ui_for_mode()
        self.root.after(100, self._poll_queue)

    # ---------- 构建各区域 ----------
    def _build_header(self):
        """顶部标题 + 分隔线。"""
        ttk.Label(self.frm,
                  text="📹 多平台视频转文字工具",
                  font=("Microsoft YaHei UI", 14, "bold")).pack(anchor="w")
        ttk.Label(self.frm,
                  text="基于腾讯云语音识别 (ASR) · 支持抖音/快手/小红书/B站等 20+ 平台",
                  font=("Microsoft YaHei UI", 9),
                  foreground="#666666").pack(anchor="w", pady=(0, 6))
        ttk.Separator(self.frm, orient="horizontal").pack(fill="x", pady=(0, 10))

    def _build_cred(self):
        """腾讯云凭证卡片。"""
        f = ttk.LabelFrame(self.frm, text="🔑 腾讯云凭证", padding=12)
        f.pack(fill="x", pady=(0, 8))

        pad = {"pady": 3}
        # SecretId 行
        row = ttk.Frame(f)
        row.pack(fill="x", **pad)
        ttk.Label(row, text="SecretId：", width=12, anchor="e").pack(side="left")
        self.ent_id = ttk.Entry(row)
        self.ent_id.pack(side="left", fill="x", expand=True, padx=(6, 0))

        # SecretKey 行
        row = ttk.Frame(f)
        row.pack(fill="x", **pad)
        ttk.Label(row, text="SecretKey：", width=12, anchor="e").pack(side="left")
        self.ent_key = ttk.Entry(row, show="*")
        self.ent_key.pack(side="left", fill="x", expand=True, padx=(6, 0))

        # 地域 + 引擎 同行
        row = ttk.Frame(f)
        row.pack(fill="x", **pad)
        ttk.Label(row, text="地域 Region：", width=12, anchor="e").pack(side="left")
        self.ent_region = ttk.Combobox(row, values=_REGION_PRESETS, width=18)
        self.ent_region.pack(side="left", padx=(6, 0))
        ttk.Label(row, text="引擎模型：", anchor="e").pack(side="left", padx=(12, 4))
        self.ent_engine = ttk.Combobox(row, values=_ENGINE_PRESETS)
        self.ent_engine.pack(side="left", fill="x", expand=True)

    def _build_task(self):
        """任务设置卡片（模式 + 输入 + 保存路径）。"""
        f = ttk.LabelFrame(self.frm, text="⚙️ 任务设置", padding=12)
        f.pack(fill="x", pady=(0, 8))

        # 模式行
        row = ttk.Frame(f)
        row.pack(fill="x", pady=3)
        ttk.Label(row, text="模式：", width=12, anchor="e").pack(side="left")
        self.mode = tk.IntVar(value=MODE_ONLINE_TEXT)
        for text, val in (("在线视频→文字", MODE_ONLINE_TEXT),
                          ("本地文件→文字", MODE_LOCAL_TEXT),
                          ("仅下载视频", MODE_DOWNLOAD)):
            ttk.Radiobutton(row, text=text, variable=self.mode, value=val,
                            command=self.update_ui_for_mode).pack(side="left", padx=6)

        # 输入行（链接 / 本地文件，互斥）
        self.frm_input = ttk.Frame(f)
        self.frm_input.pack(fill="x", pady=3)

        # —— 链接（在线模式）——
        self.frm_online = ttk.Frame(self.frm_input)
        ttk.Label(self.frm_online, text="下载链接：", width=12, anchor="e").pack(side="left")
        self.ent_link = ttk.Entry(self.frm_online)
        self.ent_link.pack(side="left", fill="x", expand=True, padx=(6, 0))

        # —— 本地文件（本地模式）——
        self.frm_local = ttk.Frame(self.frm_input)
        ttk.Label(self.frm_local, text="本地文件：", width=12, anchor="e").pack(side="left")
        self.ent_local = ttk.Entry(self.frm_local)
        self.ent_local.pack(side="left", fill="x", expand=True, padx=(6, 0))
        ttk.Button(self.frm_local, text="选择文件…",
                   command=self.browse_local_file).pack(side="left", padx=(6, 0))

        # 保存位置
        row = ttk.Frame(f)
        row.pack(fill="x", pady=3)
        ttk.Label(row, text="保存位置：", width=12, anchor="e").pack(side="left")
        self.ent_save = ttk.Entry(row)
        self.ent_save.pack(side="left", fill="x", expand=True, padx=(6, 0))
        ttk.Button(row, text="浏览…", command=self.browse_folder).pack(side="left", padx=(6, 0))

    def _build_buttons(self):
        """操作按钮行。"""
        r = ttk.Frame(self.frm)
        r.pack(fill="x", pady=(0, 8))
        self.btn_start = ttk.Button(r, text="▶ 开始：下载 → 提取音频 → 转文字",
                                    command=self.start_process)
        self.btn_start.pack(side="left", fill="x", expand=True, ipady=4)
        ttk.Button(r, text="💾 保存密钥",
                   command=self.save_cfg_only).pack(side="left", padx=(10, 0))

    def _build_outputs(self):
        """日志 + 结果两栏。"""
        # —— 运行日志 ——
        lf = ttk.LabelFrame(self.frm, text="📋 运行日志", padding=6)
        lf.pack(fill="both", expand=True, pady=(0, 6))
        self.log_text = self._mk_ro_text(lf, height=6)

        # —— 识别结果 ——
        lf = ttk.LabelFrame(self.frm, text="📝 识别结果", padding=6)
        lf.pack(fill="both", expand=True, pady=(0, 0))
        toolbar = ttk.Frame(lf)
        toolbar.pack(fill="x", pady=(0, 4))
        ttk.Label(toolbar, text="（可选中文字后 Ctrl+C 复制）",
                  foreground="#888888").pack(side="left")
        ttk.Button(toolbar, text="📂 打开目录",
                   command=self.open_dir).pack(side="right", padx=(4, 0))
        ttk.Button(toolbar, text="📋 复制结果",
                   command=self.copy_result).pack(side="right", padx=(4, 0))
        self.result_text = self._mk_ro_text(lf, height=7)

    @staticmethod
    def _mk_ro_text(parent, height):
        """创建只读文本框（tk.Text + ttk.Scrollbar）。"""
        f = ttk.Frame(parent)
        f.pack(fill="both", expand=True)
        sb = ttk.Scrollbar(f)
        sb.pack(side="right", fill="y")
        t = tk.Text(f, height=height, wrap="word",
                    font=("Microsoft YaHei UI", 9),
                    yscrollcommand=sb.set, state="disabled",
                    relief="sunken", borderwidth=1,
                    padx=6, pady=6)
        t.pack(side="left", fill="both", expand=True)
        sb.config(command=t.yview)
        return t

    # ---------- 线程安全的界面输出 ----------
    def log(self, s):
        self.q.put(("log", s))

    def set_result(self, s):
        self.q.put(("result", s))

    def _poll_queue(self):
        try:
            while True:
                kind, text = self.q.get_nowait()
                if kind == "log":
                    self.log_text.configure(state="normal")
                    self.log_text.insert("end", text + "\n")
                    self.log_text.see("end")
                    self.log_text.configure(state="disabled")
                elif kind == "result":
                    self.result_text.configure(state="normal")
                    self.result_text.delete("1.0", "end")
                    self.result_text.insert("1.0", text)
                    self.result_text.configure(state="disabled")
                elif kind == "clear_log":
                    self.log_text.configure(state="normal")
                    self.log_text.delete("1.0", "end")
                    self.log_text.configure(state="disabled")
                elif kind == "done":
                    self.btn_start.configure(state="normal")
        except queue.Empty:
            pass
        self.root.after(100, self._poll_queue)

    # ---------- 交互动作 ----------
    def update_ui_for_mode(self):
        m = self.mode.get()
        self.frm_online.pack_forget()
        self.frm_local.pack_forget()
        if m in (MODE_ONLINE_TEXT, MODE_DOWNLOAD):
            self.frm_online.pack(fill="x")
        else:
            self.frm_local.pack(fill="x")
        lbl = ("▶  开始：仅下载视频" if m == MODE_DOWNLOAD else
               "▶  开始：本地文件 → 转文字" if m == MODE_LOCAL_TEXT else
               "▶  开始：下载 → 提取音频 → 转文字")
        self.btn_start.configure(text=lbl)

    def collect_config(self):
        cfg = {
            "secret_id": self.ent_id.get().strip(),
            "secret_key": self.ent_key.get().strip(),
            "region": self.ent_region.get().strip() or "ap-guangzhou",
            "engine": self.ent_engine.get().strip() or "16k_zh",
        }
        return cfg

    def browse_folder(self):
        d = filedialog.askdirectory(title="选择视频/音频/文字的保存位置", parent=self.root)
        if d:
            self.ent_save.delete(0, "end")
            self.ent_save.insert(0, d)

    def browse_local_file(self):
        p = filedialog.askopenfilename(
            title="选择本地视频/音频文件", parent=self.root,
            filetypes=[("视频/音频文件",
                        "*.mp4 *.m4v *.mov *.mkv *.webm *.avi *.flv "
                        "*.mp3 *.wav *.m4a *.aac *.amr *.flac *.ogg *.wma"),
                       ("所有文件", "*.*")])
        if p:
            self.ent_local.delete(0, "end")
            self.ent_local.insert(0, p)

    def copy_result(self):
        text = self.result_text.get("1.0", "end-1c")
        if text:
            self.root.clipboard_clear()
            self.root.clipboard_append(text)

    def open_dir(self):
        d = self.ent_save.get().strip() or app_dir()
        try:
            if sys.platform == "win32":
                os.startfile(d)  # noqa: S606
            elif sys.platform == "darwin":
                subprocess.Popen(["open", d])
            else:
                subprocess.Popen(["xdg-open", d])
        except Exception as e:
            messagebox.showerror("打开目录失败", str(e), parent=self.root)

    def save_cfg_only(self):
        if save_config(self.ini_path, self.collect_config()):
            messagebox.showinfo("已保存", "密钥/配置已保存，下次自动填充。", parent=self.root)

    def start_process(self):
        cfg = self.collect_config()
        mode = self.mode.get()
        need_creds = (mode != MODE_DOWNLOAD)
        if need_creds and (not cfg["secret_id"] or not cfg["secret_key"]):
            messagebox.showwarning("缺少凭证", "请先填写 SecretId 和 SecretKey。", parent=self.root)
            return

        wp = {"cfg": cfg, "mode": mode, "link": "", "local_path": "", "save_dir": ""}
        if mode == MODE_LOCAL_TEXT:
            path = self.ent_local.get().strip()
            if not path:
                messagebox.showwarning("缺少文件", "请先选择本地视频/音频文件。", parent=self.root)
                return
            wp["local_path"] = path
        else:
            link = extract_url(self.ent_link.get())
            if not link:
                messagebox.showwarning("缺少链接", "请粘贴视频分享链接（或含链接的整段文字）。",
                                       parent=self.root)
                return
            wp["link"] = link

        # 自动保存凭证到 ini（下载模式也保存，方便切换）
        if save_config(self.ini_path, cfg):
            self.log("[配置] 已保存到 %s" % self.ini_path)

        # 保存目录：用户填的 > 程序目录
        save_dir = self.ent_save.get().strip() or app_dir()
        try:
            os.makedirs(save_dir, exist_ok=True)
        except OSError:
            pass
        wp["save_dir"] = save_dir

        self.btn_start.configure(state="disabled")
        self.q.put(("clear_log", ""))
        threading.Thread(target=self._worker, args=(wp,), daemon=True).start()

    # ---------- 工作线程 ----------
    def _worker(self, wp):
        try:
            self.log("========================================")
            if wp["mode"] == MODE_LOCAL_TEXT:
                local_asr_pipeline(wp, self.log, self.set_result)
            else:
                online_pipeline(wp, self.log, self.set_result)
            self.log("========================================")
        except Exception as e:
            self.log("[异常] %s" % e)
        finally:
            self.q.put(("done", ""))


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
