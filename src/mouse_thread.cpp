#include <windows.h>
#include <stdio.h>
#include "common.h"

static HANDLE mouseThread = NULL;
static DWORD mouseThreadId = 0;
static HANDLE stopEvent = NULL;
static bool configuredButtonDown = false;
extern HHOOK mouseHook;
extern int hotkeyToggle;
extern UINT hotkeyModifiers;
extern Ihandle *filterButton;

#ifndef XBUTTON1
#define XBUTTON1 0x0001
#endif

#ifndef XBUTTON2
#define XBUTTON2 0x0002
#endif

static LRESULT CALLBACK ThreadedMouseHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0)
    {
        MSLLHOOKSTRUCT *pMouseStruct = (MSLLHOOKSTRUCT *)lParam;

        if (wParam == WM_XBUTTONDOWN)
        {
            WORD button = HIWORD(pMouseStruct->mouseData);

            LOG("Mouse button pressed: %d, looking for: %d", button, hotkeyToggle);

            if ((hotkeyToggle == VK_XBUTTON1 && button == XBUTTON1) ||
                (hotkeyToggle == VK_XBUTTON2 && button == XBUTTON2))
            {
                const bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                const bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                const bool winDown = ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) ||
                                     ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);
                const bool modifiersMatch =
                    (((hotkeyModifiers & MOD_CONTROL) != 0) == ctrlDown) &&
                    (((hotkeyModifiers & MOD_ALT) != 0) == altDown) &&
                    (((hotkeyModifiers & MOD_SHIFT) != 0) == shiftDown) &&
                    (((hotkeyModifiers & MOD_WIN) != 0) == winDown);
                if (!modifiersMatch)
                    return CallNextHookEx(NULL, nCode, wParam, lParam);
                if (configuredButtonDown)
                    return CallNextHookEx(NULL, nCode, wParam, lParam);
                configuredButtonDown = true;
                LOG("Triggering hotkey for button %d", button);

                HWND hWnd = getMainWindowHandle();
                if (hWnd && IsWindow(hWnd))
                {
                    PostMessage(hWnd, WM_USER + 1, 0, 0);
                }
                else
                {
                    LOG("Main window not ready yet; skipping mouse hotkey toggle");
                }
            }
        }
        else if (wParam == WM_XBUTTONUP)
        {
            WORD button = HIWORD(pMouseStruct->mouseData);
            if ((hotkeyToggle == VK_XBUTTON1 && button == XBUTTON1) ||
                (hotkeyToggle == VK_XBUTTON2 && button == XBUTTON2))
            {
                configuredButtonDown = false;
            }
            LOG("Mouse button released");
        }
    }

    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

static DWORD WINAPI MouseHookThread(LPVOID lpParam)
{
    UNREFERENCED_PARAMETER(lpParam);
    LOG("Mouse hook thread starting...");

    HINSTANCE hInstance = GetModuleHandle(NULL);
    if (!hInstance)
    {
        LOG("Failed to get module handle");
        return 1;
    }

    mouseHook = SetWindowsHookEx(WH_MOUSE_LL, ThreadedMouseHookProc, hInstance, 0);
    if (!mouseHook)
    {
        DWORD error = GetLastError();
        LOG("Failed to set mouse hook, error: %lu", error);
        return 1;
    }

    LOG("Low-level mouse hook set successfully");

    MSG msg;
    HANDLE waitHandles[1] = {stopEvent};
    for (;;)
    {
        DWORD result = MsgWaitForMultipleObjects(1, waitHandles, FALSE, INFINITE, QS_ALLINPUT);

        if (result == WAIT_OBJECT_0)
        {
            break;
        }
        if (result == WAIT_FAILED)
        {
            LOG("Mouse hook wait failed: %lu", GetLastError());
            break;
        }

        if (result == WAIT_OBJECT_0 + 1)
        {
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
    }

    if (mouseHook)
    {
        UnhookWindowsHookEx(mouseHook);
        mouseHook = NULL;
    }

    LOG("Mouse hook thread ending");
    return 0;
}

BOOL StartMouseHookThread()
{
    if (mouseThread)
    {
        LOG("Mouse hook thread already running");
        return TRUE;
    }

    stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!stopEvent)
    {
        LOG("Failed to create stop event");
        return FALSE;
    }

    configuredButtonDown = false;

    LOG("Creating mouse hook thread for hotkey toggle: %d", hotkeyToggle);
    mouseThread = CreateThread(NULL, 0, MouseHookThread, NULL, 0, &mouseThreadId);
    if (mouseThread)
    {
        LOG("Mouse hook thread created with ID: %lu", mouseThreadId);
        return TRUE;
    }
    else
    {
        DWORD error = GetLastError();
        LOG("Failed to create mouse hook thread, error: %lu", error);
        CloseHandle(stopEvent);
        stopEvent = NULL;
    }

    return FALSE;
}

void StopMouseHookThread()
{
    if (!mouseThread)
    {
        LOG("No mouse hook thread to stop");
        return;
    }

    LOG("Stopping mouse hook thread...");

    if (stopEvent)
        SetEvent(stopEvent);

    DWORD result = WaitForSingleObject(mouseThread, INFINITE);

    if (result == WAIT_OBJECT_0)
    {
        LOG("Mouse hook thread stopped gracefully");
    }
    else
    {
        LOG("Unexpected wait result: %lu", result);
    }

    CloseHandle(mouseThread);
    mouseThread = NULL;
    mouseThreadId = 0;
    configuredButtonDown = false;

    if (stopEvent)
    {
        CloseHandle(stopEvent);
        stopEvent = NULL;
    }

    LOG("Mouse hook thread cleanup complete");
}
