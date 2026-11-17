// GPS Assist Data Updater
//
// Sony cameras with built-in GPS (Alpha SLT-A55V / A65V / A77V / A99V, Cyber-shot "V" models,
// Action Cam "V" models, ...) read PRIVATE\SONY\GPS\assistme.dat from the memory card to get a
// fast first position fix. Sony publishes a fresh file every day at
//   control.d-imaging.sony.co.jp/GPS/assistme.dat
// with its MD5 beside it in assistme.md5 (32 hex digits followed by padding bytes).
//
// This program downloads the file, checks the MD5, writes it to the card (temp file, flush,
// rename) and reads it back uncached to prove the card holds the right bytes. The last good
// download is kept in %LOCALAPPDATA%\GpsAssistUpdater so several cards can be updated, or a
// card updated offline, while that copy is still within its 30-day validity.
//
// Test hooks (environment variables): GPSASSIST_SERVER overrides the host name,
// GPSASSIST_CACHE overrides the cache folder.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <dbt.h>

#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <memory>
#include <string>
#include <vector>

#include "resource.h"

#define APP_VERSION L"1.0.0"

namespace {

// ---------------------------------------------------------------------------------------------
// Constants

const wchar_t kDefaultHost[] = L"control.d-imaging.sony.co.jp";
const wchar_t kDatPath[] = L"/GPS/assistme.dat";
const wchar_t kMd5Path[] = L"/GPS/assistme.md5";
const wchar_t kCardName[] = L"assistme.dat";
const wchar_t kCardTemp[] = L"assistme.tmp";
const wchar_t kRegKey[] = L"Software\\GpsAssistUpdater";
const int kValidDays = 30;
const DWORD kMaxDataSize = 8u << 20;  // the real file is about 270 KB

enum {
    IDC_TITLE = 1001, IDC_SUBTITLE, IDC_CARD_LABEL, IDC_CARD, IDC_REFRESH, IDC_SHOWALL,
    IDC_CARD_STATUS, IDC_SERVER_STATUS, IDC_UPDATE, IDC_LANG, IDC_ABOUT, IDC_PROGRESS, IDC_LOG
};
enum { WM_APP_LOG = WM_APP + 1, WM_APP_PROGRESS, WM_APP_SERVER, WM_APP_DONE };
enum { RESULT_FAILED = 0, RESULT_OK = 1, RESULT_NOTHING = 2 };
enum { TIMER_DEVICE = 1 };
enum { STATE_PLAIN = 0, STATE_GOOD, STATE_BAD, STATE_WARN };

// ---------------------------------------------------------------------------------------------
// Strings: [English, Thai]

enum Str {
    S_APP_NAME, S_SUBTITLE, S_CARD, S_REFRESH, S_SHOW_ALL, S_UPDATE, S_LANG_SWITCH, S_ABOUT,
    S_NO_CARD_ITEM, S_INSERT_CARD, S_REMOVABLE, S_FIXED, S_CAMERA_CARD,
    S_CARD_NONE, S_CARD_LATEST, S_CARD_VALID, S_CARD_EXPIRED,
    S_AGE_TODAY, S_AGE_ONE, S_AGE_N,
    S_SERVER_CHECKING, S_SERVER_OK, S_SERVER_FAIL,
    S_WELCOME,
    S_LOG_CHECKING, S_LOG_SERVER, S_LOG_HTTP_FALLBACK, S_LOG_ALREADY, S_LOG_CACHE_SAME,
    S_LOG_DOWNLOADING, S_LOG_DOWNLOADED, S_LOG_BAD_MD5, S_LOG_OFFLINE, S_LOG_OFFLINE_CACHE,
    S_LOG_NO_CACHE, S_LOG_WRITING, S_LOG_WRITE_FAIL, S_LOG_VERIFIED, S_LOG_VERIFY_FAIL,
    S_LOG_DONE, S_LOG_TIP, S_LOG_FAILED,
    S_ASK_FIXED, S_ABOUT_TEXT,
    S_COUNT
};

const wchar_t* const kStrings[S_COUNT][2] = {
    /* S_APP_NAME */ {L"GPS Assist Data Updater",
                      L"โปรแกรมอัปเดตข้อมูล GPS Assist"},
    /* S_SUBTITLE */ {L"For Sony Alpha SLT-A55V / A65V / A77V / A99V and other Sony cameras with built-in GPS",
                      L"สำหรับกล้อง Sony Alpha SLT-A55V / A65V / A77V / A99V และกล้อง Sony รุ่นอื่นที่มี GPS ในตัว"},
    /* S_CARD */ {L"Memory card:", L"การ์ดหน่วยความจำ:"},
    /* S_REFRESH */ {L"Refresh", L"รีเฟรช"},
    /* S_SHOW_ALL */ {L"Show all drives (for card readers that appear as a fixed disk)",
                      L"แสดงทุกไดรฟ์ (สำหรับเครื่องอ่านการ์ดที่แสดงเป็นดิสก์ภายใน)"},
    /* S_UPDATE */ {L"Update GPS Assist Data", L"อัปเดตข้อมูล GPS Assist"},
    /* S_LANG_SWITCH */ {L"ภาษาไทย", L"English"},
    /* S_ABOUT */ {L"About", L"เกี่ยวกับ"},
    /* S_NO_CARD_ITEM */ {L"(no memory card found)", L"(ไม่พบการ์ดหน่วยความจำ)"},
    /* S_INSERT_CARD */ {L"Insert the camera's memory card, or connect the camera by USB in Mass Storage mode.",
                         L"เสียบการ์ดหน่วยความจำของกล้อง หรือต่อกล้องด้วยสาย USB ในโหมด Mass Storage"},
    /* S_REMOVABLE */ {L"removable", L"ถอดได้"},
    /* S_FIXED */ {L"fixed disk", L"ดิสก์ภายใน"},
    /* S_CAMERA_CARD */ {L"camera card", L"การ์ดกล้อง"},
    /* S_CARD_NONE */ {L"This card has no GPS assist data yet.",
                       L"การ์ดนี้ยังไม่มีข้อมูล GPS Assist"},
    /* S_CARD_LATEST */ {L"On card: data from %ls (%ls) - the latest from Sony.",
                         L"ในการ์ด: ข้อมูลวันที่ %ls (%ls) - เป็นข้อมูลล่าสุดจาก Sony แล้ว"},
    /* S_CARD_VALID */ {L"On card: data from %ls (%ls) - still valid.",
                        L"ในการ์ด: ข้อมูลวันที่ %ls (%ls) - ยังใช้งานได้"},
    /* S_CARD_EXPIRED */ {L"On card: data from %ls (%ls) - expired, please update.",
                          L"ในการ์ด: ข้อมูลวันที่ %ls (%ls) - หมดอายุแล้ว กรุณาอัปเดต"},
    /* S_AGE_TODAY */ {L"less than a day old", L"อายุไม่ถึง 1 วัน"},
    /* S_AGE_ONE */ {L"1 day old", L"อายุ 1 วัน"},
    /* S_AGE_N */ {L"%d days old", L"อายุ %d วัน"},
    /* S_SERVER_CHECKING */ {L"Sony server: checking...", L"เซิร์ฟเวอร์ Sony: กำลังตรวจสอบ..."},
    /* S_SERVER_OK */ {L"Sony server: latest data published %ls",
                       L"เซิร์ฟเวอร์ Sony: ข้อมูลล่าสุดเผยแพร่เมื่อ %ls"},
    /* S_SERVER_FAIL */ {L"Sony server: not reachable (%ls)",
                         L"เซิร์ฟเวอร์ Sony: เชื่อมต่อไม่ได้ (%ls)"},
    /* S_WELCOME */ {L"1. Insert the camera's memory card (or connect the camera by USB in Mass Storage mode).\r\n"
                     L"2. Choose it above and click \"Update GPS Assist Data\".\r\n"
                     L"3. Put the card back in the camera and turn the camera on.\r\n",
                     L"1. เสียบการ์ดหน่วยความจำของกล้อง (หรือต่อกล้องด้วยสาย USB ในโหมด Mass Storage)\r\n"
                     L"2. เลือกการ์ดด้านบน แล้วกด \"อัปเดตข้อมูล GPS Assist\"\r\n"
                     L"3. ใส่การ์ดกลับเข้ากล้อง แล้วเปิดกล้อง\r\n"},
    /* S_LOG_CHECKING */ {L"Checking Sony's server...", L"กำลังตรวจสอบเซิร์ฟเวอร์ Sony..."},
    /* S_LOG_SERVER */ {L"Latest data on the server: %ls (MD5 %ls)",
                        L"ข้อมูลล่าสุดบนเซิร์ฟเวอร์: %ls (MD5 %ls)"},
    /* S_LOG_HTTP_FALLBACK */ {L"HTTPS failed (%ls); trying HTTP.",
                               L"HTTPS ใช้ไม่ได้ (%ls) กำลังลองผ่าน HTTP"},
    /* S_LOG_ALREADY */ {L"This card already has the latest data. Nothing to do.",
                         L"การ์ดนี้มีข้อมูลล่าสุดอยู่แล้ว ไม่ต้องอัปเดต"},
    /* S_LOG_CACHE_SAME */ {L"Using the copy already downloaded to this PC (same MD5).",
                            L"ใช้ไฟล์ที่ดาวน์โหลดไว้แล้วในเครื่องนี้ (MD5 ตรงกัน)"},
    /* S_LOG_DOWNLOADING */ {L"Downloading assistme.dat...", L"กำลังดาวน์โหลด assistme.dat..."},
    /* S_LOG_DOWNLOADED */ {L"Downloaded %u bytes; MD5 matches.", L"ดาวน์โหลดแล้ว %u ไบต์ MD5 ถูกต้อง"},
    /* S_LOG_BAD_MD5 */ {L"The download failed the MD5 check (got %ls).",
                         L"ไฟล์ที่ดาวน์โหลดไม่ผ่านการตรวจ MD5 (ได้ %ls)"},
    /* S_LOG_OFFLINE */ {L"Cannot reach Sony's server: %ls", L"เชื่อมต่อเซิร์ฟเวอร์ Sony ไม่ได้: %ls"},
    /* S_LOG_OFFLINE_CACHE */ {L"Using the copy saved on this PC: data from %ls (%ls).",
                               L"ใช้ไฟล์ที่บันทึกไว้ในเครื่องนี้: ข้อมูลวันที่ %ls (%ls)"},
    /* S_LOG_NO_CACHE */ {L"No usable copy is saved on this PC (none yet, or older than 30 days). Connect to the internet and try again.",
                          L"ในเครื่องนี้ไม่มีไฟล์ที่ใช้ได้ (ยังไม่เคยดาวน์โหลด หรืออายุเกิน 30 วัน) กรุณาเชื่อมต่ออินเทอร์เน็ตแล้วลองใหม่"},
    /* S_LOG_WRITING */ {L"Writing %ls ...", L"กำลังเขียน %ls ..."},
    /* S_LOG_WRITE_FAIL */ {L"Cannot write to the card: %ls", L"เขียนลงการ์ดไม่ได้: %ls"},
    /* S_LOG_VERIFIED */ {L"Read back from the card: MD5 matches.", L"อ่านกลับจากการ์ดแล้ว: MD5 ถูกต้อง"},
    /* S_LOG_VERIFY_FAIL */ {L"Read-back check failed. The card may be faulty or locked (check its lock switch).",
                             L"ตรวจสอบหลังเขียนไม่ผ่าน การ์ดอาจเสียหรือถูกล็อก (ตรวจสวิตช์ล็อกที่การ์ด)"},
    /* S_LOG_DONE */ {L"Done. Safely remove the card, put it back in the camera and turn the camera on; it loads the new data by itself.",
                      L"เสร็จแล้ว ถอดการ์ดอย่างปลอดภัย ใส่กลับเข้ากล้องแล้วเปิดกล้อง กล้องจะโหลดข้อมูลใหม่เอง"},
    /* S_LOG_TIP */ {L"The data is valid for about 30 days. Update again after that.",
                     L"ข้อมูลมีอายุประมาณ 30 วัน หลังจากนั้นให้อัปเดตใหม่"},
    /* S_LOG_FAILED */ {L"Update failed.", L"อัปเดตไม่สำเร็จ"},
    /* S_ASK_FIXED */ {L"%lc: is not a removable drive.\n\nWrite the GPS assist data to it anyway?",
                       L"ไดรฟ์ %lc: ไม่ใช่ไดรฟ์แบบถอดได้\n\nต้องการเขียนข้อมูล GPS Assist ลงไดรฟ์นี้หรือไม่?"},
    /* S_ABOUT_TEXT */ {L"GPS Assist Data Updater %ls\n\n"
                        L"Downloads Sony's GPS assist data (assistme.dat) from\n%ls\n"
                        L"checks its MD5 and writes it to \\PRIVATE\\SONY\\GPS on the memory card, "
                        L"the same file Sony's own PC software puts there.\n\n"
                        L"A camera with built-in GPS uses it to find its position faster. "
                        L"The data is valid for about 30 days.\n\n"
                        L"Not made by or affiliated with Sony.",
                        L"โปรแกรมอัปเดตข้อมูล GPS Assist %ls\n\n"
                        L"ดาวน์โหลดข้อมูล GPS Assist (assistme.dat) ของ Sony จาก\n%ls\n"
                        L"ตรวจสอบ MD5 แล้วเขียนลง \\PRIVATE\\SONY\\GPS ในการ์ดหน่วยความจำ "
                        L"ซึ่งเป็นไฟล์เดียวกับที่โปรแกรมของ Sony เขียน\n\n"
                        L"กล้องที่มี GPS ในตัวจะใช้ข้อมูลนี้เพื่อหาตำแหน่งได้เร็วขึ้น "
                        L"ข้อมูลมีอายุประมาณ 30 วัน\n\n"
                        L"โปรแกรมนี้ไม่ได้จัดทำโดย Sony และไม่มีส่วนเกี่ยวข้องกับ Sony"},
};

// ---------------------------------------------------------------------------------------------
// State

HINSTANCE g_inst;
HWND g_hwnd, g_title, g_subtitle, g_cardLabel, g_combo, g_refresh, g_showAll, g_cardStatus,
    g_serverStatus, g_update, g_langBtn, g_about, g_progress, g_log;
HFONT g_font, g_fontTitle, g_fontBold;
UINT g_dpi = 96;
int g_lang = 0;  // 0 English, 1 Thai
bool g_busy = false, g_checking = false, g_logUsed = false, g_refreshPending = false;
int g_cardState = STATE_PLAIN, g_serverState = STATE_PLAIN;
std::wstring g_host;

struct ServerInfo {
    bool ok = false;
    std::wstring md5;
    FILETIME published{};
    std::wstring error;
};
bool g_serverKnown = false;
ServerInfo g_server;

struct DriveInfo {
    wchar_t letter;
    UINT type;
    std::wstring label, fs;
    ULONGLONG total;
    bool camera;
};
std::vector<DriveInfo> g_drives;

const wchar_t* T(Str id) { return kStrings[id][g_lang]; }

std::wstring Fmt(const wchar_t* f, ...) {
    va_list ap;
    va_start(ap, f);
    int n = _vscwprintf(f, ap);
    va_end(ap);
    if (n <= 0) return std::wstring();
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    va_start(ap, f);
    vswprintf(&s[0], s.size(), f, ap);
    va_end(ap);
    s.resize(static_cast<size_t>(n));
    return s;
}

std::wstring SysError(DWORD e) {
    wchar_t* buf = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    HMODULE mod = nullptr;
    if (e >= 12000 && e < 12200) {
        mod = GetModuleHandleW(L"winhttp.dll");
        if (mod) flags |= FORMAT_MESSAGE_FROM_HMODULE;
    }
    std::wstring s;
    if (FormatMessageW(flags, mod, e, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr) && buf) {
        s = buf;
        LocalFree(buf);
    }
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ' || s.back() == L'.'))
        s.pop_back();
    if (s.empty()) s = L"error";
    return Fmt(L"%ls [%lu]", s.c_str(), e);
}

// ---------------------------------------------------------------------------------------------
// Time

ULONGLONG ToU64(const FILETIME& ft) { return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

FILETIME NowUtc() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ft;
}

int AgeDays(const FILETIME& ft) {
    ULONGLONG now = ToU64(NowUtc()), then = ToU64(ft);
    if (then >= now) return 0;
    return static_cast<int>((now - then) / 864000000000ULL);
}

std::wstring AgeText(int days) {
    if (days <= 0) return T(S_AGE_TODAY);
    if (days == 1) return T(S_AGE_ONE);
    return Fmt(T(S_AGE_N), days);
}

std::wstring FormatLocal(const FILETIME& utc) {
    SYSTEMTIME su, sl;
    if (!FileTimeToSystemTime(&utc, &su) || !SystemTimeToTzSpecificLocalTime(nullptr, &su, &sl)) return L"?";
    return Fmt(L"%04u-%02u-%02u %02u:%02u", sl.wYear, sl.wMonth, sl.wDay, sl.wHour, sl.wMinute);
}

// ---------------------------------------------------------------------------------------------
// MD5

std::wstring Md5Hex(const std::vector<BYTE>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    BYTE d[16] = {};
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0)) &&
              BCRYPT_SUCCESS(BCryptHashData(h, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0)) &&
              BCRYPT_SUCCESS(BCryptFinishHash(h, d, sizeof d, 0));
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return std::wstring();
    std::wstring s;
    for (BYTE b : d) s += Fmt(L"%02x", b);
    return s;
}

