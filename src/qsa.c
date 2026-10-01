/* Essential Audio Enhancements for The Quarry.
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
 *
 * Dialogue=positional rewrites the distance settings of the game's dialogue
 * as it loads, so each voice comes from its character relative to the
 * camera and gets quieter and muffled with distance (dialogue.c).
 */
#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <spatialaudioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define QSA_VERSION "1.2.0"

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
    _snwprintf(path, MAX_PATH, L"%lsQuarryEssentialAudio.log", g_dir);
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
typedef enum { REQ_AUTO, REQ_SPATIAL, REQ_LAYOUT, REQ_STEREO, REQ_HEADPHONES, REQ_MONO } request_t;

/* A channel layout. 5.1 uses side speakers, as receivers and Wwise's own 5.1
 * do. ch == 0 means none. */
typedef struct { int ch; DWORD mask; const char *name; } layout_t;

typedef struct {
    BOOL open_gate;        /* let Wwise use Windows spatial sound */
    int report_channels;   /* 0 = leave the device format alone; else report this many */
    DWORD report_mask;     /* speaker mask to report; 0 = Windows' own mix format */
    BOOL headphone_pan;    /* Wwise's headphone panning rule */
    BOOL mono;             /* Wwise mixes to one channel */
    const char *name;
} plan_t;

/* Pure function: exported for the tests. `layout` is used by REQ_LAYOUT. */
__declspec(dllexport) plan_t qsa_decide(request_t req, layout_t layout, BOOL spatial_on, int mix_ch, int hw_ch)
{
    plan_t p = { FALSE, 0, 0, FALSE, FALSE, "unchanged (the game's own output)" };
    BOOL wider = mix_ch > hw_ch;
    switch (req) {
    case REQ_AUTO:
    case REQ_SPATIAL:
    case REQ_HEADPHONES:
        if (spatial_on) {
            p.open_gate = TRUE;
            p.report_channels = wider ? mix_ch : 0;   /* the fallback if Wwise's spatial setup fails */
            p.name = "3D spatial sound (Windows spatial sound: 7.1.4 plus positioned objects)";
        } else if (req == REQ_HEADPHONES) {
            p.report_channels = hw_ch != 2 ? 2 : 0;
            p.report_mask = 0x3;
            p.headphone_pan = TRUE;
            p.name = "stereo with headphone panning";
        } else if (wider) {
            p.report_channels = mix_ch;
            p.name = "surround (the channel layout Windows mixes at)";
        }
        break;
    case REQ_LAYOUT:
        if (layout.ch && layout.ch != hw_ch) {
            p.report_channels = layout.ch;
            p.report_mask = layout.mask;
        }
        p.name = layout.name;
        break;
    case REQ_STEREO:
        p.report_channels = hw_ch != 2 ? 2 : 0;
        p.report_mask = 0x3;
        p.name = "stereo";
        break;
    case REQ_MONO:
        /* Windows' shared mode refuses a 1-channel stream on most devices
         * (measured: all three here), so the output stays as it is and Wwise
         * mixes to one channel inside, which then plays on both sides. */
        p.mono = TRUE;
        p.name = "mono (the game mixes to one channel, played on both sides)";
        break;
    }
    return p;
}

static void ini_path(WCHAR *path)
{
    _snwprintf(path, MAX_PATH, L"%lsQuarryEssentialAudio.ini", g_dir);
}

static request_t read_request(char *raw, size_t n, layout_t *layout)
{
    static const layout_t layouts[] = { { 4, 0x33, "quad" }, { 6, 0x60f, "5.1" }, { 8, 0x63f, "7.1" } };
    WCHAR path[MAX_PATH], v[32];
    ini_path(path);
    GetPrivateProfileStringW(L"Audio", L"Output", L"auto", v, 32, path);
    WideCharToMultiByte(CP_UTF8, 0, v, -1, raw, (int)n, NULL, NULL);
    layout->ch = 0;
    for (size_t i = 0; i < sizeof layouts / sizeof layouts[0]; i++) {
        WCHAR w[8];
        MultiByteToWideChar(CP_UTF8, 0, layouts[i].name, -1, w, 8);
        if (!_wcsicmp(v, w)) { *layout = layouts[i]; return REQ_LAYOUT; }
    }
    if (!_wcsicmp(v, L"spatial")) return REQ_SPATIAL;
    if (!_wcsicmp(v, L"stereo")) return REQ_STEREO;
    if (!_wcsicmp(v, L"headphones")) return REQ_HEADPHONES;
    if (!_wcsicmp(v, L"mono")) return REQ_MONO;
    if (_wcsicmp(v, L"auto"))
        qlog("Unknown Output value; using auto. Choices: auto, spatial, 7.1, 5.1, quad, stereo, headphones, mono.");
    return REQ_AUTO;
}

