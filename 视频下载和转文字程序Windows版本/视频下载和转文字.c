/*
 * 多平台视频 -> 音频 -> 腾讯云录音转文字（ASR）  图形界面版  纯 C + Win32 API
 *
 * 三大功能：
 *   模式 A「在线视频转文字」：调用第三方「聚合解析」接口，自动识别 20+ 平台
 *        （抖音/快手/小红书/B站/油管/TikTok/西瓜/微博/知乎/A站…），无需手动选平台 ->
 *        解析直链 -> 下载 -> 提取 AAC -> 腾讯云 ASR -> 结果。
 *   模式 B「本地文件转文字」：选择本机视频(MP4)或音频(mp3/wav/m4a/aac…)文件，
 *        直接调用腾讯云 ASR 转成文字。
 *   模式 C「仅下载视频」：只把在线视频下载到本地，不做转文字。
 *
 * 依赖：仅 Windows 自带 DLL（winhttp / bcrypt / crypt32 / shell32 / ole32 / gdi32 /
 *      comdlg32），无第三方、无 ffmpeg。
 *
 * 编译：
 *   gcc -O2 -s -mwindows -ffunction-sections -fdata-sections -Wl,--gc-sections \
 *       -o vd_asr_gui.exe vd_asr_gui.c -lwinhttp -lbcrypt -lcrypt32 -lshell32 -lole32 -lgdi32 -lcomdlg32
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

#define MAX_URL    4096
#define BUF_SIZE   65536

/* ============================ 全局 GUI 句柄 ============================ */
static HWND g_hMain = NULL;
static HWND g_hId = NULL, g_hKey = NULL, g_hRegion = NULL, g_hEngine = NULL;
static HWND g_hLink = NULL, g_hSave = NULL;
static HWND g_hLog = NULL, g_hResult = NULL;
static HWND g_hBtnStart = NULL;
static HWND g_hLblLink = NULL;
static HWND g_hLblLocal = NULL, g_hLocal = NULL, g_hLocBrowse = NULL;
static HFONT g_font = NULL;
static wchar_t g_iniPath[MAX_PATH] = {0};

/* 当前模式：1=在线视频转文字  2=本地文件转文字  3=仅下载视频 */
#define MODE_ONLINE_TEXT 1
#define MODE_LOCAL_TEXT  2
#define MODE_DOWNLOAD    3
static int g_mode = MODE_ONLINE_TEXT;

/* 第三方「聚合解析」接口：自动识别 20+ 平台（抖音/快手/小红书/B站/油管/TikTok/西瓜/微博/
   知乎/A站…），无需手动选择平台，直接把分享链接丢给它即可。可按需替换为自建/其它多平台解析服务。 */
#define PARSE_BASE "https://api.bugpk.com/api/short_videos?url="

/* 控件 ID */
#define IDC_ID       1001
#define IDC_KEY      1002
#define IDC_REGION   1003
#define IDC_ENGINE   1004
#define IDC_LINK     1005
#define IDC_SAVE     1006
#define IDC_BROWSE   1010
#define IDC_SAVECFG  1011
#define IDC_START    1012
#define IDC_COPY     1013
#define IDC_OPENDIR  1014
#define IDC_MODE_ONLINE 1020
#define IDC_MODE_LOCAL  1021
#define IDC_MODE_DL     1022
#define IDC_LOCAL    1031
#define IDC_LOCBROWSE 1032

/* ============================ 日志（向界面日志框追加） ============================ */
static void logln(const char *fmt, ...) {
    char buf[4096];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_hLog) {
        wchar_t wbuf[4096];
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf, 4096);
        int len = GetWindowTextLengthW(g_hLog);
        SendMessageW(g_hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
        SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)wbuf);
        SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    }
}

/* ============================ 加密：SHA256 / HMAC-SHA256 / HEX / Base64 ============================ */

static int sha256(const unsigned char *data, DWORD len, unsigned char out[32]) {
    BCRYPT_ALG_HANDLE hAlg = NULL; BCRYPT_HASH_HANDLE hHash = NULL;
    int ok = 0;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) return 0;
    if (BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0) != 0) goto done;
    if (BCryptHashData(hHash, (PUCHAR)data, len, 0) != 0) goto done;
    if (BCryptFinishHash(hHash, out, 32, 0) != 0) goto done;
    ok = 1;
done:
    if (hHash) BCryptDestroyHash(hHash);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

static int hmac_sha256(const unsigned char *key, DWORD keylen,
                       const unsigned char *data, DWORD datalen,
                       unsigned char out[32]) {
    BCRYPT_ALG_HANDLE hAlg = NULL; BCRYPT_HASH_HANDLE hHash = NULL;
    int ok = 0;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) return 0;
    if (BCryptCreateHash(hAlg, &hHash, NULL, 0, (PUCHAR)key, keylen, 0) != 0) goto done;
    if (BCryptHashData(hHash, (PUCHAR)data, datalen, 0) != 0) goto done;
    if (BCryptFinishHash(hHash, out, 32, 0) != 0) goto done;
    ok = 1;
done:
    if (hHash) BCryptDestroyHash(hHash);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

static void hex_encode(const unsigned char *in, int len, char *out) {
    static const char *hx = "0123456789abcdef";
    for (int i = 0; i < len; i++) {
        out[i*2]   = hx[(in[i] >> 4) & 0xF];
        out[i*2+1] = hx[in[i] & 0xF];
    }
    out[len*2] = 0;
}

static char* base64_encode(const unsigned char *in, DWORD len) {
    DWORD outlen = 0;
    if (!CryptBinaryToStringA(in, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &outlen))
        return NULL;
    char *out = (char*)malloc(outlen + 1);
    if (!out) return NULL;
    if (!CryptBinaryToStringA(in, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out, &outlen)) {
        free(out); return NULL;
    }
    out[outlen] = 0;
    return out;
}

/* ============================ JSON 极简提取 ============================ */

static char* json_str(const char *s, const char *key, char *out, int maxlen) {
    out[0] = 0;
    const char *p = strstr(s, key);
    if (!p) return NULL;
    p = strchr(p + strlen(key), ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return NULL;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < maxlen - 1) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': out[i++] = '\n'; break;
                case 't': out[i++] = '\t'; break;
                case 'r': out[i++] = '\r'; break;
                case '\\': out[i++] = '\\'; break;
                case '"': out[i++] = '"'; break;
                case '/': out[i++] = '/'; break;
                case 'u': {
                    if (p[1] && p[2] && p[3] && p[4]) {
                        char hx[5] = { p[1], p[2], p[3], p[4], 0 };
                        unsigned int cp = (unsigned int)strtol(hx, NULL, 16);
                        p += 4;
                        if (cp < 0x80) { out[i++] = (char)cp; }
                        else if (cp < 0x800) {
                            if (i < maxlen - 2) { out[i++] = 0xC0 | (cp >> 6); out[i++] = 0x80 | (cp & 0x3F); }
                        } else {
                            if (i < maxlen - 3) { out[i++] = 0xE0 | (cp >> 12); out[i++] = 0x80 | ((cp >> 6) & 0x3F); out[i++] = 0x80 | (cp & 0x3F); }
                        }
                    }
                    break;
                }
                default: out[i++] = *p;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = 0;
    return out;
}

