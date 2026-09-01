/* Portable-package launcher.
 *
 * A portable BlitzView is a directory containing the executable plus ~60 MB of
 * Qt DLLs and plugin folders. Presenting that to a user is unfriendly: the
 * thing they should double-click is buried among files they must not touch.
 *
 * So the package looks like this:
 *
 *     BlitzView.exe        <- this program
 *     app/
 *       blitzview.exe      <- the real application
 *       Qt6Core.dll, platforms/, imageformats/, ...
 *
 * This launcher does nothing but start app\blitzview.exe, forward its own
 * command line to it, and return its exit code. It is linked statically so it
 * needs no DLLs of its own — otherwise it would have the very problem it
 * exists to hide.
 *
 * Unicode throughout: media directories have accented and non-Latin names.
 */

#include <windows.h>
#include <shlwapi.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prevInstance,
                    LPWSTR commandLine, int showCommand)
{
    WCHAR ownPath[MAX_PATH];
    WCHAR appDir[MAX_PATH];
    WCHAR exePath[MAX_PATH];
    WCHAR *child = NULL;
    size_t childChars;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD exitCode = 1;

    (void)instance;
    (void)prevInstance;
    (void)showCommand;

    if (!GetModuleFileNameW(NULL, ownPath, MAX_PATH))
        return 1;

    lstrcpynW(appDir, ownPath, MAX_PATH);
    PathRemoveFileSpecW(appDir);              /* directory holding this exe */

    lstrcpynW(exePath, appDir, MAX_PATH);
    PathAppendW(exePath, L"app\\blitzview.exe");

    /* The application resolves relative paths and finds a bundled exiftool
     * next to itself, so start it with the package as the current directory. */
    SetCurrentDirectoryW(appDir);

    /* wWinMain's lpCmdLine already excludes the program name — unlike
     * GetCommandLineW(). So it IS the argument list, verbatim, and must not
     * have a "program name" stripped off it: doing that silently swallows the
     * first real argument, and BlitzView opens with nothing selected. */

    /* CreateProcessW may modify the command line it is given, so build a
     * writable copy: "exePath" + optional space + arguments. */
    childChars = lstrlenW(exePath) + lstrlenW(commandLine) + 4;
    child = (WCHAR *)LocalAlloc(LPTR, childChars * sizeof(WCHAR));
    if (!child)
        return 1;

    child[0] = L'"';
    lstrcatW(child, exePath);
    lstrcatW(child, L"\"");
    if (*commandLine) {
        lstrcatW(child, L" ");
        lstrcatW(child, commandLine);
    }

    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    ZeroMemory(&process, sizeof(process));

    if (!CreateProcessW(exePath, child, NULL, NULL, FALSE,
                        0, NULL, appDir, &startup, &process)) {
        WCHAR message[MAX_PATH + 64];
        lstrcpynW(message, L"Cannot start:\n", MAX_PATH);
        lstrcatW(message, exePath);
        MessageBoxW(NULL, message, L"BlitzView", MB_OK | MB_ICONERROR);
        LocalFree(child);
        return 1;
    }

    /* Let the child take the foreground — without this its window can open
     * behind whatever the user was looking at. */
    AllowSetForegroundWindow(process.dwProcessId);
    WaitForInputIdle(process.hProcess, 10000);

    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &exitCode);

    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    LocalFree(child);

    return (int)exitCode;
}
