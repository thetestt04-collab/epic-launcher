#pragma once
#include "iup.h"
#include "windivert.h"
#include <cassert>
#include <cstdio>

#define APP_NAME "EpicGamesLauncher"
#define MSG_BUFSIZE 512
#define FILTER_BUFSIZE 1024
#define NAME_SIZE 16
#define MODULE_CNT 1
#define ICON_UPDATE_MS 500

#define CONTROLS_HANDLE "__CONTROLS_HANDLE"
#define SYNCED_VALUE "__SYNCED_VALUE"
#define INTEGER_MAX "__INTEGER_MAX"
#define INTEGER_MIN "__INTEGER_MIN"
#define FIXED_MAX "__FIXED_MAX"
#define FIXED_MIN "__FIXED_MIN"
#define FIXED_EPSILON 0.01

#define I2S(x) ((short)((x) & 0xFFFF))

#define INLINE_FUNCTION inline

namespace Theme
{
inline constexpr char Background[] = "18 18 18";
inline constexpr char Panel[] = "18 18 18";
inline constexpr char Surface[] = "38 38 38";
inline constexpr char Button[] = "64 64 64";
inline constexpr char ButtonActive[] = "82 82 82";
inline constexpr char ButtonTransition[] = "96 96 96";
inline constexpr char Border[] = "150 150 150";
inline constexpr char Text[] = "224 224 224";
inline constexpr char MutedText[] = "178 178 178";
inline constexpr char ButtonSymbol[] = "28 28 28";
inline constexpr char ResumeGreen[] = "28 145 72";
inline constexpr char PauseRed[] = "198 52 52";
} // namespace Theme

#ifdef __MINGW32__
#ifdef InterlockedAnd16
#undef InterlockedAnd16
#endif

#define InterlockedAnd16(p, val) (__atomic_and_fetch((short *)(p), (val), __ATOMIC_SEQ_CST))

#ifdef InterlockedExchange16
#undef InterlockedExchange16
#endif
#define InterlockedExchange16(p, val) (__atomic_exchange_n((short *)(p), (val), __ATOMIC_SEQ_CST))

#ifdef InterlockedIncrement16
#undef InterlockedIncrement16
#endif
#define InterlockedIncrement16(p) (__atomic_add_fetch((short *)(p), 1, __ATOMIC_SEQ_CST))

#ifdef InterlockedDecrement16
#undef InterlockedDecrement16
#endif
#define InterlockedDecrement16(p) (__atomic_sub_fetch((short *)(p), 1, __ATOMIC_SEQ_CST))

#endif

[[nodiscard]] inline short atomicLoad16(const volatile short *value) noexcept
{
#ifdef __MINGW32__
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#else
    return InterlockedCompareExchange16(
        reinterpret_cast<volatile SHORT *>(const_cast<volatile short *>(value)), 0, 0);
#endif
}

inline void atomicStore16(volatile short *target, short value) noexcept
{
    InterlockedExchange16(target, value);
}

#define LOG(...) logMessage(__VA_ARGS__)

struct PacketNode
{
    char *packet = nullptr;
    UINT packetLen = 0;
    WINDIVERT_ADDRESS addr{};
    DWORD timestamp = 0;
    PacketNode *prev = nullptr;
    PacketNode *next = nullptr;
};

void initPacketNodeList();
PacketNode *createNode(const char *buf, UINT len, const WINDIVERT_ADDRESS *addr);
void freeNode(PacketNode *node);
PacketNode *popNode(PacketNode *node);
PacketNode *insertBefore(PacketNode *node, PacketNode *target);
PacketNode *insertAfter(PacketNode *node, PacketNode *target);
PacketNode *appendNode(PacketNode *node);
short isListEmpty();

int uiSyncChance(Ihandle *ih);
int uiSyncToggle(Ihandle *ih, int state);
int uiSyncInteger(Ihandle *ih);
int uiSyncFixed(Ihandle *ih);
int uiSyncInt32(Ihandle *ih);

struct Module
{

    const char *displayName;
    const char *shortName;
    volatile short *enabledFlag;
    Ihandle *(*setupUIFunc)();
    void (*startUp)();
    short (*closeDown)(PacketNode *head, PacketNode *tail);
    short (*process)(PacketNode *head, PacketNode *tail);

    short lastEnabled;
    volatile short processTriggered;
    Ihandle *iconHandle;
};

extern Module lagModule;
extern Module *modules[MODULE_CNT];

#define SEND_STATUS_NONE 0
#define SEND_STATUS_SEND 1
#define SEND_STATUS_FAIL -1
extern volatile short sendState;
extern volatile short lagEnabled;
extern volatile short lagInbound;
extern volatile short lagOutbound;
extern volatile short lagTime;

void showStatus(const char *line);

int divertStart(const char *filter, char buf[]);
void divertStop();
void cleanupPacketPool();

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

short calcChance(short chance);

[[nodiscard]] static INLINE_FUNCTION BOOL checkDirection(BOOL outboundPacket, short handleInbound,
                                                         short handleOutbound) noexcept
{
    return (handleInbound && !outboundPacket) || (handleOutbound && outboundPacket);
}

#define TIMER_RESOLUTION 1
void startTimePeriod();
void endTimePeriod();

BOOL IsElevated();
BOOL IsRunAsAdmin();
BOOL tryElevate(HWND hWnd, BOOL silent);

extern const unsigned char icon8x8[8 * 8];

extern BOOL parameterized;
void setFromParameter(Ihandle *ih, const char *field, const char *key);
BOOL parseArgs(int argc, char *argv[]);

void logMessage(const char *fmt, ...)
#ifdef __MINGW32__
    __attribute__((format(printf, 1, 2)))
#endif
    ;

void applyDarkMode();

HWND getMainWindowHandle(void);
