#include "nx_threads.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#define THREADS_TBL "orchestration_v2_projection_threads"
#define EVENTS_TBL  "orchestration_events"

int nx_threads_db_path(char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (!home || !*home || !out || cap == 0) return -1;
    snprintf(out, cap, "%s/.deepai-code/userdata/statev2.sqlite", home);
    return 0;
}

static sqlite3 *open_db(const char *db, int rw) {
    char path[1024];
    if (!db || !*db) {
        if (nx_threads_db_path(path, sizeof(path)) != 0) return NULL;
        db = path;
    }
    sqlite3 *h = NULL;
    int flags = rw ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY;
    if (sqlite3_open_v2(db, &h, flags, NULL) != SQLITE_OK) {
        if (h) sqlite3_close(h);
        return NULL;
    }
    sqlite3_busy_timeout(h, 4000);
    return h;
}

/* JSON 文字列としてエスケープした 1 行を作る (payload 用)。 */
static void json_escape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    if (!out || cap == 0) return;
    for (; s && *s && o + 2 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\r' || c == '\t') { out[o++] = ' '; }
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

static void now_iso(char *out, size_t cap) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm g;
    gmtime_r(&tv.tv_sec, &g);
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
             g.tm_hour, g.tm_min, g.tm_sec, (int)(tv.tv_usec / 1000));
}

static void norm(char *s) {
    if (!s) return;
    char *w = s, *r = s;
    int sp = 1;
    for (; *r; r++) {
        unsigned char c = (unsigned char)*r;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!sp) { *w++ = ' '; sp = 1; }
        } else { *w++ = (char)c; sp = 0; }
    }
    while (w > s && w[-1] == ' ') w--;
    *w = 0;
    /* 末尾の ? / ？ の差で重複を見逃さない */
    while (w > s && (w[-1] == '?' || w[-1] == ' ')) w--;
    *w = 0;
}

static long long parse_epoch(const char *s) {
    if (!s) return 0;
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 3) return 0;
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = se;
    return (long long)timegm(&tm) * 1000;
}

typedef struct {
    char id[128];
    char title[512];
    char created[64];
    char updated[64];
    int deleted;
} row_t;

static int load_rows(sqlite3 *h, row_t **out, int *n_out) {
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT thread_id, coalesce(title,''), coalesce(created_at,''),"
             " coalesce(updated_at,''), (deleted_at IS NOT NULL)"
             " FROM %s ORDER BY created_at", THREADS_TBL);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) != SQLITE_OK) return -1;

    int n = 0, cap = 64;
    row_t *rows = (row_t *)calloc((size_t)cap, sizeof(row_t));
    if (!rows) { sqlite3_finalize(st); return -1; }
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            cap *= 2;
            row_t *p = (row_t *)realloc(rows, (size_t)cap * sizeof(row_t));
            if (!p) break;
            rows = p;
        }
        row_t *r = &rows[n++];
        memset(r, 0, sizeof(*r));
        const unsigned char *v;
        if ((v = sqlite3_column_text(st, 0))) snprintf(r->id, sizeof(r->id), "%s", (const char *)v);
        if ((v = sqlite3_column_text(st, 1))) snprintf(r->title, sizeof(r->title), "%s", (const char *)v);
        if ((v = sqlite3_column_text(st, 2))) snprintf(r->created, sizeof(r->created), "%s", (const char *)v);
        if ((v = sqlite3_column_text(st, 3))) snprintf(r->updated, sizeof(r->updated), "%s", (const char *)v);
        r->deleted = sqlite3_column_int(st, 4);
        norm(r->title);
    }
    sqlite3_finalize(st);
    *out = rows;
    *n_out = n;
    return 0;
}

int nx_threads_list(const char *db, FILE *out, int include_deleted) {
    sqlite3 *h = open_db(db, 0);
    if (!h) { fprintf(out, "threads: db を開けない (readonly)\n"); return -1; }
    row_t *rows = NULL;
    int n = 0;
    int rc = load_rows(h, &rows, &n);
    sqlite3_close(h);
    if (rc != 0) { free(rows); return -1; }

    fprintf(out, "THREADS\t%d\n", n);
    for (int i = 0; i < n; i++) {
        if (rows[i].deleted && !include_deleted) continue;
        fprintf(out, "%s\t%s\t%.60s\n", rows[i].id,
                rows[i].deleted ? "deleted" : "live", rows[i].title);
    }
    free(rows);
    return 0;
}

