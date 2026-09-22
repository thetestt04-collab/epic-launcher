#include <atomic>
#include <array>
#include <memory>
#include <string>
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include "common.h"
#include "config.h"
#include "update.h"
#include "version.h"
#include "iup.h"
#include <mmsystem.h>
#include <process.h>
#include <shellapi.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern float lagGetBufferFill();
extern int lagGetFlushCount();
extern int lagGetLastFlushSize();
extern DWORD lagGetLastFlushTime();
extern void lagResetFlushStats();

#ifdef _MSC_VER
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winmm.lib")
#endif

Module *modules[MODULE_CNT] = {&lagModule};

volatile short sendState = SEND_STATUS_NONE;

static Ihandle *dialog, *topFrame, *bottomFrame, *statusLabel;
static Ihandle *filterText, *filterSelectList;
Ihandle *filterButton;
static Ihandle *stateIcon, *timer, *timeout = NULL;
static Ihandle *pingLabel, *hotkeyLabel, *bufferPercentLabel;
static Ihandle *hotkeyChangeButton = NULL;
static bool capturingHotkey = false;
static bool captureEligible[256] = {false};
static Ihandle *updateButton = NULL, *updateLabel = NULL;
static bool updateCheckInFlight = false;
#define UPDATE_WM_RESULT (WM_USER + 3)
static Ihandle *pingTimer = NULL, *hotkeyTimer = NULL, *wndProcTimer = NULL;
static HWND mainWindowHandle = NULL;
static WNDPROC previousDialogWndProc = NULL;
static bool shutdownComplete = false;
HANDLE instanceMutex = NULL;

static BOOL pingEnabled = FALSE, hotkeyRegistered = FALSE;
volatile BOOL hotkeyPressed = FALSE;
static int hotkeyId = 0;
int hotkeyToggle = 0;
UINT hotkeyModifiers = 0;
static char hotkeyDisplayName[48] = "NONE";
HHOOK mouseHook = NULL;
static char pingTargetOutbound[256] = {0};
static bool filterTransitionInProgress = false;
static bool filterRunning = false;

#define WINDOW_CORNER_RADIUS 20

#define PING_SAMPLE_WINDOW 8
#define PING_FAIL_HOLD_MS 15000
#define PING_FAIL_RESET_STREAK 6
#define PING_MAX_SAMPLE_MS 5000

namespace
{
constexpr int WIFI_ICON_WIDTH = 16;
constexpr int WIFI_ICON_HEIGHT = 12;

using WifiRows = std::array<unsigned short, WIFI_ICON_HEIGHT>;
using WifiPixels = std::array<unsigned char, WIFI_ICON_WIDTH * WIFI_ICON_HEIGHT>;

constexpr WifiRows WIFI_FULL_ROWS = {0x07E0, 0x1818, 0x2004, 0x4002, 0x0000, 0x03C0,
                                     0x0C30, 0x1008, 0x0000, 0x0180, 0x0180, 0x0000};

constexpr WifiRows WIFI_MEDIUM_ROWS = {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x03C0,
                                       0x0C30, 0x1008, 0x0000, 0x0180, 0x0180, 0x0000};

constexpr WifiRows WIFI_LOW_ROWS = {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
                                    0x0000, 0x0000, 0x0420, 0x0240, 0x0180, 0x0000};

constexpr WifiPixels makeWifiPixels(const WifiRows &rows)
{
    WifiPixels pixels{};
    for (int row = 0; row < WIFI_ICON_HEIGHT; ++row)
    {
        for (int column = 0; column < WIFI_ICON_WIDTH; ++column)
        {
            const unsigned short mask =
                static_cast<unsigned short>(1u << (WIFI_ICON_WIDTH - column - 1));
            pixels[static_cast<size_t>(row * WIFI_ICON_WIDTH + column)] =
                (rows[static_cast<size_t>(row)] & mask) ? 1u : 0u;
        }
    }
    return pixels;
}

constexpr WifiPixels WIFI_FULL_PIXELS = makeWifiPixels(WIFI_FULL_ROWS);
constexpr WifiPixels WIFI_MEDIUM_PIXELS = makeWifiPixels(WIFI_MEDIUM_ROWS);
constexpr WifiPixels WIFI_LOW_PIXELS = makeWifiPixels(WIFI_LOW_ROWS);

using SymbolRows = std::array<unsigned short, 16>;
using SymbolPixels = std::array<unsigned char, 16 * 16>;

constexpr SymbolPixels makeSymbolPixels(const SymbolRows &rows)
{
    SymbolPixels pixels{};
    for (int row = 0; row < 16; ++row)
    {
        for (int column = 0; column < 16; ++column)
        {
            const unsigned short mask = static_cast<unsigned short>(1u << (15 - column));
            pixels[static_cast<size_t>(row * 16 + column)] =
                (rows[static_cast<size_t>(row)] & mask) ? 1u : 0u;
        }
    }
    return pixels;
}

constexpr SymbolRows PLAY_ROWS = {0x0000, 0x0000, 0x1000, 0x1800, 0x1E00, 0x1F80, 0x1FE0, 0x1FF0,
                                  0x1FF0, 0x1FE0, 0x1F80, 0x1E00, 0x1800, 0x1000, 0x0000, 0x0000};
constexpr SymbolRows PAUSE_ROWS = {0x0000, 0x0000, 0x0000, 0x0C30, 0x0C30, 0x0C30, 0x0C30, 0x0C30,
                                   0x0C30, 0x0C30, 0x0C30, 0x0C30, 0x0C30, 0x0000, 0x0000, 0x0000};
constexpr SymbolRows EXIT_ROWS = {0x0000, 0x0000, 0x3F80, 0x2080, 0x2080, 0x2090, 0x2088, 0x27FC,
                                  0x27FC, 0x2088, 0x2090, 0x2080, 0x3F80, 0x0000, 0x0000, 0x0000};

constexpr SymbolPixels PLAY_PIXELS = makeSymbolPixels(PLAY_ROWS);
constexpr SymbolPixels PAUSE_PIXELS = makeSymbolPixels(PAUSE_ROWS);
constexpr SymbolPixels EXIT_PIXELS = makeSymbolPixels(EXIT_ROWS);
} // namespace

static CRITICAL_SECTION pingCritSec;
static HANDLE pingThreadHandle = NULL;
static HANDLE pingTargetEvent = NULL;
static std::atomic_bool pingRunning{false};
static BOOL pingCritInitialized = FALSE;
static int pingSamples[PING_SAMPLE_WINDOW] = {0};
static int pingSampleCount = 0;
static int pingSampleNext = 0;
static int pingLastRawMs = -1;
static int pingBaseMs = -1;
static DWORD pingFailureStreak = 0;
static ULONGLONG pingLastSuccessTick = 0;

#define PING_OVERLAY_CLASS "EPIC_GAMES_PING_OVERLAY"
#define PING_OVERLAY_X 14
#define PING_OVERLAY_Y 14

static HWND pingOverlayWindow = NULL;
static HFONT pingOverlayFont = NULL;
static COLORREF pingOverlayTextColor = RGB(64, 255, 64);
static char pingOverlayText[64] = "--";
static int pingOverlayPosX = PING_OVERLAY_X;
static int pingOverlayPosY = PING_OVERLAY_Y;

void showStatus(const char *line);

void logMessage(const char *fmt, ...)
{
#ifdef _DEBUG
    char logBuf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(logBuf, sizeof(logBuf), fmt, args);
    va_end(args);
    OutputDebugStringA(logBuf);
#else
    (void)fmt;
#endif
}

static int uiOnDialogShow(Ihandle *ih, int state);
static int uiStopCb(Ihandle *ih);
static int uiStartCb(Ihandle *ih);
static int uiKillCb(Ihandle *ih);
static int uiTimerCb(Ihandle *ih);
static int uiTimeoutCb(Ihandle *ih);
static int uiCloseCb(Ihandle *ih);
static int uiListSelectCb(Ihandle *ih, char *text, int item, int state);
static int uiFilterTextCb(Ihandle *ih);
static int uiHotkeyTimerCb(Ihandle *ih);
static int uiInstallWndProcTimer(Ihandle *ih);
static int uiPingTimerCb(Ihandle *ih);
static int uiCheckUpdatesCb(Ihandle *ih);
static int uiChangeHotkeyCb(Ihandle *ih);
static void pollKeyboardHotkeyFallback();
static void pollHotkeyCapture();
static std::string configFilePath();
static void applyHotkeyBinding(const HotkeyBinding &binding);
static void updateSetupUI(Ihandle *parent);
static void updateStartCheck(bool automatic);
static void handleUpdateResult(WPARAM automatic, LPARAM payload);
static void uiSetupModule(Module *module, Ihandle *parent);
static void updatePing();
static void updateBufferBar();
static void updateMainWindowShape(HWND hWnd);
static BOOL resolvePingTargetAddress(const char *target, IPAddr *targetAddress);
static int getLagCompensationMs();
static int getDisplayedPingMs();
static void pingPushSampleLocked(int pingMs);
static int pingCalcRobustAverageLocked();
static void pingRecordSuccessLocked(int pingMs);
static void pingRecordFailureLocked();
static int performFilterToggle();
static void updatePingIcon();
static void updateFilterButtonAnimation(BOOL pulseOn);
static void createPingOverlay();
static void destroyPingOverlay();
static void updatePingOverlay();
static LRESULT CALLBACK PingOverlayWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
static void registerHotkey(HWND hWnd);
static void unregisterHotkey();
static void addTrayIcon();
static void removeTrayIcon();
static int toggleFiltering(Ihandle *ih);
static LRESULT CALLBACK CustomWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

BOOL StartMouseHookThread();
void StopMouseHookThread();

#define CONFIG_FILE "config.json"
static AppConfig appConfig = makeDefaultConfig();
static std::string configLoadMessage;
static UINT filterListCount = 0;
BOOL parameterized = 0;

static void setStoredAttributeIfChanged(Ihandle *ih, const char *name, const char *value)
{
    const char *current;

    if (!ih || !name || !value)
        return;

    current = IupGetAttribute(ih, name);
    if (current && strcmp(current, value) == 0)
        return;

    IupStoreAttribute(ih, name, value);
}

