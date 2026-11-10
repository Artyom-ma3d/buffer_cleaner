#include <windows.h>
#include <shellapi.h>
#include "resource.h"

#define ID_TRAY_ICON 1001
#define ID_TRAY_PAUSE 1002
#define ID_TRAY_AUTOSTART 1003
#define ID_TRAY_EXIT 1004
#define WM_TRAYICON (WM_USER + 1)

#define WM_ASYNC_CLEAR (WM_USER + 2)
#define TIMER_BLINK 2
#define TIMER_SELF_DESTRUCT 3 

// Настройка времени самоликвидации: 1 минута (1 * 60 * 1000 миллисекунд)
#define SELF_DESTRUCT_TIMEOUT (1 * 60 * 1000) 

// Идентификатор глобальной горячей клавиши
#define HOTKEY_PAUSE_ID 1

const wchar_t* APP_REG_NAME = L"BufferCleanerApp";
const wchar_t* MUTEX_NAME = L"Global\\BufferCleanerUniqueMutexName_12345";

NOTIFYICONDATAW nid;
bool isPaused = false;

HICON hIconActive = NULL;
HICON hIconPaused = NULL;
HICON hIconFlash = NULL;
HANDLE hMutex = NULL;

HHOOK hKeyboardHook = NULL;
HHOOK hMouseHook = NULL;
HWND hMainHwnd = NULL;

bool isInternalChange = false; 
bool hasContentToClear = false;
bool wasRightLinkClicked = false; // Флаг: было ли вызвано кастомное меню через ПКМ

// Проверка системного языка пользователя
bool IsRussianSystem() {
    return (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN);
}

bool IsAutoStartEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD type = 0;
        DWORD size = 0;
        LONG result = RegQueryValueExW(hKey, APP_REG_NAME, NULL, &type, NULL, &size);
        RegCloseKey(hKey);
        return (result == ERROR_SUCCESS);
    }
    return false;
}

void SetAutoStart(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            wchar_t quotedPath[MAX_PATH + 2];
            wsprintfW(quotedPath, L"\"%s\"", exePath);
            RegSetValueExW(hKey, APP_REG_NAME, 0, REG_SZ, (BYTE*)quotedPath, (wcslen(quotedPath) + 1) * sizeof(wchar_t));
        } else {
            RegDeleteValueW(hKey, APP_REG_NAME);
        }
        RegCloseKey(hKey);
    }
}

void UpdateTrayIcon(HICON hIcon, const wchar_t* tooltip) {
    nid.hIcon = hIcon;
    wcscpy_s(nid.szTip, tooltip);
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void ShowNotification(const wchar_t* title, const wchar_t* message) {
    NOTIFYICONDATAW nidNotify = nid;
    nidNotify.uFlags = NIF_INFO;
    nidNotify.dwInfoFlags = NIIF_INFO;
    wcscpy_s(nidNotify.szInfoTitle, title);
    wcscpy_s(nidNotify.szInfo, message);
    Shell_NotifyIconW(NIM_MODIFY, &nidNotify);
}

void ClearClipboard() {
    isInternalChange = true; 
    if (OpenClipboard(NULL)) {
        EmptyClipboard();
        CloseClipboard();
    }
    isInternalChange = false;
}

void TriggerFlashEffect() {
    UpdateTrayIcon(hIconFlash, IsRussianSystem() ? L"Буфер очищен!" : L"Clipboard cleared!");
    SetTimer(hMainHwnd, TIMER_BLINK, 400, NULL);
}

DWORD WINAPI ClearBufferAsyncThread(LPVOID lpParam) {
    Sleep(300); // Ваша комфортная задержка 300мс
    PostMessageW(hMainHwnd, WM_ASYNC_CLEAR, 0, 0);
    return 0;
}

void StartAsyncClear() {
    hasContentToClear = false;
    wasRightLinkClicked = false;
    KillTimer(hMainHwnd, TIMER_SELF_DESTRUCT); 
    CreateThread(NULL, 0, ClearBufferAsyncThread, NULL, 0, NULL);
}

void TogglePause(HWND hwnd) {
    isPaused = !isPaused;
    if (isPaused) {
        KillTimer(hwnd, TIMER_SELF_DESTRUCT);
        hasContentToClear = false;
        wasRightLinkClicked = false;
        UpdateTrayIcon(hIconPaused, IsRussianSystem() ? L"Автоочистка: ПАУЗА" : L"Auto-clear: PAUSED");
    } else {
        UpdateTrayIcon(hIconActive, IsRussianSystem() ? L"Автоочистка буфера активна" : L"Clipboard auto-clear active");
    }
}

// 1. ХУК КЛАВИАТУРЫ: Перехват Ctrl+V и Shift+Insert
LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && !isPaused && hasContentToClear) {
        KBDLLHOOKSTRUCT* kbd = (KBDLLHOOKSTRUCT*)lParam;
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            bool ctrlPressed = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shiftPressed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

            if ((ctrlPressed && kbd->vkCode == 'V') || (shiftPressed && kbd->vkCode == VK_INSERT)) {
                StartAsyncClear();
            }
        }
    }
    return CallNextHookEx(hKeyboardHook, nCode, wParam, lParam);
}

