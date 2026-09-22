#include <atomic>
#include <cstdlib>
#include <cstring>
#include <winsock2.h>
#include <Ws2tcpip.h>
#include "windivert.h"
#include "common.h"
#define DIVERT_PRIORITY 0
#define MAX_PACKETSIZE 0xFFFF
#define READ_TIME_PER_STEP 3
#define CLOCK_WAITMS 10
#define NORMAL_SEND_PER_CYCLE 200
#define DRAIN_SEND_PER_CYCLE 16
#define DRAIN_WAIT_MS 10
#define QUEUE_LEN (2 << 12)
#define QUEUE_TIME (2 << 9)

static std::atomic<HANDLE> divertHandle{INVALID_HANDLE_VALUE};
static std::atomic_bool stopLooping{false};
static HANDLE loopThread = nullptr;
static HANDLE clockThread = nullptr;
static HANDLE mutex = nullptr;
#ifdef _DEBUG
static volatile LONG packetsReceived = 0;
static volatile LONG packetsSent = 0;
static volatile LONG packetsDropped = 0;
static DWORD lastStatsTime = 0;
#endif

static DWORD divertReadLoop(LPVOID arg);
static DWORD divertClockLoop(LPVOID arg);

extern PacketNode *const head;
extern PacketNode *const tail;

#ifdef _DEBUG
PWINDIVERT_IPHDR dbg_ip_header;
PWINDIVERT_IPV6HDR dbg_ipv6_header;
PWINDIVERT_TCPHDR dbg_tcp_header;
PWINDIVERT_UDPHDR dbg_udp_header;
PWINDIVERT_ICMPHDR dbg_icmp_header;
PWINDIVERT_ICMPV6HDR dbg_icmpv6_header;
UINT payload_len;
void dumpPacket(char *buf, int len, PWINDIVERT_ADDRESS paddr)
{
    char *protocol;
    UINT16 srcPort = 0, dstPort = 0;

    WinDivertHelperParsePacket(buf, len, &dbg_ip_header, &dbg_ipv6_header, NULL, &dbg_icmp_header,
                               &dbg_icmpv6_header, &dbg_tcp_header, &dbg_udp_header, NULL,
                               &payload_len, NULL, NULL);
    if (dbg_tcp_header != NULL)
    {
        protocol = "TCP ";
        srcPort = ntohs(dbg_tcp_header->SrcPort);
        dstPort = ntohs(dbg_tcp_header->DstPort);
    }
    else if (dbg_udp_header != NULL)
    {
        protocol = "UDP ";
        srcPort = ntohs(dbg_udp_header->SrcPort);
        dstPort = ntohs(dbg_udp_header->DstPort);
    }
    else if (dbg_icmp_header || dbg_icmpv6_header)
    {
        protocol = "ICMP";
    }
    else
    {
        protocol = "???";
    }

    if (dbg_ip_header != NULL)
    {
        UINT8 *src_addr = (UINT8 *)&dbg_ip_header->SrcAddr;
        UINT8 *dst_addr = (UINT8 *)&dbg_ip_header->DstAddr;
        LOG("%s.%s: %u.%u.%u.%u:%d->%u.%u.%u.%u:%d", protocol, paddr->Outbound ? "OUT " : "IN  ",
            src_addr[0], src_addr[1], src_addr[2], src_addr[3], srcPort, dst_addr[0], dst_addr[1],
            dst_addr[2], dst_addr[3], dstPort);
    }
    else if (dbg_ipv6_header != NULL)
    {
        UINT16 *src_addr6 = (UINT16 *)&dbg_ipv6_header->SrcAddr;
        UINT16 *dst_addr6 = (UINT16 *)&dbg_ipv6_header->DstAddr;
        LOG("%s.%s: %x:%x:%x:%x:%x:%x:%x:%x:%d->%x:%x:%x:%x:%x:%x:%x:%x:%d", protocol,
            paddr->Outbound ? "OUT " : "IN  ", src_addr6[0], src_addr6[1], src_addr6[2],
            src_addr6[3], src_addr6[4], src_addr6[5], src_addr6[6], src_addr6[7], srcPort,
            dst_addr6[0], dst_addr6[1], dst_addr6[2], dst_addr6[3], dst_addr6[4], dst_addr6[5],
            dst_addr6[6], dst_addr6[7], dstPort);
    }
}
#else
#define dumpPacket(x, y, z)
#endif

