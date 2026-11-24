#include "storage.hpp"
#include "ui_window.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

#include <filesystem>
#include <vector>

namespace {

void enablePerMonitorDpiAwareness() noexcept {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
    auto setCtx = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
        reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
    if (setCtx) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = (DPI_AWARENESS_CONTEXT)-4
        if (setCtx(reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-4)))) {
            return;
        }
    }
    SetProcessDPIAware();
}

std::filesystem::path executableDirectory() {
    std::vector<wchar_t> buf(MAX_PATH + 1, L'\0');
    for (;;) {
        DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return std::filesystem::current_path();
        }
        if (len < buf.size() - 1) {
            return std::filesystem::path(std::wstring_view(buf.data(), len)).parent_path();
        }
        buf.resize(buf.size() * 2, L'\0');
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    // 保证全局单实例运行，避免多个进程并发接管同一份 todo.md
    HANDLE singleInstanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\TodoFloatSingleInstanceMutex_v1");
    if (singleInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(singleInstanceMutex);
        return 0;
    }

    enablePerMonitorDpiAwareness();
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int exitCode = 0;
    {
        todofloat::TodoStorage storage(executableDirectory());
        todofloat::FloatingTodoWindow window(storage);
        if (!window.create(hInstance)) {
            exitCode = 1;
        } else {
            MSG msg{};
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            exitCode = static_cast<int>(msg.wParam);
        }
    }

    if (SUCCEEDED(comHr)) {
        CoUninitialize();
    }
    if (singleInstanceMutex) {
        CloseHandle(singleInstanceMutex);
    }
    return exitCode;
}