// 2. ХУК МЫШИ: Гибридный перехват (Геометрия для Блокнота + Последовательность для VS Code/Браузеров)
LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && !isPaused && hasContentToClear) {
        
        // Фиксируем клик ПКМ (пользователь открыл контекстное меню в любой программе)
        if (wParam == WM_RBUTTONUP) {
            wasRightLinkClicked = true;
        }
        
        // Ловим нажатие ЛКМ (пользователь выбирает пункт меню)
        else if (wParam == WM_LBUTTONDOWN) {
            // Сценарий А: Стандартные окна Windows (Блокнот). Проверяем геометрию меню
            HWND menuHwnd = FindWindowW(L"#32768", NULL);
            if (menuHwnd != NULL) {
                MSLLHOOKSTRUCT* mouse = (MSLLHOOKSTRUCT*)lParam;
                RECT rect;
                if (GetWindowRect(menuHwnd, &rect)) {
                    if (mouse->pt.x >= rect.left && mouse->pt.x <= rect.right &&
                        mouse->pt.y >= rect.top && mouse->pt.y <= rect.bottom) {
                        StartAsyncClear();
                        return CallNextHookEx(hMouseHook, nCode, wParam, lParam);
                    }
                }
            }
            
            // Сценарий Б: Кастомные меню Chromium/Electron (VS Code, Браузеры, Discord).
            // Если перед кликом ЛКМ было зафиксировано нажатие ПКМ — значит, идет клик по кастомному меню вставки
            if (wasRightLinkClicked) {
                StartAsyncClear();
            }
        }
    }
    return CallNextHookEx(hMouseHook, nCode, wParam, lParam);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE:
            AddClipboardFormatListener(hwnd);
            RegisterHotKey(hwnd, HOTKEY_PAUSE_ID, MOD_CONTROL | MOD_ALT, 'P');
            break;

        case WM_CLIPBOARDUPDATE:
            if (isPaused || isInternalChange) break;

            if (OpenClipboard(hwnd)) {
                UINT format = EnumClipboardFormats(0);
                CloseClipboard();
                
                if (format != 0) {
                    hasContentToClear = true; 
                    wasRightLinkClicked = false; // Сбрасываем флаг для новых данных
                    SetTimer(hwnd, TIMER_SELF_DESTRUCT, SELF_DESTRUCT_TIMEOUT, NULL);
                }
            }
            break;

        case WM_HOTKEY:
            if (wParam == HOTKEY_PAUSE_ID) {
                TogglePause(hwnd);
            }
            break;

        case WM_ASYNC_CLEAR:
            ClearClipboard();
            TriggerFlashEffect();
            break;

        case WM_TIMER:
            if (wParam == TIMER_SELF_DESTRUCT) {
                KillTimer(hwnd, TIMER_SELF_DESTRUCT);
                if (hasContentToClear && !isPaused) {
                    ClearClipboard();
                    TriggerFlashEffect();
                    
                    if (IsRussianSystem()) {
                        ShowNotification(L"Безопасность буфера", L"Данные автоматически удалены по истечении времени.");
                    } else {
                        ShowNotification(L"Clipboard Security", L"Data has been automatically cleared due to timeout.");
                    }
                }
            }
            else if (wParam == TIMER_BLINK) {
                KillTimer(hwnd, TIMER_BLINK);
                if (isPaused) {
                    UpdateTrayIcon(hIconPaused, IsRussianSystem() ? L"Автоочистка: ПАУЗА" : L"Auto-clear: PAUSED");
                } else {
                    UpdateTrayIcon(hIconActive, IsRussianSystem() ? L"Автоочистка буфера активна" : L"Clipboard auto-clear active");
                }
            }
            break;

        case WM_TRAYICON:
            if (lParam == WM_LBUTTONUP) {
                TogglePause(hwnd);
            }
            else if (lParam == WM_RBUTTONUP) {
                POINT curPoint;
                GetCursorPos(&curPoint);

                HMENU hMenu = CreatePopupMenu();
                UINT pauseFlags = MF_STRING | (isPaused ? MF_CHECKED : MF_UNCHECKED);
                
                if (IsRussianSystem()) {
                    AppendMenuW(hMenu, pauseFlags, ID_TRAY_PAUSE, L"Пауза (Ctrl+Alt+P)");
                    AppendMenuW(hMenu, MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_AUTOSTART, L"Запускать вместе с Windows");
                    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Выход");
                } else {
                    AppendMenuW(hMenu, pauseFlags, ID_TRAY_PAUSE, L"Pause (Ctrl+Alt+P)");
                    AppendMenuW(hMenu, MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_AUTOSTART, L"Start with Windows");
                    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
                    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");
                }

                SetForegroundWindow(hwnd);
                TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, curPoint.x, curPoint.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);
            }
            break;

        case WM_COMMAND:
            if (LOWORD(wParam) == ID_TRAY_PAUSE) {
                TogglePause(hwnd);
            } 
			else if (LOWORD(wParam) == ID_TRAY_AUTOSTART) {
				SetAutoStart(!IsAutoStartEnabled());
}
			else if (LOWORD(wParam) == ID_TRAY_EXIT) {
				SendMessage(hwnd, WM_CLOSE, 0, 0);
}
			break;
		case WM_CLOSE:
				UnregisterHotKey(hwnd, HOTKEY_PAUSE_ID);
			if (hKeyboardHook) UnhookWindowsHookEx(hKeyboardHook);
			if (hMouseHook) UnhookWindowsHookEx(hMouseHook);
				RemoveClipboardFormatListener(hwnd);
				KillTimer(hwnd, TIMER_SELF_DESTRUCT);
				KillTimer(hwnd, TIMER_BLINK);
				Shell_NotifyIconW(NIM_DELETE, &nid);
				PostQuitMessage(0);
		break;
		case WM_DESTROY:
				PostQuitMessage(0);
		break;
		default:
		return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}
