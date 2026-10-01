/* Offline tests for Essential Audio Enhancements for The Quarry. This exe plays the part of the game
 * (it exports stand-ins for the Wwise functions the mod calls).
 *
 *   test_qsa.exe <dll> <scenario>
 *     auto          Output=auto, spatial simulated off: Windows' mix layout;
 *                   reads by other modules see the true format
 *     layout51      Output=5.1: used if Windows accepts it
 *     layout51no    Output=5.1, refusal simulated: falls back, logged
 *     mono          Output=mono: the output is rebuilt with one channel
 *     headphones    spatial simulated off: stereo + headphone panning
 *     spatial       this exe is not The Quarry: the code patch is refused
 *     device        Device=<another active output>: detection, widening and
 *                   the rebuilt output all use that device
 *     nodevice      Device=<no match>: devices listed, default kept
 *     dialogue      Dialogue=positional: the game's own load whenever the
 *                   dialogue file will not open or is not the version the
 *                   mod knows, other banks untouched; and, when
 *                   QSA_SPEECH_BNK names the game's Speech.bnk (extracted from
 *                   its paks; not shipped here), the changed copy loaded and
 *                   written to build\dialogue_patched.bnk for
 *                   tools\check_dialogue_bank.py
 *     dialoguegame  no Dialogue setting: the game's own dialogue, nothing read
 *     meter         [Measure] Meter=on, Limiter=off: the limiter slot emptied;
 *                   every bus metered; known readings (stereo plus mono
 *                   objects, as with spatial sound) come out in the summary
 *                   with the right peaks, counts, bus names and advice; the
 *                   last session's meter log kept as .prev
 *     prevlog       the last session's log is kept as .prev
 *     meterbad      Meter=on, metering data not laid out as expected: the meter
 *                   stops and says so, and reads nothing further
 *
 * Every scenario also checks the X3DAudio pass-through, the decision table
 * and the patch verification. */
#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef enum { REQ_AUTO, REQ_SPATIAL, REQ_LAYOUT, REQ_STEREO, REQ_HEADPHONES, REQ_MONO } request_t;
typedef struct { int ch; DWORD mask; const char *name; } layout_t;
typedef struct { BOOL open_gate; int report_channels; DWORD report_mask; BOOL headphone_pan; BOOL mono; const char *name; } plan_t;
typedef plan_t (*pfn_decide)(request_t, layout_t, BOOL, int, int);
typedef int (*pfn_gate)(BYTE *);
typedef BOOL (*pfn_wait)(DWORD);
typedef HRESULT (WINAPI *pfn_x3dinit)(UINT32, float, BYTE *);
typedef int (*pfn_fwin)(IMMDevice *);

static const layout_t NONE = { 0, 0, "" }, L51 = { 6, 0x60f, "5.1" }, L71 = { 8, 0x63f, "7.1" };

extern int fw_initialized, fw_pan, fw_setpan_calls, fw_replace_calls, fw_getdevid_calls, fw_setbusfx_calls;
extern uint32_t fw_speaker_cfg, fw_replace_settings[4], fw_setbusfx_args[3];
extern uint64_t fw_replace_id;
extern WCHAR fw_getdevid_endpoint[128];
extern const unsigned char *fw_file_data;
extern size_t fw_file_size;
extern int fw_stream_fail, fw_open_calls, fw_open_lang_first, fw_streams_open, fw_unaligned_reads;
extern int fw_lbw_calls, fw_lba_calls, fw_lbcopy_calls;
extern unsigned char *fw_lbcopy_data;
extern uint32_t fw_lbcopy_size;
extern void *fw_meter_vt[5], *fw_buscb;
extern uint32_t fw_buscb_bus, fw_buscb_flags;
extern int fw_regbus_calls, fw_master_regs;
extern void *fw_sfxcb, *fw_sfxcookie;
extern uint32_t fw_sfx_flags;

