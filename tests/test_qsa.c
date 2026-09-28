/* Offline tests for Quarry Spatial Audio. This exe plays the part of the game
 * (it exports stand-ins for the Wwise functions the mod calls).
 *
 *   test_qsa.exe <dll> surround     told 7.1; Windows-internal reads see the truth
 *   test_qsa.exe <dll> headphones   spatial forced "off": stereo + headphone panning
 *   test_qsa.exe <dll> spatial      not The Quarry: the code patch must be refused
 *
 * Every mode also checks the X3DAudio pass-through, the decision table and
 * the patch verification. */
#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef enum { REQ_AUTO, REQ_SPATIAL, REQ_SURROUND, REQ_STEREO, REQ_HEADPHONES } request_t;
typedef struct { BOOL open_gate; int report_channels; DWORD report_mask; BOOL headphone_pan; const char *name; } plan_t;
typedef struct { int ch; DWORD mask; const char *name; } layout_t;
typedef plan_t (*pfn_decide)(request_t, BOOL, int, int, layout_t);
static const layout_t AUTO = { 0, 0, "auto" }, L51 = { 6, 0x60f, "5.1" }, L71 = { 8, 0x63f, "7.1" };
typedef int (*pfn_gate)(BYTE *);
typedef BOOL (*pfn_wait)(DWORD);
typedef HRESULT (WINAPI *pfn_x3dinit)(UINT32, float, BYTE *);
typedef int (*pfn_fwin)(IMMDevice *);

extern int fw_initialized, fw_pan, fw_setpan_calls;
extern uint32_t fw_speaker_cfg;

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