return 0;
}
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
	hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
		if (hMutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
		if (hMutex) CloseHandle(hMutex);
return 0;
}
	hIconActive = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON_ACTIVE));
	hIconPaused = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON_PAUSED));
	hIconFlash  = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON_FLASH));
// Квадратные скобки на месте! Строка класса создается корректно
const wchar_t CLASS_NAME[] = L"ClipboardCleanerTrayClass";
	WNDCLASSW wc = {};
		wc.lpfnWndProc = WindowProc;
		wc.hInstance = hInstance;
		wc.lpszClassName = CLASS_NAME;
				RegisterClassW(&wc);
	hMainHwnd = CreateWindowExW(0, CLASS_NAME, L"Buffer Cleaner", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
				ZeroMemory(&nid, sizeof(nid));
		nid.cbSize = sizeof(NOTIFYICONDATAW);
		nid.hWnd = hMainHwnd;
		nid.uID = ID_TRAY_ICON;
		nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
		nid.uCallbackMessage = WM_TRAYICON;
		nid.hIcon = hIconActive;
				wcscpy_s(nid.szTip, IsRussianSystem() ? L"Автоочистка буфера активна" : L"Clipboard auto-clear active");
				Shell_NotifyIconW(NIM_ADD, &nid);
	hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, hInstance, 0);
	hMouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, hInstance, 0);
	MSG msg = {};
		while (GetMessage(&msg, NULL, 0, 0)) {
				TranslateMessage(&msg);
				DispatchMessage(&msg);
}
				ReleaseMutex(hMutex);
				CloseHandle(hMutex);
return 0;
}
