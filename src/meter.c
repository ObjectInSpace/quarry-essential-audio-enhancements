/* Level meter (Meter=on) and Limiter=off, for measuring how loud the game gets.
 * Included by qsa.c.
 *
 * Wwise meters a bus "after having mixed all their inputs and processed their
 * effects" (AkCallback.h, 2021.1, the game's version), so the Master Audio
 * Bus meter reads AFTER the -1 dB Peak Limiter in its slot 1. Limiter=off
 * empties slot 1 so the same meter shows the unlimited level.
 *
 * Measured in the game (2026-10-01): with Windows spatial sound, the master
 * bus callback runs once for each 3D audio object (up to ~90 per frame), so
 * readings are kept apart by channel layout. Stereo and 3-channel objects
 * read the same with the compressor and limiter on or off: they skip them.
 *
 * Every other audio bus of the game (bus_table.h, from Init.bnk) is metered
 * for peak only, so the log can say which bus a loud sound came through.
 * (Wwise's output-device metering callback was tried and was never called.)
 *
 * Wwise calls these on its audio thread(s); they only update numbers here,
 * under a lock held for a moment. A writer thread turns them into
 * QuarryEssentialAudio_meter.log once a second, plus a summary file. */
#include "bus_table.h"

/* Layouts from the Wwise 2021.1 SDK headers (AkCallback.h, AkCommonDefs.h). */
typedef struct { void **vt; } ak_metering;   /* AK::IAkMetering; slot 0 = destructor */
enum { AKM_PEAK = 1, AKM_TRUEPEAK = 2, AKM_RMS = 4, AKM_KPOWER = 16 };
typedef struct {
    void *cookie; uint64_t game_obj;
    ak_metering *m; uint32_t cfg; uint32_t flags;
} ak_bus_meter_info;
typedef const float *(*pfn_mvec)(ak_metering *);
typedef float (*pfn_mk)(ak_metering *);
typedef void (*pfn_buscb)(ak_bus_meter_info *);
typedef int (*pfn_regbusm)(uint32_t, pfn_buscb, uint32_t, void *);

#define MASTER_LIMITER_SLOT 1u
#define METER_BUS_FLAGS (AKM_PEAK | AKM_TRUEPEAK | AKM_RMS | AKM_KPOWER)
#define METER_SUB_FLAGS AKM_PEAK
#define CEILING_TP 0.8810f   /* -1.1 dBTP: at the limiter's -1 dB ceiling */
#define MAX_CFGS 8

/* Master bus readings for one channel layout (cfg = AkChannelConfig). */
typedef struct { uint32_t cfg, calls, at_ceiling, over_0; float peak, tp; } cfg_stats;

typedef struct {
    cfg_stats cfg[MAX_CFGS]; int ncfg;
    uint32_t readings;
    float mom;                                   /* loudest momentary K power, widest layout */
    float bus[N_BUSES];                          /* peak per bus */
} meter_win;

static struct {
    BOOL on, limiter_off;
    volatile LONG bad;                           /* data did not look as expected: stop reading it */
    SRWLOCK lock;
    meter_win win, all;                          /* this second; since the start (never reset) */
    ULONGLONG start, tp_at, mom_at, bus_at[N_BUSES];
    uint32_t widest;
    float tp_max;                                /* loudest master true peak, any layout */
    float kring[19]; int kpos, kfill;            /* ~400 ms of frames, widest layout only */
    BOOL bus_checked, sub_checked;
    int subs_registered;
    char why[160];
} g_meter = { .lock = SRWLOCK_INIT };

static void meter_read_settings(void)
{
    WCHAR path[MAX_PATH], v[16];
    ini_path(path);
    GetPrivateProfileStringW(L"Measure", L"Meter", L"off", v, 16, path);
    g_meter.on = !_wcsicmp(v, L"on");
    GetPrivateProfileStringW(L"Measure", L"Limiter", L"on", v, 16, path);
    g_meter.limiter_off = !_wcsicmp(v, L"off");
}