/* Compression=on (the game as designed) or off. */
static BOOL read_compression_off(char *raw, size_t n)
{
    WCHAR path[MAX_PATH], v[16];
    ini_path(path);
    GetPrivateProfileStringW(L"Audio", L"Compression", L"on", v, 16, path);
    WideCharToMultiByte(CP_UTF8, 0, v, -1, raw, (int)n, NULL, NULL);
    if (!_wcsicmp(v, L"off")) return TRUE;
    if (_wcsicmp(v, L"on")) qlog("Unknown Compression value; using on. Choices: on, off.");
    return FALSE;
}

static void read_device(WCHAR *out, int n)
{
    WCHAR path[MAX_PATH];
    ini_path(path);
    GetPrivateProfileStringW(L"Audio", L"Device", L"", out, n, path);
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
/* PKEY_AudioEndpoint_GUID: identifies which device a property store belongs to. */
static const PROPERTYKEY k_epguid = { {0x1da5d803,0xd492,0x4edd,{0x8c,0x23,0xe0,0xc0,0xff,0xee,0x7f,0x0e}}, 4 };
static WAVEFORMATEXTENSIBLE g_report;   /* what to answer with */
static BOOL g_have_report;
static WCHAR g_target_guid[64];         /* only this device's format is changed */
static LONG g_reported;

typedef HRESULT (STDMETHODCALLTYPE *pfn_psget)(void *, REFPROPERTYKEY, PROPVARIANT *);
typedef HRESULT (STDMETHODCALLTYPE *pfn_openps)(void *, DWORD, void **);

static BOOL store_is_target(void *self, pfn_psget o)
{
    PROPVARIANT g;
    PropVariantInit(&g);
    BOOL same = SUCCEEDED(o(self, &k_epguid, &g)) && g.vt == VT_LPWSTR && g.pwszVal &&
                !_wcsicmp(g.pwszVal, g_target_guid);
    PropVariantClear(&g);
    return same;
}

static HRESULT STDMETHODCALLTYPE det_ps_getvalue(void *self, REFPROPERTYKEY key, PROPVARIANT *v)
{
    pfn_psget o = (pfn_psget)orig_of(self, 5);
    HRESULT hr = o ? o(self, key, v) : E_FAIL;
    if (!g_have_report || FAILED(hr) || !key || !v || v->vt != VT_BLOB || !v->blob.pBlobData ||
        v->blob.cbSize < sizeof(WAVEFORMATEX) || !IsEqualGUID(&key->fmtid, &k_devfmt.fmtid) ||
        key->pid != k_devfmt.pid || !direct_game_call() || !store_is_target(self, o))
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

/* The format to report: Windows' own mix format when that is the layout
 * wanted (mask 0 and the same channel count), otherwise 32-bit float at the
 * mix rate with the given speaker mask. */
static BOOL build_report(const WAVEFORMATEX *mix, int ch, DWORD mask)
{
    if (!mix || ch <= 0) return FALSE;
    if (!mask && ch == mix->nChannels) {
        size_t sz = sizeof(WAVEFORMATEX) + mix->cbSize;
        if (sz > sizeof g_report) return FALSE;
        memcpy(&g_report, mix, sz);
        return TRUE;
    }
    if (!mask) return FALSE;
    memset(&g_report, 0, sizeof g_report);
    g_report.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    g_report.Format.nChannels = (WORD)ch;
    g_report.Format.nSamplesPerSec = mix->nSamplesPerSec;
    g_report.Format.wBitsPerSample = 32;
    g_report.Format.nBlockAlign = (WORD)(ch * 4);
    g_report.Format.nAvgBytesPerSec = mix->nSamplesPerSec * ch * 4;
    g_report.Format.cbSize = 22;
    g_report.Samples.wValidBitsPerSample = 32;
    g_report.dwChannelMask = mask;
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
/* AkOutputSettings, read from the initializer the game's GetDefaultInitSettings
 * tail-calls (0x144879c40): device shareset, device id, panning rule,
 * AkChannelConfig. Shareset 0 = Wwise's standard system output. */
typedef struct { uint32_t shareset, device_id, panning, channels; } ak_output_settings_t;
typedef int (*pfn_replaceout)(const ak_output_settings_t *, uint64_t, uint64_t *);
/* SetBusEffect(bus id, effect slot, effect shareset id; 0 = empty the slot). */
typedef int (*pfn_setbusfx)(uint32_t, uint32_t, uint32_t);
/* From the game's Init.bnk: the Master Audio Bus (the last stage of the mix)
 * carries a Compressor in slot 0 (-20 dB, 3:1, 100 ms), a Peak Limiter in
 * slot 1 (-1 dB brick wall) and a Meter in slot 2. Always on, not linked to
 * any game setting. */
#define AK_MASTER_BUS_ID      3803692087u   /* "Master Audio Bus" */
#define MASTER_COMPRESSOR_SLOT 0u
#define AKCFG_MONO 0x00004101u   /* 1 ch, standard, front centre */

/* ---- level meter and Limiter=off ([Measure]) ---- */
#include "meter.c"

static void cfg_name(uint32_t v, char *out, size_t n)
{
    unsigned ch = v & 0xff, type = (v >> 8) & 0xf, mask = v >> 12;
    if (type == 3) snprintf(out, n, "audio objects");
    else if (ch == 1) snprintf(out, n, "mono");
    else if (ch == 2) snprintf(out, n, "stereo");
    else if (ch == 6) snprintf(out, n, "5.1");
    else if (ch == 8) snprintf(out, n, "7.1");
    else if (ch == 12) snprintf(out, n, "7.1.4");
    else snprintf(out, n, "%u channels (mask 0x%x)", ch, mask);
}

/* After Wwise starts: log what it mixes to; if mono or another device was
 * chosen, rebuild its main output (id 0) once with ReplaceOutput; set
 * headphone panning. wwise_dev = Wwise's id for the chosen device, 0 for the
 * Windows default. */
static void wwise_followup(BOOL headphone_pan, BOOL mono, uint32_t wwise_dev, BOOL no_compression)
{
    BOOL replace = mono || wwise_dev;
    pfn_setbusfx setbusfx = (pfn_setbusfx)(void *)GetProcAddress(g_game,
        "?SetBusEffect@SoundEngine@AK@@YA?AW4AKRESULT@@III@Z");
    BOOL limiter_off = g_meter.limiter_off, meter_pending = g_meter.on;
    if ((no_compression || limiter_off) && !setbusfx) {
        qlog("Could not find Wwise's SetBusEffect in the game; compression and the limiter stay on.");
        no_compression = limiter_off = FALSE;
    }
    pfn_replaceout replaceout = (pfn_replaceout)(void *)GetProcAddress(g_game,
        "?ReplaceOutput@SoundEngine@AK@@YA?AW4AKRESULT@@AEBUAkOutputSettings@@_KPEA_K@Z");
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
    if (replace && !replaceout) {
        qlog("Could not find Wwise's ReplaceOutput in the game; mono and the Device setting are not available.");
        replace = FALSE;
    }
    /* Wwise starts a few seconds in; its spatial stream a little later. Checks
     * run every half second for up to two minutes. */
    uint32_t last = 0;
    int ready_at = -1;
    for (int i = 0; i < 240; i++) {
        Sleep(500);
        if (!isinit()) continue;
        uint32_t cfg = 0;
        getspk(&cfg, 0);
        if ((cfg & 0xff) == 0 && ((cfg >> 8) & 0xf) != 3) continue;
        if (ready_at < 0) ready_at = i;
        if (cfg != last) {
            char d[64];
            cfg_name(cfg, d, sizeof d);
            qlog("Wwise is mixing to: %s.", d);
            last = cfg;
        }
        /* Rebuild the main output: on the chosen device, and/or with a
         * one-channel layout for mono (Wwise then mixes to one channel and
         * spreads it over the device's real channels; changing the master bus
         * by name did nothing, the game's bus has another name). Layout 0 =
         * take it from the device, which is where the widened format comes in.
         * Done once; the read-back logs the result on the next pass. */
        if (replace) {
            ak_output_settings_t s = { 0, wwise_dev, headphone_pan ? 1u : 0u, mono ? AKCFG_MONO : 0 };
            uint64_t id = 0;
            int r = replaceout(&s, 0, &id);
            if (r == 1)
                qlog("Asked Wwise to %s%s%s.", wwise_dev ? "play on the chosen device" : "",
                     wwise_dev && mono ? " and " : "", mono ? "mix in mono" : "");
            else
                qlog("Wwise refused the new output (result %d).", r);
            replace = FALSE;
        }
        if (headphone_pan) {
            int rule = -1;
            if (setpan && setpan(1, 0) == 1 && getpan(&rule, 0) == 1 && rule == 1)
                qlog("Wwise panning set to headphones.");
            else
                qlog("Could not set Wwise's headphone panning.");
            headphone_pan = FALSE;
        }
        /* Compression off: empty the master bus's compressor slot; the limiter
         * after it stays. Measured in the game: the compressor appeared 10 s
         * after Wwise's output, and Wwise terminated it 80 ms after this
         * request, for the rest of the session. Nothing can be read back, so
         * the request is repeated every 2 s for the first minute, for slower
         * machines (emptying an empty slot changes nothing). */
        if (no_compression && (i - ready_at) % 4 == 0) {
            int r = setbusfx(AK_MASTER_BUS_ID, MASTER_COMPRESSOR_SLOT, 0);
            if (i == ready_at)
                qlog(r == 1 ? "Asked Wwise to remove the master compressor (the limiter stays)."
                            : "Wwise refused to remove the master compressor (result %d).", r);
        }
        /* [Measure] Limiter=off: the same, for the limiter in slot 1. */
        if (limiter_off && (i - ready_at) % 4 == 0) {
            int r = setbusfx(AK_MASTER_BUS_ID, MASTER_LIMITER_SLOT, 0);
            if (i == ready_at)
                qlog(r == 1 ? "Asked Wwise to remove the master limiter (for measuring only)."
                            : "Wwise refused to remove the master limiter (result %d).", r);
        }
        if (meter_pending && (i - ready_at) % 4 == 0) meter_pending = !meter_try_register();
        if (i - ready_at >= (no_compression || limiter_off || meter_pending ? 120 : 40)) break;
    }
}

/* ---- positional dialogue ---- */
#include "dialogue.c"

/* ---- startup ---- */
static volatile LONG g_setup_done;

/* Case-insensitive "does `hay` contain `needle`". */
static BOOL name_contains(const WCHAR *hay, const WCHAR *needle)
{
    size_t n = wcslen(needle);
    for (; *hay; hay++)
        if (!_wcsnicmp(hay, needle, n)) return TRUE;
    return FALSE;
}

static void friendly_name(IPropertyStore *ps, WCHAR *out, int n)
{
    PROPVARIANT v;
    PropVariantInit(&v);
    out[0] = 0;
    if (SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
        wcsncpy(out, v.pwszVal, n - 1), out[n - 1] = 0;
    PropVariantClear(&v);
}

/* The active output whose name contains `want`; NULL if none. Lists the
 * names in the log when nothing matches, so the setting can be corrected. */
static IMMDevice *find_device(IMMDeviceEnumerator *en, const WCHAR *want)
{
    IMMDeviceCollection *col = NULL;
    IMMDevice *found = NULL;
    UINT n = 0;
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col))) return NULL;
    IMMDeviceCollection_GetCount(col, &n);
    for (UINT i = 0; i < n && !found; i++) {
        IMMDevice *d = NULL;
        IPropertyStore *ps = NULL;
        WCHAR name[256];
        if (FAILED(IMMDeviceCollection_Item(col, i, &d))) continue;
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps))) {
            friendly_name(ps, name, 256);
            IPropertyStore_Release(ps);
            if (name[0] && name_contains(name, want)) { found = d; continue; }
        }
        IMMDevice_Release(d);
    }
    if (!found) {
        qlog("No output device matches the Device setting. Available devices:");
        for (UINT i = 0; i < n; i++) {
            IMMDevice *d = NULL;
            IPropertyStore *ps = NULL;
            WCHAR name[256];
            char u[512];
            if (FAILED(IMMDeviceCollection_Item(col, i, &d))) continue;
            if (SUCCEEDED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps))) {
                friendly_name(ps, name, 256);
                WideCharToMultiByte(CP_UTF8, 0, name, -1, u, sizeof u, NULL, NULL);
                qlog("    %s", u);
                IPropertyStore_Release(ps);
            }
            IMMDevice_Release(d);
        }
    }
    IMMDeviceCollection_Release(col);
    return found;
}

