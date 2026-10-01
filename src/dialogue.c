/* Positional dialogue (Dialogue=positional). Included by qsa.c.
 *
 * WHAT THE GAME DOES. Almost all dialogue is positioned on the speaking
 * character, with the listener on the camera (measured in game, 2026-09/10),
 * but every distance setting ("attenuation") it uses spreads the voice over
 * 75% of the circle close up and 100% further away, and none makes it quieter
 * with distance. So voices sound wide and central from any angle and range.
 *
 * WHAT THIS CHANGES. In the dialogue bank (Speech.bnk), each of the 32
 * attenuations used by a positioned voice -- listed with a fingerprint in
 * dialogue_table.h -- gets three curves APPENDED and three slots pointed at
 * them; the original curves stay in the item:
 *   slot 0  volume    flat to DialogueFalloffStart, then DialogueFalloffPerDoubling
 *                     dB each time the distance doubles, at most DialogueFalloffMax
 *   slot 5  spread    DialogueSpreadNear % at 0 m, DialogueSpreadFar % from DialogueSpreadFarAt
 *   slot 3  low-pass  none to DialogueMuffleStart, rising with the log of the
 *                     distance to DialogueMuffleMax at DialogueMuffleFullAt
 * Slots 1 and 2 (the reverb sends) keep the original curve: the reverb is as
 * the game has it, so a distant voice is also relatively more reverberant.
 * The four attenuations used only by unpositioned voices (phone calls and the
 * like) and eight unused ones are left alone. Settings are in metres; the
 * game's unit is the centimetre.
 *
 * HOW. The game loads the bank BY NAME, so Wwise reads the file itself. On
 * that call this reads the same file through Wwise's own stream manager
 * (exported as IAkStreamMgr::m_pStreamMgr) -- the bytes the game would have
 * loaded, from wherever it keeps them -- rewrites them in memory, and loads
 * the result with LoadBankMemoryCopy. Nothing on disk changes. The vtable
 * slots were read off the exe's own bank reader:
 *   stream manager +0x20 CreateStd(name, flags, mode, &stream, sync), tried
 *                        language-specific first, then not (0x14488948b)
 *   stream +0x08 Destroy, +0x28 GetBlockSize, +0x30 Read(buf, size, wait,
 *   priority, deadline, &got), +0x68 WaitForPendingOperation (4 = error)
 *
 * ENCODING. Volume curves (scaling 2, "dB") are STORED as dB / 20, i.e. log10
 * of the amplitude: the game's own end at values like -0.6019 (-12.04 dB).
 * The first version wrote plain dB, so -6 dB at 4 m became -120 dB and every
 * voice beyond 2 m went silent until the next camera cut (heard in game,
 * 2026-10-01). Spread and low-pass curves are stored as they read: the game's
 * own low-pass curves run 0 to 35.
 *
 * SAFETY. Anything unexpected -- the file will not open or read, it is not
 * the dialogue bank, no attenuation matches its fingerprint (a different
 * version of the game), the rebuilt bank fails its structural check, or Wwise
 * refuses it -- and the game's own LoadBank runs, untouched, and the log says
 * why. With Dialogue=game nothing is hooked at all. */

#include "MinHook.h"
#include <math.h>
#include <stdlib.h>
#include "dialogue_table.h"

typedef struct {
    BOOL positional;
    float spread_near, spread_far, spread_far_at;
    float falloff_start, falloff_per_doubling, falloff_max;
    float muffle_start, muffle_full_at, muffle_max;
    float max_distance;
} dlg_cfg_t;

static dlg_cfg_t g_dlg;

/* Wwise turns names into ids with 32-bit FNV-1 over the lower-cased name. */
static uint32_t dlg_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 'A' && c <= 'Z') c += 32;
        h = h * 16777619u ^ c;
    }
    return h;
}

/* ---- settings ---- */
static float dlg_float(const WCHAR *ini, const WCHAR *key, float def, float lo, float hi)
{
    WCHAR buf[64];
    GetPrivateProfileStringW(L"Audio", key, L"", buf, 64, ini);
    if (!buf[0]) return def;
    WCHAR *end;
    double v = wcstod(buf, &end);
    if (end == buf || v < lo || v > hi) {
        char k[64];
        WideCharToMultiByte(CP_UTF8, 0, key, -1, k, sizeof k, NULL, NULL);
        qlog("%s is not a number from %g to %g; using %g.", k, lo, hi, def);
        return def;
    }
    return (float)v;
}

