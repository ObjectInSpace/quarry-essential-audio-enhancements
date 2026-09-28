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
