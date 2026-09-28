/* Quarry Spatial Audio -- surround and Windows spatial sound for The Quarry.
 * Copyright (C) 2026 ObjectInSpace
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version. It is distributed WITHOUT ANY WARRANTY; see LICENSE.
 *
 * How it works (details in README.md):
 *
 * The game loads X3DAudio1_7.dll from its own folder before any game code
 * runs, so this DLL takes that name and passes the two real functions through
 * to the Windows copy.
 *
 * The game's sound engine (Audiokinetic Wwise) sizes its output from the
 * audio device's HARDWARE format (PKEY_AudioEngine_DeviceFormat). A stereo
 * interface therefore gets stereo even when Dolby Atmos or Windows Sonic has
 * Windows mixing at 7.1. Answering that one property read with the format
 * Windows actually mixes at makes Wwise open, and pan into, that layout.
 *
 * Wwise also contains Windows spatial sound support (7.1.4 plus positioned
 * objects), gated on an "Allow 3D Audio" setting the game ships switched off.
 * The gate is one conditional jump; when spatial output is wanted and
 * available, it is turned into two NOPs after all 18 surrounding bytes are
 * verified.
 */
#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <spatialaudioclient.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#define QSA_VERSION "1.0.0"

static HMODULE g_self, g_game;
static WCHAR g_dir[MAX_PATH];
static CRITICAL_SECTION g_logcs, g_patchcs;

/* ---- log: a handful of lines per session ---- */
static void qlog(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 3) n = sizeof buf - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    WCHAR path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%lsQuarrySpatial.log", g_dir);
    EnterCriticalSection(&g_logcs);
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(f, buf, (DWORD)n, &w, NULL);
        CloseHandle(f);
    }
    LeaveCriticalSection(&g_logcs);
}

/* ---- the output decision ---- */
typedef enum { REQ_AUTO, REQ_SPATIAL, REQ_SURROUND, REQ_STEREO, REQ_HEADPHONES } request_t;

typedef struct {
    BOOL open_gate;        /* let Wwise use Windows spatial sound */
    int report_channels;   /* 0 = leave the device format alone; else report this many */
    BOOL headphone_pan;    /* Wwise's headphone panning rule */
    const char *name;
} plan_t;

/* Pure function: exported for the tests. */
__declspec(dllexport) plan_t qsa_decide(request_t req, BOOL spatial_on, int mix_ch, int hw_ch)
{
    plan_t p = { FALSE, 0, FALSE, "unchanged (the game's own output)" };
    BOOL wider = mix_ch > hw_ch;
    switch (req) {
    case REQ_SPATIAL:
    case REQ_AUTO:
        if (spatial_on) {
            p.open_gate = TRUE;
            p.report_channels = wider ? mix_ch : 0;   /* the fallback if Wwise's spatial setup fails */
            p.name = "spatial (Windows spatial sound: 7.1.4 plus positioned objects)";
        } else if (wider) {
            p.report_channels = mix_ch;
            p.name = "surround (the channel layout Windows mixes at)";
        }
        break;
    case REQ_SURROUND:
        if (wider) {
            p.report_channels = mix_ch;
            p.name = "surround (the channel layout Windows mixes at)";
        }
        break;
    case REQ_HEADPHONES:
        if (spatial_on) {
            p.open_gate = TRUE;
            p.report_channels = wider ? mix_ch : 0;
            p.name = "spatial (Windows spatial sound: 7.1.4 plus positioned objects)";
        } else {
            p.report_channels = hw_ch != 2 ? 2 : 0;
            p.headphone_pan = TRUE;
            p.name = "stereo with headphone panning";
        }
        break;
    case REQ_STEREO:
        p.report_channels = hw_ch != 2 ? 2 : 0;
        p.name = "stereo";
        break;
    }
    return p;
}