int divertStart(const char *filter, char buf[])
{
    int ix;

    const HANDLE openedHandle = WinDivertOpen(filter, WINDIVERT_LAYER_NETWORK, DIVERT_PRIORITY, 0);
    if (openedHandle == INVALID_HANDLE_VALUE)
    {
        DWORD lastError = GetLastError();
        if (lastError == ERROR_INVALID_PARAMETER)
        {
            strncpy(buf, "Failed to start filtering : filter syntax error.", MSG_BUFSIZE - 1);
            buf[MSG_BUFSIZE - 1] = '\0';
        }
        else
        {
            snprintf(buf, MSG_BUFSIZE,
                     "Failed to start filtering : failed to open device (code:%lu).\n"
                     "Make sure you run EpicGamesLauncher as Administrator.",
                     lastError);
        }
        return FALSE;
    }
    divertHandle.store(openedHandle, std::memory_order_release);
    LOG("Divert opened handle.");

    WinDivertSetParam(openedHandle, WINDIVERT_PARAM_QUEUE_LENGTH, QUEUE_LEN);
    WinDivertSetParam(openedHandle, WINDIVERT_PARAM_QUEUE_TIME, QUEUE_TIME);
    LOG("WinDivert internal queue Len: %d, queue time: %d", QUEUE_LEN, QUEUE_TIME);

    initPacketNodeList();

    for (ix = 0; ix < MODULE_CNT; ++ix)
    {
        modules[ix]->lastEnabled = 0;
    }

    LOG("Creating threads and mutex...");
    stopLooping.store(false, std::memory_order_release);
    mutex = CreateMutex(NULL, FALSE, NULL);
    if (mutex == NULL)
    {
        snprintf(buf, MSG_BUFSIZE, "Failed to create mutex (%lu)", GetLastError());
        WinDivertClose(divertHandle.exchange(INVALID_HANDLE_VALUE));
        cleanupPacketPool();
        return FALSE;
    }

    loopThread = CreateThread(nullptr, 0, divertReadLoop, nullptr, 0, nullptr);
    if (loopThread == NULL)
    {
        snprintf(buf, MSG_BUFSIZE, "Failed to create recv loop thread (%lu)", GetLastError());
        CloseHandle(mutex);
        mutex = NULL;
        WinDivertClose(divertHandle.exchange(INVALID_HANDLE_VALUE));
        cleanupPacketPool();
        return FALSE;
    }
    clockThread = CreateThread(nullptr, 0, divertClockLoop, nullptr, 0, nullptr);
    if (clockThread == NULL)
    {
        snprintf(buf, MSG_BUFSIZE, "Failed to create clock loop thread (%lu)", GetLastError());
        stopLooping.store(true, std::memory_order_release);
        const HANDLE handle = divertHandle.load(std::memory_order_acquire);
        if (handle != INVALID_HANDLE_VALUE)
        {
            if (!WinDivertShutdown(handle, WINDIVERT_SHUTDOWN_RECV))
                WinDivertClose(divertHandle.exchange(INVALID_HANDLE_VALUE));
        }
        WaitForSingleObject(loopThread, INFINITE);
        CloseHandle(loopThread);
        loopThread = nullptr;
        WinDivertClose(divertHandle.exchange(INVALID_HANDLE_VALUE));
        CloseHandle(mutex);
        mutex = nullptr;
        cleanupPacketPool();
        return FALSE;
    }

    LOG("Threads created");

    return TRUE;
}