static void setImageIfChanged(Ihandle *ih, const char *imageName)
{
    const char *current;

    if (!ih || !imageName)
        return;

    current = IupGetAttribute(ih, "IMAGE");
    if (current && strcmp(current, imageName) == 0)
        return;

    IupSetAttribute(ih, "IMAGE", imageName);
}

static void styleFlatButton(Ihandle *button, const char *foreground)
{
    if (!button || !foreground)
        return;
    IupSetAttribute(button, "BGCOLOR", Theme::Button);
    IupSetAttribute(button, "FGCOLOR", foreground);
    IupSetAttribute(button, "FLAT", "YES");
    IupSetAttribute(button, "NOTHEME", "YES");
    IupSetAttribute(button, "BORDER", "NO");
    IupSetAttribute(button, "BORDERCOLOR", Theme::Border);
    IupSetAttribute(button, "HLCOLOR", Theme::ButtonActive);
    IupSetAttribute(button, "PSCOLOR", Theme::Surface);
    IupSetAttribute(button, "CANFOCUS", "NO");
}

HWND getMainWindowHandle(void) { return mainWindowHandle; }

static BOOL isInteractiveChildWindow(HWND rootWindow, HWND childWindow)
{
    char className[64];
    LONG_PTR buttonStyle;

    if (!childWindow || childWindow == rootWindow)
        return FALSE;

    if (GetClassNameA(childWindow, className, sizeof(className)) == 0)
        return FALSE;

    if (_stricmp(className, "Edit") == 0 || _stricmp(className, "ComboBox") == 0 ||
        _stricmp(className, "ComboLBox") == 0 || _stricmp(className, "ListBox") == 0 ||
        _stricmp(className, "ScrollBar") == 0)
    {
        return TRUE;
    }

    if (_stricmp(className, "Button") == 0)
    {
        buttonStyle = GetWindowLongPtr(childWindow, GWL_STYLE) & BS_TYPEMASK;
        return buttonStyle != BS_GROUPBOX;
    }

    return FALSE;
}

static void updateMainWindowShape(HWND hWnd)
{
    RECT windowRect;
    HRGN windowRegion;
    int width;
    int height;

    if (!hWnd || !GetWindowRect(hWnd, &windowRect))
        return;

    width = windowRect.right - windowRect.left;
    height = windowRect.bottom - windowRect.top;
    if (width <= 0 || height <= 0)
        return;

    windowRegion =
        CreateRoundRectRgn(0, 0, width + 1, height + 1, WINDOW_CORNER_RADIUS, WINDOW_CORNER_RADIUS);
    if (!windowRegion)
        return;

    if (!SetWindowRgn(hWnd, windowRegion, TRUE))
    {
        DeleteObject(windowRegion);
    }
}

static bool configureHotkey(const std::string &hotkey)
{
    HotkeyBinding binding;
    if (!parseHotkeyString(hotkey, binding))
        return false;
    hotkeyToggle = binding.vk;
    hotkeyModifiers = static_cast<UINT>(binding.modifiers);
    strncpy_s(hotkeyDisplayName, sizeof(hotkeyDisplayName), binding.canonical.c_str(), _TRUNCATE);
    return true;
}

static std::string configFilePath()
{
    char executablePath[MAX_PATH];
    const DWORD pathLength = GetModuleFileNameA(nullptr, executablePath, ARRAYSIZE(executablePath));
    if (pathLength == 0 || pathLength >= ARRAYSIZE(executablePath))
        return std::string();
    std::string configPath(executablePath, pathLength);
    const std::size_t separator = configPath.find_last_of("\\/");
    if (separator == std::string::npos)
        return CONFIG_FILE;
    return configPath.substr(0, separator + 1) + CONFIG_FILE;
}

static void applyPresetPingTarget(size_t presetIndex)
{
    const char *target = "";
    if (presetIndex < appConfig.filters.size() &&
        !appConfig.filters[presetIndex].pingTarget.empty())
    {
        target = appConfig.filters[presetIndex].pingTarget.c_str();
    }
    else if (!appConfig.pingTarget.empty())
    {
        target = appConfig.pingTarget.c_str();
    }
    if (pingCritInitialized)
    {
        EnterCriticalSection(&pingCritSec);
        strncpy_s(pingTargetOutbound, sizeof(pingTargetOutbound), target, _TRUNCATE);
        LeaveCriticalSection(&pingCritSec);
    }
    else
    {
        strncpy_s(pingTargetOutbound, sizeof(pingTargetOutbound), target, _TRUNCATE);
    }
    if (pingTargetEvent != NULL)
    {
        SetEvent(pingTargetEvent);
    }
}

void loadConfig()
{
    appConfig = makeDefaultConfig();
    configLoadMessage.clear();

    const std::string configPath = configFilePath();
    if (configPath.empty())
    {
        configLoadMessage = "Using defaults: executable path is unavailable.";
        configureHotkey(appConfig.hotkey);
        applyPresetPingTarget(0);
        return;
    }

    std::string parseError;
    if (!loadConfigJson(configPath, appConfig, parseError))
        configLoadMessage = "Using defaults: " + parseError;

    if (!configureHotkey(appConfig.hotkey))
    {
        configLoadMessage = "Using MOUSE5: unsupported hotkey '" + appConfig.hotkey + "'.";
        appConfig.hotkey = "MOUSE5";
        configureHotkey(appConfig.hotkey);
    }
    else
    {
        appConfig.hotkey = hotkeyDisplayName;
    }
    applyPresetPingTarget(0);
}

static void registerHotkey(HWND hWnd)
{
    if (hotkeyToggle == 0)
        return;

    if (hotkeyToggle == VK_XBUTTON1 || hotkeyToggle == VK_XBUTTON2)
    {
        hotkeyRegistered = StartMouseHookThread();
    }
    else
    {
        hotkeyId = 1;
        hotkeyRegistered = RegisterHotKey(hWnd, hotkeyId, hotkeyModifiers, hotkeyToggle);
    }
}

static void unregisterHotkey()
{
    if (!hotkeyRegistered)
        return;

    if (hotkeyId != 0)
    {
        HWND hWnd = (HWND)IupGetAttribute(dialog, "HWND");
        if (hWnd)
            UnregisterHotKey(hWnd, hotkeyId);
    }
    hotkeyRegistered = FALSE;
}

static int performFilterToggle()
{
    const bool shouldStart = !filterRunning;
    int ret;
    HWND buttonWindow;

    if (!filterButton || filterTransitionInProgress)
        return IUP_DEFAULT;

    filterTransitionInProgress = true;
    IupSetAttribute(filterButton, "ACTIVE", "NO");
    IupSetAttribute(filterButton, "BGCOLOR", Theme::ButtonTransition);
    showStatus(shouldStart ? "Starting filter..." : "Stopping filter...");

    buttonWindow = (HWND)IupGetAttribute(filterButton, "HWND");
    if (buttonWindow)
        RedrawWindow(buttonWindow, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);

    if (shouldStart)
    {
        ret = uiStartCb(filterButton);
    }
    else
    {
        ret = uiStopCb(filterButton);
    }

    filterTransitionInProgress = false;
    IupSetAttribute(filterButton, "ACTIVE", "YES");
    updatePing();
    return ret;
}

static int toggleFiltering(Ihandle *ih)
{
    if (IupGetInt(ih, "ACTIVE") == 0)
        return IUP_DEFAULT;

    return performFilterToggle();
}