typedef uint32_t (*pfn_getdevid)(IMMDevice *);   /* AK::GetDeviceID(IMMDevice*) */

static void worker_body(void)
{
    /* First: the game loads its sound banks seconds later, and the dialogue
     * hook must be in place before it does. */
    dialogue_setup();
    plan_t p = { FALSE, 0, 0, FALSE, FALSE, "unchanged" };
    char raw[32], devu[512];
    layout_t layout;
    request_t req = read_request(raw, sizeof raw, &layout);
    WCHAR want[256];
    read_device(want, 256);
    WideCharToMultiByte(CP_UTF8, 0, want, -1, devu, sizeof devu, NULL, NULL);
    char rawc[16];
    BOOL no_compression = read_compression_off(rawc, sizeof rawc);
    qlog("Essential Audio Enhancements for The Quarry %s. Output: %s. Device: %s. Compression: %s. Dialogue: %s.",
         QSA_VERSION, raw, want[0] ? devu : "Windows default", no_compression ? "off" : "on",
         g_dlg.positional ? "positional" : "game");
    dialogue_report();
    meter_read_settings();
    if (g_meter.on || g_meter.limiter_off)
        qlog("Measuring: level meter %s, limiter %s.", g_meter.on ? "on" : "off",
             g_meter.limiter_off ? "OFF (for measuring only; the game can clip)" : "on");

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
    uint32_t wwise_dev = 0;

    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator,
                                (void **)&en))) {
        qlog("Could not list audio devices; doing nothing.");
        goto out;
    }
    if (want[0]) dev = find_device(en, want);
    BOOL named = dev != NULL;
    if (!dev && FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev))) {
        qlog("No default audio output found; doing nothing.");
        dev = NULL;
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
        PropVariantInit(&v);
        if (SUCCEEDED(IPropertyStore_GetValue(ps, &k_epguid, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
            wcsncpy(g_target_guid, v.pwszVal, 63);
        PropVariantClear(&v);
        WCHAR name[256];
        char u[512];
        friendly_name(ps, name, 256);
        WideCharToMultiByte(CP_UTF8, 0, name, -1, u, sizeof u, NULL, NULL);
        qlog("Using %s: %s.", named ? "the chosen device" : "the Windows default device", u);
    }
    if (named) {
        pfn_getdevid getdevid = (pfn_getdevid)(void *)GetProcAddress(g_game, "?GetDeviceID@AK@@YAIPEAUIMMDevice@@@Z");
        wwise_dev = getdevid ? getdevid(dev) : 0;
        if (!wwise_dev) qlog("Could not get Wwise's id for that device; the game stays on the Windows default.");
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

    p = qsa_decide(req, layout, maxdyn > 0, mix_ch, hw_ch);
    /* Any layout the mod makes up (7.1, 5.1, quad, stereo) is used only if
     * Windows accepts it on this device -- Wwise asks the same question, and
     * a refused format could leave the game silent. Windows' own mix format
     * needs no check. */
    if (p.report_channels && p.report_mask && ac && mix) {
        WAVEFORMATEX *closest = NULL;
        BOOL refuse_for_test = GetEnvironmentVariableW(L"QSA_TEST_REFUSE", NULL, 0) > 0;
        if (build_report(mix, p.report_channels, p.report_mask) &&
            (refuse_for_test ||
             IAudioClient_IsFormatSupported(ac, AUDCLNT_SHAREMODE_SHARED, &g_report.Format, &closest) != S_OK)) {
            qlog("Windows does not accept %s on this device.", p.name);
            if (req == REQ_LAYOUT) {
                /* Next best: the layout Windows mixes at, if wider than the hardware. */
                layout_t none = { 0, 0, "" };
                p = qsa_decide(REQ_AUTO, none, FALSE, mix_ch, hw_ch);
            } else {
                p.report_channels = 0;
                p.report_mask = 0;
                p.name = "unchanged (the game's own output)";
            }
        }
        if (closest) CoTaskMemFree(closest);
    }
    qlog("Chosen: %s.", p.name);
    if (req == REQ_SPATIAL && !p.open_gate) qlog("Spatial was requested but Windows spatial sound is off.");

    if (p.report_channels && hw_ch && build_report(mix, p.report_channels, p.report_mask)) {
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
    if (en) IMMDeviceEnumerator_Release(en);
    InterlockedExchange(&g_setup_done, 1);
    if (dev) {
        wwise_followup(p.headphone_pan, p.mono, wwise_dev, no_compression);
        IMMDevice_Release(dev);
    }
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
    _snwprintf(log, MAX_PATH, L"%lsQuarryEssentialAudio.log", g_dir);
    DeleteFileW(log);
    /* COM is not allowed under the loader lock; the thread starts as soon as
     * it is released, seconds before the game starts its audio. */
    HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (t) CloseHandle(t);
    return TRUE;
}