// assistme.md5 is 32 hex digits followed by spaces and NUL bytes.
std::wstring ParseMd5(const std::vector<BYTE>& body) {
    std::wstring s;
    for (BYTE c : body) {
        if (s.size() == 32) break;
        if (c >= '0' && c <= '9') s += static_cast<wchar_t>(c);
        else if (c >= 'a' && c <= 'f') s += static_cast<wchar_t>(c);
        else if (c >= 'A' && c <= 'F') s += static_cast<wchar_t>(c - 'A' + 'a');
        else break;
    }
    return s.size() == 32 ? s : std::wstring();
}

// ---------------------------------------------------------------------------------------------
// Worker -> UI

void PostLog(const std::wstring& s) {
    auto* p = new std::wstring(s);
    if (!PostMessageW(g_hwnd, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(p))) delete p;
}

void PostProgress(int pct) { PostMessageW(g_hwnd, WM_APP_PROGRESS, static_cast<WPARAM>(pct), 0); }

void PostServer(const ServerInfo& si) {
    auto* p = new ServerInfo(si);
    if (!PostMessageW(g_hwnd, WM_APP_SERVER, 0, reinterpret_cast<LPARAM>(p))) delete p;
}

// ---------------------------------------------------------------------------------------------
// HTTP

struct HttpResult {
    std::vector<BYTE> body;
    bool haveLastModified = false;
    FILETIME lastModified{};
};