static request_t read_request(char *raw, size_t n)
{
    WCHAR path[MAX_PATH], v[32];
    _snwprintf(path, MAX_PATH, L"%lsQuarrySpatial.ini", g_dir);
    GetPrivateProfileStringW(L"Audio", L"Output", L"auto", v, 32, path);
    WideCharToMultiByte(CP_UTF8, 0, v, -1, raw, (int)n, NULL, NULL);
    if (!_wcsicmp(v, L"spatial")) return REQ_SPATIAL;
    if (!_wcsicmp(v, L"surround")) return REQ_SURROUND;
    if (!_wcsicmp(v, L"stereo")) return REQ_STEREO;
    if (!_wcsicmp(v, L"headphones")) return REQ_HEADPHONES;
    return REQ_AUTO;
}

/* ---- vtable patching ---- */
typedef struct { void **vtbl; void *orig[8]; } patched_t;
static patched_t g_pt[16];
static LONG g_npt;

static void *orig_of(void *self, int slot)
{
    void **vt = *(void ***)self;
    for (LONG i = 0; i < g_npt; i++)
        if (g_pt[i].vtbl == vt) return g_pt[i].orig[slot];
    return NULL;
}

/* Patches a class's vtable slot once. If another hook (the Steam overlay does
 * this) has since chained in front of ours, the slot no longer holds our
 * detour but the saved original is ours already: re-patching would make the
 * two call each other forever, so an existing entry is never re-patched. */
static BOOL patch_slot(void *obj, int slot, void *detour)
{
    void **vt = *(void ***)obj;
    BOOL ok = FALSE;
    EnterCriticalSection(&g_patchcs);
    patched_t *e = NULL;
    for (LONG i = 0; i < g_npt; i++)
        if (g_pt[i].vtbl == vt) { e = &g_pt[i]; break; }
    if (!e && g_npt < (LONG)(sizeof g_pt / sizeof g_pt[0])) {
        e = &g_pt[g_npt];
        memset(e, 0, sizeof *e);
        e->vtbl = vt;
        MemoryBarrier();
        g_npt++;
    }
    if (e) {
        if (vt[slot] == detour || e->orig[slot]) {
            ok = TRUE;
        } else {
            DWORD old;
            if (VirtualProtect(&vt[slot], sizeof(void *), PAGE_READWRITE, &old)) {
                e->orig[slot] = vt[slot];
                MemoryBarrier();
                vt[slot] = detour;
                VirtualProtect(&vt[slot], sizeof(void *), old, &old);
                ok = TRUE;
            }
        }
    }
    LeaveCriticalSection(&g_patchcs);
    return ok;
}

/* TRUE when the game exe itself made the call: the first stack frame outside
 * this DLL and the Steam overlay belongs to the exe. Windows reads the same
 * property internally while serving the game's calls; those reads must see
 * the truth, so they do not count. */
static BOOL direct_game_call(void)
{
    void *fr[16];
    USHORT c = RtlCaptureStackBackTrace(1, 16, fr, NULL);
    for (USHORT i = 0; i < c; i++) {
        HMODULE m = NULL;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)fr[i], &m))
            return FALSE;
        if (m == g_self) continue;
        WCHAR p[MAX_PATH];
        if (GetModuleFileNameW(m, p, MAX_PATH)) {
            const WCHAR *b = wcsrchr(p, L'\\');
            if (!_wcsicmp(b ? b + 1 : p, L"gameoverlayrenderer64.dll")) continue;
        }
        return m == g_game;
    }
    return FALSE;
}

/* ---- the device format the game is told ---- */
static const PROPERTYKEY k_devfmt = { {0xf19f064d,0x082c,0x4e27,{0xbc,0x73,0x68,0x82,0xa1,0xbb,0x8e,0x4c}}, 0 };
static WAVEFORMATEXTENSIBLE g_report;   /* what to answer with */
static BOOL g_have_report;
static LONG g_reported;

typedef HRESULT (STDMETHODCALLTYPE *pfn_psget)(void *, REFPROPERTYKEY, PROPVARIANT *);
typedef HRESULT (STDMETHODCALLTYPE *pfn_openps)(void *, DWORD, void **);