/* A restart must not wipe the last session's readings: keep them as .prev. */
static void keep_previous(const WCHAR *name)
{
    WCHAR path[MAX_PATH], prev[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%ls%ls", g_dir, name);
    _snwprintf(prev, MAX_PATH, L"%ls%ls.prev", g_dir, name);
    MoveFileExW(path, prev, MOVEFILE_REPLACE_EXISTING);
}

/* Is [p, p+n) committed, readable memory? Used on the first call of each
 * callback type, to check the layout this file assumes. */
static BOOL readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return FALSE;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return FALSE;
    return (const BYTE *)p + n <= (const BYTE *)mbi.BaseAddress + mbi.RegionSize;
}

static BOOL sane(float v) { return v == v && v >= 0.0f && v < 1000.0f; }

static void meter_fail(const char *why)
{
    if (InterlockedExchange(&g_meter.bad, 1)) return;
    snprintf(g_meter.why, sizeof g_meter.why, "%s", why);
}

/* slots: 1 GetPeak, 2 GetTruePeak, 3 GetRMS (checked when asked for) */
static BOOL check_metering(ak_metering *m, int nch, BOOL want_tp, BOOL want_rms)
{
    if (!readable(m, sizeof *m) || !readable(m->vt, 5 * sizeof(void *))) return FALSE;
    for (int i = 1; i < 5; i++)
        if (!readable(m->vt[i], 1)) return FALSE;
    const float *pk = ((pfn_mvec)m->vt[1])(m);
    if (!readable(pk, nch * sizeof(float))) return FALSE;
    for (int c = 0; c < nch; c++)
        if (!sane(pk[c])) return FALSE;
    if (want_tp) {
        const float *tp = ((pfn_mvec)m->vt[2])(m);
        if (!readable(tp, nch * sizeof(float))) return FALSE;
        for (int c = 0; c < nch; c++)
            if (!sane(tp[c])) return FALSE;
    }
    if (want_rms && !readable(((pfn_mvec)m->vt[3])(m), nch * sizeof(float))) return FALSE;
    return TRUE;
}

static float vmax(const float *v, int n)
{
    float m = 0;
    for (int i = 0; i < n; i++) if (v[i] > m) m = v[i];
    return m;
}

static cfg_stats *cfg_slot(meter_win *w, uint32_t cfg)
{
    for (int i = 0; i < w->ncfg; i++)
        if (w->cfg[i].cfg == cfg) return &w->cfg[i];
    if (w->ncfg == MAX_CFGS) return NULL;
    cfg_stats *s = &w->cfg[w->ncfg++];
    memset(s, 0, sizeof *s);
    s->cfg = cfg;
    return s;
}

static void cfg_add(meter_win *w, uint32_t cfg, float p, float t)
{
    cfg_stats *s = cfg_slot(w, cfg);
    if (!s) return;
    s->calls++;
    if (p > s->peak) s->peak = p;
    if (t > s->tp) s->tp = t;
    if (t >= CEILING_TP) s->at_ceiling++;
    if (t > 1.0f) s->over_0++;
}

static void meter_bus_cb(ak_bus_meter_info *in)
{
    if (g_meter.bad || !in) return;
    int nch = (int)(in->cfg & 0xff);
    AcquireSRWLockExclusive(&g_meter.lock);
    if (!g_meter.bus_checked) {
        if (!readable(in, sizeof *in) || in->flags != METER_BUS_FLAGS || nch < 1 || nch > 32 ||
            !check_metering(in->m, nch, TRUE, TRUE)) {
            ReleaseSRWLockExclusive(&g_meter.lock);
            meter_fail("the master bus's metering data did not have the expected layout");
            return;
        }
        g_meter.bus_checked = TRUE;
    }
    ak_metering *m = in->m;
    const float *pk = m && nch ? ((pfn_mvec)m->vt[1])(m) : NULL, *tp = m && nch ? ((pfn_mvec)m->vt[2])(m) : NULL;
    if (pk && tp) {
        float p = vmax(pk, nch), t = vmax(tp, nch);
        g_meter.win.readings++;
        g_meter.all.readings++;
        cfg_add(&g_meter.win, in->cfg, p, t);
        cfg_add(&g_meter.all, in->cfg, p, t);
        ULONGLONG now = GetTickCount64();
        if (t > g_meter.tp_max) { g_meter.tp_max = t; g_meter.tp_at = now; }
        /* Momentary loudness from the widest layout (the main mix / the bed). */
        if ((uint32_t)nch > g_meter.widest) { g_meter.widest = nch; g_meter.kfill = 0; }
        if ((uint32_t)nch == g_meter.widest) {
            float k = ((pfn_mk)m->vt[4])(m);
            g_meter.kring[g_meter.kpos] = k;
            g_meter.kpos = (g_meter.kpos + 1) % 19;
            if (g_meter.kfill < 19) g_meter.kfill++;
            float ks = 0;
            for (int i = 0; i < g_meter.kfill; i++) ks += g_meter.kring[i];
            float mom = ks / (float)g_meter.kfill;
            if (mom > g_meter.win.mom) g_meter.win.mom = mom;
            if (mom > g_meter.all.mom) { g_meter.all.mom = mom; g_meter.mom_at = now; }
        }
    }
    ReleaseSRWLockExclusive(&g_meter.lock);
}