/* Dialogue=game (the game as designed) or positional, and the numbers. */
static void read_dialogue(char *raw, size_t n)
{
    WCHAR path[MAX_PATH], v[32];
    ini_path(path);
    GetPrivateProfileStringW(L"Audio", L"Dialogue", L"game", v, 32, path);
    WideCharToMultiByte(CP_UTF8, 0, v, -1, raw, (int)n, NULL, NULL);
    g_dlg.positional = !_wcsicmp(v, L"positional");
    if (!g_dlg.positional && _wcsicmp(v, L"game"))
        qlog("Unknown Dialogue value; using game. Choices: game, positional.");
    g_dlg.spread_near = dlg_float(path, L"DialogueSpreadNear", 30, 0, 100);
    g_dlg.spread_far = dlg_float(path, L"DialogueSpreadFar", 5, 0, 100);
    g_dlg.spread_far_at = dlg_float(path, L"DialogueSpreadFarAt", 3, 0, 1000);
    g_dlg.falloff_start = dlg_float(path, L"DialogueFalloffStart", 2, 0.01f, 1000);
    g_dlg.falloff_per_doubling = dlg_float(path, L"DialogueFalloffPerDoubling", 6, 0, 48);
    g_dlg.falloff_max = dlg_float(path, L"DialogueFalloffMax", 12, 0, 96);
    g_dlg.muffle_start = dlg_float(path, L"DialogueMuffleStart", 2, 0.01f, 1000);
    g_dlg.muffle_full_at = dlg_float(path, L"DialogueMuffleFullAt", 20, 0.01f, 1000);
    g_dlg.muffle_max = dlg_float(path, L"DialogueMuffleMax", 35, 0, 100);
    g_dlg.max_distance = dlg_float(path, L"DialogueMaxDistance", 20, 0.1f, 1000);
    if (g_dlg.muffle_full_at <= g_dlg.muffle_start) {
        qlog("DialogueMuffleFullAt must be further than DialogueMuffleStart; using 2 and 20.");
        g_dlg.muffle_start = 2;
        g_dlg.muffle_full_at = 20;
    }
}

/* ---- the three curves ----
 * Points are (distance in cm, value). Wwise interpolates linearly between
 * points, so the log-shaped curves get a point every half-doubling. */
typedef struct { float x, y; } dlg_pt;
#define DLG_MAX_PTS 48

static int dlg_add(dlg_pt *p, int n, float x, float y)
{
    if (n && x <= p[n - 1].x) return n;   /* distances must increase */
    if (n < DLG_MAX_PTS) { p[n].x = x; p[n].y = y; n++; }
    return n;
}

static int dlg_volume_curve(const dlg_cfg_t *c, dlg_pt *p)
{
    float max = c->max_distance * 100, start = c->falloff_start * 100;
    int n = dlg_add(p, 0, 0, 0);
    if (start >= max || c->falloff_per_doubling <= 0 || c->falloff_max <= 0) return dlg_add(p, n, max, 0);
    n = dlg_add(p, n, start, 0);
    for (float d = start * 1.41421356f; d < max; d *= 1.41421356f) {
        float db = -c->falloff_per_doubling * log2f(d / start);
        if (db <= -c->falloff_max) {
            float at = start * exp2f(c->falloff_max / c->falloff_per_doubling);   /* where the cap is reached */
            n = dlg_add(p, n, at < max ? at : max, -c->falloff_max);
            break;
        }
        n = dlg_add(p, n, d, db);
    }
    float last = -c->falloff_per_doubling * log2f(max / start);
    return dlg_add(p, n, max, last < -c->falloff_max ? -c->falloff_max : last);
}

static int dlg_spread_curve(const dlg_cfg_t *c, dlg_pt *p)
{
    float max = c->max_distance * 100, at = c->spread_far_at * 100;
    if (at <= 0) return dlg_add(p, dlg_add(p, 0, 0, c->spread_far), max, c->spread_far);
    int n = dlg_add(p, 0, 0, c->spread_near);
    if (at >= max)   /* the curve ends before it gets there: stop part way */
        return dlg_add(p, n, max, c->spread_near + (c->spread_far - c->spread_near) * max / at);
    n = dlg_add(p, n, at, c->spread_far);
    return dlg_add(p, n, max, c->spread_far);
}