[[nodiscard]] bool init(int argc, char *argv[])
{
    UINT ix;
    Ihandle *topVbox, *bottomVbox, *dialogVBox, *metricsHbox;
    Ihandle *noneIcon, *doingIcon, *errorIcon;
    Ihandle *pingGoodIcon, *pingWarnIcon, *pingBadIcon;
    Ihandle *resumeIcon, *pauseIcon, *exitIcon;
    Ihandle *presetsLabel, *filterExpressionLabel, *pingTextLabel, *bufferTextLabel,
        *hotkeyTextLabel;
    char *arg_value = NULL;
    WSADATA wsaData;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        fputs("Failed to initialize Winsock.\n", stderr);
        return false;
    }
    loadConfig();
    if (IupOpen(&argc, &argv) == IUP_ERROR)
    {
        fputs("Failed to initialize IUP.\n", stderr);
        WSACleanup();
        return false;
    }
    IupSetGlobal("DEFAULTFONT", "Segoe UI, 10");
    IupSetGlobal("DLGBGCOLOR", Theme::Background);
    IupSetGlobal("DLGFGCOLOR", Theme::Text);

    statusLabel = IupLabel("");
    IupSetAttribute(statusLabel, "EXPAND", "HORIZONTAL");
    IupSetAttribute(statusLabel, "FGCOLOR", Theme::MutedText);
    IupSetAttribute(statusLabel, "BGCOLOR", Theme::Background);
    IupSetAttribute(statusLabel, "FONT", "Segoe UI, 9");
    IupSetAttribute(statusLabel, "PADDING", "8x1");
    if (!configLoadMessage.empty())
        IupStoreAttribute(statusLabel, "TITLE", configLoadMessage.c_str());

    Ihandle *pingNoteLabel = IupLabel("Ping is approximate (ICMP).");
    IupSetAttribute(pingNoteLabel, "EXPAND", "HORIZONTAL");
    IupSetAttribute(pingNoteLabel, "ALIGNMENT", "ACENTER");
    IupSetAttribute(pingNoteLabel, "FGCOLOR", Theme::MutedText);
    IupSetAttribute(pingNoteLabel, "BGCOLOR", Theme::Background);
    IupSetAttribute(pingNoteLabel, "FONT", "Segoe UI, 8");
    IupSetAttribute(pingNoteLabel, "PADDING", "8x2");
    IupSetAttribute(pingNoteLabel, "TIP",
                    "Games use UDP; this measures ICMP replies, so routes may differ.");

    filterText = IupText(NULL);
    filterButton = IupButton(NULL, NULL);
    Ihandle *killButton = IupButton(NULL, NULL);
    styleFlatButton(filterButton, Theme::ButtonSymbol);
    styleFlatButton(killButton, Theme::ButtonSymbol);
    IupSetAttribute(filterButton, "TIP", "Resume filtering");
    IupSetAttribute(killButton, "TIP", "Exit application");
    IupSetAttribute(filterButton, "RASTERSIZE", "36x26");
    IupSetAttribute(killButton, "RASTERSIZE", "36x26");
    stateIcon = IupLabel(NULL);
    IupSetAttribute(stateIcon, "TIP",
                    "Ping quality: green is good, amber is elevated, red is poor or unavailable");
    IupSetAttribute(stateIcon, "RASTERSIZE", "18x16");
    filterSelectList = IupList(NULL);
    pingLabel = IupLabel("-- ms");
    IupSetAttribute(pingLabel, "RASTERSIZE", "66x20");
    IupSetAttribute(pingLabel, "FGCOLOR", Theme::Text);
    IupSetAttribute(pingLabel, "FONT", "Segoe UI Semibold, 10");
    hotkeyLabel = IupLabel(hotkeyDisplayName);
    IupSetAttribute(hotkeyLabel, "RASTERSIZE", "116x20");
    IupSetAttribute(hotkeyLabel, "ALIGNMENT", "ALEFT");
    IupSetAttribute(hotkeyLabel, "FONT", "Segoe UI Semibold, 9");
    IupSetAttribute(hotkeyLabel, "TIP", "Toggle hotkey — press Change to rebind");
    hotkeyChangeButton = IupButton("Change", NULL);
    IupSetAttribute(hotkeyChangeButton, "FGCOLOR", "0 0 0");
    IupSetAttribute(hotkeyChangeButton, "TIP", "Change the toggle hotkey");
    IupSetAttribute(hotkeyChangeButton, "RASTERSIZE", "58x20");
    IupSetCallback(hotkeyChangeButton, "ACTION", (Icallback)uiChangeHotkeyCb);
    bufferPercentLabel = IupLabel("0%");
    IupSetAttribute(bufferPercentLabel, "RASTERSIZE", "42x20");
    IupSetAttribute(bufferPercentLabel, "FGCOLOR", Theme::Text);
    IupSetAttribute(bufferPercentLabel, "FONT", "Segoe UI Semibold, 10");

    topFrame = IupFrame(
        topVbox = IupVbox(presetsLabel = IupLabel("Presets:"), filterSelectList,
                          filterExpressionLabel = IupLabel("Filter expression:"), filterText,
                          metricsHbox =
                              IupHbox(stateIcon, pingTextLabel = IupLabel("Ping"), pingLabel,
                                      bufferTextLabel = IupLabel("Buffer"), bufferPercentLabel,
                                      IupFill(), hotkeyTextLabel = IupLabel("Hotkey"), hotkeyLabel,
                                      hotkeyChangeButton, filterButton, killButton, NULL),
                          NULL));

    if (argc > 1)
    {
        if (!parseArgs(argc, argv))
        {
            fprintf(stderr, "invalid argument count. ensure you're using options as \"--drop on\"");
            IupClose();
            WSACleanup();
            return false;
        }
        parameterized = 1;
    }

    IupSetAttribute(topFrame, "TITLE", "Filtering");
    IupSetAttribute(topFrame, "EXPAND", "HORIZONTAL");
    IupSetAttribute(topFrame, "BGCOLOR", Theme::Panel);
    IupSetAttribute(topFrame, "FGCOLOR", Theme::Text);
    IupSetAttribute(topFrame, "NOTHEME", "YES");
    IupSetAttribute(topFrame, "FONT", "Segoe UI Semibold, 10");
    IupSetAttribute(topVbox, "BGCOLOR", Theme::Panel);
    IupSetAttribute(metricsHbox, "BGCOLOR", Theme::Panel);
    Ihandle *themedLabels[] = {
        presetsLabel,    filterExpressionLabel, stateIcon,       pingTextLabel, pingLabel,
        bufferTextLabel, bufferPercentLabel,    hotkeyTextLabel, hotkeyLabel};
    for (Ihandle *label : themedLabels)
    {
        IupSetAttribute(label, "BGCOLOR", Theme::Panel);
        IupSetAttribute(label, "FGCOLOR", Theme::Text);
    }
    IupSetAttribute(filterText, "EXPAND", "HORIZONTAL");
    IupSetAttribute(filterText, "BGCOLOR", Theme::Surface);
    IupSetAttribute(filterText, "FGCOLOR", Theme::Text);
    IupSetAttribute(filterText, "NOTHEME", "YES");
    IupSetAttribute(filterText, "PADDING", "5x4");
    IupSetAttribute(filterSelectList, "BGCOLOR", Theme::Surface);
    IupSetAttribute(filterSelectList, "FGCOLOR", Theme::Text);
    IupSetAttribute(filterSelectList, "NOTHEME", "YES");
    IupSetCallback(filterText, "VALUECHANGED_CB", (Icallback)uiFilterTextCb);
    IupSetAttribute(filterButton, "PADDING", "4x3");
    IupSetAttribute(killButton, "PADDING", "4x3");
    IupSetCallback(filterButton, "ACTION", (Icallback)toggleFiltering);

    IupSetCallback(killButton, "ACTION", (Icallback)uiKillCb);

    IupSetAttribute(topVbox, "NCMARGIN", "10x8");
    IupSetAttribute(topVbox, "NCGAP", "6");
    IupSetAttribute(metricsHbox, "ALIGNMENT", "ACENTER");
    IupSetAttribute(metricsHbox, "GAP", "7");
    IupSetAttribute(hotkeyTextLabel, "PADDING", "10x0");
    IupSetAttribute(hotkeyChangeButton, "PADDING", "4x0");

    IupSetAttribute(stateIcon, "IMAGE", "none_icon");
    IupSetAttribute(stateIcon, "PADDING", "4x");

    IupSetAttribute(filterSelectList, "VISIBLECOLUMNS", "24");
    IupSetAttribute(filterSelectList, "DROPDOWN", "YES");
    filterListCount = 0;
    for (ix = 0; ix < static_cast<UINT>(appConfig.filters.size()); ++ix)
    {
        char ixBuf[4];
        ++filterListCount;
        snprintf(ixBuf, sizeof(ixBuf), "%u", filterListCount);
        IupStoreAttribute(filterSelectList, ixBuf, appConfig.filters[ix].name.c_str());
    }

    if (filterListCount > 0)
    {
        IupSetAttribute(filterSelectList, "VALUE", "1");
        IupStoreAttribute(filterText, "VALUE", appConfig.filters.front().expression.c_str());
    }
    IupSetCallback(filterSelectList, "ACTION", (Icallback)uiListSelectCb);

    bottomFrame = IupFrame(bottomVbox = IupVbox(NULL));
    IupSetAttribute(bottomFrame, "TITLE", "Functions");
    IupSetAttribute(bottomFrame, "BGCOLOR", Theme::Panel);
    IupSetAttribute(bottomFrame, "FGCOLOR", Theme::Text);
    IupSetAttribute(bottomFrame, "NOTHEME", "YES");
    IupSetAttribute(bottomFrame, "FONT", "Segoe UI Semibold, 10");
    IupSetAttribute(bottomVbox, "BGCOLOR", Theme::Panel);
    IupSetAttribute(bottomVbox, "NCMARGIN", "10x8");
    IupSetAttribute(bottomVbox, "NCGAP", "6");

    noneIcon = IupImage(8, 8, icon8x8);
    doingIcon = IupImage(8, 8, icon8x8);
    errorIcon = IupImage(8, 8, icon8x8);
    pingGoodIcon = IupImage(WIFI_ICON_WIDTH, WIFI_ICON_HEIGHT, WIFI_FULL_PIXELS.data());
    pingWarnIcon = IupImage(WIFI_ICON_WIDTH, WIFI_ICON_HEIGHT, WIFI_MEDIUM_PIXELS.data());
    pingBadIcon = IupImage(WIFI_ICON_WIDTH, WIFI_ICON_HEIGHT, WIFI_LOW_PIXELS.data());
    resumeIcon = IupImage(16, 16, PLAY_PIXELS.data());
    pauseIcon = IupImage(16, 16, PAUSE_PIXELS.data());
    exitIcon = IupImage(16, 16, EXIT_PIXELS.data());
    IupSetAttribute(noneIcon, "0", "BGCOLOR");
    IupSetAttribute(noneIcon, "1", "80 80 80");
    IupSetAttribute(doingIcon, "0", "BGCOLOR");
    IupSetAttribute(doingIcon, "1", "0 200 0");
    IupSetAttribute(errorIcon, "0", "BGCOLOR");
    IupSetAttribute(errorIcon, "1", "190 64 64");
    IupSetAttribute(pingGoodIcon, "0", "BGCOLOR");
    IupSetAttribute(pingGoodIcon, "1", "45 168 78");
    IupSetAttribute(pingWarnIcon, "0", "BGCOLOR");
    IupSetAttribute(pingWarnIcon, "1", "224 157 42");
    IupSetAttribute(pingBadIcon, "0", "BGCOLOR");
    IupSetAttribute(pingBadIcon, "1", "205 67 67");
    IupSetAttribute(resumeIcon, "0", "BGCOLOR");
    IupSetAttribute(resumeIcon, "1", Theme::ResumeGreen);
    IupSetAttribute(pauseIcon, "0", "BGCOLOR");
    IupSetAttribute(pauseIcon, "1", Theme::PauseRed);
    IupSetAttribute(exitIcon, "0", "BGCOLOR");
    IupSetAttribute(exitIcon, "1", Theme::MutedText);
    IupSetHandle("none_icon", noneIcon);
    IupSetHandle("doing_icon", doingIcon);
    IupSetHandle("error_icon", errorIcon);
    IupSetHandle("ping_good_icon", pingGoodIcon);
    IupSetHandle("ping_warn_icon", pingWarnIcon);
    IupSetHandle("ping_bad_icon", pingBadIcon);
    IupSetHandle("resume_icon", resumeIcon);
    IupSetHandle("pause_icon", pauseIcon);
    IupSetHandle("exit_icon", exitIcon);
    IupSetAttribute(filterButton, "IMAGE", "resume_icon");
    IupSetAttribute(killButton, "IMAGE", "exit_icon");

    for (ix = 0; ix < MODULE_CNT; ++ix)
    {
        uiSetupModule(*(modules + ix), bottomVbox);
    }
    updateSetupUI(bottomVbox);

    Ihandle *footerBox = IupVbox(statusLabel, pingNoteLabel, NULL);
    IupSetAttribute(footerBox, "EXPAND", "HORIZONTAL");
    IupSetAttribute(footerBox, "BGCOLOR", Theme::Background);
    IupSetAttribute(footerBox, "GAP", "0");
    IupSetAttribute(footerBox, "NCGAP", "0");
    dialog = IupDialog(dialogVBox = IupVbox(topFrame, bottomFrame, footerBox, NULL));

    IupSetAttribute(dialog, "TITLE", "");
    IupSetAttribute(dialog, "RESIZE", "NO");
    IupSetAttribute(dialog, "BGCOLOR", Theme::Background);
    IupSetAttribute(dialogVBox, "BGCOLOR", Theme::Background);
    IupSetCallback(dialog, "CLOSE_CB", (Icallback)uiCloseCb);
    IupSetCallback(dialog, "SHOW_CB", (Icallback)uiOnDialogShow);

    IupSetAttribute(dialogVBox, "ALIGNMENT", "ACENTER");
    IupSetAttribute(dialogVBox, "NCMARGIN", "8x8");
    IupSetAttribute(dialogVBox, "NCGAP", "6");

    timer = IupTimer();
    IupSetAttribute(timer, "TIME", "100");
    IupSetCallback(timer, "ACTION_CB", uiTimerCb);

    arg_value = IupGetGlobal("timeout");
    if (arg_value != NULL)
    {
        char valueBuf[16];
        snprintf(valueBuf, sizeof(valueBuf), "%s000", arg_value);
        timeout = IupTimer();
        IupStoreAttribute(timeout, "TIME", valueBuf);
        IupSetCallback(timeout, "ACTION_CB", uiTimeoutCb);
        IupSetAttribute(timeout, "RUN", "YES");
    }

    pingTimer = IupTimer();
    IupSetAttribute(pingTimer, "TIME", "1000");
    IupSetCallback(pingTimer, "ACTION_CB", (Icallback)uiPingTimerCb);
    IupSetAttribute(pingTimer, "RUN", "YES");

    pingEnabled = FALSE;
    return true;
}

