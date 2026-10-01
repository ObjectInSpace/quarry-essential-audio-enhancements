/* Stand-ins for the Wwise functions the mod calls, exported from the test exe
 * under the game's MSVC names (test_qsa.def). Calling conventions follow the
 * game's machine code: GetSpeakerConfiguration(rcx = AkChannelConfig* return
 * slot, rdx = device id); Get/SetPanningRule(rule* or rule, device id). */
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <stdint.h>

/* AK::GetDeviceID(IMMDevice*): records which device it was asked about and
 * answers with a recognisable id. */
int fw_getdevid_calls;
WCHAR fw_getdevid_endpoint[128];

__attribute__((noinline)) uint32_t fw_GetDeviceID(IMMDevice *d)
{
    LPWSTR id = NULL;
    fw_getdevid_calls++;
    if (d && SUCCEEDED(IMMDevice_GetId(d, &id))) {
        wcsncpy(fw_getdevid_endpoint, id, 127);
        CoTaskMemFree(id);
    }
    return 4242;
}

int fw_initialized;
uint32_t fw_speaker_cfg = 0x3102;   /* stereo */
int fw_pan = 0;                     /* speakers */
int fw_setpan_calls;

__attribute__((noinline)) char fw_IsInitialized(void) { return fw_initialized ? 1 : 0; }

__attribute__((noinline)) uint32_t *fw_GetSpeakerConfiguration(uint32_t *out, uint64_t dev)
{
    *out = dev == 0 ? fw_speaker_cfg : 0;
    return out;
}

__attribute__((noinline)) int fw_GetPanningRule(int *out, uint64_t dev)
{
    if (dev != 0) return 2;
    *out = fw_pan;
    return 1;
}

/* SetBusEffect(AkUniqueID bus, AkUInt32 slot, AkUniqueID shareset). */
int fw_setbusfx_calls;
uint32_t fw_setbusfx_args[3];

__attribute__((noinline)) int fw_SetBusEffect(uint32_t bus, uint32_t slot, uint32_t shareset)
{
    fw_setbusfx_calls++;
    fw_setbusfx_args[0] = bus;
    fw_setbusfx_args[1] = slot;
    fw_setbusfx_args[2] = shareset;
    return 1;
}

/* ReplaceOutput(const AkOutputSettings&, AkOutputDeviceID, AkOutputDeviceID*):
 * records what it was given and, like Wwise, the new layout then shows up in
 * GetSpeakerConfiguration. */
uint32_t fw_replace_settings[4];
uint64_t fw_replace_id = 99;
int fw_replace_calls;

__attribute__((noinline)) int fw_ReplaceOutput(const uint32_t *settings, uint64_t id, uint64_t *out)
{
    fw_replace_calls++;
    for (int i = 0; i < 4; i++) fw_replace_settings[i] = settings[i];
    fw_replace_id = id;
    if (out) *out = 0;
    fw_speaker_cfg = settings[3];
    return 1;
}

__attribute__((noinline)) int fw_SetPanningRule(int rule, uint64_t dev)
{
    fw_setpan_calls++;
    if (dev != 0) return 2;
    fw_pan = rule;
    return 1;
}

/* For Dialogue=positional: a stream manager that serves one file from memory,
 * laid out as the vtable slots read off the game's bank reader (see
 * src/dialogue.c); LoadBank by wide and narrow name (the game's own path,
 * which the mod must fall back to); and LoadBankMemoryCopy (the changed
 * path). Each records what it was given. */
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static uint32_t fw_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { unsigned char c = (unsigned char)*s; if (c >= 'A' && c <= 'Z') c += 32; h = h * 16777619u ^ c; }
    return h;
}

const unsigned char *fw_file_data;
size_t fw_file_size;
int fw_stream_fail, fw_open_calls, fw_open_lang_first = -1, fw_streams_open, fw_unaligned_reads;
int fw_lbw_calls, fw_lba_calls, fw_lbcopy_calls;
unsigned char *fw_lbcopy_data;
uint32_t fw_lbcopy_size;

typedef struct { void **vt; size_t pos; } fw_stream;
static void fw_st_nop(fw_stream *s) { (void)s; }
static void fw_st_destroy(fw_stream *s) { fw_streams_open--; free(s); }
static uint32_t fw_st_block(fw_stream *s) { (void)s; return 2048; }
static int fw_st_read(fw_stream *s, void *buf, uint32_t size, unsigned char wait, signed char prio, float dl, uint32_t *got)
{
    (void)wait; (void)prio; (void)dl;
    if (size % 2048) fw_unaligned_reads++;
    size_t left = fw_file_size - s->pos, n = size < left ? size : left;
    memcpy(buf, fw_file_data + s->pos, n);
    s->pos += n;
    *got = (uint32_t)n;
    return 1;
}
static int fw_st_wait(fw_stream *s) { (void)s; return 1; }
static void *fw_stream_vt[14] = {
    (void *)fw_st_nop, (void *)fw_st_destroy, (void *)fw_st_nop, (void *)fw_st_nop, (void *)fw_st_nop,
    (void *)fw_st_block, (void *)fw_st_read, (void *)fw_st_nop, (void *)fw_st_nop, (void *)fw_st_nop,
    (void *)fw_st_nop, (void *)fw_st_nop, (void *)fw_st_nop, (void *)fw_st_wait,
};