static HRESULT STDMETHODCALLTYPE det_ps_getvalue(void *self, REFPROPERTYKEY key, PROPVARIANT *v)
{
    pfn_psget o = (pfn_psget)orig_of(self, 5);
    HRESULT hr = o ? o(self, key, v) : E_FAIL;
    if (!g_have_report || FAILED(hr) || !key || !v || v->vt != VT_BLOB || !v->blob.pBlobData ||
        v->blob.cbSize < sizeof(WAVEFORMATEX) || !IsEqualGUID(&key->fmtid, &k_devfmt.fmtid) ||
        key->pid != k_devfmt.pid || !direct_game_call())
        return hr;
    const WAVEFORMATEX *hw = (const WAVEFORMATEX *)v->blob.pBlobData;
    if (hw->nChannels == g_report.Format.nChannels) return hr;
    size_t sz = sizeof(WAVEFORMATEX) + g_report.Format.cbSize;
    BYTE *p = CoTaskMemAlloc(sz);
    if (!p) return hr;
    memcpy(p, &g_report, sz);
    WORD was = hw->nChannels;
    CoTaskMemFree(v->blob.pBlobData);   /* the caller frees ours with PropVariantClear */
    v->blob.pBlobData = p;
    v->blob.cbSize = (ULONG)sz;
    if (InterlockedIncrement(&g_reported) == 1)
        qlog("The game asked for the device format: told %u channels instead of %u.", g_report.Format.nChannels, was);
    return hr;
}

static HRESULT STDMETHODCALLTYPE det_dev_openps(void *self, DWORD mode, void **out)
{
    pfn_openps o = (pfn_openps)orig_of(self, 4);
    HRESULT hr = o ? o(self, mode, out) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out) patch_slot(*out, 5, (void *)det_ps_getvalue);
    return hr;
}

/* Test hook: pretend Windows mixes at 7.1 float 48 kHz. */
static void synthetic_71(WAVEFORMATEXTENSIBLE *x)
{
    memset(x, 0, sizeof *x);
    x->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    x->Format.nChannels = 8;
    x->Format.nSamplesPerSec = 48000;
    x->Format.wBitsPerSample = 32;
    x->Format.nBlockAlign = 32;
    x->Format.nAvgBytesPerSec = 48000 * 32;
    x->Format.cbSize = 22;
    x->Samples.wValidBitsPerSample = 32;
    x->dwChannelMask = 0x63f;
    x->SubFormat = (GUID){ WAVE_FORMAT_IEEE_FLOAT, 0, 0x10, { 0x80,0,0,0xaa,0,0x38,0x9b,0x71 } };
}

/* Turns a mix format into the one to report with `ch` channels (stereo is
 * the only narrowing case). */
static BOOL build_report(const WAVEFORMATEX *mix, int ch)
{
    if (!mix || ch <= 0) return FALSE;
    if (ch == mix->nChannels) {
        size_t sz = sizeof(WAVEFORMATEX) + mix->cbSize;
        if (sz > sizeof g_report) return FALSE;
        memcpy(&g_report, mix, sz);
        return TRUE;
    }
    if (ch != 2) return FALSE;
    memset(&g_report, 0, sizeof g_report);
    g_report.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    g_report.Format.nChannels = 2;
    g_report.Format.nSamplesPerSec = mix->nSamplesPerSec;
    g_report.Format.wBitsPerSample = 32;
    g_report.Format.nBlockAlign = 8;
    g_report.Format.nAvgBytesPerSec = mix->nSamplesPerSec * 8;
    g_report.Format.cbSize = 22;
    g_report.Samples.wValidBitsPerSample = 32;
    g_report.dwChannelMask = 0x3;
    g_report.SubFormat = (GUID){ WAVE_FORMAT_IEEE_FLOAT, 0, 0x10, { 0x80,0,0,0xaa,0,0x38,0x9b,0x71 } };
    return TRUE;
}