void startup()
{
    IupShowXY(dialog, IUP_CENTER, IUP_CENTER);
    IupMainLoop();
}

void cleanup()
{
    LOG("Final cleanup started");
    if (!shutdownComplete)
        uiKillCb(NULL);
    IupClose();
}

void showStatus(const char *line)
{
    if (!statusLabel)
        return;
    setStoredAttributeIfChanged(statusLabel, "TITLE", line);
}

static BOOL check32RunningOn64(HWND hWnd)
{
    BOOL is64ret;
    if (IsWow64Process(GetCurrentProcess(), &is64ret) && is64ret)
    {
        MessageBox(hWnd,
                   (LPCSTR) "You're running 32bit EpicGamesLauncher on 64bit Windows, which "
                            "wouldn't work.",
                   (LPCSTR) "Aborting", MB_OK);
        return TRUE;
    }
    return FALSE;
}

static BOOL checkIsRunning()
{
    instanceMutex = CreateMutexA(NULL, FALSE, "Epic_Games_Instance_Mutex");
    if (!instanceMutex)
        return FALSE;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

static int getLagCompensationMs()
{
    int delayMs;
    int compensationMs = 0;

    if (!filterRunning || !atomicLoad16(&lagEnabled))
        return 0;

    delayMs = atomicLoad16(&lagTime);
    if (delayMs < 0)
        delayMs = 0;

    if (atomicLoad16(&lagOutbound))
        compensationMs += delayMs;
    if (atomicLoad16(&lagInbound))
        compensationMs += delayMs;

    if (compensationMs > 30000)
        compensationMs = 30000;

    return compensationMs;
}

static int getDisplayedPingMs()
{
    int lagCompMs = getLagCompensationMs();
    int rawPingMs;
    int basePingMs;
    DWORD failureStreak;
    ULONGLONG lastSuccessTick;
    ULONGLONG nowTick;
    int displayedPingMs;

    if (!pingCritInitialized)
        return lagCompMs > 0 ? lagCompMs : -1;

    EnterCriticalSection(&pingCritSec);
    rawPingMs = pingLastRawMs;
    basePingMs = pingBaseMs;
    failureStreak = pingFailureStreak;
    lastSuccessTick = pingLastSuccessTick;
    LeaveCriticalSection(&pingCritSec);

    if (rawPingMs > 0)
    {
        basePingMs = rawPingMs;
    }
    else if (basePingMs < 0)
    {
        return lagCompMs > 0 ? lagCompMs : -1;
    }

    if (failureStreak > 0 && lastSuccessTick != 0)
    {
        nowTick = GetTickCount64();
        if ((nowTick - lastSuccessTick) > PING_FAIL_HOLD_MS)
            return lagCompMs > 0 ? lagCompMs : -1;
    }

    displayedPingMs = basePingMs + lagCompMs;
    return (displayedPingMs > 0) ? displayedPingMs : 1;
}

static void pingPushSampleLocked(int pingMs)
{
    pingSamples[pingSampleNext] = pingMs;
    pingSampleNext = (pingSampleNext + 1) % PING_SAMPLE_WINDOW;
    if (pingSampleCount < PING_SAMPLE_WINDOW)
        ++pingSampleCount;
}

static int pingCalcRobustAverageLocked()
{
    int sorted[PING_SAMPLE_WINDOW];
    int i;
    int j;
    int value;
    int sum = 0;
    int start = 0;
    int end = pingSampleCount;

    if (pingSampleCount <= 0)
        return -1;

    for (i = 0; i < pingSampleCount; ++i)
    {
        sorted[i] = pingSamples[i];
    }

    for (i = 1; i < pingSampleCount; ++i)
    {
        value = sorted[i];
        j = i - 1;
        while (j >= 0 && sorted[j] > value)
        {
            sorted[j + 1] = sorted[j];
            --j;
        }
        sorted[j + 1] = value;
    }

    if (pingSampleCount >= 5)
    {
        start = 1;
        end = pingSampleCount - 1;
    }

    for (i = start; i < end; ++i)
    {
        sum += sorted[i];
    }

    value = end - start;
    if (value <= 0)
        return -1;

    sum = (sum + (value / 2)) / value;
    return (sum > 0) ? sum : 1;
}

static void pingRecordSuccessLocked(int pingMs)
{
    int robustMs;

    if (pingMs <= 0)
        pingMs = 1;
    else if (pingMs > PING_MAX_SAMPLE_MS)
        pingMs = PING_MAX_SAMPLE_MS;

    pingPushSampleLocked(pingMs);
    robustMs = pingCalcRobustAverageLocked();
    if (robustMs <= 0)
        robustMs = pingMs;
    pingLastRawMs = pingMs;

    if (pingBaseMs < 0)
    {
        pingBaseMs = robustMs;
    }
    else
    {
        pingBaseMs = (pingBaseMs + robustMs + 1) / 2;
    }

    if (pingBaseMs <= 0)
        pingBaseMs = 1;

    pingFailureStreak = 0;
    pingLastSuccessTick = GetTickCount64();
}

static void pingRecordFailureLocked()
{
    ULONGLONG nowTick;

    ++pingFailureStreak;
    if (pingLastSuccessTick == 0)
        return;

    nowTick = GetTickCount64();
    if (pingFailureStreak >= PING_FAIL_RESET_STREAK &&
        (nowTick - pingLastSuccessTick) > PING_FAIL_HOLD_MS)
    {
        pingLastRawMs = -1;
        pingBaseMs = -1;
        pingSampleCount = 0;
        pingSampleNext = 0;
    }
}

static LRESULT CALLBACK PingOverlayWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);

    switch (message)
    {
        case WM_NCHITTEST:
            return HTCAPTION;
        case WM_MOVE:
            pingOverlayPosX = (int)(short)LOWORD(lParam);
            pingOverlayPosY = (int)(short)HIWORD(lParam);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);

            HBRUSH bgBrush = CreateSolidBrush(RGB(12, 12, 12));
            FillRect(hdc, &rc, bgBrush);
            DeleteObject(bgBrush);

            HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
            HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
            HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(borderPen);

            SetBkMode(hdc, TRANSPARENT);

            HFONT oldFont = NULL;
            if (pingOverlayFont)
            {
                oldFont = (HFONT)SelectObject(hdc, pingOverlayFont);
            }

            RECT textRect = rc;
            SetTextColor(hdc, RGB(0, 0, 0));
            RECT outline = textRect;
            outline.left -= 1;
            DrawTextA(hdc, pingOverlayText, -1, &outline,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            outline = textRect;
            outline.left += 1;
            DrawTextA(hdc, pingOverlayText, -1, &outline,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            outline = textRect;
            outline.top -= 1;
            DrawTextA(hdc, pingOverlayText, -1, &outline,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            outline = textRect;
            outline.top += 1;
            DrawTextA(hdc, pingOverlayText, -1, &outline,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SetTextColor(hdc, pingOverlayTextColor);
            DrawTextA(hdc, pingOverlayText, -1, &textRect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            if (oldFont)
            {
                SelectObject(hdc, oldFont);
            }

            EndPaint(hWnd, &ps);
            return 0;
        }
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
}

static void createPingOverlay()
{
    if (pingOverlayWindow)
        return;

    HINSTANCE hInstance = GetModuleHandle(NULL);

    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PingOverlayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = PING_OVERLAY_CLASS;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExA(&wc);

    pingOverlayWindow = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE, PING_OVERLAY_CLASS, "",
        WS_POPUP, pingOverlayPosX, pingOverlayPosY, 72, 20, NULL, NULL, hInstance, NULL);
    if (!pingOverlayWindow)
        return;

    SetLayeredWindowAttributes(pingOverlayWindow, RGB(1, 0, 1), 255, LWA_COLORKEY | LWA_ALPHA);

    LOGFONTA fontSpec;
    ZeroMemory(&fontSpec, sizeof(fontSpec));
    fontSpec.lfHeight = -18;
    fontSpec.lfWeight = FW_BOLD;
    fontSpec.lfOutPrecision = OUT_TT_PRECIS;
    fontSpec.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    fontSpec.lfQuality = CLEARTYPE_NATURAL_QUALITY;
    fontSpec.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    strcpy_s(fontSpec.lfFaceName, LF_FACESIZE, "Segoe UI");
    pingOverlayFont = CreateFontIndirectA(&fontSpec);

    ShowWindow(pingOverlayWindow, SW_SHOWNOACTIVATE);
    UpdateWindow(pingOverlayWindow);
}

static void destroyPingOverlay()
{
    if (pingOverlayWindow)
    {
        DestroyWindow(pingOverlayWindow);
        pingOverlayWindow = NULL;
    }
    if (pingOverlayFont)
    {
        DeleteObject(pingOverlayFont);
        pingOverlayFont = NULL;
    }
}

static void updatePingOverlay()
{
    if (!pingOverlayWindow)
        return;

    int pingMs = getDisplayedPingMs();
    int overlayWidth = 76;
    int overlayHeight = 26;

    if (pingMs >= 0)
    {
        snprintf(pingOverlayText, sizeof(pingOverlayText), "%d", pingMs);
        if (pingMs <= 180)
        {
            pingOverlayTextColor = RGB(64, 255, 64);
        }
        else if (pingMs <= 250)
        {
            pingOverlayTextColor = RGB(255, 255, 64);
        }
        else if (pingMs <= 500)
        {
            pingOverlayTextColor = RGB(255, 165, 0);
        }
        else
        {
            pingOverlayTextColor = RGB(255, 64, 64);
        }
    }
    else
    {
        snprintf(pingOverlayText, sizeof(pingOverlayText), "--");
        pingOverlayTextColor = RGB(255, 64, 64);
    }

    HDC hdc = GetDC(pingOverlayWindow);
    if (hdc)
    {
        HFONT oldFont = NULL;
        if (pingOverlayFont)
        {
            oldFont = (HFONT)SelectObject(hdc, pingOverlayFont);
        }

        SIZE textSize;
        if (GetTextExtentPoint32A(hdc, pingOverlayText, (int)strlen(pingOverlayText), &textSize))
        {
            overlayWidth = textSize.cx + 20;
            overlayHeight = textSize.cy + 10;
            if (overlayWidth < 44)
                overlayWidth = 44;
            if (overlayHeight < 24)
                overlayHeight = 24;
        }

        if (oldFont)
        {
            SelectObject(hdc, oldFont);
        }
        ReleaseDC(pingOverlayWindow, hdc);
    }

    SetWindowPos(pingOverlayWindow, HWND_TOPMOST, pingOverlayPosX, pingOverlayPosY, overlayWidth,
                 overlayHeight, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(pingOverlayWindow, NULL, TRUE);
}

static int uiOnDialogShow(Ihandle *ih, int state)
{
    HWND hWnd;
    BOOL exit;
    HICON icon;
    HINSTANCE hInstance;
    RECT windowRect;
    int windowWidth = 0;
    int windowHeight = 0;
    if (state != IUP_SHOW)
        return IUP_DEFAULT;
    hWnd = (HWND)IupGetAttribute(ih, "HWND");
    mainWindowHandle = hWnd;
    hInstance = GetModuleHandle(NULL);

    if (GetWindowRect(hWnd, &windowRect))
    {
        windowWidth = windowRect.right - windowRect.left;
        windowHeight = windowRect.bottom - windowRect.top;
    }
    LONG_PTR style = GetWindowLongPtr(hWnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    SetWindowLongPtr(hWnd, GWL_STYLE, style);

    LONG_PTR extendedStyle = GetWindowLongPtr(hWnd, GWL_EXSTYLE);
    extendedStyle &= ~WS_EX_APPWINDOW;
    extendedStyle |= WS_EX_TOOLWINDOW;
    SetWindowLongPtr(hWnd, GWL_EXSTYLE, extendedStyle);

    if (windowWidth > 0 && windowHeight > 0)
    {
        SetWindowPos(hWnd, NULL, 0, 0, windowWidth, windowHeight,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    else
    {
        SetWindowPos(hWnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    IupRefresh(ih);
    updateMainWindowShape(hWnd);
    InvalidateRect(hWnd, NULL, TRUE);

    icon = LoadIcon(hInstance, MAKEINTRESOURCE(1));
    SendMessage(hWnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
    SendMessage(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);

#ifdef _WIN32
    exit = check32RunningOn64(hWnd);
    if (exit)
    {
        return IUP_CLOSE;
    }
#endif

    exit = tryElevate(hWnd, parameterized);
    if (exit)
        return IUP_CLOSE;

    exit = checkIsRunning();
    if (exit)
    {
        MessageBox(hWnd, (LPCSTR) "There's already an instance of EpicGamesLauncher running.",
                   (LPCSTR) "Aborting", MB_OK);
        return IUP_CLOSE;
    }

    if (parameterized)
    {
        setFromParameter(filterText, "VALUE", "filter");
        LOG("is parameterized, start filtering upon execution.");
        uiStartCb(filterButton);
    }

    if (hotkeyTimer == NULL)
    {
        hotkeyTimer = IupTimer();
        IupSetAttribute(hotkeyTimer, "TIME", "200");
        IupSetCallback(hotkeyTimer, "ACTION_CB", (Icallback)uiHotkeyTimerCb);
        IupSetAttribute(hotkeyTimer, "RUN", "YES");
    }

    IupSetAttribute(timer, "RUN", "YES");
    pingEnabled = TRUE;

    if (wndProcTimer == NULL)
    {
        wndProcTimer = IupTimer();
        IupSetAttribute(wndProcTimer, "TIME", "100");
        IupSetCallback(wndProcTimer, "ACTION_CB", (Icallback)uiInstallWndProcTimer);
        IupSetAttribute(wndProcTimer, "RUN", "YES");
    }

    addTrayIcon();
    createPingOverlay();
    updatePingOverlay();

    if (!appConfig.updateCheckUrl.empty())
        updateStartCheck(true);

    return IUP_DEFAULT;
}

static NOTIFYICONDATA nid{};
static BOOL trayIconAdded = FALSE;

void addTrayIcon()
{
    HWND hWnd;

    if (trayIconAdded)
        return;

    hWnd = mainWindowHandle ? mainWindowHandle : (HWND)IupGetAttribute(dialog, "HWND");
    if (!hWnd)
        return;

    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_USER + 2;
    nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(1));
    strcpy_s(nid.szTip, sizeof(nid.szTip), APP_NAME);

    Shell_NotifyIcon(NIM_ADD, &nid);
    trayIconAdded = TRUE;
}

void removeTrayIcon()
{
    if (!trayIconAdded)
        return;
    Shell_NotifyIcon(NIM_DELETE, &nid);
    trayIconAdded = FALSE;
}

static int uiKillCb(Ihandle *ih)
{
    UNREFERENCED_PARAMETER(ih);
    if (shutdownComplete)
        return IUP_CLOSE;
    shutdownComplete = true;

    LOG("Graceful shutdown initiated");

    if (filterRunning)
    {
        uiStopCb(filterButton);
        Sleep(100);
    }

    if (pingRunning.load(std::memory_order_acquire) || pingThreadHandle)
    {
        LOG("Stopping ping thread...");
        pingRunning.store(false, std::memory_order_release);
        if (pingTargetEvent != NULL)
        {
            SetEvent(pingTargetEvent);
        }

        if (pingThreadHandle)
        {
            WaitForSingleObject(pingThreadHandle, INFINITE);
            LOG("Ping thread stopped gracefully");
            CloseHandle(pingThreadHandle);
            pingThreadHandle = NULL;
        }
        LOG("Ping thread stopped");
    }

    if (pingCritInitialized)
    {
        DeleteCriticalSection(&pingCritSec);
        pingCritInitialized = FALSE;
    }
    if (pingTargetEvent != NULL)
    {
        CloseHandle(pingTargetEvent);
        pingTargetEvent = NULL;
    }
    LOG("Ping cleanup completed");

    StopMouseHookThread();
    unregisterHotkey();

    if (mainWindowHandle && previousDialogWndProc)
    {
        SetWindowLongPtr(mainWindowHandle, GWLP_WNDPROC, (LONG_PTR)previousDialogWndProc);
        previousDialogWndProc = NULL;
    }
    mainWindowHandle = NULL;

    if (timer)
    {
        IupSetAttribute(timer, "RUN", "NO");
        IupDestroy(timer);
        timer = NULL;
    }
    if (timeout)
    {
        IupSetAttribute(timeout, "RUN", "NO");
        IupDestroy(timeout);
        timeout = NULL;
    }
    if (pingTimer)
    {
        IupSetAttribute(pingTimer, "RUN", "NO");
        IupDestroy(pingTimer);
        pingTimer = NULL;
    }
    if (hotkeyTimer)
    {
        IupSetAttribute(hotkeyTimer, "RUN", "NO");
        IupDestroy(hotkeyTimer);
        hotkeyTimer = NULL;
    }
    if (wndProcTimer)
    {
        IupSetAttribute(wndProcTimer, "RUN", "NO");
        IupDestroy(wndProcTimer);
        wndProcTimer = NULL;
    }

    divertStop();
    Sleep(200);
    cleanupPacketPool();
    endTimePeriod();
    WSACleanup();
    destroyPingOverlay();
    removeTrayIcon();

    if (instanceMutex)
    {
        ReleaseMutex(instanceMutex);
        CloseHandle(instanceMutex);
        instanceMutex = NULL;
    }

    IupExitLoop();
    return IUP_CLOSE;
}

static int uiStartCb(Ihandle *ih)
{
    char buf[MSG_BUFSIZE];
    char runtimeFilter[FILTER_BUFSIZE + 64];
    const char *baseFilter;
    int written;
    UNREFERENCED_PARAMETER(ih);

    baseFilter = IupGetAttribute(filterText, "VALUE");
    if (!baseFilter || baseFilter[0] == '\0')
    {
        showStatus("Filter cannot be empty.");
        return IUP_DEFAULT;
    }

    written =
        snprintf(runtimeFilter, sizeof(runtimeFilter), "(%s) and !icmp and !icmpv6", baseFilter);
    if (written <= 0 || (size_t)written >= sizeof(runtimeFilter))
    {
        showStatus("Filter is too long.");
        return IUP_DEFAULT;
    }

    if (divertStart(runtimeFilter, buf) == 0)
    {
        showStatus(buf);
        return IUP_DEFAULT;
    }

    showStatus("Started filtering. Enable functionalities to take effect.");
    IupSetAttribute(filterText, "ACTIVE", "NO");
    filterRunning = true;
    IupSetAttribute(filterButton, "IMAGE", "pause_icon");
    IupSetAttribute(filterButton, "TIP", "Pause filtering");
    IupSetAttribute(timer, "RUN", "YES");
    pingEnabled = TRUE;
    updatePingOverlay();

    return IUP_DEFAULT;
}

static int uiStopCb(Ihandle *ih)
{
    int ix;
    UNREFERENCED_PARAMETER(ih);

    divertStop();

    IupSetAttribute(filterText, "ACTIVE", "YES");
    filterRunning = false;
    IupSetAttribute(filterButton, "IMAGE", "resume_icon");
    IupSetAttribute(filterButton, "TIP", "Resume filtering");

    for (ix = 0; ix < MODULE_CNT; ++ix)
    {
        atomicStore16(&modules[ix]->processTriggered, 0);
        setImageIfChanged(modules[ix]->iconHandle, "none_icon");
    }
    atomicStore16(&sendState, SEND_STATUS_NONE);
    pingEnabled = FALSE;
    updatePingOverlay();

    showStatus("Paused. Edit the filter if needed, then press Resume.");
    return IUP_DEFAULT;
}

static int uiToggleControls(Ihandle *ih, int state)
{
    Ihandle *controls = (Ihandle *)IupGetAttribute(ih, CONTROLS_HANDLE);
    short *target = (short *)IupGetAttribute(ih, SYNCED_VALUE);
    int controlsActive = IupGetInt(controls, "ACTIVE");
    if (controlsActive && !state)
    {
        IupSetAttribute(controls, "ACTIVE", "NO");
        InterlockedExchange16(target, I2S(state));
    }
    else if (!controlsActive && state)
    {
        IupSetAttribute(controls, "ACTIVE", "YES");
        InterlockedExchange16(target, I2S(state));
    }
    return IUP_DEFAULT;
}

static void updatePingIcon()
{
    const int pingMs = getDisplayedPingMs();
    const char *imageName;

    if (pingMs < 0)
    {
        imageName = "ping_bad_icon";
    }
    else if (pingMs <= 180)
    {
        imageName = "ping_good_icon";
    }
    else if (pingMs <= 300)
    {
        imageName = "ping_warn_icon";
    }
    else
    {
        imageName = "ping_bad_icon";
    }

    setImageIfChanged(stateIcon, imageName);
}

static void updateFilterButtonAnimation(BOOL pulseOn)
{
    UNREFERENCED_PARAMETER(pulseOn);
    if (!filterButton || filterTransitionInProgress)
        return;

    setStoredAttributeIfChanged(filterButton, "BGCOLOR",
                                filterRunning ? Theme::ButtonActive : Theme::Button);
}

static int uiTimerCb(Ihandle *ih)
{
    static DWORD lastAggressiveFlickerTime = 0;
    static BOOL aggressiveFlickerState = FALSE;
    UNREFERENCED_PARAMETER(ih);

    DWORD currentTime = GetTickCount();

    if (capturingHotkey)
        pollHotkeyCapture();
    pollKeyboardHotkeyFallback();

    if (currentTime - lastAggressiveFlickerTime >= 50)
    {
        lastAggressiveFlickerTime = currentTime;
        aggressiveFlickerState = !aggressiveFlickerState;
    }

    const BOOL isActive = filterRunning ? TRUE : FALSE;

    for (int ix = 0; ix < MODULE_CNT; ++ix)
    {
        const char *iconName = "none_icon";

        if (atomicLoad16(&modules[ix]->processTriggered))
        {
            atomicStore16(&modules[ix]->processTriggered, 0);
            iconName = aggressiveFlickerState ? "doing_icon" : "none_icon";
        }
        else if (isActive)
        {
            iconName = "doing_icon";
        }

        setImageIfChanged(modules[ix]->iconHandle, iconName);
    }

    atomicStore16(&sendState, SEND_STATUS_NONE);
    updatePingIcon();
    updateFilterButtonAnimation(FALSE);

    updateBufferBar();

    return IUP_DEFAULT;
}

static BOOL resolvePingTargetAddress(const char *target, IPAddr *targetAddress)
{
    if (!target || !targetAddress)
        return FALSE;

    IN_ADDR ipv4Address;
    if (InetPtonA(AF_INET, target, &ipv4Address) == 1)
    {
        *targetAddress = ipv4Address.S_un.S_addr;
        return TRUE;
    }

    struct addrinfo hints;
    struct addrinfo *result = NULL;

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    if (getaddrinfo(target, NULL, &hints, &result) != 0 || !result)
    {
        if (result)
            freeaddrinfo(result);
        return FALSE;
    }

    struct sockaddr_in *address = (struct sockaddr_in *)result->ai_addr;
    *targetAddress = address->sin_addr.S_un.S_addr;
    freeaddrinfo(result);
    return TRUE;
}

static void sanitizePingTarget(char *out, size_t outSize, const char *target)
{
    size_t outIx = 0;
    if (target != nullptr)
    {
        for (size_t inIx = 0; target[inIx] != '\0' && outIx + 1 < outSize; ++inIx)
        {
            char c = target[inIx];
            if (c == '"' || c == '\r' || c == '\n')
                continue;
            out[outIx++] = c;
        }
    }
    out[outIx] = '\0';
}

static unsigned __stdcall PingThreadFunc(void *arg)
{
    UNREFERENCED_PARAMETER(arg);

    char safeTarget[256] = {0};
    HANDLE icmpHandle = INVALID_HANDLE_VALUE;
    IPAddr targetAddress = 0;
    ULONGLONG lastResolveAttempt = 0;
    const char probeData[] = "dc";
    BYTE replyBuffer[sizeof(ICMP_ECHO_REPLY) + sizeof(probeData) + 16];
    const DWORD pingIntervalMs = 1000;
    const DWORD pingRetryMs = 3000;

    while (pingRunning.load(std::memory_order_acquire))
    {
        ULONGLONG cycleStart = GetTickCount64();
        DWORD cycleDelayMs = pingIntervalMs;
        char currentTarget[256];
        char sanitizedTarget[256];

        EnterCriticalSection(&pingCritSec);
        strncpy_s(currentTarget, sizeof(currentTarget), pingTargetOutbound, _TRUNCATE);
        LeaveCriticalSection(&pingCritSec);
        sanitizePingTarget(sanitizedTarget, sizeof(sanitizedTarget), currentTarget);
        if (strcmp(sanitizedTarget, safeTarget) != 0)
        {
            strcpy_s(safeTarget, sizeof(safeTarget), sanitizedTarget);
            targetAddress = 0;
            EnterCriticalSection(&pingCritSec);
            pingSampleCount = 0;
            pingSampleNext = 0;
            pingLastRawMs = -1;
            pingBaseMs = -1;
            pingFailureStreak = 0;
            pingLastSuccessTick = 0;
            LeaveCriticalSection(&pingCritSec);
        }

        if (safeTarget[0] == '\0')
        {
            EnterCriticalSection(&pingCritSec);
            pingRecordFailureLocked();
            LeaveCriticalSection(&pingCritSec);
            cycleDelayMs = pingRetryMs;
            targetAddress = 0;
        }
        else if (targetAddress == 0 || (cycleStart - lastResolveAttempt) >= 60000)
        {
            lastResolveAttempt = cycleStart;
            if (!resolvePingTargetAddress(safeTarget, &targetAddress))
            {
                EnterCriticalSection(&pingCritSec);
                pingRecordFailureLocked();
                LeaveCriticalSection(&pingCritSec);
                cycleDelayMs = pingRetryMs;
                targetAddress = 0;
                if (icmpHandle != INVALID_HANDLE_VALUE)
                {
                    IcmpCloseHandle(icmpHandle);
                    icmpHandle = INVALID_HANDLE_VALUE;
                }
            }
        }

        if (targetAddress != 0 && icmpHandle == INVALID_HANDLE_VALUE)
        {
            icmpHandle = IcmpCreateFile();
            if (icmpHandle == INVALID_HANDLE_VALUE)
            {
                EnterCriticalSection(&pingCritSec);
                pingRecordFailureLocked();
                LeaveCriticalSection(&pingCritSec);
                cycleDelayMs = pingRetryMs;
            }
        }

        if (targetAddress != 0 && icmpHandle != INVALID_HANDLE_VALUE)
        {
            DWORD pingTimeoutMs = 1500;

            DWORD replyCount =
                IcmpSendEcho(icmpHandle, targetAddress, (LPVOID)probeData, (WORD)sizeof(probeData),
                             NULL, replyBuffer, sizeof(replyBuffer), pingTimeoutMs);

            if (replyCount > 0)
            {
                PICMP_ECHO_REPLY reply = (PICMP_ECHO_REPLY)replyBuffer;
                EnterCriticalSection(&pingCritSec);
                if (reply->Status == IP_SUCCESS)
                {
                    pingRecordSuccessLocked((int)reply->RoundTripTime);
                }
                else
                {
                    pingRecordFailureLocked();
                }
                LeaveCriticalSection(&pingCritSec);
            }
            else
            {
                EnterCriticalSection(&pingCritSec);
                pingRecordFailureLocked();
                LeaveCriticalSection(&pingCritSec);
            }
        }

        ULONGLONG elapsedMs = GetTickCount64() - cycleStart;
        DWORD remainingMs = (elapsedMs < cycleDelayMs) ? (DWORD)(cycleDelayMs - elapsedMs) : 0;

        while (pingRunning.load(std::memory_order_acquire) && remainingMs > 0)
        {
            DWORD chunkMs = (remainingMs > 100) ? 100 : remainingMs;
            DWORD waitStatus = WAIT_TIMEOUT;
            if (pingTargetEvent != NULL)
                waitStatus = WaitForSingleObject(pingTargetEvent, chunkMs);
            else
                Sleep(chunkMs);
            if (waitStatus == WAIT_OBJECT_0)
                break;
            remainingMs -= chunkMs;
        }
    }

    if (icmpHandle != INVALID_HANDLE_VALUE)
    {
        IcmpCloseHandle(icmpHandle);
    }

    return 0;
}

static int uiPingTimerCb(Ihandle *ih)
{
    int displayMs;
    char pingText[32];
    UNREFERENCED_PARAMETER(ih);

    if (pingThreadHandle && WaitForSingleObject(pingThreadHandle, 0) == WAIT_OBJECT_0)
    {
        CloseHandle(pingThreadHandle);
        pingThreadHandle = NULL;
        pingRunning.store(false, std::memory_order_release);
    }

    if (!pingRunning.load(std::memory_order_acquire) && !pingThreadHandle)
    {
        HANDLE newThreadHandle;

        if (!pingCritInitialized)
        {
            InitializeCriticalSection(&pingCritSec);
            pingCritInitialized = TRUE;
            pingTargetEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
            memset(pingSamples, 0, sizeof(pingSamples));
            pingSampleCount = 0;
            pingSampleNext = 0;
            pingLastRawMs = -1;
            pingBaseMs = -1;
            pingFailureStreak = 0;
            pingLastSuccessTick = 0;
        }
        pingRunning.store(true, std::memory_order_release);
        newThreadHandle = (HANDLE)_beginthreadex(NULL, 0, PingThreadFunc, NULL, 0, NULL);
        if (!newThreadHandle)
        {
            pingRunning.store(false, std::memory_order_release);
            pingThreadHandle = NULL;
            snprintf(pingText, sizeof(pingText), " --");
            IupStoreAttribute(pingLabel, "TITLE", pingText);
            updatePingOverlay();
            return IUP_DEFAULT;
        }
        pingThreadHandle = newThreadHandle;
    }

    displayMs = getDisplayedPingMs();
    if (displayMs < 0)
    {
        snprintf(pingText, sizeof(pingText), " --");
    }
    else
    {
        snprintf(pingText, sizeof(pingText), " %d ms", displayMs);
    }

    setStoredAttributeIfChanged(pingLabel, "TITLE", pingText);
    updatePingOverlay();
    return IUP_DEFAULT;
}

static int uiTimeoutCb(Ihandle *ih)
{
    UNREFERENCED_PARAMETER(ih);
    return IUP_CLOSE;
}

static int uiCloseCb(Ihandle *ih)
{
    UNREFERENCED_PARAMETER(ih);
    return uiKillCb(ih);
}

static int uiListSelectCb(Ihandle *ih, char *text, int item, int state)
{
    UNREFERENCED_PARAMETER(text);
    UNREFERENCED_PARAMETER(ih);
    if (state == 1 && item >= 1 && static_cast<UINT>(item) <= filterListCount)
    {
        const size_t presetIndex = static_cast<size_t>(item - 1);
        IupStoreAttribute(filterText, "VALUE", appConfig.filters[presetIndex].expression.c_str());
        applyPresetPingTarget(presetIndex);
    }
    return IUP_DEFAULT;
}

static int uiFilterTextCb(Ihandle *ih)
{
    UNREFERENCED_PARAMETER(ih);
    IupSetAttribute(filterSelectList, "VALUE", "0");
    return IUP_DEFAULT;
}

static int uiHotkeyTimerCb(Ihandle *ih)
{
    UNREFERENCED_PARAMETER(ih);
    HWND hWnd = (HWND)IupGetAttribute(dialog, "HWND");
    if (hWnd)
    {
        registerHotkey(hWnd);
    }
    IupSetAttribute(ih, "RUN", "NO");
    return IUP_DEFAULT;
}

static int uiInstallWndProcTimer(Ihandle *ih)
{
    UNREFERENCED_PARAMETER(ih);
    HWND hWnd = (HWND)IupGetAttribute(dialog, "HWND");
    if (hWnd)
    {
        LONG_PTR previousProc;

        mainWindowHandle = hWnd;
        if (!previousDialogWndProc)
        {
            SetLastError(0);
            previousProc = SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)CustomWndProc);
            if (previousProc != 0 || GetLastError() == 0)
            {
                previousDialogWndProc = (WNDPROC)previousProc;
            }
            else
            {
                showStatus("Failed to install window hook.");
            }
        }
    }
    IupSetAttribute(ih, "RUN", "NO");
    return IUP_DEFAULT;
}

static void uiSetupModule(Module *module, Ihandle *parent)
{
    Ihandle *groupBox, *toggle, *controls, *icon, *nameLabel;

    controls = module->setupUIFunc();

    groupBox = IupHbox(icon = IupLabel(NULL), toggle = IupToggle(NULL, NULL),
                       nameLabel = IupLabel(module->displayName), IupFill(), controls, NULL);
    IupSetAttribute(groupBox, "EXPAND", "HORIZONTAL");
    IupSetAttribute(groupBox, "ALIGNMENT", "ACENTER");
    IupSetAttribute(groupBox, "BGCOLOR", Theme::Panel);
    IupSetAttribute(controls, "ALIGNMENT", "ACENTER");
    IupAppend(parent, groupBox);

    IupSetCallback(toggle, "ACTION", (Icallback)uiToggleControls);
    IupSetAttribute(toggle, CONTROLS_HANDLE, (char *)controls);
    IupSetAttribute(toggle, SYNCED_VALUE, (char *)module->enabledFlag);
    IupSetAttribute(toggle, "BGCOLOR", Theme::Panel);
    IupSetAttribute(toggle, "FGCOLOR", Theme::Text);
    IupSetAttribute(toggle, "NOTHEME", "YES");
    IupSetAttribute(toggle, "TIP", module->displayName);
    IupSetAttribute(nameLabel, "BGCOLOR", Theme::Panel);
    IupSetAttribute(nameLabel, "FGCOLOR", Theme::Text);
    IupSetAttribute(controls, "ACTIVE", "NO");
    IupSetAttribute(controls, "NCGAP", "4");

    IupSetAttribute(icon, "IMAGE", "none_icon");
    IupSetAttribute(icon, "BGCOLOR", Theme::Panel);
    IupSetAttribute(icon, "PADDING", "4x");
    module->iconHandle = icon;

    IupSetAttribute(toggle, "VALUE", "ON");
    *(module->enabledFlag) = 1;
    IupSetAttribute(controls, "ACTIVE", "YES");

    if (parameterized)
    {
        setFromParameter(toggle, "VALUE", module->shortName);
    }
}

static unsigned __stdcall updateWorkerFunc(void *arg)
{
    const bool automatic = arg != nullptr;
    auto result = std::unique_ptr<UpdateCheckResult>(new (std::nothrow) UpdateCheckResult());
    if (result == nullptr)
        return 1;
    *result = updateCheckForUpdates(APP_VERSION, appConfig.updateCheckUrl, appConfig.updatePageUrl);
    result->automatic = automatic;
    const HWND target = mainWindowHandle;
    if (target != nullptr && !shutdownComplete &&
        PostMessage(target, UPDATE_WM_RESULT, automatic ? 1 : 0, (LPARAM)result.get()) != 0)
    {
        result.release();
    }
    return 0;
}

static void updateStartCheck(bool automatic)
{
    if (updateCheckInFlight)
        return;
    if (appConfig.updateCheckUrl.empty())
    {
        if (!automatic)
            showStatus("Updates are disabled (no updateCheckUrl configured).");
        return;
    }
    updateCheckInFlight = true;
    if (updateButton != nullptr)
        IupSetAttribute(updateButton, "ACTIVE", "NO");
    if (!automatic)
        showStatus("Checking for updates...");
    uintptr_t thread =
        _beginthreadex(nullptr, 0, updateWorkerFunc, automatic ? (void *)1 : nullptr, 0, nullptr);
    if (thread == 0)
    {
        updateCheckInFlight = false;
        if (updateButton != nullptr)
            IupSetAttribute(updateButton, "ACTIVE", "YES");
        if (!automatic)
            showStatus("Update check failed to start.");
    }
    else
    {
        CloseHandle((HANDLE)thread);
    }
}

static void pollKeyboardHotkeyFallback()
{
    if (hotkeyToggle == 0 || hotkeyToggle == VK_XBUTTON1 || hotkeyToggle == VK_XBUTTON2 ||
        hotkeyRegistered)
    {
        hotkeyPressed = FALSE;
        return;
    }
    const bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool winDown =
        ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) || ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);
    bool down = (GetAsyncKeyState(hotkeyToggle) & 0x8000) != 0;
    if (down)
    {
        if (((hotkeyModifiers & MOD_CONTROL) != 0) != ctrlDown)
            down = false;
        if (((hotkeyModifiers & MOD_ALT) != 0) != altDown)
            down = false;
        if (((hotkeyModifiers & MOD_SHIFT) != 0) != shiftDown)
            down = false;
        if (((hotkeyModifiers & MOD_WIN) != 0) != winDown)
            down = false;
    }
    if (down && !capturingHotkey)
    {
        if (!hotkeyPressed)
        {
            hotkeyPressed = TRUE;
            performFilterToggle();
        }
    }
    else if (!down)
    {
        hotkeyPressed = FALSE;
    }
}

static void endHotkeyCaptureUI()
{
    capturingHotkey = false;
    if (hotkeyChangeButton != nullptr)
        IupStoreAttribute(hotkeyChangeButton, "TITLE", "Change");
}

static void cancelHotkeyCapture()
{
    endHotkeyCaptureUI();
    showStatus("Hotkey unchanged.");
}

static void applyHotkeyBinding(const HotkeyBinding &binding)
{
    unregisterHotkey();
    StopMouseHookThread();
    if (!configureHotkey(binding.canonical))
    {
        showStatus("That key can't be used as a hotkey.");
        return;
    }
    appConfig.hotkey = binding.canonical;
    if (mainWindowHandle != NULL)
        registerHotkey(mainWindowHandle);
    if (hotkeyLabel != nullptr)
        IupStoreAttribute(hotkeyLabel, "TITLE", hotkeyDisplayName);
    if (hotkeyLabel != nullptr)
        IupStoreAttribute(hotkeyLabel, "TIP", hotkeyDisplayName);

    std::string message = "Hotkey set to " + binding.canonical + ".";
    const bool typesText = binding.modifiers == 0 && hotkeyIsTypingKey(binding.vk);
    if (binding.modifiers == 0 && typesText)
        message += " Note: single keys toggle while typing anywhere.";
    const std::string path = configFilePath();
    std::string saveError;
    if (path.empty() || !saveConfigJson(path, appConfig, saveError))
        message += " Config could not be saved.";
    showStatus(message.c_str());
}

static void captureHotkeyKey(int vk)
{
    if (vk == VK_LWIN || vk == VK_RWIN)
    {
        showStatus("Hold WIN and press another key (e.g. WIN+K). Esc cancels.");
        return;
    }
    std::string keyName;
    if (!hotkeyKeyName(vk, keyName))
    {
        cancelHotkeyCapture();
        showStatus("That key can't be used as a hotkey.");
        return;
    }
    std::string text;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0)
        text += "CTRL+";
    if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0)
        text += "ALT+";
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)
        text += "SHIFT+";
    if (((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) || ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0))
        text += "WIN+";
    text += keyName;

    HotkeyBinding binding;
    if (!parseHotkeyString(text, binding))
    {
        cancelHotkeyCapture();
        showStatus("That key can't be used as a hotkey.");
        return;
    }
    endHotkeyCaptureUI();
    applyHotkeyBinding(binding);
}

static const std::vector<int> &captureKeyList() { return hotkeyCaptureKeys(); }

static void pollHotkeyCapture()
{
    for (int vk : captureKeyList())
    {
        if ((GetAsyncKeyState(vk) & 0x8000) == 0)
            captureEligible[vk] = true;
    }
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) == 0)
        captureEligible[VK_ESCAPE] = true;

    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0 && captureEligible[VK_ESCAPE])
    {
        cancelHotkeyCapture();
        return;
    }
    for (int vk : captureKeyList())
    {
        if ((GetAsyncKeyState(vk) & 0x8000) != 0 && captureEligible[vk])
        {
            captureHotkeyKey(vk);
            return;
        }
    }
}