static BOOL log_has(const char *dir, const char *text)
{
    char path[MAX_PATH], buf[8192];
    snprintf(path, sizeof path, "%sQuarrySpatial.log", dir);
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

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: test_qsa <dll> surround|headphones|spatial\n"); return 2; }
    const char *dll = argv[1], *mode = argv[2];
    char dir[MAX_PATH];
    GetFullPathNameA(dll, MAX_PATH, dir, NULL);
    char *slash = strrchr(dir, '\\');
    if (slash) slash[1] = 0;

    char ini[MAX_PATH];
    snprintf(ini, sizeof ini, "%sQuarrySpatial.ini", dir);
    FILE *f = fopen(ini, "w");
    if (!strcmp(mode, "surround51") || !strcmp(mode, "surround51refused"))
        fprintf(f, "[Audio]\nOutput=surround\nSpeakers=5.1\n");
    else fprintf(f, "[Audio]\nOutput=%s\n", mode);
    fclose(f);
    SetEnvironmentVariableA("QSA_TEST_TARGET71", "1");
    if (!strcmp(mode, "headphones")) SetEnvironmentVariableA("QSA_TEST_NOSPATIAL", "1");
    if (!strcmp(mode, "surround51refused")) SetEnvironmentVariableA("QSA_TEST_REFUSE", "1");

    HMODULE m = LoadLibraryA(dll);
    if (!m) { printf("FAIL: cannot load %s\n", dll); return 1; }
    pfn_decide decide = (pfn_decide)(void *)GetProcAddress(m, "qsa_decide");
    pfn_gate gate = (pfn_gate)(void *)GetProcAddress(m, "qsa_patch_gate_at");
    pfn_wait wait = (pfn_wait)(void *)GetProcAddress(m, "qsa_wait_setup");
    pfn_x3dinit px = (pfn_x3dinit)(void *)GetProcAddress(m, "X3DAudioInitialize");
    if (!decide || !gate || !wait || !px) { printf("FAIL: missing exports\n"); return 1; }
    printf("mode %s\n", mode);
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
        p = decide(REQ_AUTO, TRUE, 8, 2, AUTO);
        CHECK(p.open_gate && p.report_channels == 8 && !p.report_mask && !p.headphone_pan, "auto, spatial on, 8>2: spatial with 7.1 fallback");
        p = decide(REQ_AUTO, FALSE, 8, 2, AUTO);
        CHECK(!p.open_gate && p.report_channels == 8 && !p.report_mask, "auto, spatial off, 8>2: surround at the mix format");
        p = decide(REQ_AUTO, FALSE, 2, 2, AUTO);
        CHECK(!p.open_gate && p.report_channels == 0, "auto, plain stereo device: unchanged");
        p = decide(REQ_AUTO, FALSE, 8, 8, AUTO);
        CHECK(!p.open_gate && p.report_channels == 0, "auto, real 7.1 device: unchanged (already 7.1)");
        p = decide(REQ_SPATIAL, FALSE, 8, 2, AUTO);
        CHECK(!p.open_gate && p.report_channels == 8, "spatial requested but off: surround instead");
        p = decide(REQ_SURROUND, TRUE, 8, 2, AUTO);
        CHECK(!p.open_gate && p.report_channels == 8, "surround requested with spatial on: surround only");
        p = decide(REQ_STEREO, TRUE, 8, 8, AUTO);
        CHECK(!p.open_gate && p.report_channels == 2 && p.report_mask == 0x3, "stereo on a 7.1 device: report 2 channels");
        p = decide(REQ_STEREO, TRUE, 8, 2, AUTO);
        CHECK(!p.open_gate && p.report_channels == 0, "stereo on a stereo device: unchanged");
        p = decide(REQ_HEADPHONES, TRUE, 8, 2, AUTO);
        CHECK(p.open_gate && !p.headphone_pan, "headphones with spatial on: spatial");
        p = decide(REQ_HEADPHONES, FALSE, 2, 2, AUTO);
        CHECK(!p.open_gate && p.headphone_pan && p.report_channels == 0, "headphones, spatial off: stereo + headphone panning");
        p = decide(REQ_SURROUND, FALSE, 2, 2, L51);
        CHECK(p.report_channels == 6 && p.report_mask == 0x60f, "Speakers=5.1 on a device that looks stereo: 5.1");
        p = decide(REQ_SURROUND, FALSE, 8, 2, L51);
        CHECK(p.report_channels == 6 && p.report_mask == 0x60f, "Speakers=5.1 wins over a 7.1 mix");
        p = decide(REQ_AUTO, FALSE, 8, 8, L71);
        CHECK(p.report_channels == 0, "Speakers=7.1 on real 7.1 hardware: nothing to change");
        p = decide(REQ_AUTO, TRUE, 8, 2, L51);
        CHECK(p.open_gate && p.report_channels == 6 && p.report_mask == 0x60f, "spatial on with Speakers=5.1: spatial, 5.1 fallback");
        p = decide(REQ_STEREO, FALSE, 8, 8, L51);
        CHECK(p.report_channels == 2 && p.report_mask == 0x3, "Output=stereo ignores Speakers");
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

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en);
    if (!en || FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev))) {
        printf("FAIL: no audio device\n");
        return 1;
    }

    if (!strcmp(mode, "surround")) {
        printf("surround\n");
        HMODULE fw = LoadLibraryA("build\\fake_windows.dll");
        pfn_fwin fwin = fw ? (pfn_fwin)(void *)GetProcAddress(fw, "fwin_devfmt_channels") : NULL;
        int direct = devfmt_channels(dev);
        int indirect = fwin ? fwin(dev) : -9;
        CHECK(direct == 8, "the game's own read: told 8 channels (got %d)", direct);
        CHECK(indirect > 0 && indirect < 8, "a read made by another module: the true format (got %d)", indirect);
        CHECK(log_has(dir, "told 8 channels instead of"), "logged once");
        fw_speaker_cfg = 0x63f108;
        fw_initialized = 1;
        CHECK(wait_log(dir, "Wwise is mixing to: 7.1.", 3000), "Wwise's layout read back and logged as 7.1");
        CHECK(fw_setpan_calls == 0, "panning left alone");
    } else if (!strcmp(mode, "surround51")) {
        printf("surround with Speakers=5.1\n");
        /* Ask Windows the same question the mod asks, then hold the mod to the answer. */
        IAudioClient *ac = NULL;
        WAVEFORMATEX *mix = NULL, *closest = NULL;
        IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
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
        if (mix) CoTaskMemFree(mix);
        if (ac) IAudioClient_Release(ac);
        int direct = devfmt_channels(dev);
        printf("  (Windows %s 5.1 on this device)\n", accepted ? "accepts" : "refuses");
        if (accepted)
            CHECK(direct == 6 && log_has(dir, "Chosen: surround (the speaker layout set in QuarrySpatial.ini)."),
                  "accepted: the game is told 5.1 (got %d)", direct);
        else
            CHECK(direct == 8 && log_has(dir, "Windows does not accept 5.1 on this device"),
                  "refused: logged, automatic 7.1 used instead (got %d)", direct);
    } else if (!strcmp(mode, "surround51refused")) {
        printf("surround with Speakers=5.1, Windows refusal simulated\n");
        int direct = devfmt_channels(dev);
        CHECK(direct == 8 && log_has(dir, "Windows does not accept 5.1 on this device") &&
              log_has(dir, "Chosen: surround (the channel layout Windows mixes at)."),
              "refused: logged, automatic 7.1 used instead (got %d)", direct);
    } else if (!strcmp(mode, "headphones")) {
        printf("headphones (spatial simulated off)\n");
        CHECK(log_has(dir, "Chosen: stereo with headphone panning."), "chose stereo with headphone panning");
        fw_initialized = 1;
        CHECK(wait_log(dir, "Wwise panning set to headphones.", 3000) && fw_pan == 1 && fw_setpan_calls == 1,
              "headphone panning set once and read back (pan %d, calls %d)", fw_pan, fw_setpan_calls);
    } else if (!strcmp(mode, "spatial")) {
        printf("spatial (this exe is not The Quarry)\n");
        CHECK(log_has(dir, "Spatial sound NOT enabled: this game version is not the one this mod knows."),
              "refused to patch an exe whose bytes do not match");
    }

    IMMDevice_Release(dev);
    IMMDeviceEnumerator_Release(en);
    printf("%s: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