static int sendAllListPackets(int maxSendCount)
{
    int sendCount = 0;
    UINT sendLen;
    PacketNode *pnode;
#ifdef _DEBUG

    PacketNode *p = head;
    do
    {
        p = p->next;
    } while (p->next);
    assert(p == tail);
#endif

    while (!isListEmpty() && sendCount < maxSendCount)
    {
        pnode = popNode(tail->prev);
        sendLen = 0;
        assert(pnode != head);

        const HANDLE handle = divertHandle.load(std::memory_order_acquire);
        if (handle == INVALID_HANDLE_VALUE ||
            !WinDivertSend(handle, pnode->packet, pnode->packetLen, &sendLen, &(pnode->addr)))
        {
            PWINDIVERT_ICMPHDR icmp_header = NULL;
            PWINDIVERT_ICMPV6HDR icmpv6_header = NULL;
            PWINDIVERT_IPHDR ip_header = NULL;
            PWINDIVERT_IPV6HDR ipv6_header = NULL;
            DWORD lastError = GetLastError();

#ifdef _DEBUG
            LOG("Failed to send a packet. (%lu)", lastError);
            dumpPacket(pnode->packet, pnode->packetLen, &(pnode->addr));
#endif

            BOOL parsedOk = WinDivertHelperParsePacket(
                pnode->packet, pnode->packetLen, &ip_header, &ipv6_header, NULL, &icmp_header,
                &icmpv6_header, NULL, NULL, NULL, NULL, NULL, NULL);
            if (parsedOk && (icmp_header || icmpv6_header) && !pnode->addr.Outbound)
            {
                BOOL resent;
                pnode->addr.Outbound = TRUE;
                if (ip_header)
                {
                    UINT32 tmp = ip_header->SrcAddr;
                    ip_header->SrcAddr = ip_header->DstAddr;
                    ip_header->DstAddr = tmp;
                }
                else if (ipv6_header)
                {
                    UINT32 tmpArr[4];
                    memcpy(tmpArr, ipv6_header->SrcAddr, sizeof(tmpArr));
                    memcpy(ipv6_header->SrcAddr, ipv6_header->DstAddr, sizeof(tmpArr));
                    memcpy(ipv6_header->DstAddr, tmpArr, sizeof(tmpArr));
                }
                resent = WinDivertSend(handle, pnode->packet, pnode->packetLen, &sendLen,
                                       &(pnode->addr));
#ifdef _DEBUG
                LOG("Resend failed inbound ICMP packets as outbound: %s",
                    resent ? "SUCCESS" : "FAIL");
#endif
                if (resent)
                {
                    InterlockedExchange16(&sendState, SEND_STATUS_SEND);
                }
                else
                {
                    InterlockedExchange16(&sendState, SEND_STATUS_FAIL);
                }
            }
            else if (lastError == ERROR_INSUFFICIENT_BUFFER || lastError == ERROR_INVALID_DATA)
            {
                Sleep(1);
                if (WinDivertSend(handle, pnode->packet, pnode->packetLen, &sendLen,
                                  &(pnode->addr)))
                {
                    InterlockedExchange16(&sendState, SEND_STATUS_SEND);
                }
                else
                {
                    InterlockedExchange16(&sendState, SEND_STATUS_FAIL);
                }
            }
            else
            {
                InterlockedExchange16(&sendState, SEND_STATUS_FAIL);
            }
        }
        else
        {
            if (sendLen < pnode->packetLen)
            {
                LOG("Internal Error: DivertSend truncated send packet.");
                InterlockedExchange16(&sendState, SEND_STATUS_FAIL);
            }
            else
            {
                InterlockedExchange16(&sendState, SEND_STATUS_SEND);
            }
        }

        freeNode(pnode);
        ++sendCount;
    }
    assert(isListEmpty() || sendCount >= maxSendCount);

    return sendCount;
}

