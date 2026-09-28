/* Stand-ins for the Wwise functions the mod calls, exported from the test exe
 * under the game's MSVC names (test_qsa.def). Calling conventions follow the
 * game's machine code: GetSpeakerConfiguration(rcx = AkChannelConfig* return
 * slot, rdx = device id); Get/SetPanningRule(rule* or rule, device id). */
#include <stdint.h>

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

__attribute__((noinline)) int fw_SetPanningRule(int rule, uint64_t dev)
{
    fw_setpan_calls++;
    if (dev != 0) return 2;
    fw_pan = rule;
    return 1;
}
