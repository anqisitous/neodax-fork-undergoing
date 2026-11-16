/* ================================================================
 * os/win — Windows でしかできない操作 (プロセス解決 + inspector 起動)
 *
 * mac 版 (os/mac/nx_proc.c) と同じ 3 つの関数を実装する。
 * 分析側 (integration/) は OS を意識しない。
 * ================================================================ */

#ifdef _WIN32

#include "nx_inspect.h"

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* プロセス名の一部から PID。本体 (Helper でない) を優先する。 */
long nx_inspect_find_pid(const char *app_name) {
    if (!app_name || !*app_name) app_name = "electron";
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    PROCESSENTRY32 pe;
    memset(&pe, 0, sizeof(pe));
    pe.dwSize = sizeof(pe);

    long best = -1, fallback = -1;
    if (Process32First(snap, &pe)) {
        do {
            if (!strstr(pe.szExeFile, app_name)) continue;
            /* Helper / 子プロセスは本体ではない。最初に見つかった本体を優先。 */
            if (!strstr(pe.szExeFile, "Helper")) {
                best = (long)pe.th32ProcessID;
                break;
            }
            if (fallback < 0) fallback = (long)pe.th32ProcessID;
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return best > 0 ? best : fallback;
}

/* 実行ファイルのパスから Electron/Node かを判定する。 */
int nx_os_is_inspectable(long pid) {
    if (pid <= 0) return 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return 0;
    char path[MAX_PATH] = {0};
    DWORD n = (DWORD)sizeof(path);
    int ok = QueryFullProcessImageNameA(h, 0, path, &n) != 0;
    CloseHandle(h);
    if (!ok) return 0;

    const char *base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    if (_strnicmp(base, "node", 4) == 0) return 1;
    if (strstr(path, "electron") || strstr(path, "Electron")) return 1;
    return 0;
}

/* Windows: SIGUSR1 は無い。別の Node から process._debugProcess(pid) を
   呼ぶのが、起動中のプロセスに inspector を開かせる正規の方法。 */
int nx_os_inspector_kick(long pid) {
    if (pid <= 0) return -1;
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "node -e \"process._debugProcess(%ld)\"", pid);
    int rc = system(cmd);
    return rc == 0 ? 0 : -1;
}

int nx_os_executable_path(long pid, char *out, size_t cap) {
    if (pid <= 0 || !out || cap == 0) return -1;
    out[0] = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return -1;
    DWORD n = (DWORD)cap;
    int ok = QueryFullProcessImageNameA(h, 0, out, &n) != 0;
    CloseHandle(h);
    return ok ? 0 : -1;
}

#endif /* _WIN32 */
