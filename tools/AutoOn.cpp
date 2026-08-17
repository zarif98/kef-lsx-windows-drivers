// Watches the default Windows audio output for activity and wakes the KEF
// speakers (power on + aux input) whenever playback starts after a period of
// silence. Runs forever, invisibly. Launched at logon via a shortcut in the
// Startup folder.
//
// Build (from a "x64 Native Tools Command Prompt for VS 2022"):
//   cl /EHsc /O2 /DUNICODE /D_UNICODE /Fe:AutoOn.exe AutoOn.cpp ole32.lib

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <string>
#include <fstream>
#include <ctime>
#include <cctype>

#pragma comment(lib, "ole32.lib")

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

static std::string RunKefctlCapture(const std::wstring& args)
{
    std::wstring cmdline = L"perl \"" + g_kefctlPath + L"\" " + args;

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return "";
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;

    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmdline;

    std::string output;
    if (CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(writePipe);
        writePipe = nullptr;
        char readBuf[4096];
        DWORD bytesRead = 0;
        while (ReadFile(readPipe, readBuf, sizeof(readBuf), &bytesRead, nullptr) && bytesRead > 0)
            output.append(readBuf, bytesRead);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (writePipe) CloseHandle(writePipe);
    CloseHandle(readPipe);
    return output;
}

static float GetPeak()
{
    IMMDeviceEnumerator* enumerator = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (!enumerator) return 0.0f;

    IMMDevice* device = nullptr;
    enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (!device) return 0.0f;

    IAudioMeterInformation* meter = nullptr;
    device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, (void**)&meter);
    device->Release();
    if (!meter) return 0.0f;

    float peak = 0.0f;
    meter->GetPeakValue(&peak);
    meter->Release();
    return peak;
}

static bool IsPowerOff(const std::string& status)
{
    auto pos = status.find("Power:");
    if (pos == std::string::npos) return false;
    pos += 6;
    while (pos < status.size() && isspace((unsigned char)status[pos])) pos++;
    return status.compare(pos, 3, "Off") == 0;
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
    g_logFile = logDir + L"\\auto-on.log";

    Log(L"auto-on (C++) watcher started");

    const float threshold = 0.02f;
    const int requiredHits = 2;
    int hits = 0;
    bool wasQuiet = true;

    while (true)
    {
        float peak = GetPeak();

        if (peak > threshold) hits++;
        else { hits = 0; wasQuiet = true; }

        if (wasQuiet && hits >= requiredHits)
        {
            wasQuiet = false;
            std::string status = RunKefctlCapture(L"--status");
            if (IsPowerOff(status))
            {
                Log(L"audio detected, speakers off -> powering on (aux)");
                RunKefctlCapture(L"-i aux");
            }
        }

        Sleep(5000);
    }

    return 0;
}
