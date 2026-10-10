#include "mute.h"
#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

namespace mute {

bool mute_process() {
    bool ok = false;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* devices = nullptr;
    IMMDevice* device = nullptr;
    IAudioSessionManager* sessions = nullptr;
    ISimpleAudioVolume* volume = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&devices))) &&
        SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
        SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(&sessions))) &&
        // GUID_NULL + FALSE = this process's default session, which every stream the game opens joins.
        SUCCEEDED(sessions->GetSimpleAudioVolume(nullptr, FALSE, &volume))) {
        volume->SetMute(TRUE, nullptr);
        BOOL muted = FALSE;
        ok = SUCCEEDED(volume->GetMute(&muted)) && muted;
    }
    if (volume) volume->Release();
    if (sessions) sessions->Release();
    if (device) device->Release();
    if (devices) devices->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}

}  // namespace mute