static void divertConsumeStep()
{
#ifdef _DEBUG
    DWORD startTick = GetTickCount(), dt;
#endif
    int ix;
    bool pacedModuleDrain = false;
    for (ix = 0; ix < MODULE_CNT; ++ix)
    {
        Module *module = modules[ix];
        if (atomicLoad16(module->enabledFlag))
        {
            if (!module->lastEnabled)
            {
                module->startUp();
                module->lastEnabled = 1;
            }
            if (module->process(head, tail))
            {
                InterlockedIncrement16(&(module->processTriggered));
            }
        }
        else
        {
            if (module->lastEnabled)
            {
                if (module->closeDown(head, tail))
                    pacedModuleDrain = true;
                else
                    module->lastEnabled = 0;
            }
        }
    }
    const int sentCount =
        sendAllListPackets(pacedModuleDrain ? DRAIN_SEND_PER_CYCLE : NORMAL_SEND_PER_CYCLE);
#ifdef _DEBUG
    dt = GetTickCount() - startTick;
    if (dt > CLOCK_WAITMS / 2)
    {
        LOG("Costy consume step: %lu ms, sent %d packets", GetTickCount() - startTick, sentCount);
    }
#else
    (void)sentCount;
#endif
}

static DWORD divertClockLoop(LPVOID arg)
{
    DWORD startTick, stepTick, waitResult;
    int ix;

    UNREFERENCED_PARAMETER(arg);

    for (;;)
    {
        startTick = GetTickCount();
        waitResult = WaitForSingleObject(mutex, CLOCK_WAITMS);
        switch (waitResult)
        {
            case WAIT_OBJECT_0:
                divertConsumeStep();
                if (!ReleaseMutex(mutex))
                {
                    stopLooping.store(true, std::memory_order_release);
                    LOG("Fatal: Failed to release mutex (%lu)", GetLastError());
                    assert(0);
                }
                stepTick = GetTickCount() - startTick;
                if (stepTick < CLOCK_WAITMS)
                {
                    Sleep(CLOCK_WAITMS - stepTick);
                }
                break;
            case WAIT_TIMEOUT:
                Sleep(CLOCK_WAITMS);
                break;
            case WAIT_ABANDONED:
                LOG("Acquired abandoned mutex");
                stopLooping.store(true, std::memory_order_release);
                if (!ReleaseMutex(mutex))
                {
                    LOG("Fatal: Failed to release mutex after WAIT_ABANDONED (%lu)",
                        GetLastError());
                }
                break;
            case WAIT_FAILED:
                LOG("Acquire failed (%lu)", GetLastError());
                stopLooping.store(true, std::memory_order_release);
                break;
        }

        if (stopLooping.load(std::memory_order_acquire))
        {
            int totalSendCount = 0;
            BOOL closed;

            waitResult = WaitForSingleObject(mutex, INFINITE);
            if (waitResult != WAIT_OBJECT_0)
            {
                LOG("Acquire failed/abandoned mutex during shutdown (%lu). Aborting clean close.",
                    GetLastError());
                return 0;
            }

            LOG("Read stopLooping, stopping...");
            bool drainPending;
            do
            {
                drainPending = false;
                for (ix = 0; ix < MODULE_CNT; ++ix)
                {
                    Module *module = modules[ix];
                    if (module->lastEnabled)
                    {
                        if (module->closeDown(head, tail))
                            drainPending = true;
                        else
                            module->lastEnabled = 0;
                    }
                }

                totalSendCount += sendAllListPackets(DRAIN_SEND_PER_CYCLE);
                if (drainPending || !isListEmpty())
                    Sleep(DRAIN_WAIT_MS);
            } while (drainPending || !isListEmpty());
            LOG("Paced shutdown sent %d packets. Closing...", totalSendCount);

            HANDLE h = divertHandle.exchange(INVALID_HANDLE_VALUE, std::memory_order_acq_rel);
            if (h != INVALID_HANDLE_VALUE)
            {
                closed = WinDivertClose(h);
                if (!closed)
                    LOG("Failed to close divert handle");
            }

            if (!ReleaseMutex(mutex))
            {
                LOG("Fatal: Failed to release mutex (%lu)", GetLastError());
                assert(0);
            }
            return 0;
        }
    }
}

