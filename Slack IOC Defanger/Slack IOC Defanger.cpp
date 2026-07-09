#include <windows.h>
#include <psapi.h>
#include <string>
#include <regex>
#include <vector>
#include <thread>
#include <chrono>
#include "resource.h"

#pragma comment(lib, "psapi.lib")

// --- Constants for the Tray Icon ---
#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 1001

// Global variables for hook and tray
HHOOK hKeyboardHook = nullptr;
NOTIFYICONDATAW nid = {};

// --- Defanging Logic ---
std::wstring defangURLs(const std::wstring& text) {
    std::wregex urlRegex(
        LR"((?:https?://|www\.)[^\s]+|\b(?:\d{1,3}\.){3}\d{1,3}\b|\b[a-zA-Z0-9.-]+\.(?!(?:exe|dll|bin|sys|elf|sh|bat|txt|log|csv|zip|rar|tar|gz|7z|pdf|docx?|xlsx?|py|js|ps1|apk|msi)\b)[a-zA-Z]{2,15}\b(?:/[^\s]*)?)",
        std::regex_constants::icase
    );

    std::wstring output = text;
    std::vector<std::pair<size_t, size_t>> matches;

    for (std::wsregex_iterator it(text.begin(), text.end(), urlRegex), end; it != end; ++it) {
        matches.push_back({ it->position(), it->length() });
    }

    for (auto it = matches.rbegin(); it != matches.rend(); ++it) {
        std::wstring matchStr = output.substr(it->first, it->second);

        std::wregex httpRegex(LR"(http)", std::regex_constants::icase);
        matchStr = std::regex_replace(matchStr, httpRegex, L"hxxp");

        std::wregex dotRegex(LR"(\.)");
        matchStr = std::regex_replace(matchStr, dotRegex, L"[.]");

        output.replace(it->first, it->second, matchStr);
    }

    return output;
}

// --- Active Window Checking ---
bool isSlackForeground() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return false;

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return false;

    wchar_t processPath[MAX_PATH];
    DWORD size = MAX_PATH;
    bool isSlack = false;

    if (QueryFullProcessImageNameW(hProcess, 0, processPath, &size)) {
        std::wstring path(processPath);
        std::wstring exeName = L"slack.exe";

        if (path.length() >= exeName.length()) {
            std::wstring suffix = path.substr(path.length() - exeName.length());
            if (_wcsicmp(suffix.c_str(), exeName.c_str()) == 0) {
                isSlack = true;
            }
        }
    }
    CloseHandle(hProcess);
    return isSlack;
}

// --- Clipboard Manipulation ---
bool defangClipboard() {
    if (!OpenClipboard(nullptr)) return false;

    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (!hData) {
        CloseClipboard();
        return false;
    }

    wchar_t* pszText = static_cast<wchar_t*>(GlobalLock(hData));
    if (!pszText) {
        CloseClipboard();
        return false;
    }

    std::wstring clipboardText(pszText);
    GlobalUnlock(hData);

    std::wstring defangedText = defangURLs(clipboardText);

    if (defangedText != clipboardText) {
        EmptyClipboard();

        size_t bufferSize = (defangedText.length() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bufferSize);
        if (hMem) {
            memcpy(GlobalLock(hMem), defangedText.c_str(), bufferSize);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
        CloseClipboard();
        return true;
    }

    CloseClipboard();
    return false;
}

// --- Global Hook Callback ---
LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            KBDLLHOOKSTRUCT* pkbhs = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

            if (pkbhs->vkCode == 0x56) { // 'V' key
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
                    if (isSlackForeground()) {
                        if (defangClipboard()) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        }
                    }
                }
            }
        }
    }
    return CallNextHookEx(hKeyboardHook, nCode, wParam, lParam);
}

// --- Hidden Window Procedure for Tray Messages ---
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_TRAYICON:
        // Respond to right-click on the tray icon
        if (LOWORD(lParam) == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);

            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit Defanger");

            // SetForegroundWindow is a known requirement in Win32 to ensure 
            // the popup menu closes if you click outside of it.
            SetForegroundWindow(hwnd);

            TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
        }
        break;

    case WM_COMMAND:
        // Handle the "Exit" click from the menu
        if (LOWORD(wParam) == ID_TRAY_EXIT) {
            PostQuitMessage(0);
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}

// --- Application Entry Point ---
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"SlackDefangerMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    // 1. Register the Hidden Window Class
    const wchar_t CLASS_NAME[] = L"DefangerHiddenWindowClass";

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;

    RegisterClassW(&wc);

    // 2. Create the Hidden Window
    HWND hwnd = CreateWindowExW(
        0, CLASS_NAME, L"Slack Defanger", 0,
        0, 0, 0, 0,
        NULL, NULL, hInstance, NULL
    );

    if (hwnd == NULL) {
        return 0;
    }

    // 3. Setup the System Tray Icon
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    // Load a default Windows icon (shield/application) since we don't have a custom .ico file linked
    nid.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
    wcscpy_s(nid.szTip, L"Slack Defanger (Active)");

    Shell_NotifyIconW(NIM_ADD, &nid);

    // 4. Install the keyboard hook
    hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, hInstance, 0);

    // 5. Run the Message Loop
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 6. Cleanup on Exit
    Shell_NotifyIconW(NIM_DELETE, &nid);
    UnhookWindowsHookEx(hKeyboardHook);

    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    return 0;
}