static int uiChangeHotkeyCb(Ihandle *ih)
{
    UNREFERENCED_PARAMETER(ih);
    if (capturingHotkey)
    {
        cancelHotkeyCapture();
        return IUP_DEFAULT;
    }
    capturingHotkey = true;
    for (int i = 0; i < 256; ++i)
        captureEligible[i] = false;
    if (hotkeyChangeButton != nullptr)
        IupStoreAttribute(hotkeyChangeButton, "TITLE", "Cancel");
    showStatus("Press a key, combo (CTRL/ALT/SHIFT/WIN + key), or mouse 4/5. "
               "Extra mouse keys: press it — if it sends a key it binds, "
               "otherwise remap it to F13+ in your mouse software. Esc cancels.");
    return IUP_DEFAULT;
}

static int uiCheckUpdatesCb(Ihandle *ih)
{
    UNREFERENCED_PARAMETER(ih);
    updateStartCheck(false);
    return IUP_DEFAULT;
}

static void updateSetupUI(Ihandle *parent)
{
    Ihandle *row, *nameLabel, *versionLabel;
    char versionText[64];

    snprintf(versionText, sizeof(versionText), "v%s", APP_VERSION);
    row = IupHbox(nameLabel = IupLabel("Updates"), versionLabel = IupLabel(versionText),
                  updateLabel = IupLabel(""), updateButton = IupButton("Check", NULL), NULL);
    IupSetAttribute(row, "EXPAND", "HORIZONTAL");
    IupSetAttribute(row, "ALIGNMENT", "ACENTER");
    IupSetAttribute(row, "BGCOLOR", Theme::Panel);
    IupSetAttribute(row, "GAP", "8");
    IupSetAttribute(nameLabel, "BGCOLOR", Theme::Panel);
    IupSetAttribute(nameLabel, "FGCOLOR", Theme::Text);
    IupSetAttribute(versionLabel, "BGCOLOR", Theme::Panel);
    IupSetAttribute(versionLabel, "FGCOLOR", Theme::MutedText);
    IupSetAttribute(updateLabel, "BGCOLOR", Theme::Panel);
    IupSetAttribute(updateLabel, "FGCOLOR", Theme::MutedText);
    IupSetAttribute(updateLabel, "EXPAND", "HORIZONTAL");
    IupSetAttribute(updateButton, "FGCOLOR", "0 0 0");
    IupSetAttribute(updateButton, "PADDING", "6x2");
    IupSetAttribute(updateButton, "TIP", "Check for updates now");
    IupSetCallback(updateButton, "ACTION", (Icallback)uiCheckUpdatesCb);
    IupAppend(parent, row);
}