struct HInternet {
    HINTERNET h = nullptr;
    explicit HInternet(HINTERNET x) : h(x) {}
    ~HInternet() { if (h) WinHttpCloseHandle(h); }
    HInternet(const HInternet&) = delete;
    HInternet& operator=(const HInternet&) = delete;
};

bool HttpGetOnce(const std::wstring& host, const wchar_t* path, bool https, bool progress, HttpResult& r,
                 std::wstring& err) {
    HInternet ses(WinHttpOpen(L"GpsAssistUpdater/" APP_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!ses.h)
        ses.h = WinHttpOpen(L"GpsAssistUpdater/" APP_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses.h) { err = SysError(GetLastError()); return false; }
    WinHttpSetTimeouts(ses.h, 15000, 15000, 20000, 30000);

    HInternet con(WinHttpConnect(ses.h, host.c_str(),
                                 https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0));
    if (!con.h) { err = SysError(GetLastError()); return false; }
    HInternet req(WinHttpOpenRequest(con.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0));
    if (!req.h) { err = SysError(GetLastError()); return false; }
    WinHttpAddRequestHeaders(req.h, L"Cache-Control: no-cache\r\nPragma: no-cache", static_cast<DWORD>(-1),
                             WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        err = SysError(GetLastError());
        return false;
    }

    DWORD status = 0, len = sizeof status;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) { err = Fmt(L"HTTP %lu", status); return false; }

    SYSTEMTIME st{};
    len = sizeof st;
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_LAST_MODIFIED | WINHTTP_QUERY_FLAG_SYSTEMTIME,
                            WINHTTP_HEADER_NAME_BY_INDEX, &st, &len, WINHTTP_NO_HEADER_INDEX))
        r.haveLastModified = SystemTimeToFileTime(&st, &r.lastModified) != FALSE;

    DWORD total = 0;
    len = sizeof total;
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &total, &len, WINHTTP_NO_HEADER_INDEX))
        total = 0;
    if (total > kMaxDataSize) { err = Fmt(L"file too large (%lu bytes)", total); return false; }

    r.body.clear();
    int lastPct = -1;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) { err = SysError(GetLastError()); return false; }
        if (avail == 0) break;
        size_t at = r.body.size();
        if (at + avail > kMaxDataSize) { err = L"file too large"; return false; }
        r.body.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.h, r.body.data() + at, avail, &got)) { err = SysError(GetLastError()); return false; }
        r.body.resize(at + got);
        if (progress && total) {
            int pct = static_cast<int>(r.body.size() * 100 / total);
            if (pct != lastPct) { PostProgress(pct); lastPct = pct; }
        }
    }
    if (total && r.body.size() != total) { err = L"download incomplete"; return false; }
    if (r.body.empty()) { err = L"empty response"; return false; }
    return true;
}