typedef struct { uint32_t company, codec, custom_size; void *custom; unsigned char lang, automatic; uint32_t cache; } fw_flags;
static int fw_mgr_nop(void *m) { (void)m; return 2; }
static int fw_mgr_create_name(void *m, const wchar_t *name, fw_flags *fl, int mode, fw_stream **out, unsigned char sync)
{
    (void)m; (void)sync;
    if (!fw_open_calls) fw_open_lang_first = fl ? fl->lang : -1;
    fw_open_calls++;
    if (fw_stream_fail || mode != 0 || !fw_file_data || wcscmp(name, L"Speech.bnk")) return 2;
    fw_stream *s = calloc(1, sizeof *s);
    s->vt = fw_stream_vt;
    fw_streams_open++;
    *out = s;
    return 1;
}
static void *fw_mgr_vt[6] = { (void *)fw_mgr_nop, (void *)fw_mgr_nop, (void *)fw_mgr_nop,
                              (void *)fw_mgr_nop, (void *)fw_mgr_create_name, (void *)fw_mgr_nop };
static struct { void **vt; } fw_mgr = { fw_mgr_vt };
void *fw_stream_mgr_ptr = &fw_mgr;

__attribute__((noinline)) int fw_LoadBankW(const wchar_t *n, uint32_t *out)
{
    char a[64];
    int i = 0;
    for (; n && n[i] && i < 63; i++) a[i] = (char)n[i];
    a[i] = 0;
    fw_lbw_calls++;
    *out = fw_hash(a);
    return 1;
}
__attribute__((noinline)) int fw_LoadBankA(const char *n, uint32_t *out)
{
    fw_lba_calls++;
    *out = fw_hash(n);
    return 1;
}
__attribute__((noinline)) int fw_LoadBankMemoryCopy(const void *p, uint32_t size, uint32_t *out)
{
    fw_lbcopy_calls++;
    free(fw_lbcopy_data);
    fw_lbcopy_data = malloc(size);
    memcpy(fw_lbcopy_data, p, size);
    fw_lbcopy_size = size;
    memcpy(out, (const unsigned char *)p + 12, 4);
    return 1;
}

/* Metering ([Measure] Meter=on): RegisterBusMeteringCallback records the
 * callback, as Wwise does;
 * the test then plays Wwise's audio thread by calling it with frames built
 * from fw_meter objects, laid out as the SDK's IAkMetering (an MSVC vtable:
 * slot 0 the destructor, then GetPeak, GetTruePeak, GetRMS, GetKWeightedPower). */
typedef struct { void **vt; float peak[16], tp[16], rms[16], k; } fw_meter;
static void fw_m_dtor(fw_meter *m) { (void)m; }
static const float *fw_m_peak(fw_meter *m) { return m->peak; }
static const float *fw_m_tp(fw_meter *m) { return m->tp; }
static const float *fw_m_rms(fw_meter *m) { return m->rms; }
static float fw_m_k(fw_meter *m) { return m->k; }
void *fw_meter_vt[5] = { (void *)fw_m_dtor, (void *)fw_m_peak, (void *)fw_m_tp, (void *)fw_m_rms, (void *)fw_m_k };

void *fw_buscb, *fw_sfxcb, *fw_sfxcookie;
uint32_t fw_buscb_bus, fw_buscb_flags, fw_sfx_flags;
int fw_regbus_calls, fw_master_regs;

/* Records the master bus's registration, and the SFX bus's (393239870). */
__attribute__((noinline)) int fw_RegisterBusMeteringCallback(uint32_t bus, void *cb, uint32_t flags, void *cookie)
{
    fw_regbus_calls++;
    if (bus == 3803692087u) { fw_master_regs++; fw_buscb = cb; fw_buscb_bus = bus; fw_buscb_flags = flags; }
    if (bus == 393239870u) { fw_sfxcb = cb; fw_sfxcookie = cookie; fw_sfx_flags = flags; }
    return 1;
}

/* SetOutputVolume(output id, gain) records every call; GetOutputID(shareset,
 * device) answers with a recognisable id built as Wwise builds them. */
int fw_setoutvol_calls;
uint64_t fw_setoutvol_id;
float fw_setoutvol_gain;
uint32_t fw_getoutid_args[2] = { 99, 99 };

__attribute__((noinline)) int fw_SetOutputVolume(uint64_t id, float gain)
{
    fw_setoutvol_calls++;
    fw_setoutvol_id = id;
    fw_setoutvol_gain = gain;
    return 1;
}
__attribute__((noinline)) uint64_t fw_GetOutputID(uint32_t shareset, uint32_t dev)
{
    fw_getoutid_args[0] = shareset;
    fw_getoutid_args[1] = dev;
    return ((uint64_t)dev << 32 | shareset) + 0x5000;   /* + a marker */
}