static DWORD divertReadLoop(LPVOID arg)
{
    char packetBuf[MAX_PACKETSIZE];
    WINDIVERT_ADDRESS addrBuf;
    UINT readLen;
    PacketNode *pnode;
    DWORD waitResult;

    UNREFERENCED_PARAMETER(arg);

    for (;;)
    {
#ifdef _DEBUG
        if (!isListEmpty())
            LOG("List not empty at start of read loop");
#endif

        const HANDLE handle = divertHandle.load(std::memory_order_acquire);
        if (handle == INVALID_HANDLE_VALUE ||
            !WinDivertRecv(handle, packetBuf, MAX_PACKETSIZE, &readLen, &addrBuf))
        {
            DWORD lastError = GetLastError();
            if (stopLooping.load(std::memory_order_acquire) || lastError == ERROR_INVALID_HANDLE ||
                lastError == ERROR_OPERATION_ABORTED || lastError == ERROR_NO_DATA)
            {
                LOG("Handle died or operation aborted. Exit loop.");
                return 0;
            }
            LOG("Failed to recv a packet. (%lu)", GetLastError());
            continue;
        }
        if (readLen > MAX_PACKETSIZE)
        {
            LOG("Internal Error: DivertRecv truncated recv packet.");
        }

        waitResult = WaitForSingleObject(mutex, INFINITE);
        switch (waitResult)
        {
            case WAIT_OBJECT_0:
                if (stopLooping.load(std::memory_order_acquire))
                {
                    LOG("Lost last recved packet but user stopped. Stop read loop.");
                    if (!ReleaseMutex(mutex))
                    {
                        LOG("Fatal: Failed to release mutex on stopping (%lu). Will stop anyway.",
                            GetLastError());
                    }
                    return 0;
                }
                pnode = createNode(packetBuf, readLen, &addrBuf);
                if (pnode)
                {
                    appendNode(pnode);
#ifdef _DEBUG
                    InterlockedIncrement(&packetsReceived);
#endif
                }
                else
                {
                    LOG("Packet dropped: allocation failed (pool exhausted)");
                }
                divertConsumeStep();
                if (!ReleaseMutex(mutex))
                {
                    LOG("Fatal: Failed to release mutex (%lu)", GetLastError());
                    assert(0);
                }
                break;
            case WAIT_TIMEOUT:
                LOG("Acquire timeout, dropping one read packet");
                continue;
            case WAIT_ABANDONED:
                LOG("Acquire abandoned.");
                return 0;
            case WAIT_FAILED:
                LOG("Acquire failed.");
                return 0;
        }
    }
}

void divertStop()
{
    LOG("Stopping divert system...");

    stopLooping.store(true, std::memory_order_release);

    HANDLE h = divertHandle.load(std::memory_order_acquire);
    if (h != INVALID_HANDLE_VALUE)
    {
        if (!WinDivertShutdown(h, WINDIVERT_SHUTDOWN_RECV))
        {
            LOG("WinDivert receive shutdown failed (%lu); forcing handle close", GetLastError());
            h = divertHandle.exchange(INVALID_HANDLE_VALUE, std::memory_order_acq_rel);
            if (h != INVALID_HANDLE_VALUE)
                WinDivertClose(h);
        }
    }

    if (loopThread || clockThread)
    {
        HANDLE threads[2]{};
        DWORD threadCount = 0;
        if (loopThread)
            threads[threadCount++] = loopThread;
        if (clockThread)
            threads[threadCount++] = clockThread;

        WaitForMultipleObjects(threadCount, threads, TRUE, INFINITE);
        for (DWORD index = 0; index < threadCount; ++index)
            CloseHandle(threads[index]);
        loopThread = nullptr;
        clockThread = nullptr;
    }

    h = divertHandle.exchange(INVALID_HANDLE_VALUE, std::memory_order_acq_rel);
    if (h != INVALID_HANDLE_VALUE)
        WinDivertClose(h);

    if (mutex)
    {
        CloseHandle(mutex);
        mutex = nullptr;
    }

    LOG("Divert system stopped successfully.");
}
