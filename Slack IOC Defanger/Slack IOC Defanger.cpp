#include <windows.h>
#include <psapi.h>
#include <string>
#include <regex>
#include <vector>
#include <thread>
#include <chrono>
#include <algorithm>
#include <set>
#include <cwctype>
#include "resource.h"

#pragma comment(lib, "psapi.lib")

// --- Constants for the Tray Icon, Menu, and Settings UI ---
#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_EXIT 1001
#define ID_TRAY_TOGGLE 1002
#define ID_TRAY_SETTINGS 1003

#define IDC_SETTINGS_DEFANG_IP 2001
#define IDC_SETTINGS_TLD_LIST 2002
#define IDC_SETTINGS_TLD_INPUT 2003
#define IDC_SETTINGS_ADD_TLD 2004
#define IDC_SETTINGS_REMOVE_TLD 2005
#define IDC_SETTINGS_SAVE 2006
#define IDC_SETTINGS_CANCEL 2007

struct DefangConfig {
    bool isEnabled = true;
    bool defangIPs = true;
    std::vector<std::wstring> tlds;
};

HHOOK hKeyboardHook = nullptr;
NOTIFYICONDATAW nid = {};
DefangConfig g_config = {};
HWND g_settingsWindow = nullptr;

static const wchar_t* kRegPath = L"Software\\SlackIOCDefanger";
static const wchar_t* kValueDefangEnabled = L"DefangEnabled";
static const wchar_t* kValueDefangIPs = L"DefangIPs";
static const wchar_t* kValueTldList = L"TldList";

std::vector<std::wstring> GetDefaultTlds()
{
    return { L"com", L"net", L"org", L"io", L"co", L"gov", L"edu", L"mil", L"app", L"dev" };
}

std::wstring NormalizeTld(const std::wstring& value)
{
    std::wstring normalized;
    for (wchar_t ch : value) {
        if (ch == L'.' || iswspace(ch)) {
            continue;
        }
        normalized.push_back(static_cast<wchar_t>(towlower(ch)));
    }
    return normalized;
}

std::wstring JoinTlds(const std::vector<std::wstring>& tlds)
{
    std::wstring joined;
    for (size_t i = 0; i < tlds.size(); ++i) {
        if (i > 0) {
            joined += L";";
        }
        joined += tlds[i];
    }
    return joined;
}

std::vector<std::wstring> ParseTlds(const std::wstring& raw)
{
    std::vector<std::wstring> parsed;
    std::set<std::wstring> seen;
    size_t start = 0;

    while (start <= raw.size()) {
        size_t end = raw.find(L';', start);
        std::wstring token = raw.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        std::wstring normalized = NormalizeTld(token);

        if (!normalized.empty() && seen.insert(normalized).second) {
            parsed.push_back(normalized);
        }

        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
    }

    return parsed;
}

bool LoadDwordValue(const wchar_t* valueName, DWORD defaultValue)
{
    DWORD data = defaultValue;
    DWORD size = sizeof(data);
    DWORD type = REG_DWORD;

    if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, valueName,
        RRF_RT_REG_DWORD, &type, &data, &size) == ERROR_SUCCESS)
    {
        return data != 0;
    }

    return defaultValue != 0;
}

std::wstring LoadStringValue(const wchar_t* valueName)
{
    DWORD type = REG_SZ;
    DWORD bytes = 0;

    if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, valueName, RRF_RT_REG_SZ, &type, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0) {
        return L"";
    }

    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, valueName, RRF_RT_REG_SZ, &type, buffer.data(), &bytes) != ERROR_SUCCESS) {
        return L"";
    }

    return std::wstring(buffer.data());
}

void SaveDwordValue(HKEY hKey, const wchar_t* valueName, bool enabled)
{
    DWORD data = enabled ? 1u : 0u;
    RegSetValueExW(hKey, valueName, 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&data), sizeof(data));
}

