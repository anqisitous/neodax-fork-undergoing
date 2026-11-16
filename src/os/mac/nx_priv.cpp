/* ================================================================
 * os/mac — macOS でしかできない操作 (署名 / 管理者権限)
 *
 * integration/nx_track.cpp から切り出した。挙動は同一 (コードを移しただけ)。
 * Windows 版は os/win/nx_priv.cpp が同じ関数を実装する。
 * ================================================================ */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <sys/stat.h>
#include <Security/Authorization.h>
#include <Security/AuthorizationTags.h>

// ad-hoc 再署名 (codesign --force --sign -) + 検証。
extern "C" int nx_resign(const char *path, FILE *out) {
    if (!out) out = stdout;
    if (!path || !*path) return 2;
    std::string q = "'";
    for (const char *p = path; *p; p++) { if (*p == '\'') q += "'\\''"; else q += *p; }
    q += "'";
    std::string sign = "codesign --force --sign - " + q;
    int rc = std::system(sign.c_str());
    std::fprintf(out, "codesign rc=%d\n", rc);
    std::string verify = "codesign --verify --strict " + q + " 2>&1";
    int vr = std::system(verify.c_str());
    std::fprintf(out, "verify rc=%d\n", vr);
    return (rc == 0 && vr == 0) ? 0 : 1;
}

// ---------------------------------------------------------------- move (auth)
// Authorization Services で root として mv する。Finder も AppleScript も使わない。
// 初回のみ管理者パスワードのダイアログが出る。
extern "C" int nx_move_auth(const char *src, const char *dst, FILE *out) {
    if (!out) out = stdout;
    if (!src || !dst) return 2;
    AuthorizationRef auth = NULL;
    OSStatus st = AuthorizationCreate(NULL, kAuthorizationEmptyEnvironment,
                                      kAuthorizationFlagDefaults, &auth);
    if (st != errAuthorizationSuccess) {
        std::fprintf(out, "AuthorizationCreate 失敗: %d\n", (int)st);
        return 1;
    }
    char *args[3];
    args[0] = (char *)src;
    args[1] = (char *)dst;
    args[2] = NULL;
    FILE *pipe = NULL;
    st = AuthorizationExecuteWithPrivileges(auth, "/bin/mv",
                                           kAuthorizationFlagDefaults, args, &pipe);
    if (st != errAuthorizationSuccess) {
        std::fprintf(out, "権限実行 失敗: %d (-60006 はキャンセル)\n", (int)st);
        AuthorizationFree(auth, kAuthorizationFlagDefaults);
        return 1;
    }
    if (pipe) {
        char buf[256];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, pipe)) > 0) fwrite(buf, 1, n, out);
        pclose(pipe);
    }
    AuthorizationFree(auth, kAuthorizationFlagDestroyRights);
    struct stat s2;
    if (stat(dst, &s2) == 0) {
        std::fprintf(out, "move 完了 (root): %s -> %s\n", src, dst);
        return 0;
    }
    std::fprintf(out, "move 失敗: dst がありません\n");
    return 1;
}

// ---------------------------------------------------------------- priv-run
// 管理者パスワードのダイアログを出し、任意のツールを root で一度だけ実行する。
// 「パスワード許可の開始手続き」を neodax 自身が持つ。move / chown / その他に使う。
extern "C" int nx_priv_exec(const char *tool, char *const args[], FILE *out) {
    if (!out) out = stdout;
    if (!tool || !*tool) {
        std::fprintf(out, "使い方: neodax --priv-run <tool> [args...]\n");
        return 2;
    }
    AuthorizationRef auth = NULL;
    OSStatus st = AuthorizationCreate(NULL, kAuthorizationEmptyEnvironment,
                                      kAuthorizationFlagDefaults, &auth);
    if (st != errAuthorizationSuccess) {
        std::fprintf(out, "AuthorizationCreate 失敗: %d\n", (int)st);
        return 1;
    }
    FILE *pipe = NULL;
    st = AuthorizationExecuteWithPrivileges(auth, tool, kAuthorizationFlagDefaults,
                                           args, &pipe);
    if (st != errAuthorizationSuccess) {
        std::fprintf(out, "権限実行 失敗: %d (-60006 はキャンセル)\n", (int)st);
        AuthorizationFree(auth, kAuthorizationFlagDefaults);
        return 1;
    }
    if (pipe) {
        char b[512];
        size_t n;
        while ((n = fread(b, 1, sizeof b, pipe)) > 0) fwrite(b, 1, n, out);
    }
    int rc = pipe ? pclose(pipe) : 0;
    AuthorizationFree(auth, kAuthorizationFlagDestroyRights);
    std::fprintf(out, "priv-run 完了: %s (status=%d)\n", tool, rc);
    return 0;
}