/* ---- Wwise's "Allow 3D Audio" gate ----
 * System sink setup (exe+0x48f6d87 in the Steam build of 2026-09):
 *   cmp byte [r13+8],r12b   ; sink params: Allow 3D Audio == 0 ?
 *   je  no3d                ; 74 13  <- becomes 90 90
 *   mov eax,esi / and eax,0xfffff000 / cmp eax,0x4000   ; not mono
 * The 18 bytes occur once in the exe. Anything else there (a game update)
 * is refused and logged. */
static const BYTE k_gate_sig[18] = { 0x45,0x38,0x65,0x08, 0x74,0x13, 0x8b,0xc6, 0x25,0x00,0xf0,0xff,0xff,
                                     0x3d,0x00,0x40,0x00,0x00 };
#define GATE_RVA 0x48f6d87

/* 1 patched, 0 already patched, -1 bytes differ (refused), -2 not writable */
__declspec(dllexport) int qsa_patch_gate_at(BYTE *p)
{
    BYTE open[18];
    memcpy(open, k_gate_sig, sizeof open);
    open[4] = open[5] = 0x90;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) ||
        (mbi.Protect & PAGE_GUARD) || (BYTE *)mbi.BaseAddress + mbi.RegionSize < p + sizeof k_gate_sig)
        return -1;
    if (!memcmp(p, open, sizeof open)) return 0;
    if (memcmp(p, k_gate_sig, sizeof k_gate_sig)) return -1;
    DWORD old;
    if (!VirtualProtect(p + 4, 2, PAGE_EXECUTE_READWRITE, &old)) return -2;
    p[4] = p[5] = 0x90;
    VirtualProtect(p + 4, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p + 4, 2);
    return 1;
}

/* ---- reading Wwise's result back (and headphone panning) ----
 * Exported by the game exe under MSVC names. GetSpeakerConfiguration returns
 * its 32-bit AkChannelConfig through a pointer in rcx (device id in rdx);
 * the panning functions take (rule or rule*, device id). 0 = main output. */
typedef char (*pfn_isinit)(void);
typedef uint32_t *(*pfn_getspk)(uint32_t *, uint64_t);
typedef int (*pfn_getpan)(int *, uint64_t);
typedef int (*pfn_setpan)(int, uint64_t);

static void cfg_name(uint32_t v, char *out, size_t n)
{
    unsigned ch = v & 0xff, type = (v >> 8) & 0xf, mask = v >> 12;
    if (type == 3) snprintf(out, n, "audio objects");
    else if (ch == 2) snprintf(out, n, "stereo");
    else if (ch == 6) snprintf(out, n, "5.1");
    else if (ch == 8) snprintf(out, n, "7.1");
    else if (ch == 12) snprintf(out, n, "7.1.4");
    else snprintf(out, n, "%u channels (mask 0x%x)", ch, mask);
}