// HTTPS first; Sony's server also answers plain HTTP, which helps behind broken TLS inspection.
bool HttpGet(const std::wstring& host, const wchar_t* path, bool progress, bool verbose, HttpResult& r,
             std::wstring& err) {
    if (HttpGetOnce(host, path, true, progress, r, err)) return true;
    if (err.compare(0, 5, L"HTTP ") == 0) return false;  // the server answered; plain HTTP will not help
    if (verbose) PostLog(Fmt(T(S_LOG_HTTP_FALLBACK), err.c_str()));
    std::wstring err2;
    if (HttpGetOnce(host, path, false, progress, r, err2)) return true;
    err = err2;
    return false;
}

// ---------------------------------------------------------------------------------------------
// Files

bool ReadWhole(const std::wstring& path, std::vector<BYTE>& out, FILETIME* mtime) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= kMaxDataSize;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size();
    }
    if (ok && mtime) ok = GetFileTime(h, nullptr, nullptr, mtime) != FALSE;
    CloseHandle(h);
    return ok;
}

// Reads around the system cache, so the bytes really come from the card.
bool ReadUncached(const std::wstring& path, std::vector<BYTE>& out, std::wstring& err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_NO_BUFFERING, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = SysError(GetLastError()); return false; }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > kMaxDataSize) {
        CloseHandle(h);
        err = L"bad file size";
        return false;
    }
    const DWORD chunk = 64 * 1024;  // a multiple of any sector size
    BYTE* buf = static_cast<BYTE*>(VirtualAlloc(nullptr, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    bool ok = buf != nullptr;
    out.clear();
    while (ok && out.size() < static_cast<size_t>(size.QuadPart)) {
        DWORD got = 0;
        if (!ReadFile(h, buf, chunk, &got, nullptr)) { err = SysError(GetLastError()); ok = false; break; }
        if (got == 0) break;
        size_t want = static_cast<size_t>(size.QuadPart) - out.size();
        out.insert(out.end(), buf, buf + (got < want ? got : want));
    }
    if (buf) VirtualFree(buf, 0, MEM_RELEASE);
    CloseHandle(h);
    if (ok && out.size() != static_cast<size_t>(size.QuadPart)) { err = L"short read"; ok = false; }
    return ok;
}

// Temp file + flush + rename, so an interrupted write never leaves a half file under the real name.
bool WriteAtomic(const std::wstring& dir, const wchar_t* tmpName, const wchar_t* finalName,
                 const std::vector<BYTE>& data, const FILETIME* mtime, std::wstring& err) {
    std::wstring tmp = dir + tmpName, fin = dir + finalName;
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = SysError(GetLastError()); return false; }
    DWORD written = 0;
    bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
              written == data.size();
    if (ok && mtime) ok = SetFileTime(h, nullptr, nullptr, mtime) != FALSE;
    if (ok) ok = FlushFileBuffers(h) != FALSE;
    DWORD e = GetLastError();
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        err = SysError(e);
        return false;
    }
    SetFileAttributesW(fin.c_str(), FILE_ATTRIBUTE_NORMAL);  // clear read-only, if any
    if (!MoveFileExW(tmp.c_str(), fin.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        e = GetLastError();
        DeleteFileW(tmp.c_str());
        err = SysError(e);
        return false;
    }
    return true;
}

