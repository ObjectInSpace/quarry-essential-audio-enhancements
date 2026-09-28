/* Offline tests for Quarry Spatial Audio. This exe plays the part of the game
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
    snprintf(ini, sizeof ini, "%sQuarrySpatial.ini", dir);
    FILE *f = fopen(ini, "w");
    const char *out = !strcmp(mode, "layout51") || !strcmp(mode, "layout51no") ? "5.1"
                    : !strcmp(mode, "device") || !strcmp(mode, "nodevice") || !strcmp(mode, "nocomp") ? "auto" : mode;
    fprintf(f, "[Audio]\nOutput=%s\n", out);
    if (!strcmp(mode, "nocomp")) fprintf(f, "Compression=off\n");
    if (!strcmp(mode, "device")) fprintf(f, "Device=%s\n", other_name);
    if (!strcmp(mode, "nodevice")) fprintf(f, "Device=zz no such device zz\n");
    fclose(f);
    SetEnvironmentVariableA("QSA_TEST_TARGET71", "1");
    if (!strcmp(mode, "auto") || !strcmp(mode, "headphones") || !strcmp(mode, "device") || !strcmp(mode, "nodevice") ||
        !strcmp(mode, "nocomp"))
        SetEnvironmentVariableA("QSA_TEST_NOSPATIAL", "1");
    if (!strcmp(mode, "layout51no")) SetEnvironmentVariableA("QSA_TEST_REFUSE", "1");

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