static int dlg_muffle_curve(const dlg_cfg_t *c, dlg_pt *p)
{
    float max = c->max_distance * 100, s = c->muffle_start * 100, f = c->muffle_full_at * 100;
    int n = dlg_add(p, 0, 0, 0);
    if (s >= max || c->muffle_max <= 0) return dlg_add(p, n, max, 0);
    n = dlg_add(p, n, s, 0);
    float span = log2f(f / s);
    for (float d = s * 1.41421356f; d < max && d < f; d *= 1.41421356f)
        n = dlg_add(p, n, d, c->muffle_max * log2f(d / s) / span);
    if (f < max) n = dlg_add(p, n, f, c->muffle_max);
    float last = f <= max ? c->muffle_max : c->muffle_max * log2f(max / s) / span;
    return dlg_add(p, n, max, last);
}

/* ---- the rebuild ---- */
typedef struct { BYTE *p; size_t n, cap; BOOL bad; } dlg_buf;

static void dlg_put(dlg_buf *b, const void *src, size_t n)
{
    if (b->bad || b->n + n > b->cap) { b->bad = TRUE; return; }
    memcpy(b->p + b->n, src, n);
    b->n += n;
}
static void dlg_u8(dlg_buf *b, BYTE v) { dlg_put(b, &v, 1); }
static void dlg_u16(dlg_buf *b, uint16_t v) { dlg_put(b, &v, 2); }
static void dlg_u32(dlg_buf *b, uint32_t v) { dlg_put(b, &v, 4); }
static void dlg_f32(dlg_buf *b, float v) { dlg_put(b, &v, 4); }

/* Scaling 2 (dB) is stored as dB / 20: see ENCODING above. */
static void dlg_curve(dlg_buf *b, BYTE scaling, const dlg_pt *p, int n)
{
    dlg_u8(b, scaling);
    dlg_u16(b, (uint16_t)n);
    for (int i = 0; i < n; i++) {
        dlg_f32(b, p[i].x);
        dlg_f32(b, scaling == 2 ? p[i].y / 20.0f : p[i].y);
        dlg_u32(b, 4 /* linear */);
    }
}