bool DirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Root(wchar_t letter) { return std::wstring(1, letter) + L":\\"; }
std::wstring CardDir(wchar_t letter) { return Root(letter) + L"PRIVATE\\SONY\\GPS\\"; }
std::wstring CardFile(wchar_t letter) { return CardDir(letter) + kCardName; }

bool MakeCardDir(wchar_t letter, std::wstring& err) {
    std::wstring dir = Root(letter);
    for (const wchar_t* part : {L"PRIVATE", L"SONY", L"GPS"}) {
        dir += part;
        if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            err = SysError(GetLastError());
            return false;
        }
        dir += L'\\';
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Cache of the last good download

std::wstring CacheDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"GPSASSIST_CACHE", buf, MAX_PATH);
    if (n && n < MAX_PATH) {
        std::wstring d = buf;
        if (d.back() != L'\\') d += L'\\';
        return d;
    }
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) n = GetTempPathW(MAX_PATH, buf);
    std::wstring d = buf;
    if (!d.empty() && d.back() != L'\\') d += L'\\';
    return d + L"GpsAssistUpdater\\";
}

bool LoadCache(std::vector<BYTE>& data, std::wstring& md5, FILETIME& published) {
    std::wstring dir = CacheDir();
    std::vector<BYTE> m;
    if (!ReadWhole(dir + L"assistme.dat", data, &published) || !ReadWhole(dir + L"assistme.md5", m, nullptr))
        return false;
    md5 = ParseMd5(m);
    return !md5.empty() && Md5Hex(data) == md5;  // never trust a damaged cache
}

void SaveCache(const std::vector<BYTE>& data, const std::wstring& md5, const FILETIME& published) {
    std::wstring dir = CacheDir(), err;
    CreateDirectoryW(dir.c_str(), nullptr);
    std::vector<BYTE> m;
    for (wchar_t c : md5) m.push_back(static_cast<BYTE>(c));  // ASCII hex digits
    m.push_back('\n');
    WriteAtomic(dir, L"assistme.md5.tmp", L"assistme.md5", m, nullptr, err);
    WriteAtomic(dir, L"assistme.dat.tmp", L"assistme.dat", data, &published, err);
}

// ---------------------------------------------------------------------------------------------
// Workers

DWORD WINAPI ServerCheckThread(void* p) {
    std::unique_ptr<std::wstring> host(static_cast<std::wstring*>(p));
    ServerInfo si;
    HttpResult r;
    if (HttpGet(*host, kMd5Path, false, false, r, si.error)) {
        si.md5 = ParseMd5(r.body);
        si.ok = !si.md5.empty();
        if (!si.ok) si.error = L"bad assistme.md5";
        si.published = r.haveLastModified ? r.lastModified : NowUtc();
    }
    PostServer(si);
    return 0;
}

struct UpdateJob {
    wchar_t letter;
    std::wstring host;
};

int DoUpdate(const UpdateJob& job) {
    PostProgress(-1);
    PostLog(T(S_LOG_CHECKING));

    ServerInfo si;
    HttpResult md5r;
    if (HttpGet(job.host, kMd5Path, false, true, md5r, si.error)) {
        si.md5 = ParseMd5(md5r.body);
        si.ok = !si.md5.empty();
        if (!si.ok) si.error = L"bad assistme.md5";
        si.published = md5r.haveLastModified ? md5r.lastModified : NowUtc();
    }
    PostServer(si);

    std::vector<BYTE> data;
    std::wstring dataMd5, err;
    FILETIME published{};
    const std::wstring cardFile = CardFile(job.letter);

    if (si.ok) {
        PostLog(Fmt(T(S_LOG_SERVER), FormatLocal(si.published).c_str(), si.md5.c_str()));
        std::vector<BYTE> onCard;
        if (ReadWhole(cardFile, onCard, nullptr) && Md5Hex(onCard) == si.md5) {
            PostLog(T(S_LOG_ALREADY));
            PostProgress(100);
            return RESULT_NOTHING;
        }
        std::vector<BYTE> cached;
        std::wstring cachedMd5;
        FILETIME cachedTime{};
        if (LoadCache(cached, cachedMd5, cachedTime) && cachedMd5 == si.md5) {
            data.swap(cached);
            published = cachedTime;
            PostLog(T(S_LOG_CACHE_SAME));
        } else {
            PostLog(T(S_LOG_DOWNLOADING));
            for (int attempt = 0; attempt < 2 && data.empty(); ++attempt) {
                HttpResult r;
                if (!HttpGet(job.host, kDatPath, true, true, r, err)) {
                    PostLog(Fmt(T(S_LOG_OFFLINE), err.c_str()));
                    return RESULT_FAILED;
                }
                std::wstring got = Md5Hex(r.body);
                if (got != si.md5) {
                    PostLog(Fmt(T(S_LOG_BAD_MD5), got.c_str()));
                    continue;
                }
                data.swap(r.body);
                published = r.haveLastModified ? r.lastModified : si.published;
            }
            if (data.empty()) return RESULT_FAILED;
            PostLog(Fmt(T(S_LOG_DOWNLOADED), static_cast<unsigned>(data.size())));
            SaveCache(data, si.md5, published);
        }
        dataMd5 = si.md5;
    } else {
        PostLog(Fmt(T(S_LOG_OFFLINE), si.error.c_str()));
        if (!LoadCache(data, dataMd5, published) || AgeDays(published) > kValidDays) {
            PostLog(T(S_LOG_NO_CACHE));
            return RESULT_FAILED;
        }
        PostLog(Fmt(T(S_LOG_OFFLINE_CACHE), FormatLocal(published).c_str(), AgeText(AgeDays(published)).c_str()));
    }

    PostProgress(-1);
    PostLog(Fmt(T(S_LOG_WRITING), cardFile.c_str()));
    if (!MakeCardDir(job.letter, err) || !WriteAtomic(CardDir(job.letter), kCardTemp, kCardName, data, &published, err)) {
        PostLog(Fmt(T(S_LOG_WRITE_FAIL), err.c_str()));
        return RESULT_FAILED;
    }
    std::vector<BYTE> back;
    if (!ReadUncached(cardFile, back, err) || Md5Hex(back) != dataMd5) {
        PostLog(T(S_LOG_VERIFY_FAIL));
        return RESULT_FAILED;
    }
    PostLog(T(S_LOG_VERIFIED));
    PostProgress(100);
    PostLog(T(S_LOG_DONE));
    PostLog(T(S_LOG_TIP));
    return RESULT_OK;
}