static int json_num(const char *s, const char *key, long long *val) {
    const char *p = strstr(s, key);
    if (!p) return 0;
    p = strchr(p + strlen(key), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    if (*p < '0' || *p > '9') return 0;
    long long v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
    *val = neg ? -v : v;
    return 1;
}

/* ============================ WinHTTP ============================ */

static char* http_get(const wchar_t *url, DWORD *out_len) {
    *out_len = 0;
    URL_COMPONENTS uc = {0};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[512] = {0}, path[MAX_URL] = {0};
    uc.lpszHostName = host; uc.dwHostNameLength = 511;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = MAX_URL - 1;
    uc.dwSchemeLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return NULL;

    HINTERNET hS = WinHttpOpen(L"vd_asr/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return NULL;
    HINTERNET hC = WinHttpConnect(hS, host, uc.nPort, 0);
    if (!hC) { WinHttpCloseHandle(hS); return NULL; }
    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", path, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hR) { WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return NULL; }

    DWORD timeout = 30000;
    WinHttpSetOption(hR, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(hR, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    char *buf = NULL;
    if (WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(hR, 0)) {
        DWORD total = 0, cap = BUF_SIZE * 4, read;
        buf = (char*)malloc(cap);
        while (buf && WinHttpReadData(hR, buf + total,
                (cap - total > BUF_SIZE) ? BUF_SIZE : cap - total, &read) && read > 0) {
            total += read;
            if (cap - total < BUF_SIZE) { cap *= 2; buf = (char*)realloc(buf, cap); }
        }
        if (buf) { buf = (char*)realloc(buf, total + 1); buf[total] = 0; *out_len = total; }
    }
    WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
    return buf;
}

static DWORD http_download_file(const wchar_t *url, const wchar_t *savePath) {
    URL_COMPONENTS uc = {0};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[512] = {0}, path[MAX_URL] = {0};
    uc.lpszHostName = host; uc.dwHostNameLength = 511;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = MAX_URL - 1;
    uc.dwSchemeLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return 0;

    HINTERNET hS = WinHttpOpen(L"vd_asr/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return 0;
    HINTERNET hC = WinHttpConnect(hS, host, uc.nPort, 0);
    if (!hC) { WinHttpCloseHandle(hS); return 0; }
    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", path, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hR) { WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return 0; }

    DWORD total = 0;
    if (WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(hR, 0)) {
        HANDLE hFile = CreateFileW(savePath, GENERIC_WRITE, 0, NULL,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            char *buf = (char*)malloc(BUF_SIZE);
            DWORD read, written;
            while (buf && WinHttpReadData(hR, buf, BUF_SIZE, &read) && read > 0) {
                WriteFile(hFile, buf, read, &written, NULL);
                total += read;
            }
            free(buf);
            CloseHandle(hFile);
        }
    }
    WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
    return total;
}

static char* https_post(const wchar_t *host, const wchar_t *path,
                        const wchar_t *headers, const char *body, DWORD bodylen,
                        DWORD *out_len) {
    *out_len = 0;
    HINTERNET hS = WinHttpOpen(L"vd_asr/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return NULL;
    HINTERNET hC = WinHttpConnect(hS, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hC) { WinHttpCloseHandle(hS); return NULL; }
    HINTERNET hR = WinHttpOpenRequest(hC, L"POST", path, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hR) { WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return NULL; }

    DWORD timeout = 60000;
    WinHttpSetOption(hR, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(hR, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(hR, WINHTTP_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));

    char *buf = NULL;
    if (WinHttpSendRequest(hR, headers, (DWORD)-1L, (LPVOID)body, bodylen, bodylen, 0)
        && WinHttpReceiveResponse(hR, 0)) {
        DWORD total = 0, cap = BUF_SIZE, read;
        buf = (char*)malloc(cap);
        while (buf && WinHttpReadData(hR, buf + total,
                (cap - total > BUF_SIZE) ? BUF_SIZE : cap - total, &read) && read > 0) {
            total += read;
            if (cap - total < BUF_SIZE) { cap *= 2; buf = (char*)realloc(buf, cap); }
        }
        if (buf) { buf = (char*)realloc(buf, total + 1); buf[total] = 0; *out_len = total; }
    }
    WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
    return buf;
}

/* ============================ MP4 解复用：提取 AAC 音轨 -> ADTS(.aac) ============================ */

static unsigned int  rd_be32(const unsigned char *p) { return ((unsigned int)p[0]<<24)|((unsigned int)p[1]<<16)|((unsigned int)p[2]<<8)|p[3]; }
static unsigned long long rd_be64(const unsigned char *p) {
    unsigned long long v = 0; for (int i = 0; i < 8; i++) v = (v<<8) | p[i]; return v;
}

static const unsigned char* mp4_find(const unsigned char *base, size_t size,
                                     const char *type, size_t *outLen) {
    size_t off = 0;
    while (off + 8 <= size) {
        unsigned long long bsize = rd_be32(base + off);
        const unsigned char *btype = base + off + 4;
        size_t hdr = 8;
        if (bsize == 1) {
            if (off + 16 > size) break;
            bsize = rd_be64(base + off + 8);
            hdr = 16;
        } else if (bsize == 0) {
            bsize = size - off;
        }
        if (bsize < hdr || off + bsize > size) break;
        if (memcmp(btype, type, 4) == 0) {
            if (outLen) *outLen = (size_t)bsize - hdr;
            return base + off + hdr;
        }
        off += (size_t)bsize;
    }
    return NULL;
}

typedef struct {
    const unsigned char *base;
    size_t size;
    size_t off;
} mp4_iter;

static void mp4_iter_init(mp4_iter *it, const unsigned char *base, size_t size) {
    it->base = base; it->size = size; it->off = 0;
}
static const unsigned char* mp4_iter_next(mp4_iter *it, char type4[4], size_t *outLen) {
    if (it->off + 8 > it->size) return NULL;
    unsigned long long bsize = rd_be32(it->base + it->off);
    const unsigned char *btype = it->base + it->off + 4;
    size_t hdr = 8;
    if (bsize == 1) {
        if (it->off + 16 > it->size) return NULL;
        bsize = rd_be64(it->base + it->off + 8);
        hdr = 16;
    } else if (bsize == 0) {
        bsize = it->size - it->off;
    }
    if (bsize < hdr || it->off + bsize > it->size) return NULL;
    memcpy(type4, btype, 4);
    const unsigned char *content = it->base + it->off + hdr;
    if (outLen) *outLen = (size_t)bsize - hdr;
    it->off += (size_t)bsize;
    return content;
}

static const int aac_sr_table[16] = {
    96000,88200,64000,48000,44100,32000,24000,22050,
    16000,12000,11025,8000,7350,0,0,0
};

static int parse_esds(const unsigned char *esds, size_t len,
                      int *objType, int *freqIdx, int *chan) {
    size_t i = 4;
    while (i + 1 < len) {
        unsigned char tag = esds[i];
        if (tag == 0x05) {
            size_t j = i + 1; unsigned int dlen = 0; int cnt = 0;
            while (j < len && cnt < 4) { unsigned char b = esds[j++]; dlen = (dlen<<7)|(b&0x7F); cnt++; if (!(b&0x80)) break; }
            if (j + 2 <= len && dlen >= 2) {
                unsigned char a0 = esds[j], a1 = esds[j+1];
                int ot = (a0 >> 3) & 0x1F;
                int fi = ((a0 & 0x07) << 1) | ((a1 >> 7) & 0x01);
                int ch = (a1 >> 3) & 0x0F;
                *objType = ot; *freqIdx = fi; *chan = ch;
                return 1;
            }
            return 0;
        }
        i++;
    }
    return 0;
}

static void write_adts(unsigned char *out, int profile, int freqIdx, int chan, int frameLen) {
    int aac_frame_length = frameLen + 7;
    out[0] = 0xFF;
    out[1] = 0xF1;
    out[2] = (unsigned char)(((profile & 0x3) << 6) | ((freqIdx & 0xF) << 2) | ((chan >> 2) & 0x1));
    out[3] = (unsigned char)(((chan & 0x3) << 6) | ((aac_frame_length >> 11) & 0x3));
    out[4] = (unsigned char)((aac_frame_length >> 3) & 0xFF);
    out[5] = (unsigned char)(((aac_frame_length & 0x7) << 5) | 0x1F);
    out[6] = 0xFC;
}

static int mp4_extract_aac(const unsigned char *file, size_t fsize,
                           const wchar_t *outPath, int *sampleRateOut) {
    size_t moovLen;
    const unsigned char *moov = mp4_find(file, fsize, "moov", &moovLen);
    if (!moov) { logln("  [MP4] 未找到 moov box"); return 0; }

    mp4_iter it; mp4_iter_init(&it, moov, moovLen);
    char t4[4]; size_t clen;
    const unsigned char *audioStbl = NULL; size_t audioStblLen = 0;
    while (1) {
        const unsigned char *box = mp4_iter_next(&it, t4, &clen);
        if (!box) break;
        if (memcmp(t4, "trak", 4) != 0) continue;
        size_t mdiaLen; const unsigned char *mdia = mp4_find(box, clen, "mdia", &mdiaLen);
        if (!mdia) continue;
        size_t hdlrLen; const unsigned char *hdlr = mp4_find(mdia, mdiaLen, "hdlr", &hdlrLen);
        if (!hdlr || hdlrLen < 12) continue;
        if (memcmp(hdlr + 8, "soun", 4) != 0) continue;
        size_t minfLen; const unsigned char *minf = mp4_find(mdia, mdiaLen, "minf", &minfLen);
        if (!minf) continue;
        size_t stblLen; const unsigned char *stbl = mp4_find(minf, minfLen, "stbl", &stblLen);
        if (!stbl) continue;
        audioStbl = stbl; audioStblLen = stblLen;
        break;
    }
    if (!audioStbl) { logln("  [MP4] 未找到音频轨(soun)"); return 0; }

    size_t stsdLen; const unsigned char *stsd = mp4_find(audioStbl, audioStblLen, "stsd", &stsdLen);
    if (!stsd || stsdLen < 8) { logln("  [MP4] 缺 stsd"); return 0; }
    size_t esdsLen; const unsigned char *esds = mp4_find(stsd + 8, stsdLen - 8, "esds", &esdsLen);
    if (!esds) {
        for (size_t k = 0; k + 4 <= stsdLen; k++) {
            if (memcmp(stsd + k, "esds", 4) == 0) {
                esds = stsd + k + 4;
                esdsLen = stsdLen - (k + 4);
                break;
            }
        }
    }
    int objType = 2, freqIdx = 4, chan = 2;
    if (esds && parse_esds(esds, esdsLen, &objType, &freqIdx, &chan)) {
    } else {
        logln("  [MP4] 未解析到 esds/ASC，使用默认 AAC-LC 参数");
    }
    if (freqIdx < 0 || freqIdx > 12) freqIdx = 4;
    int profile = objType - 1; if (profile < 0) profile = 1;
    if (sampleRateOut) *sampleRateOut = aac_sr_table[freqIdx];

    size_t stszLen; const unsigned char *stsz = mp4_find(audioStbl, audioStblLen, "stsz", &stszLen);
    if (!stsz || stszLen < 12) { logln("  [MP4] 缺 stsz"); return 0; }
    unsigned int fixedSize = rd_be32(stsz + 4);
    unsigned int sampleCount = rd_be32(stsz + 8);
    const unsigned char *stszTable = stsz + 12;

    size_t stscLen; const unsigned char *stsc = mp4_find(audioStbl, audioStblLen, "stsc", &stscLen);
    if (!stsc || stscLen < 8) { logln("  [MP4] 缺 stsc"); return 0; }
    unsigned int stscCount = rd_be32(stsc + 4);
    const unsigned char *stscTable = stsc + 8;

    size_t stcoLen; const unsigned char *stco = mp4_find(audioStbl, audioStblLen, "stco", &stcoLen);
    int is64 = 0;
    if (!stco) { stco = mp4_find(audioStbl, audioStblLen, "co64", &stcoLen); is64 = 1; }
    if (!stco || stcoLen < 8) { logln("  [MP4] 缺 stco/co64"); return 0; }
    unsigned int chunkCount = rd_be32(stco + 4);
    const unsigned char *stcoTable = stco + 8;

    HANDLE hOut = CreateFileW(outPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hOut == INVALID_HANDLE_VALUE) { logln("  [MP4] 无法创建输出音频文件"); return 0; }

    unsigned int sampleIdx = 0;
    unsigned long long wrote = 0;
    unsigned char adts[7];
    for (unsigned int c = 0; c < chunkCount && sampleIdx < sampleCount; c++) {
        unsigned int spc = 1;
        for (unsigned int e = 0; e < stscCount; e++) {
            unsigned int firstChunk = rd_be32(stscTable + e*12);
            unsigned int samplesPer = rd_be32(stscTable + e*12 + 4);
            if (firstChunk <= c + 1) spc = samplesPer; else break;
        }
        unsigned long long chunkOff = is64 ? rd_be64(stcoTable + (size_t)c*8)
                                           : rd_be32(stcoTable + (size_t)c*4);
        unsigned long long pos = chunkOff;
        for (unsigned int s = 0; s < spc && sampleIdx < sampleCount; s++) {
            unsigned int ssize = fixedSize ? fixedSize : rd_be32(stszTable + (size_t)sampleIdx*4);
            if (pos + ssize > fsize) { sampleIdx = sampleCount; break; }
            write_adts(adts, profile, freqIdx, chan, (int)ssize);
            DWORD w;
            WriteFile(hOut, adts, 7, &w, NULL);
            WriteFile(hOut, file + pos, ssize, &w, NULL);
            wrote += 7 + ssize;
            pos += ssize;
            sampleIdx++;
        }
    }
    CloseHandle(hOut);
    logln("  [MP4] 提取 %u 个 AAC 帧，采样率 %d Hz，声道 %d，音频约 %.2f MB",
          sampleIdx, aac_sr_table[freqIdx], chan, wrote / 1048576.0);
    return sampleIdx > 0;
}

/* ============================ 文件读取 ============================ */

static unsigned char* read_file_w(const wchar_t *path, DWORD *size) {
    *size = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER li; GetFileSizeEx(h, &li);
    DWORD fsize = (DWORD)li.QuadPart;
    unsigned char *buf = (unsigned char*)malloc(fsize ? fsize : 1);
    if (!buf) { CloseHandle(h); return NULL; }
    DWORD got = 0, read;
    while (got < fsize && ReadFile(h, buf + got, fsize - got, &read, NULL) && read > 0) got += read;
    CloseHandle(h);
    *size = got;
    return buf;
}

/* ============================ 配置（读/写） ============================ */

typedef struct {
    char secretId[256];
    char secretKey[256];
    char region[64];
    char engine[64];
} Config;

static void trim(char *s) {
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1]=='\r'||s[n-1]=='\n'||s[n-1]==' '||s[n-1]=='\t')) s[--n] = 0;
    int i = 0; while (s[i]==' '||s[i]=='\t') i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}

static void load_config(const wchar_t *iniPath, Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->region, "ap-guangzhou");
    strcpy(cfg->engine, "16k_zh");
    DWORD sz; unsigned char *data = read_file_w(iniPath, &sz);
    if (!data) return;
    char *p = (char*)data; char line[512]; int li = 0;
    for (DWORD i = 0; i <= sz; i++) {
        char c = (i < sz) ? p[i] : '\n';
        if (c == '\n') {
            line[li] = 0; li = 0;
            char *eq = strchr(line, '=');
            if (eq && line[0] != '#' && line[0] != ';') {
                *eq = 0; char *k = line; char *v = eq + 1;
                trim(k); trim(v);
                if (!_stricmp(k, "SecretId")) strncpy(cfg->secretId, v, sizeof(cfg->secretId)-1);
                else if (!_stricmp(k, "SecretKey")) strncpy(cfg->secretKey, v, sizeof(cfg->secretKey)-1);
                else if (!_stricmp(k, "Region")) strncpy(cfg->region, v, sizeof(cfg->region)-1);
                else if (!_stricmp(k, "EngineModelType")) strncpy(cfg->engine, v, sizeof(cfg->engine)-1);
            }
        } else if (li < (int)sizeof(line)-1) {
            line[li++] = c;
        }
    }
    free(data);
}

/* 保存配置到 ini（UTF-8）。成功返回 1 */
static int save_config(const wchar_t *iniPath, const Config *cfg) {
    char buf[2048];
    int n = snprintf(buf, sizeof(buf),
        "# 腾讯云语音识别（ASR）配置——由程序自动保存\r\n"
        "SecretId=%s\r\n"
        "SecretKey=%s\r\n"
        "Region=%s\r\n"
        "EngineModelType=%s\r\n",
        cfg->secretId, cfg->secretKey, cfg->region, cfg->engine);
    HANDLE h = CreateFileW(iniPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD wn; WriteFile(h, buf, (DWORD)n, &wn, NULL);
    CloseHandle(h);
    return 1;
}

/* ============================ 腾讯云 TC3 签名调用 ============================ */

#define ASR_HOST "asr.tencentcloudapi.com"
#define ASR_SERVICE "asr"
#define ASR_VERSION "2019-06-14"

static char* tc3_call(const Config *cfg, const char *action,
                      const char *payload, DWORD payloadLen, DWORD *outLen) {
    *outLen = 0;
    long long ts = (long long)time(NULL);
    time_t tt = (time_t)ts;
    struct tm gmt; gmtime_s(&gmt, &tt);
    char date[16];
    sprintf(date, "%04d-%02d-%02d", gmt.tm_year + 1900, gmt.tm_mon + 1, gmt.tm_mday);

    unsigned char h[32]; char hashedPayload[65];
    if (!sha256((const unsigned char*)payload, payloadLen, h)) return NULL;
    hex_encode(h, 32, hashedPayload);

    char canonical[1024];
    snprintf(canonical, sizeof(canonical),
        "POST\n/\n\ncontent-type:application/json; charset=utf-8\nhost:%s\n\ncontent-type;host\n%s",
        ASR_HOST, hashedPayload);

    unsigned char hc[32]; char hashedCanonical[65];
    if (!sha256((const unsigned char*)canonical, (DWORD)strlen(canonical), hc)) return NULL;
    hex_encode(hc, 32, hashedCanonical);

    char credScope[128];
    snprintf(credScope, sizeof(credScope), "%s/%s/tc3_request", date, ASR_SERVICE);
    char stringToSign[512];
    snprintf(stringToSign, sizeof(stringToSign),
        "TC3-HMAC-SHA256\n%lld\n%s\n%s", ts, credScope, hashedCanonical);

    unsigned char kSecret[300]; int kSecretLen = snprintf((char*)kSecret, sizeof(kSecret), "TC3%s", cfg->secretKey);
    unsigned char kDate[32], kService[32], kSigning[32], sig[32];
    if (!hmac_sha256(kSecret, kSecretLen, (const unsigned char*)date, (DWORD)strlen(date), kDate)) return NULL;
    if (!hmac_sha256(kDate, 32, (const unsigned char*)ASR_SERVICE, (DWORD)strlen(ASR_SERVICE), kService)) return NULL;
    if (!hmac_sha256(kService, 32, (const unsigned char*)"tc3_request", 11, kSigning)) return NULL;
    if (!hmac_sha256(kSigning, 32, (const unsigned char*)stringToSign, (DWORD)strlen(stringToSign), sig)) return NULL;
    char signature[65]; hex_encode(sig, 32, signature);

    char headersA[2048];
    snprintf(headersA, sizeof(headersA),
        "Content-Type: application/json; charset=utf-8\r\n"
        "Authorization: TC3-HMAC-SHA256 Credential=%s/%s, SignedHeaders=content-type;host, Signature=%s\r\n"
        "X-TC-Action: %s\r\n"
        "X-TC-Timestamp: %lld\r\n"
        "X-TC-Version: %s\r\n"
        "X-TC-Region: %s\r\n",
        cfg->secretId, credScope, signature, action, ts, ASR_VERSION, cfg->region);

    wchar_t headersW[2048];
    MultiByteToWideChar(CP_UTF8, 0, headersA, -1, headersW, 2048);

    return https_post(L"" _CRT_WIDE(ASR_HOST), L"/", headersW, payload, payloadLen, outLen);
}

/* ============================ 文本 / 文件名 工具 ============================ */

static void extract_url(const char *text, char *out, int maxlen) {
    out[0] = 0;
    const char *p = strstr(text, "http");
    if (!p) { strncpy(out, text, maxlen-1); out[maxlen-1]=0; trim(out); return; }
    int i = 0;
    while (p[i] && p[i] != ' ' && p[i] != '\r' && p[i] != '\n' && p[i] != '\t'
           && p[i] != '"' && p[i] != '\'' && i < maxlen - 1) {
        out[i] = p[i]; i++;
    }
    out[i] = 0;
}

static void clean_transcript(const char *raw, char *out, int maxlen) {
    int oi = 0; const char *p = raw;
    while (*p && oi < maxlen - 1) {
        if (*p == '[') { const char *q = strchr(p, ']'); if (q) { p = q + 1; while (*p==' ') p++; continue; } }
        out[oi++] = *p++;
    }
    out[oi] = 0;
}

/* 对链接做 URL 编码（保留非保留字符，其余 %XX） */
static void url_encode(const char *in, char *out, int cap) {
    int j = 0;
    for (int i = 0; in[i] && j < cap - 4; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') {
            out[j++] = (char)c;
        } else {
            sprintf(out + j, "%%%02X", c); j += 3;
        }
    }
    out[j] = 0;
}

/* 判断是否为直链媒体文件（可直接下载，自动判断，无需手动选平台） */
static int is_direct_media(const char *url) {
    char low[512]; int n = (int)strlen(url); if (n >= (int)sizeof(low)) n = (int)sizeof(low) - 1;
    for (int i = 0; i < n; i++) low[i] = (char)tolower((unsigned char)url[i]);
    low[n] = 0;
    char *q = strchr(low, '?'); if (q) *q = 0;            /* 去掉查询串 */
    return (strstr(low, ".mp4") || strstr(low, ".m3u8") || strstr(low, ".webm") ||
            strstr(low, ".mov") || strstr(low, ".m4v") || strstr(low, ".flv"));
}

/* 清理文件名中的非法字符（就地修改，UTF-8 安全：只删 Windows 禁用字符） */
static void sanitize_filename(char *s) {
    static const char *bad = "\\/:*?\"<>|\r\n\t";
    int o = 0;
    for (int i = 0; s[i]; i++) {
        if (strchr(bad, s[i])) continue;
        if ((unsigned char)s[i] < 0x20) continue;
        s[o++] = s[i];
    }
    s[o] = 0;
    /* 去掉首尾空格/点 */
    while (o > 0 && (s[o-1]==' '||s[o-1]=='.')) s[--o] = 0;
}

/* 从直链推导文件名（UTF-8），cap 足够 */
static void make_filename_from_url(const char *url, char *out, int cap) {
    char low[1024]; int n = (int)strlen(url); if (n >= (int)sizeof(low)) n = (int)sizeof(low) - 1;
    for (int i = 0; i < n; i++) low[i] = (char)tolower((unsigned char)url[i]);
    low[n] = 0;
    char *q = strchr(low, '?'); if (q) *q = 0;
    const char *slash = strrchr(url, '/');
    const char *name = slash ? slash + 1 : url;
    if (!name[0]) name = "video";
    strncpy(out, name, cap - 1); out[cap - 1] = 0;
    sanitize_filename(out);
    if (out[0] == 0) strcpy(out, "video");
    if (!strchr(out, '.')) strcat(out, ".mp4");
}

/* 去掉扩展名（就地），返回是否原本带扩展名 */
static int strip_ext(char *s) {
    char *dot = strrchr(s, '.');
    if (dot && dot != s) { *dot = 0; return 1; }
    return 0;
}

/* 小写扩展名提取（UTF-8 输入，输出小写，不含点） */
static void get_ext(const char *path, char *out, int cap) {
    out[0] = 0;
    const char *dot = strrchr(path, '.');
    if (!dot) return;
    int i = 0; dot++;
    while (dot[i] && dot[i] != '?' && i < cap - 1) { out[i] = (char)tolower((unsigned char)dot[i]); i++; }
    out[i] = 0;
}

/* 是否为常见可直接送 ASR 的音频扩展名 */
static int is_audio_ext(const char *ext) {
    return (_stricmp(ext, "mp3") == 0 || _stricmp(ext, "wav") == 0 ||
            _stricmp(ext, "m4a") == 0 || _stricmp(ext, "aac") == 0 ||
            _stricmp(ext, "amr") == 0 || _stricmp(ext, "flac") == 0 ||
            _stricmp(ext, "wma") == 0 || _stricmp(ext, "ogg") == 0);
}

/* 是否为可用 MP4 解复用提取音轨的视频扩展名 */
static int is_mp4_video_ext(const char *ext) {
    return (_stricmp(ext, "mp4") == 0 || _stricmp(ext, "m4v") == 0 ||
            _stricmp(ext, "mov") == 0);
}

/* 拼接保存路径：videoPath 用 fnameW，audio/txt 用 baseW（已去扩展名） */
static void build_save_paths(const wchar_t *saveDir, const wchar_t *fnameW,
                             const wchar_t *baseW,
                             wchar_t *videoPath, wchar_t *audioPath, wchar_t *txtPath) {
    wsprintfW(videoPath, L"%s%s", saveDir, fnameW);
    wsprintfW(audioPath, L"%s%s_audio.aac", saveDir, baseW);
    wsprintfW(txtPath,   L"%s%s_transcript.txt", saveDir, baseW);
}

/* ============================ GUI 辅助 ============================ */

#define WM_APP_DONE (WM_APP + 1)

/* 读控件文本为 UTF-8。返回字节数 */
static int get_text_utf8(HWND h, char *out, int cap) {
    int wl = GetWindowTextLengthW(h);
    wchar_t *w = (wchar_t*)malloc((wl + 1) * sizeof(wchar_t));
    if (!w) { out[0] = 0; return 0; }
    GetWindowTextW(h, w, wl + 1);
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, 0, 0);
    free(w);
    if (n <= 0) out[0] = 0;
    return n;
}

/* 设置控件文本（UTF-8 -> 宽字符） */
static void set_text_utf8(HWND h, const char *s) {
    int wl = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t*)malloc((wl > 0 ? wl : 1) * sizeof(wchar_t));
    if (!w) return;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, wl);
    SetWindowTextW(h, w);
    free(w);
}

