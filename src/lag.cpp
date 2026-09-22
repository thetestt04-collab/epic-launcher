#include <algorithm>
#include <atomic>
#include <winsock2.h>
#include "common.h"
#include "iup.h"
#define NAME "lag"
#define LAG_MIN "0"
#define LAG_MAX "15000"
#define KEEP_AT_MOST 2000
#define RELEASE_PER_CYCLE 32
#define RELEASE_WINDOW_MS 10
#define DRAIN_PER_CYCLE 16
#define PRESSURE_HIGH_WATERMARK 1750
#define PRESSURE_RELEASE_PER_CYCLE 8
#define LAG_DEFAULT 500

static Ihandle *inboundCheckbox, *outboundCheckbox, *timeInput;

volatile short lagEnabled = 1, lagInbound = 1, lagOutbound = 1, lagTime = LAG_DEFAULT;

static PacketNode lagHeadNode{}, lagTailNode{};
static PacketNode *bufHead = &lagHeadNode, *bufTail = &lagTailNode;
static std::atomic<int> bufSize{0};

static std::atomic<int> flushCount{0};
static std::atomic<int> lastFlushSize{0};
static std::atomic<DWORD> lastFlushTime{0};
static DWORD releaseWindowStart = 0;
static int releaseWindowBudget = RELEASE_PER_CYCLE;

static INLINE_FUNCTION BOOL isDelayProtocol(const PacketNode *packet)
{
    const unsigned char *data = (const unsigned char *)packet->packet;
    unsigned char protocol;
    unsigned char version;

    if (!data || packet->packetLen < 1)
        return FALSE;

    version = (unsigned char)(data[0] >> 4);
    if (version == 4)
    {
        if (packet->packetLen < 20)
            return FALSE;
        protocol = data[9];
    }
    else if (version == 6)
    {
        unsigned int offset = 40;
        if (packet->packetLen < offset)
            return FALSE;
        protocol = data[6];
        while (protocol != IPPROTO_TCP && protocol != IPPROTO_UDP)
        {
            unsigned int hdrBytes;
            if (protocol == 0 || protocol == 43 || protocol == 60)
            {
                if (packet->packetLen < offset + 1)
                    return FALSE;
                hdrBytes = ((unsigned int)data[offset] + 1) * 8;
            }
            else if (protocol == 51)
            {
                if (packet->packetLen < offset + 2)
                    return FALSE;
                hdrBytes = ((unsigned int)data[offset + 1] + 2) * 4;
            }
            else if (protocol == 44)
            {
                if (packet->packetLen < offset + 8)
                    return FALSE;
                if (data[offset + 2] != 0 || (data[offset + 3] >> 3) != 0)
                    return FALSE;
                hdrBytes = 8;
            }
            else
            {
                return FALSE;
            }
            offset += hdrBytes;
            if (packet->packetLen < offset + 1)
                return FALSE;
            protocol = data[offset];
        }
    }
    else
    {
        return FALSE;
    }

    return protocol == IPPROTO_TCP || protocol == IPPROTO_UDP;
}

int lagGetFlushCount() { return flushCount.load(std::memory_order_relaxed); }
int lagGetLastFlushSize() { return lastFlushSize.load(std::memory_order_relaxed); }
DWORD lagGetLastFlushTime() { return lastFlushTime.load(std::memory_order_relaxed); }

void lagResetFlushStats() { flushCount.store(0, std::memory_order_relaxed); }

static INLINE_FUNCTION short isBufEmpty()
{
    short ret = bufHead->next == bufTail;
    if (ret)
        assert(bufSize.load(std::memory_order_relaxed) == 0);
    return ret;
}

float lagGetBufferFill()
{
    return static_cast<float>(bufSize.load(std::memory_order_relaxed)) /
           static_cast<float>(KEEP_AT_MOST);
}

