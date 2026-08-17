// Polls the virtual "Speakers (Virtual Audio Device (WDM) - Tablet Sample)"
// endpoint's mute/volume state and forwards changes to the real KEF speaker
// via kefctl: unmute -> power on (aux), mute -> power off, volume change ->
// set real volume. One-directional (Windows control -> real speaker).
//
// Build (from a "x64 Native Tools Command Prompt for VS 2022"):
//   cl /EHsc /O2 /DUNICODE /D_UNICODE /Fe:KefEndpointBridge.exe KefEndpointBridge.cpp ole32.lib advapi32.lib

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <string>
#include <fstream>
#include <ctime>
#include <cmath>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

static std::wstring g_kefctlPath;
static std::wstring g_logFile;

static void Log(const std::wstring& msg)
{
    std::wofstream f(g_logFile, std::ios::app);
    if (!f) return;
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    wchar_t buf[32];
    wcsftime(buf, 32, L"%Y-%m-%d %H:%M:%S", &lt);
    f << buf << L"  " << msg << std::endl;
}

static void RunKefctl(const std::wstring& args)
{
    std::wstring cmdline = L"perl \"" + g_kefctlPath + L"\" " + args;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmdline;

    if (CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

// Registry key names under MMDevices\Audio\Render omit the
// "{0.0.0.00000000}." endpoint-type prefix that IMMDevice::GetId() returns.
static std::wstring StripEndpointPrefix(const std::wstring& id)
{
    auto pos = id.find(L".{");
    if (pos == std::wstring::npos) return id;
    return id.substr(pos + 1);
}

static bool GetNameAndDescription(const std::wstring& regId, std::wstring& name, std::wstring& description)
{
    std::wstring path = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + regId + L"\\Properties";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;

    wchar_t buf[512];
    DWORD size = sizeof(buf);
    DWORD type = 0;

    size = sizeof(buf);
    if (RegQueryValueExW(key, L"{a45c254e-df1c-4efd-8020-67d146a850e0},2", nullptr, &type, (BYTE*)buf, &size) == ERROR_SUCCESS)
        name.assign(buf);

    size = sizeof(buf);
    if (RegQueryValueExW(key, L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6", nullptr, &type, (BYTE*)buf, &size) == ERROR_SUCCESS)
        description.assign(buf);

    RegCloseKey(key);
    return true;
}

static IMMDevice* FindTargetEndpoint(IMMDeviceEnumerator* enumerator)
{
    IMMDeviceCollection* collection = nullptr;
    enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (!collection) return nullptr;

    UINT count = 0;
    collection->GetCount(&count);

    IMMDevice* result = nullptr;
    for (UINT i = 0; i < count && !result; i++)
    {
        IMMDevice* dev = nullptr;
        collection->Item(i, &dev);
        if (!dev) continue;

        LPWSTR pId = nullptr;
        dev->GetId(&pId);
        std::wstring id(pId ? pId : L"");
        if (pId) CoTaskMemFree(pId);

        std::wstring regId = StripEndpointPrefix(id);
        std::wstring name, description;
        GetNameAndDescription(regId, name, description);

        if (_wcsicmp(name.c_str(), L"Speakers") == 0 &&
            description.find(L"Tablet Sample") != std::wstring::npos)
        {
            Log(L"matched endpoint: " + name + L" (" + id + L")");
            result = dev;
        }
        else
        {
            dev->Release();
        }
    }

    collection->Release();
    return result;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir(exePath);
    exeDir = exeDir.substr(0, exeDir.find_last_of(L'\\'));
    std::wstring repoRoot = exeDir.substr(0, exeDir.find_last_of(L'\\'));
    g_kefctlPath = repoRoot + L"\\kefctl";

    wchar_t* localAppData = nullptr;
    size_t len = 0;
    _wdupenv_s(&localAppData, &len, L"LOCALAPPDATA");
    std::wstring logDir = std::wstring(localAppData ? localAppData : L"") + L"\\kefctl";
    if (localAppData) free(localAppData);
    CreateDirectoryW(logDir.c_str(), nullptr);
    g_logFile = logDir + L"\\endpoint-bridge.log";

    Log(L"KefEndpointBridge started (C++, polling mode)");

    IMMDeviceEnumerator* enumerator = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&enumerator);

    IAudioEndpointVolume* epVolume = nullptr;
    bool haveLast = false;
    bool lastMuted = false;
    int lastPercent = -1;

    while (true)
    {
        if (!epVolume && enumerator)
        {
            IMMDevice* target = FindTargetEndpoint(enumerator);
            if (target)
            {
                target->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&epVolume);
                target->Release();
                haveLast = false;
                if (epVolume) Log(L"activated endpoint volume interface");
            }
            else
            {
                Log(L"target endpoint not found, will retry");
            }
        }
        else if (epVolume)
        {
            BOOL muteRaw = FALSE;
            float volume = 0.0f;
            HRESULT hr1 = epVolume->GetMute(&muteRaw);
            HRESULT hr2 = epVolume->GetMasterVolumeLevelScalar(&volume);

            if (FAILED(hr1) || FAILED(hr2))
            {
                Log(L"poll error, will re-locate endpoint");
                epVolume->Release();
                epVolume = nullptr;
            }
            else
            {
                bool muted = muteRaw != 0;
                int percent = (int)std::lround(volume * 100.0f);

                if (!haveLast)
                {
                    haveLast = true;
                    lastMuted = muted;
                    lastPercent = percent;
                    Log(L"initial state: muted=" + std::to_wstring(muted) + L" volume=" + std::to_wstring(percent) + L"%");
                }
                else
                {
                    if (muted != lastMuted)
                    {
                        lastMuted = muted;
                        if (muted)
                        {
                            Log(L"muted -> powering off");
                            RunKefctl(L"--off");
                        }
                        else
                        {
                            Log(L"unmuted -> powering on (aux)");
                            RunKefctl(L"-i aux");
                        }
                    }

                    if (percent != lastPercent)
                    {
                        lastPercent = percent;
                        Log(L"volume changed -> " + std::to_wstring(percent) + L"%");
                        RunKefctl(L"-v " + std::to_wstring(percent));
                    }
                }
            }
        }

        Sleep(1000);
    }

    return 0;
}