/* 把结果文字（UTF-8，可能含 \n）设置到结果框：统一换行为 \r\n 便于 Edit 显示 */
static void set_result_utf8(const char *s) {
    int len = (int)strlen(s);
    char *tmp = (char*)malloc((size_t)len * 2 + 2);
    if (!tmp) return;
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] == '\n' && (i == 0 || s[i-1] != '\r')) { tmp[j++] = '\r'; tmp[j++] = '\n'; }
        else tmp[j++] = s[i];
    }
    tmp[j] = 0;
    set_text_utf8(g_hResult, tmp);
    free(tmp);
}

/* ============================ 工作参数 ============================ */

typedef struct {
    Config cfg;
    int mode;                /* MODE_* */
    char link[MAX_URL];
    wchar_t localPath[MAX_PATH];
    wchar_t saveDir[MAX_PATH];   /* 末尾带反斜杠 */
} WorkParams;

/* ============================ 大音频自动分片 ============================ */
/* 腾讯云录音文件识别（SourceType=1 直传 Data）单文件上限 5MB，超出需分片。
   分片必须在音频帧边界切，否则解码失败；这里按 ADTS AAC / MP3 帧、或 WAV 重建头。 */
#define ASR_MAX_CHUNK (4u * 1024u * 1024u)   /* 4MB，留安全余量（< 5MB） */
#define ASR_MAX_CHUNKS 64