/* 1 件を「アプリの削除と同じ」2 段で消す。 */
int nx_threads_delete(const char *db, const char *id, FILE *out) {
    if (!id || !*id) return -1;
    sqlite3 *h = open_db(db, 1);
    if (!h) { fprintf(out, "threads: db を開けない (rw)\n"); return -1; }

    char title[512] = "", project[128] = "", stream[160] = "";
    sqlite3_stmt *st = NULL;
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT coalesce(title,''), coalesce(project_id,'') FROM %s WHERE thread_id=?",
             THREADS_TBL);
    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *v = sqlite3_column_text(st, 0);
            if (v) snprintf(title, sizeof(title), "%s", (const char *)v);
            v = sqlite3_column_text(st, 1);
            if (v) snprintf(project, sizeof(project), "%s", (const char *)v);
        }
        sqlite3_finalize(st);
    }
    snprintf(stream, sizeof(stream), "%s", id);

    /* 既存の thread.deleted があれば二重追記しない。 */
    int already = 0;
    snprintf(sql, sizeof(sql),
             "SELECT 1 FROM %s WHERE stream_id=? AND event_type='thread.deleted' LIMIT 1",
             EVENTS_TBL);
    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, stream, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) already = 1;
        sqlite3_finalize(st);
    }

    sqlite3_exec(h, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    int ok = 0;
    if (!already) {
        long long seq = 0, ver = 0;
        sqlite3_prepare_v2(h, "SELECT coalesce(max(sequence),0) FROM " EVENTS_TBL, -1, &st, NULL);
        if (sqlite3_step(st) == SQLITE_ROW) seq = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        snprintf(sql, sizeof(sql),
                 "SELECT coalesce(max(stream_version),0) FROM %s WHERE stream_id=?",
                 EVENTS_TBL);
        if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, stream, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW) ver = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }

        char etitle[1024], eproject[256], now[64], evid[64];
        json_escape(title, etitle, sizeof(etitle));
        json_escape(project, eproject, sizeof(eproject));
        now_iso(now, sizeof(now));
        struct timeval tv; gettimeofday(&tv, NULL);
        snprintf(evid, sizeof(evid), "nx-%08x-%08x-%08x",
                 (unsigned)tv.tv_sec, (unsigned)tv.tv_usec, (unsigned)rand());

        char payload[1600];
        snprintf(payload, sizeof(payload),
                 "{\"createdBy\":\"user\",\"creationSource\":\"web\",\"id\":\"%s\","
                 "\"projectId\":\"%s\",\"title\":\"%s\"}", id, eproject, etitle);

        snprintf(sql, sizeof(sql),
                 "INSERT INTO %s (sequence,event_id,aggregate_kind,stream_id,"
                 "stream_version,event_type,occurred_at,actor_kind,payload_json,"
                 "metadata_json,application_event_version) "
                 "VALUES (?,?,?,?,?,?,?,?,?,?,1)", EVENTS_TBL);
        if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, seq + 1);
            sqlite3_bind_text(st, 2, evid, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 3, "thread", -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 4, stream, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 5, ver + 1);
            sqlite3_bind_text(st, 6, "thread.deleted", -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 7, now, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 8, "server", -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 9, payload, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 10, "{\"providerInstanceId\":\"deepai\"}", -1, SQLITE_STATIC);
            ok = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
        }
    } else {
        ok = 1;
    }

    /* projection も同じ値に (即座にサイドバーから消える)。 */
    snprintf(sql, sizeof(sql),
             "UPDATE %s SET deleted_at=?, archived_at=? WHERE thread_id=?", THREADS_TBL);
    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK) {
        char now[64];
        now_iso(now, sizeof(now));
        sqlite3_bind_text(st, 1, now, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, now, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, id, -1, SQLITE_TRANSIENT);
        ok = ok && sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    sqlite3_exec(h, ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    sqlite3_close(h);
    fprintf(out, "%s\t%s\n", ok ? "DELETED" : "FAILED", id);
    return ok ? 0 : -1;
}

static int protected_id(const char *csv, const char *id) {
    if (!csv || !*csv || !id) return 0;
    size_t n = strlen(id);
    for (const char *p = csv; *p; ) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && strncmp(p, id, n) == 0) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

int nx_threads_dups(const char *db, FILE *out, int within_sec, int apply,
                    const char *protect_csv) {
    sqlite3 *h = open_db(db, 0);
    if (!h) { fprintf(out, "threads: db を開けない\n"); return -1; }
    row_t *rows = NULL;
    int n = 0;
    int rc = load_rows(h, &rows, &n);
    sqlite3_close(h);
    if (rc != 0) { free(rows); return -1; }

    int marked = 0;
    for (int i = 0; i < n; i++) {
        if (rows[i].deleted || !rows[i].title[0]) continue;
        /* 同じタイトルのグループを作る。残す基準は updated_at ではない。
           複製間の差は同期の遅れだけなので、意味のある差ではない。
           - 現在タブが参照しているもの (protect_csv) があればそれを残す
           - なければ最初に作られたもの (原本) を残す */
        int keeper = -1;
        int members[64];
        int nm = 0, live = 0;
        members[nm++] = i;
        live = 1;
        for (int j = i + 1; j < n; j++) {
            if (rows[j].deleted || !rows[j].title[0]) continue;
            if (strcmp(rows[i].title, rows[j].title) != 0) continue;
            long long a = parse_epoch(rows[i].created), b = parse_epoch(rows[j].created);
            if (a && b && (b - a) > (long long)within_sec * 1000) continue;
            if (nm < 64) members[nm++] = j;
            live++;
        }
        /* 最後の 1 つを消してはいけない (会話自体が消える)。 */
        if (live < 2) continue;

        /* 残す基準: タブが参照しているもの → なければ原本 (最古)。
           updated_at は使わない (複製間の差は同期の遅れだけ)。 */
        for (int k = 0; k < nm; k++)
            if (protected_id(protect_csv, rows[members[k]].id)) { keeper = members[k]; break; }
        if (keeper < 0) {
            keeper = members[0];
            for (int k = 1; k < nm; k++) {
                int j = members[k];
                long long cj = parse_epoch(rows[j].created);
                long long ck = parse_epoch(rows[keeper].created);
                if (cj && (!ck || cj < ck)) keeper = j;
            }
        }
        if (keeper < 0 || rows[keeper].deleted) continue;
        for (int k = 0; k < nm; k++) {
            int j = members[k];
            if (j == keeper) continue;
            fprintf(out, "DUP\t%s\t%s\t%.60s\n", rows[j].id, rows[keeper].id, rows[j].title);
            if (apply && nx_threads_delete(db, rows[j].id, out) == 0) marked++;
            rows[j].deleted = 1;
        }
    }
    if (apply) fprintf(out, "MARKED\t%d\n", marked);
    free(rows);
    return 0;
}
