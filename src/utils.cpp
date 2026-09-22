#include <atomic>
#include <Windows.h>
#include "iup.h"
#include "common.h"

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