typedef enum { AF_UNKNOWN = 0, AF_AAC_ADTS, AF_MP3, AF_WAV } AudioFmt;

/* 由音频字节特征判断格式（优于看扩展名，提取出的 AAC 不带扩展名信息） */
static AudioFmt detect_audio_fmt(const unsigned char *d, DWORD len) {
    if (len >= 2 && d[0] == 0xFF && (d[1] & 0xF0) == 0xF0) {
        /* 同步字 0xFFF：ADTS 的 layer 位(比特2:1)==00，MP3 的 layer 位非 00 */
        return (d[1] & 0x06) == 0x00 ? AF_AAC_ADTS : AF_MP3;
    }
    if (len >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WAVE", 4) == 0)
        return AF_WAV;
    return AF_UNKNOWN;
}

/* 在 [pos,len) 内找下一个 MP3 帧同步字偏移；找不到返回 len */
static DWORD find_next_mp3_sync(const unsigned char *d, DWORD pos, DWORD len) {
    for (DWORD i = pos + 1; i + 1 < len; i++) {
        if (d[i] == 0xFF && (d[i+1] & 0xE0) == 0xE0 && (d[i+1] & 0x06) != 0x00)
            return i;
    }
    return len;
}

/* 把音频切成每块 <= ASR_MAX_CHUNK 的分片，返回分片数；分片缓冲写入 outBuf/outLen
   （调用方需逐个 free）。WAV 会为每个分片重建 WAV 头；无法识别格式则整文件作为 1 个分片。 */
static int split_audio(const unsigned char *data, DWORD len, AudioFmt fmt,
                       unsigned char **outBuf, DWORD *outLen, int maxOut) {
    int n = 0;
    if (fmt == AF_WAV) {
        DWORD p = 12, dataOff = 0, pcmLen = 0;
        while (p + 8 <= len) {
            DWORD ckSize = (DWORD)data[p+4] | ((DWORD)data[p+5]<<8) |
                           ((DWORD)data[p+6]<<16) | ((DWORD)data[p+7]<<24);
            if (memcmp(data + p, "data", 4) == 0) { dataOff = p + 8; pcmLen = ckSize; break; }
            p += 8 + ckSize + (ckSize & 1);
        }
        if (dataOff == 0 || pcmLen == 0 || dataOff + pcmLen > len) {  /* 退化：整文件 */
            unsigned char *b = malloc(len ? len : 1);
            if (b && len) memcpy(b, data, len);
            outBuf[0] = b; outLen[0] = len; return (b ? 1 : 0);
        }
        DWORD perChunk = ASR_MAX_CHUNK;
        int nchunks = (int)((pcmLen + perChunk - 1) / perChunk);
        if (nchunks > maxOut) nchunks = maxOut;
        for (int i = 0; i < nchunks; i++) {
            DWORD start = (DWORD)i * perChunk;
            DWORD clen = (i == nchunks - 1) ? (pcmLen - start) : perChunk;
            DWORD hdr = dataOff;                       /* 复用原始 WAV 头（含 fmt） */
            unsigned char *b = malloc(hdr + clen);
            if (!b) break;
            memcpy(b, data, hdr);
            DWORD riff = 36 + clen, dsize = clen;       /* 修正 RIFF 大小与 data 大小 */
            b[4] = (unsigned char)(riff & 0xFF);  b[5] = (unsigned char)((riff>>8)&0xFF);
            b[6] = (unsigned char)((riff>>16)&0xFF); b[7] = (unsigned char)((riff>>24)&0xFF);
            b[dataOff-4] = (unsigned char)(dsize & 0xFF); b[dataOff-3] = (unsigned char)((dsize>>8)&0xFF);
            b[dataOff-2] = (unsigned char)((dsize>>16)&0xFF); b[dataOff-1] = (unsigned char)((dsize>>24)&0xFF);
            memcpy(b + hdr, data + dataOff + start, clen);
            outBuf[n] = b; outLen[n] = hdr + clen; n++;
        }
        return n;
    }

    /* 帧格式（ADTS / MP3 / 未知整文件）：按帧边界累积成 <= ASR_MAX_CHUNK 的分片 */
    DWORD pos = 0, segStart = 0, segLen = 0;
    while (pos < len && n < maxOut) {
        DWORD fend;
        if (fmt == AF_AAC_ADTS) {
            if (pos + 7 > len) fend = len;
            else {
                DWORD fl = ((DWORD)(data[pos+3] & 0x03) << 11) |
                           ((DWORD)data[pos+4] << 3) |
                           ((DWORD)(data[pos+5] >> 5) & 0x07);
                if (fl < 7) fl = 7;
                fend = pos + fl;
            }
        } else if (fmt == AF_MP3) {
            fend = find_next_mp3_sync(data, pos, len);
        } else {
            fend = len;                                  /* 未知：整文件一帧 */
        }
        if (fend <= pos || fend > len) fend = len;
        DWORD frameLen = fend - pos;
        if (segLen > 0 && segLen + frameLen > ASR_MAX_CHUNK) {
            unsigned char *b = malloc(segLen);
            if (!b) break;
            memcpy(b, data + segStart, segLen);
            outBuf[n] = b; outLen[n] = segLen; n++;
            segStart = pos; segLen = 0;
        }
        if (segLen == 0) segStart = pos;
        segLen += frameLen;
        pos = fend;
    }
    if (segLen > 0 && n < maxOut) {
        unsigned char *b = malloc(segLen);
        if (b) { memcpy(b, data + segStart, segLen); outBuf[n] = b; outLen[n] = segLen; n++; }
    }
    if (n == 0) {                                        /* 兜底 */
        unsigned char *b = malloc(len ? len : 1);
        if (b && len) memcpy(b, data, len);
        outBuf[0] = b; outLen[0] = len; return (b ? 1 : 0);
    }
    return n;
}

/* 安全地把 src 追加到 dst（addSep 时先加一个换行），受 cap 限制 */
static void append_text(char *dst, int cap, const char *src, int addSep) {
    int cur = (int)strlen(dst);
    int avail = cap - cur - 1;
    if (avail <= 0) return;
    if (addSep) { if (avail >= 2) { strcat(dst, "\n"); cur++; avail--; } }
    int srcl = (int)strlen(src);
    if (srcl > avail) srcl = avail;
    memcpy(dst + cur, src, srcl);
    dst[cur + srcl] = 0;
}

/* ============================ ASR 公共管线（数据 -> 文字） ============================ */
/* 成功返回 1，结果写入 transcript（UTF-8）；失败返回 0 */
static int do_asr(Config *cfg, const unsigned char *data, DWORD len,
                  char *transcript, int cap) {
    logln("  提交音频到腾讯云（%u 字节）…", len);
    char *b64 = base64_encode(data, len);
    if (!b64) { logln("音频编码失败。"); return 0; }

    DWORD payloadCap = (DWORD)strlen(b64) + 512;
    char *payload = (char*)malloc(payloadCap);
    if (!payload) { free(b64); logln("内存不足。"); return 0; }
    snprintf(payload, payloadCap,
        "{\"EngineModelType\":\"%s\",\"ChannelNum\":1,\"ResTextFormat\":0,"
        "\"SourceType\":1,\"DataLen\":%u,\"Data\":\"%s\"}",
        cfg->engine, (unsigned)len, b64);
    free(b64);

    DWORD respLen = 0;
    char *r1 = tc3_call(cfg, "CreateRecTask", payload, (DWORD)strlen(payload), &respLen);
    free(payload);
    if (!r1) { logln("提交识别任务失败：网络或签名错误。"); return 0; }

    char errCode[128] = {0};
    if (json_str(r1, "\"Code\"", errCode, sizeof(errCode)) && errCode[0]) {
        char errMsg[512] = {0};
        json_str(r1, "\"Message\"", errMsg, sizeof(errMsg));
        logln("  接口返回错误：%s - %s", errCode, errMsg);
        free(r1); return 0;
    }
    long long taskId = 0;
    if (!json_num(r1, "\"TaskId\"", &taskId) || taskId == 0) {
        logln("  未取得 TaskId。返回：%.400s", r1);
        free(r1); return 0;
    }
    free(r1);
    logln("  任务已提交，TaskId=%lld，等待识别结果…", taskId);

    int okDone = 0;
    for (int attempt = 1; attempt <= 60 && !okDone; attempt++) {
        Sleep(3000);
        char qbody[128];
        snprintf(qbody, sizeof(qbody), "{\"TaskId\":%lld}", taskId);
        DWORD ql = 0;
        char *r2 = tc3_call(cfg, "DescribeTaskStatus", qbody, (DWORD)strlen(qbody), &ql);
        if (!r2) { logln("  第 %d 次查询失败，重试…", attempt); continue; }
        long long status = -1;
        json_num(r2, "\"Status\"", &status);
        if (status == 2) {
            char *raw = (char*)malloc(1<<20);
            if (raw) {
                if (json_str(r2, "\"Result\"", raw, 1<<20))
                    clean_transcript(raw, transcript, cap);
                free(raw);
            }
            okDone = 1;
            logln("  识别完成！");
        } else if (status == 3) {
            char em[512] = {0};
            json_str(r2, "\"ErrorMsg\"", em, sizeof(em));
            logln("  识别失败：%s", em[0] ? em : "(无详情)");
            free(r2); return 0;
        } else {
            logln("  识别中…（第 %d 次轮询，状态=%lld）", attempt, status);
        }
        free(r2);
    }
    if (!okDone) { logln("  识别超时（3 分钟未完成）。"); return 0; }
    return 1;
}

/* 入口：自动处理 > 5MB 的大音频 —— 切片分别识别后合并文字。
   小音频（<= 5MB）直接走 do_asr。成功返回 1，合并结果写入 transcript。 */
static int do_asr_auto(Config *cfg, const unsigned char *data, DWORD len,
                       char *transcript, int cap) {
    if (len <= ASR_MAX_CHUNK) {
        return do_asr(cfg, data, len, transcript, cap);
    }
    logln("  音频 %.2fMB 超过 5MB 限制，将自动分片识别后合并…", len / 1048576.0);
    AudioFmt fmt = detect_audio_fmt(data, len);
    if (fmt == AF_UNKNOWN) {
        logln("  当前音频格式暂不支持自动分片（仅 ADTS AAC / MP3 / WAV）。"
              "尝试整段提交；若腾讯云拒绝，请先转码为 mp3 或 aac 再试。");
        return do_asr(cfg, data, len, transcript, cap);
    }
    unsigned char *chunks[ASR_MAX_CHUNKS]; DWORD lens[ASR_MAX_CHUNKS];
    int n = split_audio(data, len, fmt, chunks, lens, ASR_MAX_CHUNKS);
    if (n <= 0) { logln("  分片失败。"); return 0; }
    logln("  共分为 %d 个分片（每块 < 5MB），逐片识别并合并。", n);

    char part[1<<20];
    int first = 1;
    for (int i = 0; i < n; i++) {
        part[0] = 0;
        logln("  [分片 %d/%d] 大小 %u 字节", i + 1, n, lens[i]);
        if (!do_asr(cfg, chunks[i], lens[i], part, (int)sizeof(part))) {
            logln("  分片 %d 识别失败，终止合并。", i + 1);
            for (int j = 0; j <= i; j++) free(chunks[j]);
            return 0;
        }
        append_text(transcript, cap, part, !first);
        first = 0;
    }
    for (int i = 0; i < n; i++) free(chunks[i]);
    logln("  分片识别完成，已合并为完整文字。");
    return 1;
}

/* 把 transcript 存为带 BOM 的 UTF-8 文本 */
static void save_transcript_file(const wchar_t *txtPath, const char *transcript) {
    HANDLE hf = CreateFileW(txtPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf != INVALID_HANDLE_VALUE) {
        DWORD wn;
        const unsigned char bom[3] = {0xEF,0xBB,0xBF};
        WriteFile(hf, bom, 3, &wn, NULL);
        WriteFile(hf, transcript, (DWORD)strlen(transcript), &wn, NULL);
        CloseHandle(hf);
        logln("");
        logln("已保存文字 -> %ls", txtPath);
    }
}

/* ============================ 模式 B：本地文件 -> 文字 ============================ */

static void local_asr_pipeline(WorkParams *wp) {
    Config *cfg = &wp->cfg;
    logln("本地文件 → 腾讯云录音转文字");
    logln("引擎模型：%s   地域：%s", cfg->engine, cfg->region);
    logln("本地文件：%ls", wp->localPath);

    char ext[16] = {0};
    {   /* 取本地路径扩展名（宽字符 -> UTF-8） */
        char pathUtf8[MAX_PATH] = {0};
        WideCharToMultiByte(CP_UTF8, 0, wp->localPath, -1, pathUtf8, MAX_PATH, 0, 0);
        get_ext(pathUtf8, ext, sizeof(ext));
    }

    /* 基础名（去扩展名），用于音频/文字文件名 */
    wchar_t baseW[MAX_PATH] = {0};
    {   const wchar_t *name = wcsrchr(wp->localPath, L'\\');
        if (!name) name = wcsrchr(wp->localPath, L'/');
        if (name) name++; else name = wp->localPath;
        const wchar_t *dot = wcsrchr(name, L'.');
        int n = dot ? (int)(dot - name) : (int)wcslen(name);
        if (n >= MAX_PATH) n = MAX_PATH - 1;
        wcsncpy(baseW, name, n); baseW[n] = 0;
    }

    wchar_t audioPath[MAX_PATH], txtPath[MAX_PATH];
    wsprintfW(audioPath, L"%s%s_audio.aac", wp->saveDir, baseW);
    wsprintfW(txtPath,   L"%s%s_transcript.txt", wp->saveDir, baseW);

    unsigned char *data = NULL; DWORD len = 0;
    int needFreeData = 0;

    if (is_audio_ext(ext)) {
        logln("");
        logln("[1/2] 读取本地音频文件…");
        data = read_file_w(wp->localPath, &len);
        if (!data || len == 0) { logln("读取文件失败。"); return; }
        logln("  音频 %u 字节，扩展名 .%s", len, ext);
    } else if (is_mp4_video_ext(ext)) {
        logln("");
        logln("[1/2] 读取本地视频并提取 AAC 音频…");
        DWORD mp4len; unsigned char *mp4 = read_file_w(wp->localPath, &mp4len);
        if (!mp4) { logln("读取视频文件失败。"); return; }
        int sr = 0;
        int ok = mp4_extract_aac(mp4, mp4len, audioPath, &sr);
        free(mp4);
        if (!ok) { logln("音频提取失败（可能不是标准 MP4/AAC 封装）。"); return; }
        data = read_file_w(audioPath, &len);
        if (!data || len == 0) { logln("读取提取出的音频失败。"); return; }
        needFreeData = 1;
    } else {
        logln("");
        logln("不支持的格式：. %s", ext[0] ? ext : "(未知)");
        logln("本地转文字仅支持：MP4/M4V/MOV 视频，以及 mp3/wav/m4a/aac/amr/flac/wma/ogg 音频。");
        logln("其它格式请先用工具转码后再试。");
        return;
    }

    logln("");
    logln("[2/2] 调用腾讯云录音转文字…");
    char *transcript = (char*)malloc(4<<20);
    if (!transcript) { if (needFreeData) free(data); logln("内存不足。"); return; }
    transcript[0] = 0;
    if (do_asr_auto(cfg, data, len, transcript, 4<<20)) {
        set_result_utf8(transcript[0] ? transcript : "(空)");
        save_transcript_file(txtPath, transcript[0] ? transcript : "");
        logln("本地转文字完成。");
    } else {
        logln("识别失败。");
    }
    free(transcript);
    if (needFreeData) free(data);
}

/* ============================ 模式 A / C：在线视频（转文字 / 仅下载） ============================ */

static void online_pipeline(WorkParams *wp) {
    Config *cfg = &wp->cfg;
    int isDownload = (wp->mode == MODE_DOWNLOAD);

    logln(isDownload ? "在线视频 → 仅下载" : "在线视频 → 音频 → 腾讯云录音转文字");
    logln("引擎模型：%s   地域：%s", cfg->engine, cfg->region);

    char videoUrl[MAX_URL] = {0};
    char title[512] = {0};
    int direct = 0;

    if (is_direct_media(wp->link)) direct = 1;   /* 直链媒体文件：自动直接下载，无需解析 */

    /* ---- 1. 解析直链 ---- */
    logln("");
    if (isDownload) logln("[1/2] 解析链接…");
    else            logln("[1/4] 解析链接…");
    logln("  链接：%s", wp->link);

    if (direct) {
        strcpy(videoUrl, wp->link);
        logln("  直链媒体文件，跳过解析，直接下载。");
    } else {
        char api[MAX_URL];
        strcpy(api, PARSE_BASE);
        url_encode(wp->link, api + strlen(api), (int)(MAX_URL - strlen(api)));
        wchar_t wapi[MAX_URL];
        MultiByteToWideChar(CP_UTF8, 0, api, -1, wapi, MAX_URL);
        DWORD rlen; char *resp = http_get(wapi, &rlen);
        if (!resp || rlen == 0) {
            logln("解析失败：网络错误或接口不可用。");
            if (resp) free(resp);
            return;
        }
        /* 聚合接口返回 {"code":200,"msg":"...","data":{...,"url":"视频直链","title":"...","desc":"..."}} */
        char codeStr[16] = {0};
        if (json_str(resp, "\"code\"", codeStr, sizeof(codeStr)) && atoi(codeStr) != 200) {
            char msg[256] = {0};
            json_str(resp, "\"msg\"", msg, sizeof(msg));
            logln("解析失败（接口返回 %s）：%s", codeStr, msg[0] ? msg : "(无详情)");
            free(resp); return;
        }
        json_str(resp, "\"url\"",   videoUrl, sizeof(videoUrl)); /* 优先 data.url（视频直链） */
        if (!title[0]) json_str(resp, "\"title\"", title, sizeof(title));
        if (!title[0]) json_str(resp, "\"desc\"",  title, sizeof(title));
        if (!videoUrl[0]) {
            logln("未解析到视频直链（该链接可能是图集/直播，无法作为视频下载）。");
            logln("接口返回：%.400s", resp);
            free(resp); return;
        }
        if (title[0]) logln("  标题：%s", title);
        logln("  视频直链：%.80s...", videoUrl);
        free(resp);
    }

    /* ---- 文件名（视频） ---- */
    char fname[MAX_PATH] = {0};
    if (title[0]) { strncpy(fname, title, sizeof(fname) - 1); sanitize_filename(fname); }
    if (fname[0] == 0) make_filename_from_url(videoUrl, fname, sizeof(fname));
    if (fname[0] == 0) strcpy(fname, "video.mp4");
    if (!strchr(fname, '.')) strcat(fname, ".mp4");

    char baseNoExt[MAX_PATH];
    strncpy(baseNoExt, fname, sizeof(baseNoExt) - 1); baseNoExt[sizeof(baseNoExt)-1] = 0;
    strip_ext(baseNoExt);

    wchar_t videoPath[MAX_PATH], audioPath[MAX_PATH], txtPath[MAX_PATH];
    {
        wchar_t fnameW[MAX_PATH], baseW[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, fname, -1, fnameW, MAX_PATH);
        MultiByteToWideChar(CP_UTF8, 0, baseNoExt, -1, baseW, MAX_PATH);
        build_save_paths(wp->saveDir, fnameW, baseW, videoPath, audioPath, txtPath);
    }

    /* ---- 2. 下载视频 ---- */
    logln("");
    if (isDownload) logln("[2/2] 下载视频…");
    else            logln("[2/4] 下载视频…");
    wchar_t wvurl[MAX_URL];
    MultiByteToWideChar(CP_UTF8, 0, videoUrl, -1, wvurl, MAX_URL);
    DWORD vbytes = http_download_file(wvurl, videoPath);
    if (vbytes == 0) { logln("视频下载失败。"); return; }
    logln("  已下载 %.2f MB -> %ls", vbytes / 1048576.0, videoPath);

    /* ---- 模式 C：仅下载，结束 ---- */
    if (isDownload) {
        logln("");
        logln("下载完成（仅视频，未做转文字）。");
        return;
    }

    /* ---- 3. 提取音频 ---- */
    logln("");
    logln("[3/4] 从 MP4 提取 AAC 音频…");
    DWORD mp4len; unsigned char *mp4data = read_file_w(videoPath, &mp4len);
    if (!mp4data) { logln("读取视频文件失败。"); return; }
    int sampleRate = 0;
    int okAac = mp4_extract_aac(mp4data, mp4len, audioPath, &sampleRate);
    free(mp4data);
    if (!okAac) {
        logln("音频提取失败（可能不是标准 MP4/AAC 封装，或视频无音轨）。");
        return;
    }
    logln("  已保存音频 -> %ls", audioPath);

    /* ---- 4. 腾讯云录音文件识别 ---- */
    logln("");
    logln("[4/4] 调用腾讯云录音转文字…");
    DWORD aacLen; unsigned char *aacData = read_file_w(audioPath, &aacLen);
    if (!aacData || aacLen == 0) { logln("读取音频失败。"); if (aacData) free(aacData); return; }

    char *transcript = (char*)malloc(4<<20);
    if (!transcript) { free(aacData); logln("内存不足。"); return; }
    transcript[0] = 0;
    int ok = do_asr_auto(cfg, aacData, aacLen, transcript, 4<<20);
    free(aacData);
    if (!ok) { free(transcript); return; }

    set_result_utf8(transcript[0] ? transcript : "(空)");
    save_transcript_file(txtPath, transcript[0] ? transcript : "");
    free(transcript);
    logln("");
    logln("全部完成。");
}

/* ============================ 工作线程 ============================ */

static DWORD WINAPI worker_thread(LPVOID arg) {
    WorkParams *wp = (WorkParams*)arg;
    logln("========================================");
    if (wp->mode == MODE_LOCAL_TEXT)
        local_asr_pipeline(wp);
    else
        online_pipeline(wp);
    logln("========================================");
    PostMessageW(g_hMain, WM_APP_DONE, 0, 0);
    free(wp);
    return 0;
}

/* ============================ 交互动作 ============================ */

/* 从各控件收集配置 */
static void collect_config(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    get_text_utf8(g_hId, cfg->secretId, sizeof(cfg->secretId));
    get_text_utf8(g_hKey, cfg->secretKey, sizeof(cfg->secretKey));
    get_text_utf8(g_hRegion, cfg->region, sizeof(cfg->region));
    get_text_utf8(g_hEngine, cfg->engine, sizeof(cfg->engine));
    trim(cfg->secretId); trim(cfg->secretKey); trim(cfg->region); trim(cfg->engine);
    if (!cfg->region[0]) strcpy(cfg->region, "ap-guangzhou");
    if (!cfg->engine[0]) strcpy(cfg->engine, "16k_zh");
}

/* 浏览选择保存文件夹，写回 g_hSave */
static void browse_folder(void) {
    BROWSEINFOW bi = {0};
    bi.hwndOwner = g_hMain;
    bi.lpszTitle = L"选择视频/音频/文字的保存位置";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(g_hSave, path);
        CoTaskMemFree(pidl);
    }
}