DWORD WINAPI UpdateThread(void* p) {
    std::unique_ptr<UpdateJob> job(static_cast<UpdateJob*>(p));
    int result = DoUpdate(*job);
    PostMessageW(g_hwnd, WM_APP_DONE, static_cast<WPARAM>(result), 0);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Drives

std::vector<DriveInfo> ListDrives(bool all) {
    std::vector<DriveInfo> out;
    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    wchar_t sys = static_cast<wchar_t>(towupper(win[0]));
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t letter = static_cast<wchar_t>(L'A' + i);
        std::wstring root = Root(letter);
        UINT type = GetDriveTypeW(root.c_str());
        if (!(type == DRIVE_REMOVABLE || (all && type == DRIVE_FIXED && letter != sys))) continue;
        wchar_t label[MAX_PATH + 1] = {}, fs[MAX_PATH + 1] = {};
        if (!GetVolumeInformationW(root.c_str(), label, MAX_PATH + 1, nullptr, nullptr, nullptr, fs, MAX_PATH + 1))
            continue;  // empty card slot, or a locked drive
        ULARGE_INTEGER total{};
        GetDiskFreeSpaceExW(root.c_str(), nullptr, &total, nullptr);
        bool camera = DirExists(root + L"DCIM") || DirExists(root + L"PRIVATE\\SONY");
        out.push_back({letter, type, label, fs, total.QuadPart, camera});
    }
    return out;
}