static void wwise_followup(BOOL headphone_pan)
{
    pfn_isinit isinit = (pfn_isinit)(void *)GetProcAddress(g_game, "?IsInitialized@SoundEngine@AK@@YA_NXZ");
    pfn_getspk getspk = (pfn_getspk)(void *)GetProcAddress(g_game,
        "?GetSpeakerConfiguration@SoundEngine@AK@@YA?AUAkChannelConfig@@_K@Z");
    pfn_getpan getpan = (pfn_getpan)(void *)GetProcAddress(g_game,
        "?GetPanningRule@SoundEngine@AK@@YA?AW4AKRESULT@@AEAW4AkPanningRule@@_K@Z");
    pfn_setpan setpan = (pfn_setpan)(void *)GetProcAddress(g_game,
        "?SetPanningRule@SoundEngine@AK@@YA?AW4AKRESULT@@W4AkPanningRule@@_K@Z");
    if (!isinit || !getspk || !getpan) {
        qlog("Could not find Wwise's functions in the game; cannot confirm the result.");
        return;
    }
    /* Wwise starts a few seconds in; its spatial stream a little later. */
    uint32_t last = 0;
    for (int i = 0; i < 120; i++) {
        Sleep(500);
        if (!isinit()) continue;
        uint32_t cfg = 0;
        getspk(&cfg, 0);
        if ((cfg & 0xff) == 0 && ((cfg >> 8) & 0xf) != 3) continue;
        if (cfg != last) {
            char d[64];
            cfg_name(cfg, d, sizeof d);
            qlog("Wwise is mixing to: %s.", d);
            last = cfg;
        }
        if (headphone_pan) {
            int rule = -1;
            if (setpan && setpan(1, 0) == 1 && getpan(&rule, 0) == 1 && rule == 1)
                qlog("Wwise panning set to headphones.");
            else
                qlog("Could not set Wwise's headphone panning.");
            headphone_pan = FALSE;
        }
        if (i >= 40) break;   /* ~20 s after start: long enough to see the spatial switch */
    }
}

/* ---- startup ---- */
static volatile LONG g_setup_done;

static void worker_body(void)
{
    plan_t p = { FALSE, 0, FALSE, "unchanged" };
    char raw[32];
    request_t req = read_request(raw, sizeof raw);
    qlog("Quarry Spatial Audio %s. Requested output: %s.", QSA_VERSION, raw);

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) { qlog("Could not start COM; doing nothing."); return; }
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioClient *ac = NULL;
    ISpatialAudioClient *sac = NULL;
    IPropertyStore *ps = NULL;
    WAVEFORMATEX *mix = NULL;
    int hw_ch = 0;
    UINT32 maxdyn = 0;

    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator,
                                (void **)&en)) ||
        FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev))) {
        qlog("No default audio output found; doing nothing.");
        goto out;
    }
    if (SUCCEEDED(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac)))
        IAudioClient_GetMixFormat(ac, &mix);
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(IPropertyStore_GetValue(ps, &k_devfmt, &v)) && v.vt == VT_BLOB &&
            v.blob.cbSize >= sizeof(WAVEFORMATEX))
            hw_ch = ((WAVEFORMATEX *)v.blob.pBlobData)->nChannels;
        PropVariantClear(&v);
    }
    if (SUCCEEDED(IMMDevice_Activate(dev, &IID_ISpatialAudioClient, CLSCTX_ALL, NULL, (void **)&sac)))
        ISpatialAudioClient_GetMaxDynamicObjectCount(sac, &maxdyn);

    if (GetEnvironmentVariableW(L"QSA_TEST_NOSPATIAL", NULL, 0) > 0) maxdyn = 0;   /* tests only */
    WAVEFORMATEXTENSIBLE test71;
    if (GetEnvironmentVariableW(L"QSA_TEST_TARGET71", NULL, 0) > 0) {
        synthetic_71(&test71);
        if (mix) CoTaskMemFree(mix);
        mix = CoTaskMemAlloc(sizeof test71);
        if (mix) memcpy(mix, &test71, sizeof test71);
    }
    int mix_ch = mix ? mix->nChannels : 0;
    qlog("Device: hardware %d channels, Windows mixes at %d, Windows spatial sound %s.", hw_ch, mix_ch,
         maxdyn ? "on" : "off");

    p = qsa_decide(req, maxdyn > 0, mix_ch, hw_ch);
    qlog("Chosen: %s.", p.name);
    if (req == REQ_SPATIAL && !p.open_gate) qlog("Spatial was requested but Windows spatial sound is off.");
    if (req == REQ_SURROUND && !p.report_channels)
        qlog("Surround was requested but Windows does not mix this device wider than its hardware.");

    if (p.report_channels && hw_ch && build_report(mix, p.report_channels)) {
        g_have_report = TRUE;
        if (!patch_slot(dev, 4, (void *)det_dev_openps) || (ps && !patch_slot(ps, 5, (void *)det_ps_getvalue)))
            qlog("Could not attach to the audio device; the channel layout will not change.");
    }
    if (p.open_gate) {
        /* The address must lie inside the game exe's own image before its
         * bytes are even compared. */
        MEMORY_BASIC_INFORMATION mbi;
        BYTE *at = (BYTE *)g_game + GATE_RVA;
        int r = (VirtualQuery(at, &mbi, sizeof mbi) && mbi.AllocationBase == (void *)g_game)
                    ? qsa_patch_gate_at(at) : -1;
        if (r == 1 || r == 0) qlog("Spatial sound enabled in Wwise.");
        else if (r == -1) qlog("Spatial sound NOT enabled: this game version is not the one this mod knows.");
        else qlog("Spatial sound NOT enabled: could not change the game's code.");
    }