/* Every other bus: peak only. cookie = 1 + index into k_buses. */
static void meter_sub_cb(ak_bus_meter_info *in)
{
    if (g_meter.bad || !in) return;
    size_t idx = (size_t)in->cookie - 1;
    int nch = (int)(in->cfg & 0xff);
    AcquireSRWLockExclusive(&g_meter.lock);
    if (!g_meter.sub_checked) {
        if (!readable(in, sizeof *in) || in->flags != METER_SUB_FLAGS || idx >= N_BUSES || nch > 32 ||
            (nch && !check_metering(in->m, nch, FALSE, FALSE))) {
            ReleaseSRWLockExclusive(&g_meter.lock);
            meter_fail("a bus's metering data did not have the expected layout");
            return;
        }
        g_meter.sub_checked = TRUE;
    }
    if (idx < N_BUSES && in->m && nch) {
        const float *pk = ((pfn_mvec)in->m->vt[1])(in->m);
        if (pk) {
            float p = vmax(pk, nch);
            if (p > g_meter.win.bus[idx]) g_meter.win.bus[idx] = p;
            if (p > g_meter.all.bus[idx]) { g_meter.all.bus[idx] = p; g_meter.bus_at[idx] = GetTickCount64(); }
        }
    }
    ReleaseSRWLockExclusive(&g_meter.lock);
}

static double mdb(float a) { return a > 0 ? 20.0 * log10(a) : -144.0; }
static double lufs(float k) { return k > 0 ? -0.691 + 10.0 * log10(k) : -144.0; }

static void meter_append(const char *name, const char *text, BOOL truncate)
{
    WCHAR path[MAX_PATH], wname[64];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 64);
    _snwprintf(path, MAX_PATH, L"%ls%ls", g_dir, wname);
    HANDLE f = CreateFileW(path, truncate ? GENERIC_WRITE : FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           truncate ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(f, text, (DWORD)strlen(text), &w, NULL);
    CloseHandle(f);
}

