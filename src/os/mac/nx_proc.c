/* ================================================================
 * os/mac — macOS でしかできない操作 (プロセス解決 + inspector 起動)
 *
 * 分析 (integration/) から切り離してある。ここだけが libproc と
 * シグナルに依存する。Windows 版は os/win/nx_proc.c が同じ関数を実装する。
 * ================================================================ */

#include "nx_inspect.h"

#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <libproc.h>

/* プロセス名の一部から PID。本体 (Helper でない) を優先する。 */
long nx_inspect_find_pid(const char *app_name) {
    if (!app_name || !*app_name) app_name = "Electron";
    int npid = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    if (npid <= 0) return -1;
    pid_t *pids = (pid_t *)malloc((size_t)npid + 1024);
    if (!pids) return -1;
    npid = proc_listpids(PROC_ALL_PIDS, 0, pids, npid + 1024);
    if (npid <= 0) { free(pids); return -1; }
    int count = npid / (int)sizeof(pid_t);
    pid_t best = -1, fallback = -1;
    for (int i = 0; i < count; i++) {
        pid_t pid = pids[i];
        if (pid <= 0) continue;
        char name[256] = {0};
        if (proc_name(pid, name, sizeof(name)) <= 0) continue;
        if (!strstr(name, app_name)) continue;
        if (!strstr(name, "Helper")) { best = pid; break; }
        if (fallback < 0) fallback = pid;
    }
    free(pids);
    return (long)(best > 0 ? best : fallback);
}

/* Electron/Node かを実行パスから判定する (SIGUSR1 を送る前の安全確認)。 */
int nx_os_is_inspectable(long pid) {
    if (pid <= 0) return 0;
    char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
    if (proc_pidpath((pid_t)pid, path, sizeof(path)) <= 0) return 0;
    if (strstr(path, "Electron Framework") != NULL) return 1;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strncmp(base, "node", 4) == 0) return 1;
    char *app = strstr(path, ".app/");
    if (app) {
        size_t n = (size_t)(app - path) + 4;
        char probe[PROC_PIDPATHINFO_MAXSIZE];
        snprintf(probe, sizeof(probe),
                 "%.*s/Contents/Frameworks/Electron Framework.framework", (int)n, path);
        struct stat st;
        if (stat(probe, &st) == 0) return 1;
    }
    return 0;
}

/* macOS: SIGUSR1 が Node/Electron の inspector を起動する。 */
int nx_os_inspector_kick(long pid) {
    if (pid <= 0) return -1;
    return kill((pid_t)pid, SIGUSR1) == 0 ? 0 : -1;
}

int nx_os_executable_path(long pid, char *out, size_t cap) {
    if (pid <= 0 || !out || cap == 0) return -1;
    out[0] = 0;
    return proc_pidpath((pid_t)pid, out, (uint32_t)cap) > 0 ? 0 : -1;
}