static uint32_t dlg_fnv1a(const BYTE *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static const dp_att_t *dlg_find(uint32_t id)
{
    for (size_t i = 0; i < sizeof k_dp_atts / sizeof k_dp_atts[0]; i++)
        if (k_dp_atts[i].id == id) return &k_dp_atts[i];
    return NULL;
}

/* One attenuation's body -> the patched body. FALSE leaves it alone. */
static BOOL dlg_patch_att(const BYTE *body, uint32_t size, dlg_buf *out, const dlg_pt *vol, int nv,
                          const dlg_pt *spr, int ns, const dlg_pt *muf, int nm)
{
    if (size < 14 || body[5] != 0) return FALSE;   /* cone on: not a shape this knows */
    signed char ctu[7];
    memcpy(ctu, body + 6, 7);
    BYTE ncurves = body[13];
    if (ctu[0] < 0 || ctu[5] < 0 || ncurves > 16) return FALSE;
    size_t o = 14;
    for (int i = 0; i < ncurves; i++) {
        if (o + 3 > size) return FALSE;
        uint16_t np;
        memcpy(&np, body + o + 1, 2);
        o += 3 + (size_t)np * 12;
        if (o > size) return FALSE;
    }
    size_t curves_end = o;
    ctu[0] = (signed char)ncurves;
    ctu[5] = (signed char)(ncurves + 1);
    ctu[3] = (signed char)(ncurves + 2);
    dlg_put(out, body, 6);                        /* id, height spread, cone (off) */
    dlg_put(out, ctu, 7);
    dlg_u8(out, (BYTE)(ncurves + 3));
    dlg_put(out, body + 14, curves_end - 14);     /* every original curve */
    dlg_curve(out, 2 /* dB */, vol, nv);
    dlg_curve(out, 0, spr, ns);
    dlg_curve(out, 0, muf, nm);
    dlg_put(out, body + curves_end, size - curves_end);   /* RTPCs and the rest, as they were */
    return !out->bad;
}

/* TRUE when the chunks tile the bank exactly and HIRC's items tile HIRC. */
static BOOL dlg_bank_ok(const BYTE *b, size_t n, uint32_t *bank_id)
{
    size_t o = 0;
    BOOL hirc = FALSE;
    if (n < 16 || memcmp(b, "BKHD", 4)) return FALSE;
    if (bank_id) memcpy(bank_id, b + 12, 4);
    while (o < n) {
        uint32_t sz;
        if (o + 8 > n) return FALSE;
        memcpy(&sz, b + o + 4, 4);
        if (o + 8 + (size_t)sz > n) return FALSE;
        if (!memcmp(b + o, "HIRC", 4)) {
            uint32_t cnt;
            size_t p = o + 12, end = o + 8 + sz;
            if (sz < 4) return FALSE;
            memcpy(&cnt, b + o + 8, 4);
            for (uint32_t i = 0; i < cnt; i++) {
                uint32_t isz;
                if (p + 5 > end) return FALSE;
                memcpy(&isz, b + p + 1, 4);
                p += 5 + (size_t)isz;
                if (p > end) return FALSE;
            }
            if (p != end) return FALSE;
            hirc = TRUE;
        }
        o += 8 + sz;
    }
    return o == n && hirc;
}

static LONG g_dlg_patched, g_dlg_skipped, g_dlg_missing;

/* The whole bank -> a patched copy (VirtualAlloc'd), or FALSE with a reason. */
static BOOL dlg_rebuild(const BYTE *in, size_t n, BYTE **out, size_t *out_n, char *why, size_t whyn)
{
    dlg_pt vol[DLG_MAX_PTS], spr[DLG_MAX_PTS], muf[DLG_MAX_PTS];
    int nv = dlg_volume_curve(&g_dlg, vol), ns = dlg_spread_curve(&g_dlg, spr), nm = dlg_muffle_curve(&g_dlg, muf);
    size_t per = 3 * (3 + 12 * (size_t)DLG_MAX_PTS);
    dlg_buf b = { NULL, 0, n + per * (sizeof k_dp_atts / sizeof k_dp_atts[0]) + 64, FALSE };
    b.p = VirtualAlloc(NULL, b.cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!b.p) { snprintf(why, whyn, "out of memory"); return FALSE; }
    LONG patched = 0, skipped = 0;
    BYTE seen[sizeof k_dp_atts / sizeof k_dp_atts[0]] = { 0 };
    size_t o = 0;
    while (o < n) {
        uint32_t sz;
        memcpy(&sz, in + o + 4, 4);
        if (memcmp(in + o, "HIRC", 4)) {
            dlg_put(&b, in + o, 8 + (size_t)sz);
            o += 8 + sz;
            continue;
        }
        size_t hdr = b.n;
        dlg_put(&b, in + o, 12);                  /* tag, size (fixed below), count */
        uint32_t cnt;
        memcpy(&cnt, in + o + 8, 4);
        size_t p = o + 12;
        for (uint32_t i = 0; i < cnt; i++) {
            BYTE type = in[p];
            uint32_t isz, id = 0;
            memcpy(&isz, in + p + 1, 4);
            const BYTE *body = in + p + 5;
            if (isz >= 4) memcpy(&id, body, 4);
            const dp_att_t *t = type == 0x0E ? dlg_find(id) : NULL;
            BOOL done = FALSE;
            if (t) {
                seen[t - k_dp_atts] = 1;
                if (t->size == isz && t->hash == dlg_fnv1a(body, isz)) {
                    size_t item = b.n;
                    dlg_u8(&b, type);
                    dlg_u32(&b, 0);
                    if (dlg_patch_att(body, isz, &b, vol, nv, spr, ns, muf, nm)) {
                        uint32_t nsz = (uint32_t)(b.n - item - 5);
                        if (!b.bad) memcpy(b.p + item + 1, &nsz, 4);
                        done = TRUE;
                        patched++;
                    } else {
                        b.n = item;   /* undo; copied as it was below */
                    }
                }
                if (!done) skipped++;
            }
            if (!done) dlg_put(&b, in + p, 5 + (size_t)isz);
            p += 5 + isz;
        }
        uint32_t nsz = (uint32_t)(b.n - hdr - 8);
        if (!b.bad) memcpy(b.p + hdr + 4, &nsz, 4);
        o += 8 + sz;
    }
    LONG missing = 0;
    for (size_t i = 0; i < sizeof seen; i++) missing += !seen[i];
    g_dlg_patched = patched; g_dlg_skipped = skipped; g_dlg_missing = missing;
    if (b.bad) snprintf(why, whyn, "the rebuild ran out of room");
    else if (!patched) snprintf(why, whyn, "this version of the game's dialogue is not the one this mod knows");
    else if (!dlg_bank_ok(b.p, b.n, NULL)) snprintf(why, whyn, "the rebuilt dialogue failed its own check");
    else {
        *out = b.p;
        *out_n = b.n;
        return TRUE;
    }
    VirtualFree(b.p, 0, MEM_RELEASE);
    return FALSE;
}

/* ---- reading the bank through Wwise ---- */
typedef int (*pfn_sm_create_w)(void *, const wchar_t *, void *, int, void **, unsigned char);
typedef void (*pfn_st_destroy)(void *);
typedef uint32_t (*pfn_st_block)(void *);
typedef int (*pfn_st_read)(void *, void *, uint32_t, unsigned char, signed char, float, uint32_t *);
typedef int (*pfn_st_wait)(void *);

typedef struct {   /* AkFileSystemFlags (Wwise 2021), as the exe's bank reader fills it */
    uint32_t company, codec, custom_size;
    void *custom;
    unsigned char language_specific, automatic;
    uint32_t cache_id;
} dlg_fsflags_t;

static void *dlg_vfn(void *obj, int slot) { return (*(void ***)obj)[slot]; }

static BYTE *dlg_read_bank(const wchar_t *file, size_t *n, char *why, size_t whyn)
{
    void **mgr_slot = (void **)(void *)GetProcAddress(g_game, "?m_pStreamMgr@IAkStreamMgr@AK@@1PEAV12@EA");
    void *mgr = mgr_slot ? *mgr_slot : NULL;
    if (!mgr) { snprintf(why, whyn, "Wwise's file reader was not found"); return NULL; }
    void *st = NULL;
    int r = 0;
    for (int lang = 1; lang >= 0 && !st; lang--) {
        dlg_fsflags_t fl = { 0, 0, 0, NULL, (unsigned char)lang, 0, 0xffffffffu };
        r = ((pfn_sm_create_w)dlg_vfn(mgr, 4))(mgr, file, &fl, 0 /* read */, &st, 1 /* sync */);
        if (r != 1) st = NULL;
    }
    if (!st) { snprintf(why, whyn, "could not open the game's dialogue file (%d)", r); return NULL; }
    uint32_t block = ((pfn_st_block)dlg_vfn(st, 5))(st);
    if (!block || block > (1u << 20)) block = 1;
    uint32_t chunk = (256u * 1024 + block - 1) / block * block;
    size_t cap = 4u << 20, total = 0;
    BYTE *buf = VirtualAlloc(NULL, cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    BOOL ok = buf != NULL;
    if (!ok) snprintf(why, whyn, "out of memory");
    while (ok) {
        if (total + chunk > cap) {
            size_t ncap = cap * 2;
            BYTE *nb = ncap <= (64u << 20) ? VirtualAlloc(NULL, ncap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : NULL;
            if (!nb) { ok = FALSE; snprintf(why, whyn, "the dialogue file is larger than expected"); break; }
            memcpy(nb, buf, total);
            VirtualFree(buf, 0, MEM_RELEASE);
            buf = nb; cap = ncap;
        }
        uint32_t got = 0;
        r = ((pfn_st_read)dlg_vfn(st, 6))(st, buf + total, chunk, 1 /* wait */, 50, 0.0f, &got);
        int status = ((pfn_st_wait)dlg_vfn(st, 13))(st);
        if (r != 1 || status == 4) { ok = FALSE; snprintf(why, whyn, "could not read the game's dialogue file (%d, %d)", r, status); break; }
        total += got;
        if (got < chunk) break;
    }
    ((pfn_st_destroy)dlg_vfn(st, 1))(st);
    if (!ok) { if (buf) VirtualFree(buf, 0, MEM_RELEASE); return NULL; }
    *n = total;
    return buf;
}

/* ---- the LoadBank hooks ---- */
typedef int (*pfn_lb_w)(const wchar_t *, uint32_t *);
typedef int (*pfn_lb_a)(const char *, uint32_t *);
typedef int (*pfn_lb_copy)(const void *, uint32_t, uint32_t *);
static pfn_lb_w o_lb_w;
static pfn_lb_a o_lb_a;

/* TRUE when it loaded the dialogue bank itself; *ret is the result for the
 * game. FALSE: the caller runs the game's own LoadBank. */
static BOOL dlg_try_load(const char *name, uint32_t *out_id, int *ret)
{
    if (_stricmp(name, "Speech") && _stricmp(name, "Speech.bnk")) return FALSE;
    char why[128] = "";
    size_t n = 0, pn = 0;
    BYTE *bank = dlg_read_bank(L"Speech.bnk", &n, why, sizeof why), *patched = NULL;
    uint32_t id = 0;
    BOOL ok = bank != NULL;
    if (ok && !(dlg_bank_ok(bank, n, &id) && id == dlg_hash("Speech"))) {
        ok = FALSE;
        snprintf(why, sizeof why, "the file read is not the game's dialogue");
    }
    if (ok) ok = dlg_rebuild(bank, n, &patched, &pn, why, sizeof why);
    if (bank) VirtualFree(bank, 0, MEM_RELEASE);
    pfn_lb_copy copy = (pfn_lb_copy)(void *)GetProcAddress(g_game, "?LoadBankMemoryCopy@SoundEngine@AK@@YA?AW4AKRESULT@@PEBXIAEAI@Z");
    if (ok && !copy) { ok = FALSE; snprintf(why, sizeof why, "Wwise's LoadBankMemoryCopy was not found"); }
    int r = 0;
    if (ok) {
        uint32_t got_id = 0;
        r = copy(patched, (uint32_t)pn, &got_id);
        if (r != 1) { ok = FALSE; snprintf(why, sizeof why, "Wwise refused the changed dialogue (result %d)", r); }
        else if (out_id) *out_id = got_id;
    }
    if (patched) VirtualFree(patched, 0, MEM_RELEASE);
    if (!ok) {
        qlog("Dialogue NOT changed: %s. The game's own dialogue is used.", why);
        return FALSE;
    }
    qlog("Positional dialogue applied: %ld of %u distance settings changed%s.", g_dlg_patched,
         (unsigned)(sizeof k_dp_atts / sizeof k_dp_atts[0]),
         g_dlg_patched < (LONG)(sizeof k_dp_atts / sizeof k_dp_atts[0]) ? " (the others differ from the version this mod knows)" : "");
    *ret = r;
    return TRUE;
}

static int d_lb_w(const wchar_t *name, uint32_t *out)
{
    char a[64];
    size_t i = 0;
    for (; name && name[i] && i < sizeof a - 1; i++) a[i] = (char)(name[i] < 128 ? name[i] : '?');
    a[i] = 0;
    int r;
    if (dlg_try_load(a, out, &r)) return r;
    return o_lb_w(name, out);
}

static int d_lb_a(const char *name, uint32_t *out)
{
    int r;
    if (name && dlg_try_load(name, out, &r)) return r;
    return o_lb_a(name, out);
}

static int g_dlg_state;   /* 0 off, 1 hooked, -1 could not hook */

/* Called first thing on the worker thread, before the game loads any bank. */
static void dialogue_setup(void)
{
    char raw[32];
    read_dialogue(raw, sizeof raw);
    if (!g_dlg.positional) return;
    void *w = (void *)GetProcAddress(g_game, "?LoadBank@SoundEngine@AK@@YA?AW4AKRESULT@@PEB_WAEAI@Z");
    void *a = (void *)GetProcAddress(g_game, "?LoadBank@SoundEngine@AK@@YA?AW4AKRESULT@@PEBDAEAI@Z");
    MH_STATUS st = MH_Initialize();
    BOOL hooked = (st == MH_OK || st == MH_ERROR_ALREADY_INITIALIZED) && w &&
                  MH_CreateHook(w, (void *)d_lb_w, (void **)&o_lb_w) == MH_OK && MH_EnableHook(w) == MH_OK;
    if (hooked && a && MH_CreateHook(a, (void *)d_lb_a, (void **)&o_lb_a) == MH_OK) MH_EnableHook(a);
    g_dlg_state = hooked ? 1 : -1;
}

/* One log line, after the version line. */
static void dialogue_report(void)
{
    if (g_dlg_state < 0)
        qlog("Dialogue: could not attach to the game's sound engine; the game's own dialogue is used.");
    else if (g_dlg_state > 0)
        qlog("Dialogue: spread %g%% close, %g%% from %g m; %g dB quieter per doubling of distance from %g m, "
             "at most %g dB; muffling from %g m, %g at %g m.", g_dlg.spread_near, g_dlg.spread_far, g_dlg.spread_far_at,
             g_dlg.falloff_per_doubling, g_dlg.falloff_start, g_dlg.falloff_max, g_dlg.muffle_start, g_dlg.muffle_max,
             g_dlg.muffle_full_at);
}
