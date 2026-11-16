/* ================================================================
 * os/win — Windows でしかできない操作 (署名 / 管理者権限)
 *
 * mac 版 (os/mac/nx_priv.cpp) と同じ関数を実装する。
 * 分析側 (integration/) は OS を意識しない。
 * ================================================================ */

#ifdef _WIN32

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#include <windows.h>
#include <shellapi.h>

// Windows には ad-hoc 署名が無い。証明書がある場合のみ signtool で署名する。
extern "C" int nx_resign(const char *path, FILE *out) {
    if (!out) out = stdout;
    if (!path || !*path) return 2;
    std::string q = "\"";
    for (const char *p = path; *p; p++) { if (*p == '"') q += "\\\""; else q += *p; }
    q += "\"";
    std::string cmd = "signtool sign /a " + q + " 2>&1";
    int rc = std::system(cmd.c_str());
    std::fprintf(out, "signtool rc=%d\n", rc);
    return rc == 0 ? 0 : 1;
}

// 管理者権限で move する。まず通常の MoveFileEx、失敗したら UAC 昇格。
extern "C" int nx_move_auth(const char *src, const char *dst, FILE *out) {
    if (!out) out = stdout;
    if (!src || !dst) return 2;
    if (MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        std::fprintf(out, "move 完了 (root): %s -> %s\n", src, dst);
        return 0;
    }
    char cmd[4096];
    std::snprintf(cmd, sizeof(cmd),
                  "powershell -NoProfile -Command \"Start-Process cmd -ArgumentList "
                  "'/c move /Y \\\"%s\\\" \\\"%s\\\"' -Verb RunAs -Wait\"", src, dst);
    int rc = std::system(cmd);
    if (rc == 0) {
        std::fprintf(out, "move 完了 (root): %s -> %s\n", src, dst);
        return 0;
    }
    std::fprintf(out, "権限実行 失敗: %d\n", rc);
    return 1;
}

// 管理者権限で任意のツールを一度だけ実行する。
extern "C" int nx_priv_exec(const char *tool, char *const args[], FILE *out) {
    if (!out) out = stdout;
    if (!tool || !*tool) {
        std::fprintf(out, "使い方: neodax --priv-run <tool> [args...]\n");
        return 2;
    }
    std::string params;
    for (int i = 0; args && args[i]; i++) {
        params += " \"";
        params += args[i];
        params += "\"";
    }
    SHELLEXECUTEINFOA sei;
    std::memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = "runas";
    sei.lpFile = tool;
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExA(&sei)) {
        std::fprintf(out, "権限実行 失敗: %lu\n", (unsigned long)GetLastError());
        return 1;
    }
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        DWORD rc = 0;
        GetExitCodeProcess(sei.hProcess, &rc);
        CloseHandle(sei.hProcess);
        std::fprintf(out, "priv-run 完了: %s (status=%lu)\n", tool, (unsigned long)rc);
        return 0;
    }
    std::fprintf(out, "priv-run 完了: %s (status=0)\n", tool);
    return 0;
}

#endif /* _WIN32 */