static void handleUpdateResult(WPARAM automatic, LPARAM payload)
{
    UpdateCheckResult *result = (UpdateCheckResult *)payload;
    if (result == nullptr)
        return;
    updateCheckInFlight = false;
    if (updateButton != nullptr)
        IupSetAttribute(updateButton, "ACTIVE", "YES");

    if (!result->error.empty())
    {
        if (automatic == 0)
        {
            showStatus(result->error.c_str());
            if (updateLabel != nullptr)
                IupStoreAttribute(updateLabel, "TITLE", "Check failed");
        }
        delete result;
        return;
    }

    if (result->hasUpdate)
    {
        char statusBuf[256];
        snprintf(statusBuf, sizeof(statusBuf), "Update available: v%s (current v%s).",
                 result->latestVersion.c_str(), APP_VERSION);
        showStatus(statusBuf);
        if (updateLabel != nullptr)
        {
            char labelBuf[64];
            snprintf(labelBuf, sizeof(labelBuf), "v%s available", result->latestVersion.c_str());
            IupStoreAttribute(updateLabel, "TITLE", labelBuf);
        }
        if (automatic == 0)
        {
            char boxBuf[512];
            snprintf(boxBuf, sizeof(boxBuf),
                     "A new version v%s is available (you have v%s).\nOpen the download page now?",
                     result->latestVersion.c_str(), APP_VERSION);
            const std::string downloadUrl = result->downloadUrl;
            delete result;
            const int choice = MessageBoxA(mainWindowHandle, boxBuf, "Update available",
                                           MB_YESNO | MB_ICONINFORMATION);
            if (choice == IDYES && !downloadUrl.empty())
                updateOpenUrl(downloadUrl);
            return;
        }
    }
    else
    {
        if (automatic == 0)
        {
            char statusBuf[128];
            snprintf(statusBuf, sizeof(statusBuf), "Up to date (v%s).", APP_VERSION);
            showStatus(statusBuf);
            if (updateLabel != nullptr)
                IupStoreAttribute(updateLabel, "TITLE", "Up to date");
        }
        else if (updateLabel != nullptr)
        {
            IupStoreAttribute(updateLabel, "TITLE", "Up to date");
        }
    }
    delete result;
}