static void write_file(const char *dir, const char *name, const char *text)
{
    char path[MAX_PATH + 64];
    snprintf(path, sizeof path, "%s%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

/* The SDK's metering structures (AkCallback.h, Wwise 2021.1), written out
 * from the header, not from the mod. */
typedef struct { void **vt; float peak[16], tp[16], rms[16], k; } fw_meter;
typedef struct { void *pCookie; uint64_t gameObjID; void *pMetering; uint32_t channelConfig, eMeteringFlags; } sdk_bus_info;
typedef void (*pfn_buscb)(sdk_bus_info *);

/* Channel 1 carries the given levels, channel 0 half of them. */
static void fw_meter_set(fw_meter *m, int nch, float peak, float tp, float rms, float k)
{
    m->vt = fw_meter_vt;
    for (int c = 0; c < nch; c++) {
        float g = nch > 1 && c == 0 ? 0.5f : 1.0f;
        m->peak[c] = peak * g;
        m->tp[c] = tp * g;
        m->rms[c] = rms;
    }
    m->k = k;
}

static BOOL file_has(const char *dir, const char *name, const char *text)
{
    char path[MAX_PATH + 64], buf[16384];
    snprintf(path, sizeof path, "%s%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return FALSE;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, text) != NULL;
}

static BOOL wait_file(const char *dir, const char *name, const char *text, DWORD ms)
{
    ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        if (file_has(dir, name, text)) return TRUE;
        Sleep(50);
    }
    return file_has(dir, name, text);
}

static uint32_t ak_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { unsigned char c = (unsigned char)*s; if (c >= 'A' && c <= 'Z') c += 32; h = h * 16777619u ^ c; }
    return h;
}

static int fails;
#define CHECK(cond, ...) do { if (cond) printf("  ok   "); else { printf("  FAIL "); fails++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static const PROPERTYKEY k_devfmt = { {0xf19f064d,0x082c,0x4e27,{0xbc,0x73,0x68,0x82,0xa1,0xbb,0x8e,0x4c}}, 0 };

static int devfmt_channels(IMMDevice *dev)
{
    IPropertyStore *ps = NULL;
    PROPVARIANT v;
    int ch = -1;
    PropVariantInit(&v);
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps)) &&
        SUCCEEDED(IPropertyStore_GetValue(ps, &k_devfmt, &v)) && v.vt == VT_BLOB &&
        v.blob.cbSize >= sizeof(WAVEFORMATEX))
        ch = ((WAVEFORMATEX *)v.blob.pBlobData)->nChannels;
    PropVariantClear(&v);   /* must free a replaced blob cleanly */
    if (ps) IPropertyStore_Release(ps);
    return ch;
}

static void device_name(IMMDevice *d, char *out, size_t n)
{
    IPropertyStore *ps = NULL;
    PROPVARIANT v;
    PropVariantInit(&v);
    out[0] = 0;
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps)) &&
        SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
        WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, out, (int)n, NULL, NULL);
    PropVariantClear(&v);
    if (ps) IPropertyStore_Release(ps);
}

static BOOL log_has(const char *dir, const char *text)
{
    char path[MAX_PATH + 32], buf[16384];
    snprintf(path, sizeof path, "%sQuarryEssentialAudio.log", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return FALSE;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, text) != NULL;
}

static BOOL wait_log(const char *dir, const char *text, DWORD ms)
{
    ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        if (log_has(dir, text)) return TRUE;
        Sleep(50);
    }
    return log_has(dir, text);
}