void SaveStringValue(HKEY hKey, const wchar_t* valueName, const std::wstring& value)
{
    RegSetValueExW(hKey, valueName, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

void LoadConfig()
{
    g_config.isEnabled = LoadDwordValue(kValueDefangEnabled, 1);
    g_config.defangIPs = LoadDwordValue(kValueDefangIPs, 1);

    std::wstring rawTldList = LoadStringValue(kValueTldList);
    g_config.tlds = ParseTlds(rawTldList);
    if (g_config.tlds.empty()) {
        g_config.tlds = GetDefaultTlds();
    }
}

void SaveConfig()
{
    HKEY hKey{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
        KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
    {
        return;
    }

    SaveDwordValue(hKey, kValueDefangEnabled, g_config.isEnabled);
    SaveDwordValue(hKey, kValueDefangIPs, g_config.defangIPs);
    SaveStringValue(hKey, kValueTldList, JoinTlds(g_config.tlds));

    RegCloseKey(hKey);
}

std::wstring EscapeRegexLiteral(const std::wstring& input)
{
	std::wstring escaped;
	for (wchar_t ch : input) {
		switch (ch) {
		case L'\\':
		case L'^':
		case L'$':
		case L'.':
		case L'|':
		case L'?':
		case L'*':
		case L'+':
		case L'(':
		case L')':
		case L'[':
		case L']':
		case L'{':
		case L'}':
			escaped.push_back(L'\\');
			escaped.push_back(ch);
			break;
		default:
			escaped.push_back(ch);
			break;
		}
	}
	return escaped;
}

std::wstring BuildIndicatorPattern()
{
	std::vector<std::wstring> tlds = g_config.tlds;
	if (tlds.empty()) {
		tlds = GetDefaultTlds();
	}

	std::set<std::wstring> unique;
	std::vector<std::wstring> normalizedTlds;
	for (const auto& tld : tlds) {
		std::wstring normalized = NormalizeTld(tld);
		if (!normalized.empty() && unique.insert(normalized).second) {
			normalizedTlds.push_back(normalized);
		}
	}

	if (normalizedTlds.empty()) {
		normalizedTlds = GetDefaultTlds();
	}

	std::wstring tldAlternation;
	for (size_t i = 0; i < normalizedTlds.size(); ++i) {
		if (i > 0) {
			tldAlternation += L"|";
		}
		tldAlternation += EscapeRegexLiteral(normalizedTlds[i]);
	}

	std::wstring domainPattern =
		L"\\b(?:(?:https?://)?(?:www\\.)?)?[a-zA-Z0-9-]+(?:\\.[a-zA-Z0-9-]+)*\\.(?:" +
		tldAlternation +
		L")\\b(?:/[^\\s]*)?";

	if (!g_config.defangIPs) {
		return domainPattern;
	}

	std::wstring ipPattern = L"\\b(?:https?://)?(?:\\d{1,3}\\.){3}\\d{1,3}\\b(?:/[^\\s]*)?";
	return domainPattern + L"|" + ipPattern;
}

std::wstring defangURLs(const std::wstring& text)
{
	std::wregex indicatorRegex(BuildIndicatorPattern(), std::regex_constants::icase);

	std::wstring output = text;
	std::vector<std::pair<size_t, size_t>> matches;

	for (std::wsregex_iterator it(text.begin(), text.end(), indicatorRegex), end; it != end; ++it) {
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

void UpdateTrayTooltip()
{
	if (g_config.isEnabled) {
		wcscpy_s(nid.szTip, L"Slack Defanger (Active)");
	}
	else {
		wcscpy_s(nid.szTip, L"Slack Defanger (Paused)");
	}

	if (nid.hWnd != nullptr) {
		Shell_NotifyIconW(NIM_MODIFY, &nid);
	}
}

bool IsValidTld(const std::wstring& tld)
{
	if (tld.empty()) {
		return false;
	}

	for (wchar_t ch : tld) {
		if (!(iswalnum(ch) || ch == L'-')) {
			return false;
		}
	}

	return true;
}

void PopulateTldList(HWND listBox)
{
	SendMessageW(listBox, LB_RESETCONTENT, 0, 0);
	for (const auto& tld : g_config.tlds) {
		SendMessageW(listBox, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tld.c_str()));
	}
}

bool ListContainsTld(HWND listBox, const std::wstring& tld)
{
	int count = static_cast<int>(SendMessageW(listBox, LB_GETCOUNT, 0, 0));
	wchar_t buffer[128] = {};

	for (int i = 0; i < count; ++i) {
		SendMessageW(listBox, LB_GETTEXT, i, reinterpret_cast<LPARAM>(buffer));
		if (_wcsicmp(buffer, tld.c_str()) == 0) {
			return true;
		}
	}

	return false;
}

std::vector<std::wstring> ReadTldList(HWND listBox)
{
	std::vector<std::wstring> tlds;
	int count = static_cast<int>(SendMessageW(listBox, LB_GETCOUNT, 0, 0));

	for (int i = 0; i < count; ++i) {
		wchar_t buffer[128] = {};
		SendMessageW(listBox, LB_GETTEXT, i, reinterpret_cast<LPARAM>(buffer));
		std::wstring normalized = NormalizeTld(buffer);
		if (!normalized.empty()) {
			tlds.push_back(normalized);
		}
	}

	return tlds;
}

LRESULT CALLBACK SettingsWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg) {
	case WM_CREATE:
	{
		CreateWindowW(L"BUTTON", L"Defang IP addresses",
			WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
			12, 12, 180, 22, hwnd, reinterpret_cast<HMENU>(IDC_SETTINGS_DEFANG_IP), nullptr, nullptr);

		CreateWindowW(L"STATIC", L"TLDs/extensions to defang:",
			WS_VISIBLE | WS_CHILD,
			12, 42, 220, 20, hwnd, nullptr, nullptr, nullptr);

		HWND listBox = CreateWindowW(L"LISTBOX", nullptr,
			WS_VISIBLE | WS_CHILD | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
			12, 62, 260, 140, hwnd, reinterpret_cast<HMENU>(IDC_SETTINGS_TLD_LIST), nullptr, nullptr);

		CreateWindowW(L"EDIT", nullptr,
			WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
			12, 210, 170, 24, hwnd, reinterpret_cast<HMENU>(IDC_SETTINGS_TLD_INPUT), nullptr, nullptr);

		CreateWindowW(L"BUTTON", L"Add",
			WS_VISIBLE | WS_CHILD,
			188, 210, 84, 24, hwnd, reinterpret_cast<HMENU>(IDC_SETTINGS_ADD_TLD), nullptr, nullptr);

		CreateWindowW(L"BUTTON", L"Remove Selected",
			WS_VISIBLE | WS_CHILD,
			12, 240, 125, 24, hwnd, reinterpret_cast<HMENU>(IDC_SETTINGS_REMOVE_TLD), nullptr, nullptr);

		CreateWindowW(L"BUTTON", L"Save",
			WS_VISIBLE | WS_CHILD,
			162, 240, 52, 24, hwnd, reinterpret_cast<HMENU>(IDC_SETTINGS_SAVE), nullptr, nullptr);

		CreateWindowW(L"BUTTON", L"Cancel",
			WS_VISIBLE | WS_CHILD,
			220, 240, 52, 24, hwnd, reinterpret_cast<HMENU>(IDC_SETTINGS_CANCEL), nullptr, nullptr);

		SendMessageW(GetDlgItem(hwnd, IDC_SETTINGS_DEFANG_IP), BM_SETCHECK,
			g_config.defangIPs ? BST_CHECKED : BST_UNCHECKED, 0);

		PopulateTldList(listBox);
		break;
	}

	case WM_COMMAND:
	{
		const int commandId = LOWORD(wParam);

		if (commandId == IDC_SETTINGS_ADD_TLD) {
			wchar_t input[128] = {};
			GetWindowTextW(GetDlgItem(hwnd, IDC_SETTINGS_TLD_INPUT), input, 128);

			std::wstring normalized = NormalizeTld(input);
			if (!IsValidTld(normalized)) {
				MessageBoxW(hwnd, L"Enter a valid TLD/extension (letters, numbers, hyphen).", L"Invalid Entry", MB_OK | MB_ICONWARNING);
				return 0;
			}

			HWND listBox = GetDlgItem(hwnd, IDC_SETTINGS_TLD_LIST);
			if (!ListContainsTld(listBox, normalized)) {
				SendMessageW(listBox, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(normalized.c_str()));
			}

			SetWindowTextW(GetDlgItem(hwnd, IDC_SETTINGS_TLD_INPUT), L"");
			return 0;
		}

		if (commandId == IDC_SETTINGS_REMOVE_TLD) {
			HWND listBox = GetDlgItem(hwnd, IDC_SETTINGS_TLD_LIST);
			int selected = static_cast<int>(SendMessageW(listBox, LB_GETCURSEL, 0, 0));
			if (selected != LB_ERR) {
				SendMessageW(listBox, LB_DELETESTRING, static_cast<WPARAM>(selected), 0);
			}
			return 0;
		}

		if (commandId == IDC_SETTINGS_SAVE) {
			std::vector<std::wstring> tlds = ReadTldList(GetDlgItem(hwnd, IDC_SETTINGS_TLD_LIST));
			if (tlds.empty()) {
				MessageBoxW(hwnd, L"Add at least one TLD/extension.", L"Settings", MB_OK | MB_ICONWARNING);
				return 0;
			}

			g_config.defangIPs = (SendMessageW(GetDlgItem(hwnd, IDC_SETTINGS_DEFANG_IP), BM_GETCHECK, 0, 0) == BST_CHECKED);
			g_config.tlds = tlds;
			SaveConfig();
			DestroyWindow(hwnd);
			return 0;
		}

		if (commandId == IDC_SETTINGS_CANCEL) {
			DestroyWindow(hwnd);
			return 0;
		}

		break;
	}

	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;

	case WM_DESTROY:
		g_settingsWindow = nullptr;
		return 0;
	}

	return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

void ShowSettingsWindow(HWND owner, HINSTANCE hInstance)
{
	if (g_settingsWindow != nullptr) {
		ShowWindow(g_settingsWindow, SW_SHOWNORMAL);
		SetForegroundWindow(g_settingsWindow);
		return;
	}

	const wchar_t* settingsClassName = L"SlackDefangerSettingsWindowClass";
	static bool classRegistered = false;
	if (!classRegistered) {
		WNDCLASSW wc = {};
		wc.lpfnWndProc = SettingsWindowProc;
		wc.hInstance = hInstance;
		wc.lpszClassName = settingsClassName;
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
		RegisterClassW(&wc);
		classRegistered = true;
	}

	g_settingsWindow = CreateWindowExW(
		WS_EX_DLGMODALFRAME,
		settingsClassName,
		L"Slack Defanger Settings",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT, 300, 320,
		owner,
		nullptr,
		hInstance,
		nullptr);

	if (g_settingsWindow != nullptr) {
		ShowWindow(g_settingsWindow, SW_SHOW);
		SetForegroundWindow(g_settingsWindow);
	}
}

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

LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
	if (!g_config.isEnabled) {
		return CallNextHookEx(hKeyboardHook, nCode, wParam, lParam);
	}

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

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	switch (uMsg) {
	case WM_TRAYICON:
		if (LOWORD(lParam) == WM_RBUTTONUP) {
			POINT pt;
			GetCursorPos(&pt);

			HMENU hMenu = CreatePopupMenu();

			UINT checkFlag = g_config.isEnabled ? MF_CHECKED : MF_UNCHECKED;
			AppendMenuW(hMenu, MF_STRING | checkFlag, ID_TRAY_TOGGLE, L"Defang Slack Pastes");
			AppendMenuW(hMenu, MF_STRING, ID_TRAY_SETTINGS, L"Settings...");
			AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
			AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");

			SetForegroundWindow(hwnd);
			TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
			DestroyMenu(hMenu);
		}
		break;

	case WM_COMMAND:
		if (LOWORD(wParam) == ID_TRAY_TOGGLE) {
			g_config.isEnabled = !g_config.isEnabled;
			UpdateTrayTooltip();
			SaveConfig();
		}
		else if (LOWORD(wParam) == ID_TRAY_SETTINGS) {
			HINSTANCE hInstance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
			ShowSettingsWindow(hwnd, hInstance);
		}
		else if (LOWORD(wParam) == ID_TRAY_EXIT) {
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

	const wchar_t CLASS_NAME[] = L"DefangerHiddenWindowClass";

	LoadConfig();

	WNDCLASSW wc = {};
	wc.lpfnWndProc = WindowProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = CLASS_NAME;

	RegisterClassW(&wc);

	HWND hwnd = CreateWindowExW(
		0, CLASS_NAME, L"Slack Defanger", 0,
		0, 0, 0, 0,
		NULL, NULL, hInstance, NULL
	);

	if (hwnd == NULL) {
		return 0;
	}

	nid.cbSize = sizeof(NOTIFYICONDATAW);
	nid.hWnd = hwnd;
	nid.uID = 1;
	nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	nid.uCallbackMessage = WM_TRAYICON;

	nid.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));

	UpdateTrayTooltip();
	Shell_NotifyIconW(NIM_ADD, &nid);

	hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, hInstance, 0);

	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	Shell_NotifyIconW(NIM_DELETE, &nid);
	UnhookWindowsHookEx(hKeyboardHook);

	if (hMutex) {
		ReleaseMutex(hMutex);
		CloseHandle(hMutex);
	}

	return 0;
}