static void updatePing()
{
    int displayMs = getDisplayedPingMs();
    char pingText[32];
    if (displayMs < 0)
    {
        snprintf(pingText, sizeof(pingText), " --");
    }
    else
    {
        snprintf(pingText, sizeof(pingText), " %d ms", displayMs);
    }
    setStoredAttributeIfChanged(pingLabel, "TITLE", pingText);
    updatePingOverlay();
}

static void updateBufferBar()
{
    float fill = lagGetBufferFill();
    int percent = (int)(fill * 100 + 0.5f);
    if (percent > 100)
        percent = 100;
    if (percent < 0)
        percent = 0;

    char percentBuf[16];
    snprintf(percentBuf, sizeof(percentBuf), "%d%%", percent);

    setStoredAttributeIfChanged(bufferPercentLabel, "TITLE", percentBuf);

    if (fill > 0.7f)
    {
        int flushCnt = lagGetFlushCount();
        if (flushCnt > 0)
        {
            LOG("Flush stats: count=%d, last_size=%d, time_since_last=%lu ms", flushCnt,
                lagGetLastFlushSize(), GetTickCount() - lagGetLastFlushTime());
            lagResetFlushStats();
        }
    }
}

static LRESULT CALLBACK CustomWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCHITTEST)
    {
        LRESULT hitResult;
        POINT clientPoint;
        HWND childWindow;

        if (previousDialogWndProc)
            hitResult = CallWindowProc(previousDialogWndProc, hWnd, message, wParam, lParam);
        else
            hitResult = DefWindowProc(hWnd, message, wParam, lParam);

        if (hitResult != HTCLIENT)
            return hitResult;

        clientPoint.x = (int)(short)LOWORD(lParam);
        clientPoint.y = (int)(short)HIWORD(lParam);
        ScreenToClient(hWnd, &clientPoint);
        childWindow = RealChildWindowFromPoint(hWnd, clientPoint);

        if (!isInteractiveChildWindow(hWnd, childWindow))
            return HTCAPTION;

        return HTCLIENT;
    }
    if (message == WM_USER + 1)
    {
        performFilterToggle();
        return 0;
    }
    if (message == WM_HOTKEY && (int)wParam == hotkeyId)
    {
        performFilterToggle();
        return 0;
    }
    if (message == WM_USER + 2)
    {
        if (lParam == WM_LBUTTONDBLCLK)
        {
            ShowWindow(hWnd, SW_RESTORE);
            SetForegroundWindow(hWnd);
        }
        return 0;
    }
    if (message == UPDATE_WM_RESULT)
    {
        handleUpdateResult(wParam, lParam);
        return 0;
    }
    if (previousDialogWndProc)
        return CallWindowProc(previousDialogWndProc, hWnd, message, wParam, lParam);

    return DefWindowProc(hWnd, message, wParam, lParam);
}

int main(int argc, char *argv[])
{
    LOG("Is Run As Admin: %d", IsRunAsAdmin());
    LOG("Is Elevated: %d", IsElevated());
    if (!init(argc, argv))
        return EXIT_FAILURE;
    startup();
    cleanup();

    return EXIT_SUCCESS;
}