/* 浏览选择本地文件（视频/音频），写回 g_hLocal */
static void browse_local_file(void) {
    wchar_t path[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hMain;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"视频/音频文件\0*.mp4;*.m4v;*.mov;*.mkv;*.webm;*.avi;*.flv;*.mp3;*.wav;*.m4a;*.aac;*.amr;*.flac;*.ogg;*.wma\0"
                     L"所有文件\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) SetWindowTextW(g_hLocal, path);
}

/* 复制结果框内容到剪贴板 */
static void copy_result(void) {
    int wl = GetWindowTextLengthW(g_hResult);
    if (wl <= 0) return;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (size_t)(wl + 1) * sizeof(wchar_t));
    if (!hMem) return;
    wchar_t *pMem = (wchar_t*)GlobalLock(hMem);
    GetWindowTextW(g_hResult, pMem, wl + 1);
    GlobalUnlock(hMem);
    if (OpenClipboard(g_hMain)) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, hMem);
        CloseClipboard();
    } else {
        GlobalFree(hMem);
    }
}

/* 根据当前模式显示/隐藏控件并更新开始按钮文字 */
static void update_ui_for_mode(void) {
    int showOnline = (g_mode == MODE_ONLINE_TEXT || g_mode == MODE_DOWNLOAD);
    int showLocal  = (g_mode == MODE_LOCAL_TEXT);
    ShowWindow(g_hLblLink,   showOnline ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hLink,      showOnline ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hLblLocal,  showLocal  ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hLocal,     showLocal  ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hLocBrowse, showLocal  ? SW_SHOW : SW_HIDE);

    const wchar_t *lbl =
        (g_mode == MODE_DOWNLOAD)   ? L"开始：仅下载视频" :
        (g_mode == MODE_LOCAL_TEXT) ? L"开始：本地文件 → 转文字" :
                                      L"开始：下载 → 提取音频 → 转文字";
    SetWindowTextW(g_hBtnStart, lbl);
}

