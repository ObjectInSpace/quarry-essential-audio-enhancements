/* Reads the device-format property from inside a module that is neither the
 * game nor the mod -- the way Windows' own audio code reads it while serving
 * the game. The mod must answer such reads with the TRUE format. */
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>

static const PROPERTYKEY k_devfmt = { {0xf19f064d,0x082c,0x4e27,{0xbc,0x73,0x68,0x82,0xa1,0xbb,0x8e,0x4c}}, 0 };

__declspec(dllexport) __attribute__((noinline)) int fwin_devfmt_channels(IMMDevice *dev)
{
    IPropertyStore *ps = NULL;
    PROPVARIANT v;
    int ch = -1;
    PropVariantInit(&v);
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps)) &&
        SUCCEEDED(IPropertyStore_GetValue(ps, &k_devfmt, &v)) && v.vt == VT_BLOB &&
        v.blob.cbSize >= sizeof(WAVEFORMATEX))
        ch = ((WAVEFORMATEX *)v.blob.pBlobData)->nChannels;
    PropVariantClear(&v);
    if (ps) IPropertyStore_Release(ps);
    return ch;
}