std::wstring SizeText(ULONGLONG bytes) {
    double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 1.0) return Fmt(L"%.1f GB", gb);
    return Fmt(L"%.0f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

const DriveInfo* CurrentDrive() {
    LRESULT i = SendMessageW(g_combo, CB_GETCURSEL, 0, 0);
    if (i < 0 || static_cast<size_t>(i) >= g_drives.size()) return nullptr;
    return &g_drives[static_cast<size_t>(i)];
}

// ---------------------------------------------------------------------------------------------
// UI

int S(int v) { return MulDiv(v, static_cast<int>(g_dpi), 96); }

void AppendLog(const std::wstring& text) {
    if (!g_logUsed) {
        SetWindowTextW(g_log, L"");
        g_logUsed = true;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::wstring line = Fmt(L"[%02u:%02u:%02u] ", t.wHour, t.wMinute, t.wSecond) + text + L"\r\n";
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

void UpdateCardStatus() {
    const DriveInfo* d = CurrentDrive();
    std::wstring text;
    if (!d) {
        text = T(S_INSERT_CARD);
        g_cardState = STATE_PLAIN;
    } else {
        std::vector<BYTE> cur;
        FILETIME mt{};
        if (!ReadWhole(CardFile(d->letter), cur, &mt)) {
            text = T(S_CARD_NONE);
            g_cardState = STATE_WARN;
        } else {
            int days = AgeDays(mt);
            std::wstring when = FormatLocal(mt), age = AgeText(days);
            if (g_serverKnown && g_server.ok && Md5Hex(cur) == g_server.md5) {
                text = Fmt(T(S_CARD_LATEST), when.c_str(), age.c_str());
                g_cardState = STATE_GOOD;
            } else if (days <= kValidDays) {
                text = Fmt(T(S_CARD_VALID), when.c_str(), age.c_str());
                g_cardState = STATE_GOOD;
            } else {
                text = Fmt(T(S_CARD_EXPIRED), when.c_str(), age.c_str());
                g_cardState = STATE_BAD;
            }
        }
    }
    SetWindowTextW(g_cardStatus, text.c_str());
    InvalidateRect(g_cardStatus, nullptr, TRUE);
}

void UpdateServerStatus() {
    std::wstring text;
    if (!g_serverKnown) {
        text = T(S_SERVER_CHECKING);
        g_serverState = STATE_PLAIN;
    } else if (g_server.ok) {
        text = Fmt(T(S_SERVER_OK), FormatLocal(g_server.published).c_str());
        g_serverState = STATE_PLAIN;
    } else {
        text = Fmt(T(S_SERVER_FAIL), g_server.error.c_str());
        g_serverState = STATE_BAD;
    }
    SetWindowTextW(g_serverStatus, text.c_str());
    InvalidateRect(g_serverStatus, nullptr, TRUE);
}

void EnableControls() {
    bool idle = !g_busy;
    EnableWindow(g_combo, idle && !g_drives.empty());
    EnableWindow(g_refresh, idle);
    EnableWindow(g_showAll, idle);
    EnableWindow(g_langBtn, idle);
    EnableWindow(g_update, idle && !g_drives.empty());
}

void RefreshDrives() {
    const DriveInfo* cur = CurrentDrive();
    wchar_t prev = cur ? cur->letter : 0;
    g_drives = ListDrives(SendMessageW(g_showAll, BM_GETCHECK, 0, 0) == BST_CHECKED);
    SendMessageW(g_combo, CB_RESETCONTENT, 0, 0);
    int pick = -1, firstData = -1, firstCamera = -1;
    for (size_t i = 0; i < g_drives.size(); ++i) {
        const DriveInfo& d = g_drives[i];
        std::wstring item = Root(d.letter);
        if (!d.label.empty()) item += L"   " + d.label;
        item += L"   " + SizeText(d.total) + L"   " + d.fs + L"   (" +
                T(d.type == DRIVE_REMOVABLE ? S_REMOVABLE : S_FIXED) + L")";
        if (d.camera) item += std::wstring(L"   - ") + T(S_CAMERA_CARD);
        SendMessageW(g_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
        int idx = static_cast<int>(i);
        if (d.letter == prev) pick = idx;
        if (firstData < 0 && GetFileAttributesW(CardFile(d.letter).c_str()) != INVALID_FILE_ATTRIBUTES)
            firstData = idx;
        if (firstCamera < 0 && d.camera) firstCamera = idx;
    }
    if (g_drives.empty()) {
        SendMessageW(g_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(S_NO_CARD_ITEM)));
        SendMessageW(g_combo, CB_SETCURSEL, 0, 0);
    } else {
        if (pick < 0) pick = firstData >= 0 ? firstData : (firstCamera >= 0 ? firstCamera : 0);
        SendMessageW(g_combo, CB_SETCURSEL, static_cast<WPARAM>(pick), 0);
    }
    EnableControls();
    UpdateCardStatus();
}

void StartServerCheck() {
    if (g_checking) return;
    g_checking = true;
    g_serverKnown = false;
    UpdateServerStatus();
    HANDLE t = CreateThread(nullptr, 0, ServerCheckThread, new std::wstring(g_host), 0, nullptr);
    if (t) CloseHandle(t);
    else g_checking = false;
}

void SetProgress(int pct) {
    LONG_PTR style = GetWindowLongPtrW(g_progress, GWL_STYLE);
    if (pct < 0) {
        if (!(style & PBS_MARQUEE)) SetWindowLongPtrW(g_progress, GWL_STYLE, style | PBS_MARQUEE);
        SendMessageW(g_progress, PBM_SETMARQUEE, TRUE, 30);
    } else {
        SendMessageW(g_progress, PBM_SETMARQUEE, FALSE, 0);
        if (style & PBS_MARQUEE) SetWindowLongPtrW(g_progress, GWL_STYLE, style & ~PBS_MARQUEE);
        SendMessageW(g_progress, PBM_SETPOS, static_cast<WPARAM>(pct), 0);
    }
}

void StartUpdate() {
    const DriveInfo* d = CurrentDrive();
    if (!d || g_busy) return;
    if (d->type != DRIVE_REMOVABLE) {
        std::wstring q = Fmt(T(S_ASK_FIXED), d->letter);
        if (MessageBoxW(g_hwnd, q.c_str(), T(S_APP_NAME), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return;
    }
    auto* job = new UpdateJob{d->letter, g_host};
    g_busy = true;
    EnableControls();
    SendMessageW(g_progress, PBM_SETSTATE, PBST_NORMAL, 0);
    SendMessageW(g_progress, PBM_SETPOS, 0, 0);
    if (g_logUsed) AppendLog(L"--------");
    HANDLE t = CreateThread(nullptr, 0, UpdateThread, job, 0, nullptr);
    if (t) {
        CloseHandle(t);
    } else {
        delete job;
        g_busy = false;
        EnableControls();
        AppendLog(SysError(GetLastError()));
    }
}

void CreateFonts() {
    for (HFONT* f : {&g_font, &g_fontTitle, &g_fontBold})
        if (*f) { DeleteObject(*f); *f = nullptr; }
    const wchar_t* face = g_lang ? L"Leelawadee UI" : L"Segoe UI";
    auto make = [&](int pt, int weight) {
        return CreateFontW(-MulDiv(pt, static_cast<int>(g_dpi), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, face);
    };
    g_font = make(9, FW_NORMAL);
    g_fontTitle = make(14, FW_SEMIBOLD);
    g_fontBold = make(10, FW_SEMIBOLD);
}

void ApplyFonts() {
    for (HWND h : {g_subtitle, g_cardLabel, g_combo, g_refresh, g_showAll, g_cardStatus, g_serverStatus, g_langBtn,
                   g_about, g_log})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    SendMessageW(g_title, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontTitle), TRUE);
    SendMessageW(g_update, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontBold), TRUE);
}

const int kClientW = 660, kClientH = 470;

void Layout() {
    auto at = [](HWND h, int x, int y, int w, int hgt) { MoveWindow(h, S(x), S(y), S(w), S(hgt), TRUE); };
    at(g_title, 14, 10, 632, 30);
    at(g_subtitle, 14, 42, 632, 20);
    at(g_cardLabel, 14, 78, 130, 20);
    at(g_combo, 148, 74, 390, 220);
    at(g_refresh, 546, 73, 100, 26);
    at(g_showAll, 148, 106, 498, 22);
    at(g_cardStatus, 14, 138, 632, 20);
    at(g_serverStatus, 14, 162, 632, 20);
    at(g_update, 14, 194, 260, 38);
    at(g_langBtn, 440, 199, 100, 28);
    at(g_about, 546, 199, 100, 28);
    at(g_progress, 14, 246, 632, 12);
    at(g_log, 14, 266, 632, 190);
    SendMessageW(g_combo, CB_SETDROPPEDWIDTH, static_cast<WPARAM>(S(560)), 0);
}

void ApplyTexts() {
    std::wstring caption = std::wstring(T(S_APP_NAME)) + L" " APP_VERSION;
    SetWindowTextW(g_hwnd, caption.c_str());
    SetWindowTextW(g_title, T(S_APP_NAME));
    SetWindowTextW(g_subtitle, T(S_SUBTITLE));
    SetWindowTextW(g_cardLabel, T(S_CARD));
    SetWindowTextW(g_refresh, T(S_REFRESH));
    SetWindowTextW(g_showAll, T(S_SHOW_ALL));
    SetWindowTextW(g_update, T(S_UPDATE));
    SetWindowTextW(g_langBtn, T(S_LANG_SWITCH));
    SetWindowTextW(g_about, T(S_ABOUT));
    if (!g_logUsed) SetWindowTextW(g_log, T(S_WELCOME));
    RefreshDrives();
    UpdateServerStatus();
}

int LoadLanguage() {
    wchar_t v[8] = {};
    DWORD size = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, L"Language", RRF_RT_REG_SZ, nullptr, v, &size) == ERROR_SUCCESS)
        return wcscmp(v, L"th") == 0 ? 1 : 0;
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_THAI ? 1 : 0;
}

void SaveLanguage() {
    const wchar_t* v = g_lang ? L"th" : L"en";
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, L"Language", REG_SZ, v,
                    static_cast<DWORD>((wcslen(v) + 1) * sizeof(wchar_t)));
}

void FitWindow(bool center) {
    RECT rc = {0, 0, S(kClientW), S(kClientH)};
    DWORD style = static_cast<DWORD>(GetWindowLongPtrW(g_hwnd, GWL_STYLE));
    DWORD ex = static_cast<DWORD>(GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE));
    AdjustWindowRectExForDpi(&rc, style, FALSE, ex, g_dpi);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (center) {
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
        int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
        SetWindowPos(g_hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        SetWindowPos(g_hwnd, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    }
}

HWND MakeControl(const wchar_t* cls, DWORD style, int id, DWORD ex = 0) {
    return CreateWindowExW(ex, cls, L"", WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, g_hwnd,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_inst, nullptr);
}

void OnCreate(HWND hwnd) {
    g_hwnd = hwnd;
    g_dpi = GetDpiForWindow(hwnd);
    const DWORD label = SS_LEFT | SS_NOPREFIX;
    g_title = MakeControl(L"STATIC", label, IDC_TITLE);
    g_subtitle = MakeControl(L"STATIC", label | SS_ENDELLIPSIS, IDC_SUBTITLE);
    g_cardLabel = MakeControl(L"STATIC", label, IDC_CARD_LABEL);
    g_combo = MakeControl(WC_COMBOBOXW, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_CARD);
    g_refresh = MakeControl(L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP, IDC_REFRESH);
    g_showAll = MakeControl(L"BUTTON", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_SHOWALL);
    g_cardStatus = MakeControl(L"STATIC", label | SS_ENDELLIPSIS, IDC_CARD_STATUS);
    g_serverStatus = MakeControl(L"STATIC", label | SS_ENDELLIPSIS, IDC_SERVER_STATUS);
    g_update = MakeControl(L"BUTTON", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_UPDATE);
    g_langBtn = MakeControl(L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP, IDC_LANG);
    g_about = MakeControl(L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP, IDC_ABOUT);
    g_progress = MakeControl(PROGRESS_CLASSW, 0, IDC_PROGRESS);
    g_log = MakeControl(L"EDIT", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, IDC_LOG,
                        WS_EX_CLIENTEDGE);
    SendMessageW(g_log, EM_SETLIMITTEXT, 0, 0);
    SendMessageW(g_progress, PBM_SETRANGE32, 0, 100);
    CreateFonts();
    ApplyFonts();
    Layout();
    FitWindow(true);
    ApplyTexts();
    StartServerCheck();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        OnCreate(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_UPDATE: StartUpdate(); return 0;
        case IDC_REFRESH:
            RefreshDrives();
            StartServerCheck();
            return 0;
        case IDC_SHOWALL: RefreshDrives(); return 0;
        case IDC_CARD:
            if (HIWORD(wp) == CBN_SELCHANGE) UpdateCardStatus();
            return 0;
        case IDC_LANG:
            g_lang ^= 1;
            SaveLanguage();
            CreateFonts();
            ApplyFonts();
            ApplyTexts();
            return 0;
        case IDC_ABOUT: {
            std::wstring url = L"https://" + g_host + kDatPath;
            std::wstring text = Fmt(T(S_ABOUT_TEXT), APP_VERSION, url.c_str());
            MessageBoxW(hwnd, text.c_str(), T(S_APP_NAME), MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        }
        break;

    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        HWND c = reinterpret_cast<HWND>(lp);
        int state = c == g_cardStatus ? g_cardState : c == g_serverStatus ? g_serverState : STATE_PLAIN;
        COLORREF color = GetSysColor(COLOR_WINDOWTEXT);
        if (state == STATE_GOOD) color = RGB(0, 125, 50);
        else if (state == STATE_BAD) color = RGB(196, 30, 30);
        else if (state == STATE_WARN) color = RGB(170, 95, 0);
        else if (c == g_subtitle) color = GetSysColor(COLOR_GRAYTEXT);
        SetTextColor(dc, color);
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }

    case WM_APP_LOG: {
        std::unique_ptr<std::wstring> s(reinterpret_cast<std::wstring*>(lp));
        AppendLog(*s);
        return 0;
    }
    case WM_APP_PROGRESS:
        SetProgress(static_cast<int>(wp));
        return 0;
    case WM_APP_SERVER: {
        std::unique_ptr<ServerInfo> si(reinterpret_cast<ServerInfo*>(lp));
        g_checking = false;
        g_server = *si;
        g_serverKnown = true;
        UpdateServerStatus();
        UpdateCardStatus();
        return 0;
    }
    case WM_APP_DONE: {
        g_busy = false;
        int result = static_cast<int>(wp);
        if (result == RESULT_FAILED) {
            SetProgress(100);
            SendMessageW(g_progress, PBM_SETSTATE, PBST_ERROR, 0);
            AppendLog(T(S_LOG_FAILED));
            MessageBeep(MB_ICONERROR);
        } else {
            SetProgress(100);
            MessageBeep(MB_OK);
        }
        g_refreshPending = false;
        RefreshDrives();
        return 0;
    }

    case WM_DEVICECHANGE:
        if ((wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) && lp &&
            reinterpret_cast<DEV_BROADCAST_HDR*>(lp)->dbch_devicetype == DBT_DEVTYP_VOLUME)
            SetTimer(hwnd, TIMER_DEVICE, 700, nullptr);  // let the volume settle first
        return TRUE;
    case WM_TIMER:
        if (wp == TIMER_DEVICE) {
            KillTimer(hwnd, TIMER_DEVICE);
            if (g_busy) g_refreshPending = true;
            else RefreshDrives();
        }
        return 0;

    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        CreateFonts();
        ApplyFonts();
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        FitWindow(false);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    g_inst = inst;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);  // no "insert a disk" popups
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);

    wchar_t host[256] = {};
    DWORD n = GetEnvironmentVariableW(L"GPSASSIST_SERVER", host, 256);
    g_host = (n && n < 256) ? host : kDefaultHost;
    g_lang = LoadLanguage();

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"GpsAssistUpdaterWindow";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 640, 520, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return static_cast<int>(m.wParam);
}
