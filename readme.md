# Slack IOC Defanger (Windows)

A lightweight, background Win32 utility that intercepts clipboard pastes (`Ctrl + V`) into Slack to automatically defang Indicators of Compromise (IOCs). 

Built to prevent accidental unfurling or clicking of live malware domains and IP addresses in team channels, this tool sits quietly in the system tray and rewrites URLs on the fly before they hit Slack's message queue.

## Features

* **Targeted Interception:** Uses a low-level keyboard hook (`WH_KEYBOARD_LL`) to monitor for `Ctrl + V`. It only manipulates the clipboard if the active foreground window is `slack.exe`.
* **Smart Defanging:** Converts protocols (`http/https` -> `hxxp/hxxps`) and brackets periods in domains/IPs (`.` -> `[.]`).
* **File Extension Ignore List:** Uses regex negative lookaheads to ensure standard file names (e.g., `malware.exe`, `payload.dll`) are not accidentally mangled into `malware[.]exe`.

The regex targets three specific structures:
1. **Explicit URLs:** Anything starting with `http://`, `https://`, or `www.`.
2. **IPv4 Addresses:** Standard 4-octet boundaries.
3. **Bare Domains:** `word.word` structures, specifically ignoring a predefined list of file extensions (e.g., `.exe`, `.dll`, `.bin`, `.zip`, `.py`).

## How It Works

When `Ctrl + V` is pressed while Slack is focused, the app temporarily halts the keystroke event, reads the `CF_UNICODETEXT` from the Windows Clipboard, applies a C++ regex transformation to defang any matched IOCs, writes the safe string back to the clipboard, and then releases the keystroke to let the paste execute naturally.

## Getting Started

### Prerequisites
* Windows 10/11
* Visual Studio (with C++ Desktop Development workload installed)

### Usage
Run the compiled executable. You will not see a window open; instead, a generic application icon will appear in your System Tray (notification area). 

* **To use:** Simply copy an IOC (e.g., `http://evil.com`) and paste it into Slack. It will automatically paste as `hxxp://evil[.]com`.
* **To exit:** Right-click the icon in the System Tray and select **Exit Defanger**.

## Configuration

If you need to add more file extensions to the ignore list, locate the regex pattern in `defangURLs()` within the source code and append your extension to the negative lookahead group:

```cpp
// Add new extensions here separated by a pipe (|)
(?!(?:exe|dll|bin|sys|elf|sh|bat|txt|log|csv|zip|rar|tar|gz|7z|pdf|docx?|xlsx?|py|js|ps1|apk|msi)\b)
```
