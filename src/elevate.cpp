
#include "common.h"
#include <VersionHelpers.h>
#include <Windows.h>

BOOL IsRunAsAdmin()
{
    BOOL fIsRunAsAdmin = FALSE;
    DWORD dwError = ERROR_SUCCESS;
    PSID pAdministratorsGroup = NULL;

    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &pAdministratorsGroup))
    {
        dwError = GetLastError();
        goto Cleanup;
    }

    if (!CheckTokenMembership(NULL, pAdministratorsGroup, &fIsRunAsAdmin))
    {
        dwError = GetLastError();
        goto Cleanup;
    }

Cleanup:
    if (pAdministratorsGroup)
    {
        FreeSid(pAdministratorsGroup);
        pAdministratorsGroup = NULL;
    }

    if (ERROR_SUCCESS != dwError)
    {
        return FALSE;
    }

    return fIsRunAsAdmin;
}

BOOL IsElevated()
{
    BOOL fRet = FALSE;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
    {
        TOKEN_ELEVATION Elevation;
        DWORD cbSize = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &Elevation, sizeof(Elevation), &cbSize))
        {
            fRet = Elevation.TokenIsElevated;
        }
    }
    if (hToken)
    {
        CloseHandle(hToken);
    }
    return fRet;
}

BOOL tryElevate(HWND hWnd, BOOL silent)
{
    BOOL fIsRunAsAdmin;
    if (!IsWindowsVistaOrGreater())
    {
        if (!silent)
            MessageBox(hWnd,
                       (LPCSTR) "Unsupported Windows version. EpicGamesLauncher only supports "
                                "Windows Vista or above.",
                       (LPCSTR) "Aborting", MB_OK);
        return TRUE;
    }

    fIsRunAsAdmin = IsRunAsAdmin();
    if (fIsRunAsAdmin)
    {
        return FALSE;
    }

    if (!silent)
    {
        char szPath[MAX_PATH];
        if (GetModuleFileNameA(NULL, szPath, ARRAYSIZE(szPath)))
        {
            SHELLEXECUTEINFOA sei{};
            sei.cbSize = sizeof(sei);
            sei.lpVerb = "runas";
            sei.lpFile = szPath;
            sei.hwnd = hWnd;
            sei.nShow = SW_NORMAL;

            LOG("Try elevating by runas");
            if (!ShellExecuteExA(&sei))
            {
                DWORD dwError = GetLastError();
                if (dwError == ERROR_CANCELLED)
                {
                    MessageBox(hWnd,
                               (LPCSTR) "EpicGamesLauncher needs to be elevated to work. Run as "
                                        "Administrator or click Yes in promoted UAC dialog",
                               (LPCSTR) "Aborting", MB_OK);
                }
            }
        }
        else
        {
            MessageBox(hWnd,
                       (LPCSTR) "Failed to get EpicGamesLauncher path. Please place the executable "
                                "in a normal directory.",
                       (LPCSTR) "Aborting", MB_OK);
        }
    }

    return TRUE;
}
