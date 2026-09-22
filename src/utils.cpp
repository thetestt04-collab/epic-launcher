#include <atomic>
#include <cstdlib>
#include <random>
#include <Windows.h>
#include "iup.h"
#include "common.h"

short calcChance(short chance)
{
    if (chance <= 0)
        return FALSE;
    if (chance >= 10000)
        return TRUE;

    thread_local std::mt19937 generator{std::random_device{}()};
    std::uniform_int_distribution<short> distribution(0, 9999);
    return distribution(generator) < chance;
}

static std::atomic_bool resolutionSet{false};

void startTimePeriod()
{
    bool expected = false;
    if (resolutionSet.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        if (timeBeginPeriod(TIMER_RESOLUTION) != TIMERR_NOERROR)
            resolutionSet.store(false, std::memory_order_release);
    }
}

void endTimePeriod()
{
    if (resolutionSet.exchange(false, std::memory_order_acq_rel))
    {
        timeEndPeriod(TIMER_RESOLUTION);
    }
}

int uiSyncChance(Ihandle *ih)
{
    char valueBuf[32];
    float value = IupGetFloat(ih, "VALUE"), newValue = value;
    short *chancePtr = (short *)IupGetAttribute(ih, SYNCED_VALUE);
    if (newValue > 100.0f)
    {
        newValue = 100.0f;
    }
    else if (newValue < 0)
    {
        newValue = 0.0f;
    }
    if (newValue != value)
    {
        snprintf(valueBuf, sizeof(valueBuf), "%.1f", newValue);
        IupStoreAttribute(ih, "VALUE", valueBuf);
        IupStoreAttribute(ih, "CARET", "10");
    }
    InterlockedExchange16(chancePtr, (short)(newValue * 100));
    return IUP_DEFAULT;
}

int uiSyncInt32(Ihandle *ih)
{
    LONG *integerPointer = (LONG *)IupGetAttribute(ih, SYNCED_VALUE);
    const int maxValue = IupGetInt(ih, INTEGER_MAX);
    const int minValue = IupGetInt(ih, INTEGER_MIN);
    int value = IupGetInt(ih, "VALUE"), newValue = value;
    char valueBuf[32];
    if (newValue > maxValue)
    {
        newValue = maxValue;
    }
    else if (newValue < minValue)
    {
        newValue = minValue;
    }
    if (newValue != value && value != 0)
    {
        snprintf(valueBuf, sizeof(valueBuf), "%d", newValue);
        IupStoreAttribute(ih, "VALUE", valueBuf);
        IupStoreAttribute(ih, "CARET", "10");
    }
    InterlockedExchange(integerPointer, newValue);
    return IUP_DEFAULT;
}

int uiSyncToggle(Ihandle *ih, int state)
{
    short *togglePtr = (short *)IupGetAttribute(ih, SYNCED_VALUE);
    InterlockedExchange16(togglePtr, I2S(state));
    return IUP_DEFAULT;
}

int uiSyncInteger(Ihandle *ih)
{
    short *integerPointer = (short *)IupGetAttribute(ih, SYNCED_VALUE);
    const int maxValue = IupGetInt(ih, INTEGER_MAX);
    const int minValue = IupGetInt(ih, INTEGER_MIN);
    int value = IupGetInt(ih, "VALUE"), newValue = value;
    char valueBuf[32];
    if (newValue > maxValue)
    {
        newValue = maxValue;
    }
    else if (newValue < minValue)
    {
        newValue = minValue;
    }
    if (newValue != value && value != 0)
    {
        snprintf(valueBuf, sizeof(valueBuf), "%d", newValue);
        IupStoreAttribute(ih, "VALUE", valueBuf);
        IupStoreAttribute(ih, "CARET", "10");
    }
    InterlockedExchange16(integerPointer, (short)newValue);
    return IUP_DEFAULT;
}

int uiSyncFixed(Ihandle *ih)
{
    short *fixedPointer = (short *)IupGetAttribute(ih, SYNCED_VALUE);
    const float maxFixedValue = IupGetFloat(ih, FIXED_MAX);
    const float minFixedValue = IupGetFloat(ih, FIXED_MIN);
    float value = IupGetFloat(ih, "VALUE");
    float newValue = value;
    short fixValue;
    char valueBuf[32];
    if (newValue > maxFixedValue)
    {
        newValue = maxFixedValue;
    }
    else if (newValue < minFixedValue)
    {
        newValue = minFixedValue;
    }

    if (newValue != value && value != 0)
    {
        snprintf(valueBuf, sizeof(valueBuf), "%.2f", newValue);
        IupStoreAttribute(ih, "VALUE", valueBuf);
        IupStoreAttribute(ih, "CARET", "10");
    }
    fixValue = (short)(newValue / FIXED_EPSILON);
    InterlockedExchange16(fixedPointer, fixValue);
    return IUP_DEFAULT;
}

const unsigned char icon8x8[8 * 8] = {
    0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 1, 0, 0,
};

typedef int (*IstateCallback)(Ihandle *ih, int state);
void setFromParameter(Ihandle *ih, const char *field, const char *key)
{
    char *val = IupGetGlobal(key);
    Icallback cb;
    IstateCallback scb;
    if (val)
    {
        IupSetAttribute(ih, field, val);
        cb = IupGetCallback(ih, "VALUECHANGED_CB");
        if (cb)
        {
            LOG("triggered VALUECHANGED_CB on key: %s", key);
            cb(ih);
            return;
        }
        scb = (IstateCallback)IupGetCallback(ih, "ACTION");
        if (scb)
        {
            LOG("triggered ACTION on key: %s", key);
            scb(ih, IupGetInt(ih, "VALUE"));
            return;
        }
    }
}

BOOL parseArgs(int argc, char *argv[])
{
    int ix = 0;
    char *key, *value;
    if (argc == 1)
        return 0;
    for (;;)
    {
        if (++ix >= argc)
            break;
        key = argv[ix];
        if (key[0] != '-' || key[1] != '-' || key[2] == '\0')
        {
            return 0;
        }
        key = &(key[2]);
        if (++ix >= argc)
        {
            return 0;
        }
        value = argv[ix];
        IupStoreGlobal(key, value);
        LOG("option: %s : %s", key, value);
    }

    return 1;
}