static void start_process(void) {
    Config cfg; collect_config(&cfg);
    int needCreds = (g_mode != MODE_DOWNLOAD);

    if (needCreds && (!cfg.secretId[0] || !cfg.secretKey[0])) {
        MessageBoxW(g_hMain, L"请先填写 SecretId 和 SecretKey。", L"缺少凭证", MB_ICONWARNING);
        return;
    }

    WorkParams *wp = (WorkParams*)calloc(1, sizeof(WorkParams));
    if (!wp) return;
    wp->cfg = cfg;
    wp->mode = g_mode;

    if (g_mode == MODE_LOCAL_TEXT) {
        GetWindowTextW(g_hLocal, wp->localPath, MAX_PATH);
        if (wp->localPath[0] == 0) {
            MessageBoxW(g_hMain, L"请先选择本地视频/音频文件。", L"缺少文件", MB_ICONWARNING);
            free(wp); return;
        }
    } else {
        char link[MAX_URL] = {0}, rawlink[MAX_URL] = {0};
        get_text_utf8(g_hLink, rawlink, sizeof(rawlink));
        extract_url(rawlink, link, sizeof(link));
        if (!link[0]) {
            MessageBoxW(g_hMain, L"请粘贴视频分享链接（或含链接的整段文字）。", L"缺少链接", MB_ICONWARNING);
            free(wp); return;
        }
        strncpy(wp->link, link, sizeof(wp->link) - 1);
    }

    /* 自动保存凭证到 ini（下载模式也保存，方便切换） */
    if (save_config(g_iniPath, &cfg))
        logln("[配置] 已保存到 %ls", g_iniPath);

    /* 保存目录：用户填的 > exe 目录 */
    wchar_t saveDir[MAX_PATH] = {0};
    GetWindowTextW(g_hSave, saveDir, MAX_PATH);
    if (saveDir[0] == 0) {
        GetModuleFileNameW(NULL, saveDir, MAX_PATH);
        wchar_t *sl = wcsrchr(saveDir, L'\\'); if (sl) *(sl+1) = 0;
    } else {
        size_t n = wcslen(saveDir);
        if (n > 0 && saveDir[n-1] != L'\\' && n < MAX_PATH - 1) { saveDir[n] = L'\\'; saveDir[n+1] = 0; }
    }
    wcsncpy(wp->saveDir, saveDir, MAX_PATH-1);

    EnableWindow(g_hBtnStart, FALSE);
    SetWindowTextW(g_hLog, L"");
    HANDLE hT = CreateThread(NULL, 0, worker_thread, wp, 0, NULL);
    if (hT) CloseHandle(hT);
    else { EnableWindow(g_hBtnStart, TRUE); free(wp); }
}