/* Another active output than the default, for the device scenario. */
static IMMDevice *other_device(IMMDeviceEnumerator *en, IMMDevice *def)
{
    LPWSTR defid = NULL;
    IMMDevice_GetId(def, &defid);
    IMMDeviceCollection *col = NULL;
    IMMDevice *found = NULL;
    UINT n = 0;
    IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col);
    if (col) IMMDeviceCollection_GetCount(col, &n);
    for (UINT i = 0; i < n && !found; i++) {
        IMMDevice *d = NULL;
        LPWSTR id = NULL;
        IMMDeviceCollection_Item(col, i, &d);
        if (d && SUCCEEDED(IMMDevice_GetId(d, &id)) && wcscmp(id, defid)) found = d;
        else if (d) IMMDevice_Release(d);
        CoTaskMemFree(id);
    }
    if (col) IMMDeviceCollection_Release(col);
    CoTaskMemFree(defid);
    return found;
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: test_qsa <dll> <scenario>\n"); return 2; }
    const char *dll = argv[1], *mode = argv[2];
    char dir[MAX_PATH];
    GetFullPathNameA(dll, MAX_PATH, dir, NULL);
    char *slash = strrchr(dir, '\\');
    if (slash) slash[1] = 0;

    /* The device scenario needs to know the devices before writing the ini. */
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *def = NULL, *other = NULL;
    CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en);
    if (!en || FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &def))) {
        printf("FAIL: no audio device\n");
        return 1;
    }
    char other_name[256] = "";
    if (!strcmp(mode, "device")) {
        other = other_device(en, def);
        if (!other) { printf("scenario device: NOT EXERCISED (only one active output)\n"); return 0; }
        device_name(other, other_name, sizeof other_name);
    }

    char ini[MAX_PATH + 32];
    snprintf(ini, sizeof ini, "%sQuarryEssentialAudio.ini", dir);
    FILE *f = fopen(ini, "w");
    BOOL dlg = !strcmp(mode, "dialogue") || !strcmp(mode, "dialoguegame");
    BOOL meter = !strcmp(mode, "meter") || !strcmp(mode, "meterbad"), prevlog = !strcmp(mode, "prevlog");
    const char *out = !strcmp(mode, "layout51") || !strcmp(mode, "layout51no") ? "5.1"
                    : !strcmp(mode, "device") || !strcmp(mode, "nodevice") || !strcmp(mode, "nocomp") || dlg || meter || prevlog ? "auto" : mode;
    fprintf(f, "[Audio]\nOutput=%s\n", out);
    if (!strcmp(mode, "dialogue")) fprintf(f, "Dialogue=positional\n");
    if (!strcmp(mode, "nocomp")) fprintf(f, "Compression=off\n");
    if (!strcmp(mode, "device")) fprintf(f, "Device=%s\n", other_name);
    if (!strcmp(mode, "nodevice")) fprintf(f, "Device=zz no such device zz\n");
    if (meter) fprintf(f, "[Measure]\nMeter=on\n%s", !strcmp(mode, "meter") ? "Limiter=off\n" : "");
    fclose(f);
    SetEnvironmentVariableA("QSA_TEST_TARGET71", "1");
    if (!strcmp(mode, "auto") || !strcmp(mode, "headphones") || !strcmp(mode, "device") || !strcmp(mode, "nodevice") ||
        !strcmp(mode, "nocomp") || dlg || meter || prevlog)
        SetEnvironmentVariableA("QSA_TEST_NOSPATIAL", "1");
    if (!strcmp(mode, "layout51no")) SetEnvironmentVariableA("QSA_TEST_REFUSE", "1");

    /* A summary left by an earlier run would answer the checks below. */
    char stale[MAX_PATH + 64];
    snprintf(stale, sizeof stale, "%sQuarryEssentialAudio_meter_summary.txt", dir);
    DeleteFileA(stale);
    snprintf(stale, sizeof stale, "%sQuarryEssentialAudio_meter.log", dir);
    DeleteFileA(stale);
    /* The last session's files, which the mod must keep as .prev (any .prev
     * from an earlier run removed first: it would answer the check). */
    const char *prevs[] = { "QuarryEssentialAudio.log.prev", "QuarryEssentialAudio_meter.log.prev",
                            "QuarryEssentialAudio_meter_summary.txt.prev" };
    for (int i = 0; i < 3; i++) {
        snprintf(stale, sizeof stale, "%s%s", dir, prevs[i]);
        DeleteFileA(stale);
    }
    write_file(dir, "QuarryEssentialAudio.log", "OLD SESSION LOG\r\n");
    write_file(dir, "QuarryEssentialAudio_meter.log", "OLD METER LOG\r\n");
    HMODULE m = LoadLibraryA(dll);
    if (!m) { printf("FAIL: cannot load %s\n", dll); return 1; }
    pfn_decide decide = (pfn_decide)(void *)GetProcAddress(m, "qsa_decide");
    pfn_gate gate = (pfn_gate)(void *)GetProcAddress(m, "qsa_patch_gate_at");
    pfn_wait wait = (pfn_wait)(void *)GetProcAddress(m, "qsa_wait_setup");
    pfn_x3dinit px = (pfn_x3dinit)(void *)GetProcAddress(m, "X3DAudioInitialize");
    if (!decide || !gate || !wait || !px) { printf("FAIL: missing exports\n"); return 1; }
    printf("scenario %s\n", mode);
    CHECK(wait(5000), "setup finished");

    printf("pass-through\n");
    {
        char sys[MAX_PATH];
        GetSystemDirectoryA(sys, MAX_PATH);
        strcat(sys, "\\X3DAudio1_7.dll");
        HMODULE real = LoadLibraryA(sys);
        pfn_x3dinit rx = real ? (pfn_x3dinit)(void *)GetProcAddress(real, "X3DAudioInitialize") : NULL;
        BYTE a[20] = {0}, b[20] = {0}, z[20] = {0};
        px(0x3f, 343.5f, a);
        if (rx) rx(0x3f, 343.5f, b);
        CHECK(rx && !memcmp(a, b, 20) && memcmp(a, z, 20), "X3DAudioInitialize matches the real DLL");
        CHECK(GetProcAddress(m, (LPCSTR)1) == GetProcAddress(m, "X3DAudioCalculate") &&
              GetProcAddress(m, (LPCSTR)2) == GetProcAddress(m, "X3DAudioInitialize"), "ordinals 1/2 as the real DLL");
    }

    printf("decisions\n");
    {
        plan_t p;
        p = decide(REQ_AUTO, NONE, TRUE, 8, 2);
        CHECK(p.open_gate && p.report_channels == 8 && !p.report_mask, "auto, spatial on, 8>2: spatial, 7.1 fallback");
        p = decide(REQ_AUTO, NONE, FALSE, 8, 2);
        CHECK(!p.open_gate && p.report_channels == 8 && !p.report_mask, "auto, spatial off, 8>2: the mix layout");
        p = decide(REQ_AUTO, NONE, FALSE, 2, 2);
        CHECK(!p.open_gate && !p.report_channels, "auto, plain stereo device: unchanged");
        p = decide(REQ_AUTO, NONE, FALSE, 8, 8);
        CHECK(!p.report_channels, "auto, real 7.1 device: unchanged (already 7.1)");
        p = decide(REQ_SPATIAL, NONE, FALSE, 8, 2);
        CHECK(!p.open_gate && p.report_channels == 8, "spatial requested but off: the mix layout instead");
        p = decide(REQ_LAYOUT, L51, TRUE, 8, 2);
        CHECK(!p.open_gate && p.report_channels == 6 && p.report_mask == 0x60f, "5.1 with spatial on: 5.1, no spatial");
        p = decide(REQ_LAYOUT, L51, FALSE, 2, 2);
        CHECK(p.report_channels == 6 && p.report_mask == 0x60f, "5.1 on a device that looks stereo: 5.1");
        p = decide(REQ_LAYOUT, L71, FALSE, 8, 8);
        CHECK(!p.report_channels, "7.1 on real 7.1 hardware: nothing to change");
        p = decide(REQ_STEREO, NONE, TRUE, 8, 8);
        CHECK(!p.open_gate && p.report_channels == 2 && p.report_mask == 0x3, "stereo on a 7.1 device: 2 channels");
        p = decide(REQ_STEREO, NONE, TRUE, 8, 2);
        CHECK(!p.report_channels, "stereo on a stereo device: unchanged");
        p = decide(REQ_HEADPHONES, NONE, TRUE, 8, 2);
        CHECK(p.open_gate && !p.headphone_pan, "headphones with spatial on: spatial");
        p = decide(REQ_HEADPHONES, NONE, FALSE, 2, 2);
        CHECK(!p.open_gate && p.headphone_pan && !p.report_channels, "headphones, spatial off: stereo + headphone panning");
        p = decide(REQ_MONO, NONE, TRUE, 8, 2);
        CHECK(!p.open_gate && !p.report_channels && p.mono, "mono with spatial on: mono only");
        p = decide(REQ_AUTO, NONE, TRUE, 8, 2);
        CHECK(!p.mono, "only Output=mono asks for mono");
    }

    printf("patch verification\n");
    {
        static const BYTE sig[18] = { 0x45,0x38,0x65,0x08, 0x74,0x13, 0x8b,0xc6, 0x25,0x00,0xf0,0xff,0xff,
                                      0x3d,0x00,0x40,0x00,0x00 };
        BYTE *a = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        BYTE *b = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        DWORD old;
        memcpy(a + 100, sig, 18);
        memcpy(b + 100, sig, 18);
        b[109] ^= 1;
        VirtualProtect(a, 4096, PAGE_EXECUTE_READ, &old);
        VirtualProtect(b, 4096, PAGE_EXECUTE_READ, &old);
        BYTE expect[18];
        memcpy(expect, sig, 18);
        expect[4] = expect[5] = 0x90;
        int r1 = gate(a + 100);
        MEMORY_BASIC_INFORMATION mbi;
        VirtualQuery(a + 104, &mbi, sizeof mbi);
        CHECK(r1 == 1 && !memcmp(a + 100, expect, 18) && mbi.Protect == PAGE_EXECUTE_READ,
              "matching bytes: only the jump changed, protection restored (r %d)", r1);
        CHECK(gate(a + 100) == 0, "second call: already patched");
        BYTE copy[18];
        memcpy(copy, b + 100, 18);
        int r3 = gate(b + 100);
        CHECK(r3 == -1 && !memcmp(b + 100, copy, 18), "one byte different: refused, untouched (r %d)", r3);
    }

    printf("%s\n", mode);
    if (!strcmp(mode, "auto")) {
        HMODULE fw = LoadLibraryA("build\\fake_windows.dll");
        pfn_fwin fwin = fw ? (pfn_fwin)(void *)GetProcAddress(fw, "fwin_devfmt_channels") : NULL;
        int direct = devfmt_channels(def), indirect = fwin ? fwin(def) : -9;
        CHECK(direct == 8, "the game's own read: told 8 channels (got %d)", direct);
        CHECK(indirect > 0 && indirect < 8, "a read made by another module: the true format (got %d)", indirect);
        CHECK(log_has(dir, "told 8 channels instead of"), "logged");
        fw_speaker_cfg = 0x63f108;
        fw_initialized = 1;
        CHECK(wait_log(dir, "Wwise is mixing to: 7.1.", 3000), "Wwise's layout read back and logged as 7.1");
        Sleep(700);
        CHECK(!fw_replace_calls && !fw_setpan_calls, "output not rebuilt, panning left alone");
        CHECK(!fw_setbusfx_calls && log_has(dir, "Compression: on."), "compression left on by default");
    } else if (!strcmp(mode, "nocomp")) {
        CHECK(log_has(dir, "Compression: off."), "setting read");
        CHECK(!fw_setbusfx_calls, "nothing sent before Wwise is running");
        fw_initialized = 1;
        CHECK(wait_log(dir, "Asked Wwise to remove the master compressor (the limiter stays).", 3000) &&
              fw_setbusfx_args[0] == 3803692087u && fw_setbusfx_args[1] == 0 && fw_setbusfx_args[2] == 0,
              "SetBusEffect(%u, slot %u, %u): Master Audio Bus, compressor slot emptied",
              fw_setbusfx_args[0], fw_setbusfx_args[1], fw_setbusfx_args[2]);
        Sleep(2600);
        CHECK(fw_setbusfx_calls >= 2 && fw_setbusfx_args[1] == 0, "repeated while the bank loads (%d calls), always slot 0",
              fw_setbusfx_calls);
    } else if (!strcmp(mode, "meter")) {
        CHECK(log_has(dir, "Measuring: level meter on, limiter OFF"), "settings read and logged");
        CHECK(!fw_setbusfx_calls && !fw_regbus_calls, "nothing sent before Wwise is running");
        fw_initialized = 1;
        CHECK(wait_log(dir, "Asked Wwise to remove the master limiter (for measuring only).", 3000) &&
              fw_setbusfx_args[0] == 3803692087u && fw_setbusfx_args[1] == 1 && fw_setbusfx_args[2] == 0,
              "SetBusEffect(%u, slot %u, %u): Master Audio Bus, limiter slot emptied",
              fw_setbusfx_args[0], fw_setbusfx_args[1], fw_setbusfx_args[2]);
        CHECK(wait_log(dir, "Level meter on (master bus and 89 other buses)", 3000) && fw_buscb,
              "the master and the 89 other buses registered");
        CHECK(fw_master_regs == 1 && fw_regbus_calls == 90, "the master registered once (a second would replace it): %d of %d",
              fw_master_regs, fw_regbus_calls);
        CHECK(fw_buscb_flags == (1 | 2 | 4 | 16) && fw_sfx_flags == 1,
              "master: peak, true peak, RMS, K power; other buses: peak");
        /* Wwise's audio thread, as measured with spatial sound: each frame, one
         * stereo reading and one per mono object (two here). 46 frames; the
         * stereo one is quiet except one frame at +3 dBTP and five at the -1 dB
         * ceiling; one mono object reaches +8 dBTP once. */
        static fw_meter bm, ob, sfx;
        sdk_bus_info bi = { NULL, 0, &bm, 0x3102, 1 | 2 | 4 | 16 };
        sdk_bus_info obi = { NULL, 0, &ob, 0x4101, 1 | 2 | 4 | 16 };
        sdk_bus_info si = { fw_sfxcookie, 0, &sfx, 0x3102, 1 };
        fw_meter_set(&sfx, 2, 0.5f, 0, 0, 0);         /* SFX bus: -6 dBFS */
        for (int i = 0; i < 46; i++) {
            float tp = i == 20 ? 1.41254f : i >= 30 && i < 35 ? 0.89125f : 0.25f;
            fw_meter_set(&bm, 2, tp * 0.9f, tp, tp * 0.3f, i == 20 ? 0.5f : 0.01f);
            ((pfn_buscb)fw_buscb)(&bi);
            for (int k = 0; k < 2; k++) {
                float otp = i == 25 && k == 1 ? 2.51189f : 0.1f;   /* +8 dBTP */
                fw_meter_set(&ob, 1, otp, otp, otp, 9.0f);       /* big K power: must not count */
                ((pfn_buscb)fw_buscb)(&obi);
            }
            ((pfn_buscb)fw_sfxcb)(&si);
        }
        const char *sum = "QuarryEssentialAudio_meter_summary.txt";
        CHECK(wait_file(dir, sum, "  138 readings", 8000), "every reading counted (positive count)");
        CHECK(file_has(dir, sum, "stereo (0x3102): 46 readings; loudest true peak +3.0 dBTP, sample peak +2.1 dBFS;"
                                 " at or above -1.1 dBTP 6, over 0 dBTP 1"), "stereo on its own: peaks from the right slots, counts");
        CHECK(file_has(dir, sum, "mono (0x4101): 92 readings; loudest true peak +8.0 dBTP"), "objects kept apart by layout");
        CHECK(file_has(dir, sum, "loudest momentary loudness (widest layout) -15.2 LUFS"),
              "loudness from the widest layout only (19-reading mean around the +3 dB one)");
        CHECK(file_has(dir, sum, "  -6.0 dBFS  SFX [Master Audio Bus]"), "a bus named, with its parent, from the cookie");
        CHECK(file_has(dir, sum, "turn the game down by 9.0 dB"), "advice from the loudest reading (object +8.0 => 9.0 dB)");
        CHECK(file_has(dir, "QuarryEssentialAudio_meter.log", "OVER 0"), "per-second log marks the over");
        CHECK(file_has(dir, "QuarryEssentialAudio_meter.log.prev", "OLD METER LOG") &&
              !file_has(dir, "QuarryEssentialAudio_meter.log", "OLD METER LOG"), "the last session's meter log kept as .prev");
    } else if (prevlog) {
        CHECK(file_has(dir, "QuarryEssentialAudio.log.prev", "OLD SESSION LOG") && !log_has(dir, "OLD SESSION LOG"),
              "the last session's log kept as .prev, a new one started");
    } else if (!strcmp(mode, "meterbad")) {
        fw_initialized = 1;
        CHECK(wait_log(dir, "Level meter on", 3000) && fw_buscb, "meter registered");
        CHECK(!fw_setbusfx_calls, "limiter left alone when only the meter is on");
        static fw_meter bm;
        fw_meter_set(&bm, 2, 0.5f, 0.5f, 0.1f, 0.01f);
        sdk_bus_info bad = { NULL, 0, &bm, 0x3102, 1 };   /* flags not what the mod asked for */
        ((pfn_buscb)fw_buscb)(&bad);
        sdk_bus_info good = { NULL, 0, &bm, 0x3102, 1 | 2 | 4 | 16 };
        ((pfn_buscb)fw_buscb)(&good);
        CHECK(wait_log(dir, "Level meter stopped: the master bus's metering data did not have the expected layout.", 4000),
              "stopped and logged");
        CHECK(wait_file(dir, "QuarryEssentialAudio_meter_summary.txt", "no data: Wwise never called the master bus meter.", 3000),
              "nothing read after stopping, even good frames");
    } else if (!strcmp(mode, "layout51")) {
        IAudioClient *ac = NULL;
        WAVEFORMATEX *closest = NULL;
        IMMDevice_Activate(def, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
        WAVEFORMATEXTENSIBLE x;
        memset(&x, 0, sizeof x);
        x.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        x.Format.nChannels = 6;
        x.Format.nSamplesPerSec = 48000;   /* the test's synthetic mix rate */
        x.Format.wBitsPerSample = 32;
        x.Format.nBlockAlign = 24;
        x.Format.nAvgBytesPerSec = 48000 * 24;
        x.Format.cbSize = 22;
        x.Samples.wValidBitsPerSample = 32;
        x.dwChannelMask = 0x60f;
        x.SubFormat = (GUID){ WAVE_FORMAT_IEEE_FLOAT, 0, 0x10, { 0x80,0,0,0xaa,0,0x38,0x9b,0x71 } };
        BOOL accepted = ac && IAudioClient_IsFormatSupported(ac, AUDCLNT_SHAREMODE_SHARED, &x.Format, &closest) == S_OK;
        if (closest) CoTaskMemFree(closest);
        if (ac) IAudioClient_Release(ac);
        int direct = devfmt_channels(def);
        printf("  (Windows %s 5.1 on this device)\n", accepted ? "accepts" : "refuses");
        if (accepted) CHECK(direct == 6 && log_has(dir, "Chosen: 5.1."), "accepted: told 5.1 (got %d)", direct);
        else CHECK(direct == 8 && log_has(dir, "does not accept 5.1"), "refused: fell back to 7.1 (got %d)", direct);
    } else if (!strcmp(mode, "layout51no")) {
        int direct = devfmt_channels(def);
        CHECK(direct == 8 && log_has(dir, "Windows does not accept 5.1 on this device.") &&
              log_has(dir, "Chosen: surround (the channel layout Windows mixes at)."),
              "refused: logged, the mix layout used instead (got %d)", direct);
    } else if (!strcmp(mode, "mono")) {
        int direct = devfmt_channels(def);
        CHECK(direct > 0 && direct != 8 && log_has(dir, "Chosen: mono"), "device format left alone (got %d)", direct);
        CHECK(!fw_replace_calls, "nothing sent before Wwise is running");
        fw_initialized = 1;
        CHECK(wait_log(dir, "Asked Wwise to mix in mono.", 3000) && fw_replace_calls == 1 && fw_replace_id == 0 &&
              fw_replace_settings[0] == 0 && fw_replace_settings[1] == 0 && fw_replace_settings[2] == 0 &&
              fw_replace_settings[3] == 0x4101,
              "ReplaceOutput({0, 0, 0, 0x%x}, output %llu) once Wwise runs", fw_replace_settings[3],
              (unsigned long long)fw_replace_id);
        CHECK(wait_log(dir, "Wwise is mixing to: mono.", 3000), "mono read back and logged");
        Sleep(1500);
        CHECK(fw_replace_calls == 1, "done once (%d calls)", fw_replace_calls);
    } else if (!strcmp(mode, "headphones")) {
        CHECK(log_has(dir, "Chosen: stereo with headphone panning."), "chose stereo with headphone panning");
        fw_initialized = 1;
        CHECK(wait_log(dir, "Wwise panning set to headphones.", 3000) && fw_pan == 1 && fw_setpan_calls == 1,
              "headphone panning set once and read back (pan %d, calls %d)", fw_pan, fw_setpan_calls);
        CHECK(!fw_replace_calls, "output not rebuilt (default device, not mono)");
    } else if (!strcmp(mode, "spatial")) {
        CHECK(log_has(dir, "Spatial sound NOT enabled: this game version is not the one this mod knows."),
              "refused to patch an exe whose bytes do not match");
    } else if (!strcmp(mode, "device")) {
        printf("  (chosen device: %s)\n", other_name);
        char want[300];
        snprintf(want, sizeof want, "Using the chosen device: %s.", other_name);
        CHECK(log_has(dir, want), "the named device is used");
        LPWSTR id = NULL;
        IMMDevice_GetId(other, &id);
        CHECK(fw_getdevid_calls == 1 && id && !wcscmp(fw_getdevid_endpoint, id),
              "Wwise's id asked for that device (%d call(s))", fw_getdevid_calls);
        CoTaskMemFree(id);
        int on_other = devfmt_channels(other), on_def = devfmt_channels(def);
        CHECK(on_other == 8, "the chosen device's format is widened (got %d)", on_other);
        CHECK(on_def > 0 && on_def < 8, "the default device's format is not (got %d)", on_def);
        fw_initialized = 1;
        CHECK(wait_log(dir, "Asked Wwise to play on the chosen device.", 3000) && fw_replace_calls == 1 &&
              fw_replace_id == 0 && fw_replace_settings[0] == 0 && fw_replace_settings[1] == 4242 &&
              fw_replace_settings[3] == 0,
              "ReplaceOutput({0, %u, %u, 0x%x}) on the main output", fw_replace_settings[1], fw_replace_settings[2],
              fw_replace_settings[3]);
    } else if (!strcmp(mode, "dialogue") || !strcmp(mode, "dialoguegame")) {
        HMODULE me = GetModuleHandleA(NULL);
        int (*lbw)(const wchar_t *, uint32_t *) = (void *)GetProcAddress(me, "?LoadBank@SoundEngine@AK@@YA?AW4AKRESULT@@PEB_WAEAI@Z");
        int (*lba)(const char *, uint32_t *) = (void *)GetProcAddress(me, "?LoadBank@SoundEngine@AK@@YA?AW4AKRESULT@@PEBDAEAI@Z");
        uint32_t id = 0;
        /* A made-up dialogue bank: right header and id, one attenuation with
         * a real id but other contents -- what a game update would look like. */
        static unsigned char synth[64];
        uint32_t v;
        memcpy(synth, "BKHD", 4); v = 8; memcpy(synth + 4, &v, 4); v = 140; memcpy(synth + 8, &v, 4);
        v = ak_hash("Speech"); memcpy(synth + 12, &v, 4);
        memcpy(synth + 16, "HIRC", 4); v = 4 + 5 + 20; memcpy(synth + 20, &v, 4); v = 1; memcpy(synth + 24, &v, 4);
        synth[28] = 0x0E; v = 20; memcpy(synth + 29, &v, 4); v = 863852533u; memcpy(synth + 33, &v, 4);
        size_t synth_n = 33 + 20;

        if (!strcmp(mode, "dialoguegame")) {
            fw_file_data = synth; fw_file_size = synth_n;
            int r = lbw(L"Speech", &id);
            CHECK(log_has(dir, "Dialogue: game.") && r == 1 && fw_lbw_calls == 1 && !fw_open_calls && !fw_lbcopy_calls,
                  "the default is the game's own dialogue: loaded as the game does, nothing read");
        } else {
            CHECK(log_has(dir, "Dialogue: positional.") &&
                  log_has(dir, "Dialogue: spread 30% close, 5% from 3 m; 6 dB quieter per doubling of distance from 2 m, "
                               "at most 12 dB; muffling from 2 m, 35 at 20 m."),
                  "setting read; the defaults reported");
            fw_file_data = synth; fw_file_size = synth_n;
            int r = lbw(L"Speech", &id);
            CHECK(r == 1 && id == ak_hash("Speech") && fw_lbw_calls == 1 && !fw_lbcopy_calls &&
                  log_has(dir, "Dialogue NOT changed: this version of the game's dialogue is not the one this mod knows. "
                               "The game's own dialogue is used."),
                  "another version of the dialogue: the game's own load, logged");
            CHECK(fw_open_lang_first == 1 && !fw_streams_open && !fw_unaligned_reads,
                  "read as Wwise's bank reader does: language-specific first, whole blocks, the stream closed");
            fw_stream_fail = 1;
            r = lbw(L"Speech", &id);
            CHECK(r == 1 && fw_lbw_calls == 2 && !fw_lbcopy_calls && log_has(dir, "could not open the game's dialogue file"),
                  "the file will not open: the game's own load, logged");
            r = lba("Speech", &id);
            CHECK(r == 1 && fw_lba_calls == 1 && !fw_lbcopy_calls, "the same through LoadBank by narrow name");
            fw_stream_fail = 0;
            int opens = fw_open_calls;
            r = lbw(L"Init", &id);
            CHECK(r == 1 && id == ak_hash("Init") && fw_lbw_calls == 3 && fw_open_calls == opens,
                  "any other bank: the game's own load, nothing read");

            char real[MAX_PATH];
            if (GetEnvironmentVariableA("QSA_SPEECH_BNK", real, MAX_PATH)) {
                static unsigned char bank[4 << 20];
                FILE *bf = fopen(real, "rb");
                size_t n = bf ? fread(bank, 1, sizeof bank, bf) : 0;
                if (bf) fclose(bf);
                fw_file_data = bank; fw_file_size = n;
                int calls = fw_lbw_calls;
                id = 0;
                r = n ? lbw(L"Speech", &id) : -1;
                CHECK(r == 1 && id == ak_hash("Speech") && fw_lbw_calls == calls && fw_lbcopy_calls == 1 &&
                      fw_lbcopy_size > n && !fw_streams_open &&
                      log_has(dir, "Positional dialogue applied: 32 of 32 distance settings changed."),
                      "the game's own Speech.bnk: changed copy loaded instead (%u -> %u bytes)", (unsigned)n, fw_lbcopy_size);
                FILE *pf = fopen("build\\dialogue_patched.bnk", "wb");
                if (pf && fw_lbcopy_data) fwrite(fw_lbcopy_data, 1, fw_lbcopy_size, pf);
                if (pf) fclose(pf);
            } else {
                printf("  NOT EXERCISED: the game's own dialogue (set QSA_SPEECH_BNK to an extracted Speech.bnk)\n");
            }
        }
    } else if (!strcmp(mode, "nodevice")) {
        CHECK(log_has(dir, "No output device matches the Device setting. Available devices:") &&
              log_has(dir, "Using the Windows default device:"), "devices listed, default used");
        fw_initialized = 1;
        Sleep(1200);
        CHECK(!fw_replace_calls && !fw_getdevid_calls, "output not rebuilt");
    }

    if (other) IMMDevice_Release(other);
    IMMDevice_Release(def);
    IMMDeviceEnumerator_Release(en);
    printf("%s: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