out:
    if (ps) IPropertyStore_Release(ps);
    if (sac) ISpatialAudioClient_Release(sac);
    if (ac) IAudioClient_Release(ac);
    if (mix) CoTaskMemFree(mix);
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    InterlockedExchange(&g_setup_done, 1);
    if (dev) wwise_followup(p.headphone_pan);
}

static DWORD WINAPI worker(void *arg)
{
    (void)arg;
    worker_body();
    return 0;
}

/* ---- the two real X3DAudio functions, passed through ---- */
static HMODULE g_real;
static INIT_ONCE g_real_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK load_real(PINIT_ONCE o, void *p, void **c)
{
    (void)o; (void)p; (void)c;
    WCHAR path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n && n < MAX_PATH - 20) {
        wcscat(path, L"\\X3DAudio1_7.dll");
        g_real = LoadLibraryW(path);
    }
    return TRUE;
}

static FARPROC real_fn(const char *name)
{
    InitOnceExecuteOnce(&g_real_once, load_real, NULL, NULL);
    return g_real ? GetProcAddress(g_real, name) : NULL;
}

typedef HRESULT (WINAPI *pfn_x3dinit)(UINT32, float, BYTE *);
typedef void (WINAPI *pfn_x3dcalc)(const BYTE *, const void *, const void *, UINT32, void *);

HRESULT WINAPI X3DAudioInitialize(UINT32 mask, float speed, BYTE *inst)
{
    static pfn_x3dinit f;
    if (!f) f = (pfn_x3dinit)(void *)real_fn("X3DAudioInitialize");
    return f ? f(mask, speed, inst) : E_FAIL;
}

void WINAPI X3DAudioCalculate(const BYTE *inst, const void *l, const void *e, UINT32 flags, void *dsp)
{
    static pfn_x3dcalc f;
    if (!f) f = (pfn_x3dcalc)(void *)real_fn("X3DAudioCalculate");
    if (f) f(inst, l, e, flags, dsp);
}

/* Test hook: the worker's setup has finished (the Wwise follow-up may still run). */
__declspec(dllexport) BOOL qsa_wait_setup(DWORD ms)
{
    ULONGLONG end = GetTickCount64() + ms;
    while (!g_setup_done && GetTickCount64() < end) Sleep(10);
    return g_setup_done;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, void *r)
{
    (void)r;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);
    g_self = h;
    g_game = GetModuleHandleW(NULL);
    InitializeCriticalSection(&g_logcs);
    InitializeCriticalSection(&g_patchcs);
    DWORD n = GetModuleFileNameW(h, g_dir, MAX_PATH);
    WCHAR *slash = n ? wcsrchr(g_dir, L'\\') : NULL;
    if (slash) slash[1] = 0;
    else g_dir[0] = 0;
    WCHAR log[MAX_PATH];
    _snwprintf(log, MAX_PATH, L"%lsQuarrySpatial.log", g_dir);
    DeleteFileW(log);
    /* COM is not allowed under the loader lock; the thread starts as soon as
     * it is released, seconds before the game starts its audio. */
    HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (t) CloseHandle(t);
    return TRUE;
}