static void hms(ULONGLONG at, char *out, size_t n)
{
    if (!at) { snprintf(out, n, "never"); return; }
    ULONGLONG s = (at - g_meter.start) / 1000;
    snprintf(out, n, "%02u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
}

static void bus_name(size_t i, char *out, size_t n)
{
    if (k_buses[i].name[0]) snprintf(out, n, "%s", k_buses[i].name);
    else snprintf(out, n, "#%u", k_buses[i].id);
}

static void cfg_desc(uint32_t cfg, char *out, size_t n)
{
    unsigned ch = cfg & 0xff, type = (cfg >> 8) & 0xf;
    if (ch == 12) snprintf(out, n, "7.1.4");
    else if (ch == 2) snprintf(out, n, "stereo");
    else if (ch == 1) snprintf(out, n, "mono%s", type == 0 ? " (anonymous)" : "");
    else snprintf(out, n, "%u ch", ch);
    if (type == 3) snprintf(out + strlen(out), n - strlen(out), " objects");
}

/* Indices of the k loudest buses in `v`, loudest first. */
static int top_buses(const float *v, int *idx, int k)
{
    int n = 0;
    for (; n < k; n++) {
        int best = -1;
        for (int i = 0; i < N_BUSES; i++) {
            BOOL taken = FALSE;
            for (int j = 0; j < n; j++) taken |= idx[j] == i;
            if (!taken && v[i] > 0 && (best < 0 || v[i] > v[best])) best = i;
        }
        if (best < 0) break;
        idx[n] = best;
    }
    return n;
}

/* The summary, rewritten every few seconds: what you need from a session. */
static void meter_write_summary(void)
{
    AcquireSRWLockShared(&g_meter.lock);
    meter_win a = g_meter.all;
    ULONGLONG tp_at = g_meter.tp_at, mom_at = g_meter.mom_at, bus_at[N_BUSES];
    memcpy(bus_at, g_meter.bus_at, sizeof bus_at);
    ReleaseSRWLockShared(&g_meter.lock);

    char buf[6144], t1[16], el[16];
    hms(GetTickCount64(), el, sizeof el);
    int n = snprintf(buf, sizeof buf,
        "Level meter summary, %s since the meter started. Limiter: %s.\r\n"
        "Buses metered: master + %d others.\r\n\r\n"
        "MASTER BUS (%s)\r\n",
        el, g_meter.limiter_off ? "OFF (measuring only)" : "on", g_meter.subs_registered,
        g_meter.limiter_off ? "no limiter: this is the game's own level" : "after its compressor and limiter");
    float loudest = 0;
    if (!a.readings)
        n += snprintf(buf + n, sizeof buf - n, "  no data: Wwise never called the master bus meter.\r\n");
    else {
        n += snprintf(buf + n, sizeof buf - n, "  %u readings (with spatial sound, one per 3D object per frame).\r\n",
                      a.readings);
        for (int i = 0; i < a.ncfg; i++) {
            cfg_stats *s = &a.cfg[i];
            char d[48];
            cfg_desc(s->cfg, d, sizeof d);
            n += snprintf(buf + n, sizeof buf - n,
                "  %s (0x%x): %u readings; loudest true peak %+.1f dBTP, sample peak %+.1f dBFS;"
                " at or above -1.1 dBTP %u, over 0 dBTP %u\r\n",
                d, s->cfg, s->calls, mdb(s->tp), mdb(s->peak), s->at_ceiling, s->over_0);
            if (s->tp > loudest) loudest = s->tp;
        }
        hms(tp_at, t1, sizeof t1);
        n += snprintf(buf + n, sizeof buf - n, "  loudest of all at %s.\r\n", t1);
        hms(mom_at, t1, sizeof t1);
        n += snprintf(buf + n, sizeof buf - n, "  loudest momentary loudness (widest layout) %+.1f LUFS, at %s.\r\n",
                      lufs(a.mom), t1);
    }
    int top[10], nt = top_buses(a.bus, top, 10);
    n += snprintf(buf + n, sizeof buf - n, "\r\nLOUDEST BUSES (sample peak; parent in brackets)\r\n");
    if (!nt) n += snprintf(buf + n, sizeof buf - n, "  no data from any bus.\r\n");
    for (int i = 0; i < nt; i++) {
        char nm[48], pn[48] = "top";
        bus_name(top[i], nm, sizeof nm);
        for (int j = 0; j < N_BUSES; j++)
            if (k_buses[j].id == k_buses[top[i]].parent) bus_name(j, pn, sizeof pn);
        hms(bus_at[top[i]], t1, sizeof t1);
        n += snprintf(buf + n, sizeof buf - n, "  %+6.1f dBFS  %s [%s], at %s\r\n", mdb(a.bus[top[i]]), nm, pn, t1);
    }
    n += snprintf(buf + n, sizeof buf - n, "\r\n");
    if (!a.readings)
        n += snprintf(buf + n, sizeof buf - n, "Nothing measured: nothing can be concluded.\r\n");
    else if (g_meter.limiter_off)
        n += snprintf(buf + n, sizeof buf - n,
            "To keep the loudest true peak at or below -1 dBTP, turn the game down by %.1f dB.\r\n",
            mdb(loudest) + 1.0 > 0 ? mdb(loudest) + 1.0 : 0.0);
    else
        n += snprintf(buf + n, sizeof buf - n,
            "The limiter is on. Readings at -1.1 dBTP or above are where it was working; readings over 0 dBTP\r\n"
            "got past it. Set Limiter=off and measure the same scene to see the game's own level.\r\n");
    if (g_meter.bad)
        snprintf(buf + n, sizeof buf - n, "METER STOPPED: %s. The numbers above are from before it stopped.\r\n", g_meter.why);
    meter_append("QuarryEssentialAudio_meter_summary.txt", buf, TRUE);
}

static DWORD WINAPI meter_writer(void *arg)
{
    (void)arg;
    BOOL said_bad = FALSE;
    for (int tick = 1;; tick++) {
        Sleep(1000);
        AcquireSRWLockExclusive(&g_meter.lock);
        meter_win w = g_meter.win;
        memset(&g_meter.win, 0, sizeof g_meter.win);
        ReleaseSRWLockExclusive(&g_meter.lock);

        char line[1024], t[16];
        hms(GetTickCount64(), t, sizeof t);
        float any = 0;
        for (int i = 0; i < w.ncfg; i++) if (w.cfg[i].peak > any) any = w.cfg[i].peak;
        /* a line for each second with sound in it */
        if (any > 0.0003f) {
            int n = snprintf(line, sizeof line, "%s  master", t);
            for (int i = 0; i < w.ncfg; i++) {
                char d[48];
                cfg_desc(w.cfg[i].cfg, d, sizeof d);
                n += snprintf(line + n, sizeof line - n, " %s %+.1f TP%s%s (%u)", d, mdb(w.cfg[i].tp),
                              w.cfg[i].at_ceiling ? " AT CEILING" : "", w.cfg[i].over_0 ? " OVER 0" : "", w.cfg[i].calls);
            }
            n += snprintf(line + n, sizeof line - n, ", %+.1f LUFS", lufs(w.mom));
            int top[3], nt = top_buses(w.bus, top, 3);
            n += snprintf(line + n, sizeof line - n, " | buses");
            for (int i = 0; i < nt; i++) {
                char nm[48];
                bus_name(top[i], nm, sizeof nm);
                n += snprintf(line + n, sizeof line - n, " %s %+.1f", nm, mdb(w.bus[top[i]]));
            }
            snprintf(line + n, sizeof line - n, "\r\n");
            meter_append("QuarryEssentialAudio_meter.log", line, FALSE);
        }
        if (g_meter.bad && !said_bad) {
            snprintf(line, sizeof line, "%s  METER STOPPED: %s.\r\n", t, g_meter.why);
            meter_append("QuarryEssentialAudio_meter.log", line, FALSE);
            qlog("Level meter stopped: %s.", g_meter.why);
            said_bad = TRUE;
        }
        if (tick % 5 == 0 || said_bad) meter_write_summary();
    }
    return 0;
}

/* Called from the Wwise follow-up loop every 2 s until it returns TRUE. */
static BOOL meter_try_register(void)
{
    static BOOL bus_ok, subs_done;
    static int tries;
    pfn_regbusm regbus = (pfn_regbusm)(void *)GetProcAddress(g_game,
        "?RegisterBusMeteringCallback@SoundEngine@AK@@YA?AW4AKRESULT@@IP6AXPEAUAkBusMeteringCallbackInfo@@@ZW4AkMeteringFlags@@PEAX@Z");
    if (!regbus) {
        qlog("Could not find Wwise's metering functions in the game; no level meter.");
        return TRUE;
    }
    if (!g_meter.start) {
        g_meter.start = GetTickCount64();
        keep_previous(L"QuarryEssentialAudio_meter.log");
        keep_previous(L"QuarryEssentialAudio_meter_summary.txt");
        meter_append("QuarryEssentialAudio_meter.log", "", TRUE);
    }
    int rb = 0;
    if (!bus_ok) bus_ok = (rb = regbus(AK_MASTER_BUS_ID, meter_bus_cb, METER_BUS_FLAGS, NULL)) == 1;
    /* Once the master is there, the bank is loaded: every other bus, once. */
    if (bus_ok && !subs_done) {
        for (size_t i = 0; i < N_BUSES; i++)
            if (k_buses[i].id != AK_MASTER_BUS_ID &&
                regbus(k_buses[i].id, meter_sub_cb, METER_SUB_FLAGS, (void *)(i + 1)) == 1)
                g_meter.subs_registered++;
        subs_done = TRUE;
    }
    tries++;
    if (!bus_ok && tries < 30) return FALSE;
    if (!bus_ok) {
        qlog("Wwise refused the level meter (result %d).", rb);
        return TRUE;
    }
    qlog("Level meter on (master bus and %d other buses): writing QuarryEssentialAudio_meter.log and "
         "QuarryEssentialAudio_meter_summary.txt.", g_meter.subs_registered);
    HANDLE t = CreateThread(NULL, 0, meter_writer, NULL, 0, NULL);
    if (t) CloseHandle(t);
    return TRUE;
}