static Ihandle *lagSetupUI()
{
    Ihandle *directionLabel;
    Ihandle *delayLabel;
    Ihandle *inboundLabel;
    Ihandle *outboundLabel;
    Ihandle *lagControlsBox =
        IupHbox(directionLabel = IupLabel("Direction"), inboundCheckbox = IupToggle(NULL, NULL),
                inboundLabel = IupLabel("Inbound"), outboundCheckbox = IupToggle(NULL, NULL),
                outboundLabel = IupLabel("Outbound"), IupFill(),
                delayLabel = IupLabel("Delay (ms)"), timeInput = IupText(NULL), NULL);

    IupSetAttribute(lagControlsBox, "BGCOLOR", Theme::Panel);
    IupSetAttribute(directionLabel, "BGCOLOR", Theme::Panel);
    IupSetAttribute(directionLabel, "FGCOLOR", Theme::Text);
    IupSetAttribute(delayLabel, "BGCOLOR", Theme::Panel);
    IupSetAttribute(delayLabel, "FGCOLOR", Theme::Text);
    IupSetAttribute(inboundLabel, "BGCOLOR", Theme::Panel);
    IupSetAttribute(inboundLabel, "FGCOLOR", Theme::Text);
    IupSetAttribute(outboundLabel, "BGCOLOR", Theme::Panel);
    IupSetAttribute(outboundLabel, "FGCOLOR", Theme::Text);
    IupSetAttribute(inboundCheckbox, "BGCOLOR", Theme::Panel);
    IupSetAttribute(inboundCheckbox, "FGCOLOR", Theme::Text);
    IupSetAttribute(inboundCheckbox, "NOTHEME", "YES");
    IupSetAttribute(outboundCheckbox, "BGCOLOR", Theme::Panel);
    IupSetAttribute(outboundCheckbox, "FGCOLOR", Theme::Text);
    IupSetAttribute(outboundCheckbox, "NOTHEME", "YES");
    IupSetAttribute(timeInput, "BGCOLOR", Theme::Surface);
    IupSetAttribute(timeInput, "FGCOLOR", Theme::Text);
    IupSetAttribute(timeInput, "NOTHEME", "YES");

    IupSetAttribute(lagControlsBox, "GAP", "8");
    IupSetAttribute(lagControlsBox, "ALIGNMENT", "ACENTER");
    IupSetAttribute(timeInput, "VISIBLECOLUMNS", "7");
    IupSetAttribute(timeInput, "PADDING", "4x3");
    IupSetAttribute(timeInput, "VALUE", STR(LAG_DEFAULT));
    IupSetCallback(timeInput, "VALUECHANGED_CB", uiSyncInteger);
    IupSetAttribute(timeInput, SYNCED_VALUE, (char *)&lagTime);
    IupSetAttribute(timeInput, INTEGER_MAX, LAG_MAX);
    IupSetAttribute(timeInput, INTEGER_MIN, LAG_MIN);
    IupSetCallback(inboundCheckbox, "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(inboundCheckbox, SYNCED_VALUE, (char *)&lagInbound);
    IupSetCallback(outboundCheckbox, "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(outboundCheckbox, SYNCED_VALUE, (char *)&lagOutbound);

    IupSetAttribute(inboundCheckbox, "VALUE", "OFF");
    IupSetAttribute(outboundCheckbox, "VALUE", "ON");
    atomicStore16(&lagInbound, 0);
    atomicStore16(&lagOutbound, 1);

    if (parameterized)
    {
        setFromParameter(inboundCheckbox, "VALUE", NAME "-inbound");
        setFromParameter(outboundCheckbox, "VALUE", NAME "-outbound");
        setFromParameter(timeInput, "VALUE", NAME "-time");
    }

    return lagControlsBox;
}

static void lagStartUp()
{
    if (bufHead->next == NULL && bufTail->next == NULL)
    {
        bufHead->next = bufTail;
        bufTail->prev = bufHead;
        bufSize.store(0, std::memory_order_relaxed);
    }
    else
    {
        assert(isBufEmpty());
    }
    releaseWindowStart = timeGetTime();
    releaseWindowBudget = RELEASE_PER_CYCLE;
    startTimePeriod();
}

static int releaseOldestPackets(PacketNode *insertionAnchor, int limit)
{
    int released = 0;

    while (!isBufEmpty() && released < limit)
    {
        insertAfter(popNode(bufTail->prev), insertionAnchor);
        bufSize.fetch_sub(1, std::memory_order_relaxed);
        ++released;
    }

    return released;
}

static short lagCloseDown(PacketNode *head, PacketNode *tail)
{
    UNREFERENCED_PARAMETER(head);
    const int released = releaseOldestPackets(tail->prev, DRAIN_PER_CYCLE);
    const bool hasMore = !isBufEmpty();

    LOG("Paced lag drain released %d packets; %d remain", released,
        bufSize.load(std::memory_order_relaxed));
    if (!hasMore)
        endTimePeriod();

    return hasMore ? 1 : 0;
}

static short lagProcess(PacketNode *head, PacketNode *tail)
{
    DWORD currentTime = timeGetTime();
    PacketNode *pac = tail->prev;
    PacketNode *releaseAnchor = tail->prev;
    int processedCount = 0;
    int releasedThisCycle = 0;
    const DWORD delayMs = static_cast<DWORD>(atomicLoad16(&lagTime));

    const int MAX_PROCESS_PER_CYCLE = 100;

    if (releaseWindowStart == 0 ||
        static_cast<DWORD>(currentTime - releaseWindowStart) >= RELEASE_WINDOW_MS)
    {
        releaseWindowStart = currentTime;
        releaseWindowBudget = RELEASE_PER_CYCLE;
    }

    while (!isBufEmpty() && releasedThisCycle < releaseWindowBudget)
    {
        PacketNode *oldest = bufTail->prev;
        if (static_cast<DWORD>(currentTime - oldest->timestamp) < delayMs)
            break;

        insertAfter(popNode(oldest), releaseAnchor);
        bufSize.fetch_sub(1, std::memory_order_relaxed);
        ++releasedThisCycle;
    }

    const int bufferedBeforePickup = bufSize.load(std::memory_order_relaxed);
    if (bufferedBeforePickup >= PRESSURE_HIGH_WATERMARK && releasedThisCycle < releaseWindowBudget)
    {
        const int pressureLimit =
            (std::min)(PRESSURE_RELEASE_PER_CYCLE, releaseWindowBudget - releasedThisCycle);
        const int pressureReleased = releaseOldestPackets(releaseAnchor, pressureLimit);
        releasedThisCycle += pressureReleased;
        lastFlushTime.store(currentTime, std::memory_order_relaxed);
        lastFlushSize.store(pressureReleased, std::memory_order_relaxed);
        flushCount.fetch_add(1, std::memory_order_relaxed);
        LOG("Lag pressure relief released %d packets; %d remain", pressureReleased,
            bufSize.load(std::memory_order_relaxed));
    }
    releaseWindowBudget -= releasedThisCycle;

    const short delayInbound = atomicLoad16(&lagInbound);
    const short delayOutbound = atomicLoad16(&lagOutbound);

    while (bufSize.load(std::memory_order_relaxed) < KEEP_AT_MOST && pac != head &&
           processedCount < MAX_PROCESS_PER_CYCLE)
    {
        const bool protocolRelevant = isDelayProtocol(pac) != FALSE;
        const bool shouldDelay =
            protocolRelevant && checkDirection(pac->addr.Outbound, delayInbound, delayOutbound);

        if (shouldDelay)
        {
            insertAfter(popNode(pac), bufHead)->timestamp = currentTime;
            bufSize.fetch_add(1, std::memory_order_relaxed);
            pac = tail->prev;
        }
        else
        {
            pac = pac->prev;
        }
        processedCount++;
    }

    return bufSize.load(std::memory_order_relaxed) > 0;
}

Module lagModule = {"Lag",        NAME,       &lagEnabled, lagSetupUI, lagStartUp,
                    lagCloseDown, lagProcess, 0,           0,          NULL};