/* ============================ 窗口过程 ============================ */

static HWND mk_label(HWND parent, const wchar_t *text, int x, int y, int w, int h) {
    HWND hl = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE,
        x, y, w, h, parent, NULL, NULL, NULL);
    if (g_font) SendMessageW(hl, WM_SETFONT, (WPARAM)g_font, TRUE);
    return hl;
}
static HWND mk_edit(HWND parent, int id, int x, int y, int w, int h, DWORD extra) {
    HWND he = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | extra,
        x, y, w, h, parent, (HMENU)(INT_PTR)id, NULL, NULL);
    if (g_font) SendMessageW(he, WM_SETFONT, (WPARAM)g_font, TRUE);
    return he;
}
static HWND mk_btn(HWND parent, int id, const wchar_t *text, int x, int y, int w, int h) {
    HWND hb = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x, y, w, h, parent, (HMENU)(INT_PTR)id, NULL, NULL);
    if (g_font) SendMessageW(hb, WM_SETFONT, (WPARAM)g_font, TRUE);
    return hb;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hMain = hwnd;
        int x = 20, w = 560, lh = 22, eh = 26, y = 14;
        int labW = 90;

        mk_label(hwnd, L"SecretId：", x, y+3, labW, lh);
        g_hId = mk_edit(hwnd, IDC_ID, x+labW, y, w-labW, eh, 0);
        y += 34;
        mk_label(hwnd, L"SecretKey：", x, y+3, labW, lh);
        g_hKey = mk_edit(hwnd, IDC_KEY, x+labW, y, w-labW, eh, ES_PASSWORD);
        y += 34;
        mk_label(hwnd, L"地域 Region：", x, y+3, labW, lh);
        g_hRegion = mk_edit(hwnd, IDC_REGION, x+labW, y, 180, eh, 0);
        mk_label(hwnd, L"引擎 Engine：", x+labW+200, y+3, 90, lh);
        g_hEngine = mk_edit(hwnd, IDC_ENGINE, x+labW+290, y, w-(labW+290), eh, 0);
        y += 40;

        /* 模式选择 */
        mk_label(hwnd, L"模式：", x, y+3, labW, lh);
        CreateWindowW(L"BUTTON", L"在线视频→文字",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            x+labW, y, 130, 24, hwnd, (HMENU)(INT_PTR)IDC_MODE_ONLINE, NULL, NULL);
        CreateWindowW(L"BUTTON", L"本地文件→文字",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            x+labW+140, y, 130, 24, hwnd, (HMENU)(INT_PTR)IDC_MODE_LOCAL, NULL, NULL);
        CreateWindowW(L"BUTTON", L"仅下载视频",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            x+labW+280, y, 110, 24, hwnd, (HMENU)(INT_PTR)IDC_MODE_DL, NULL, NULL);
        CheckRadioButton(hwnd, IDC_MODE_ONLINE, IDC_MODE_DL, IDC_MODE_ONLINE);
        y += 34;

        /* 链接（在线模式）/ 本地文件（本地模式），同一行位置，互斥显示 */
        g_hLblLink = mk_label(hwnd, L"视频链接：", x, y+3, labW, lh);
        g_hLink = mk_edit(hwnd, IDC_LINK, x+labW, y, w-labW, eh, 0);

        g_hLblLocal = mk_label(hwnd, L"本地文件：", x, y+3, labW, lh);
        g_hLocal = mk_edit(hwnd, IDC_LOCAL, x+labW, y, w-labW-90, eh, 0);
        g_hLocBrowse = mk_btn(hwnd, IDC_LOCBROWSE, L"选择文件…", x+w-80, y-1, 80, eh+2);
        y += 34;

        /* 保存位置 */
        mk_label(hwnd, L"保存位置：", x, y+3, labW, lh);
        g_hSave = mk_edit(hwnd, IDC_SAVE, x+labW, y, w-labW-90, eh, 0);
        mk_btn(hwnd, IDC_BROWSE, L"浏览…", x+w-80, y-1, 80, eh+2);
        y += 42;

        g_hBtnStart = mk_btn(hwnd, IDC_START, L"开始：下载 → 提取音频 → 转文字", x, y, 360, 34);
        mk_btn(hwnd, IDC_SAVECFG, L"仅保存密钥", x+372, y, 100, 34);
        y += 44;

        mk_label(hwnd, L"运行日志：", x, y, labW, lh);
        y += 22;
        g_hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            x, y, w, 150, hwnd, (HMENU)0, NULL, NULL);
        if (g_font) SendMessageW(g_hLog, WM_SETFONT, (WPARAM)g_font, TRUE);
        y += 158;

        mk_label(hwnd, L"识别结果（可选中复制）：", x, y, 220, lh);
        mk_btn(hwnd, IDC_COPY, L"复制结果", x+w-180, y-3, 85, 26);
        mk_btn(hwnd, IDC_OPENDIR, L"打开目录", x+w-88, y-3, 88, 26);
        y += 26;
        g_hResult = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            x, y, w, 170, hwnd, (HMENU)0, NULL, NULL);
        if (g_font) SendMessageW(g_hResult, WM_SETFONT, (WPARAM)g_font, TRUE);

        /* 载入已保存配置并填充 */
        Config cfg; load_config(g_iniPath, &cfg);
        set_text_utf8(g_hId, cfg.secretId);
        set_text_utf8(g_hKey, cfg.secretKey);
        set_text_utf8(g_hRegion, cfg.region);
        set_text_utf8(g_hEngine, cfg.engine);

        update_ui_for_mode();
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_MODE_ONLINE) { g_mode = MODE_ONLINE_TEXT; update_ui_for_mode(); }
        else if (id == IDC_MODE_LOCAL)  { g_mode = MODE_LOCAL_TEXT;  update_ui_for_mode(); }
        else if (id == IDC_MODE_DL)     { g_mode = MODE_DOWNLOAD;    update_ui_for_mode(); }
        else if (id == IDC_START) start_process();
        else if (id == IDC_BROWSE) browse_folder();
        else if (id == IDC_LOCBROWSE) browse_local_file();
        else if (id == IDC_COPY) copy_result();
        else if (id == IDC_SAVECFG) {
            Config cfg; collect_config(&cfg);
            if (save_config(g_iniPath, &cfg))
                MessageBoxW(hwnd, L"密钥/配置已保存，下次自动填充。", L"已保存", MB_ICONINFORMATION);
        } else if (id == IDC_OPENDIR) {
            wchar_t dir[MAX_PATH] = {0};
            GetWindowTextW(g_hSave, dir, MAX_PATH);
            if (dir[0] == 0) { GetModuleFileNameW(NULL, dir, MAX_PATH); wchar_t *sl = wcsrchr(dir, L'\\'); if (sl) *(sl+1)=0; }
            ShellExecuteW(hwnd, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
        }
        return 0;
    }
    case WM_APP_DONE:
        EnableWindow(g_hBtnStart, TRUE);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ============================ WinMain ============================ */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hPrev; (void)lpCmd;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    /* 计算 ini 路径（exe 同目录，用于自动保存密钥） */
    GetModuleFileNameW(NULL, g_iniPath, MAX_PATH);
    { wchar_t *sl = wcsrchr(g_iniPath, L'\\'); if (sl) *(sl+1) = 0; }
    wcscat(g_iniPath, L"vd_asr.ini");

    /* 统一字体：微软雅黑 9pt */
    g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"VdAsrGuiWnd";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, L"VdAsrGuiWnd",
        L"多平台视频转文字工具（腾讯云 ASR）",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 620, 740,
        NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (g_font) DeleteObject(g_font);
    CoUninitialize();
    return 0;
}
