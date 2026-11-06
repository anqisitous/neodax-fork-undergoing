#include "nx_payload.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

char *nx_b64_encode(const char *data, size_t len) {
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t olen = ((len + 2) / 3) * 4 + 1;
    char *out = (char *)malloc(olen);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = ((unsigned char)data[i]) << 16;
        if (i + 1 < len) v |= ((unsigned char)data[i + 1]) << 8;
        if (i + 2 < len) v |= (unsigned char)data[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? T[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? T[v & 63] : '=';
    }
    out[o] = 0;
    return out;
}

/* ストアを解決する共通前置き JS (export_names を順に試す) */
static void append_store_resolve(char *js, size_t cap, const char *store_substr,
                                 const char *export_names) {
    if (!export_names || !*export_names) export_names = "default";
    char names[256];
    snprintf(names, sizeof(names), "%s", export_names);
    char js_names[512];
    size_t o = 0;
    js_names[o++] = '[';
    char *tok = strtok(names, ",");
    int first = 1;
    while (tok) {
        o += (size_t)snprintf(js_names + o, sizeof(js_names) - o, "%s'%s'", first ? "" : ",", tok);
        first = 0;
        tok = strtok(NULL, ",");
    }
    js_names[o++] = ']';
    js_names[o] = 0;

    size_t cur = strlen(js);
    snprintf(js + cur, cap - cur,
        "const subs='%s';"
        "const urls=performance.getEntriesByType('resource').map(e=>e.name).filter(n=>n.indexOf(subs)>=0);"
        "if(!urls.length)return JSON.stringify({err:'store asset not loaded',subs});"
        "const mod=await import(urls[urls.length-1]);"
        "const candidates=%s;"
        "let store=null;"
        "for(const nm of candidates){if(mod[nm]&&typeof mod[nm].getState==='function'){store=mod[nm];break;}}"
        "if(!store){for(const k of Object.keys(mod)){const v=mod[k];if(v&&typeof v.getState==='function'&&typeof v.setState==='function'){store=v;break;}}}"
        "if(!store)return JSON.stringify({err:'no store',keys:Object.keys(mod)});",
        store_substr, js_names);
}

char *nx_payload_build_rehydrate(const char *store_substr,
                                 const char *key, const char *value, size_t value_len,
                                 const char *export_names) {
    if (!store_substr) return NULL;
    char *b64 = NULL;
    if (value && value_len > 0) b64 = nx_b64_encode(value, value_len);

    size_t cap = 8192 + (b64 ? strlen(b64) : 0);
    char *js = (char *)malloc(cap);
    if (!js) { free(b64); return NULL; }
    js[0] = 0;

    snprintf(js, cap, "(async()=>{");
    if (b64 && key) {
        size_t cur = strlen(js);
        snprintf(js + cur, cap - cur,
            "const b64='%s';"
            "if(b64){const val=decodeURIComponent(escape(atob(b64)));localStorage.setItem('%s',val);}",
            b64, key);
    }
    append_store_resolve(js, cap, store_substr, export_names);
    {
        size_t cur = strlen(js);
        snprintf(js + cur, cap - cur,
            "const before=store.getState().tabs?store.getState().tabs.length:Object.keys(store.getState()).length;"
            "if(store.persist&&store.persist.rehydrate)await store.persist.rehydrate();"
            "const after=store.getState().tabs?store.getState().tabs.length:Object.keys(store.getState()).length;"
            "return JSON.stringify({ok:true,rehydrated:!!(store.persist&&store.persist.rehydrate),before,after});"
            "})()");
    }
    free(b64);
    return js;
}

char *nx_payload_build_remove_key(const char *store_substr, const char *key,
                                  const char *export_names) {
    if (!store_substr || !key) return NULL;
    size_t cap = 8192;
    char *js = (char *)malloc(cap);
    if (!js) return NULL;
    snprintf(js, cap, "(async()=>{localStorage.removeItem('%s');", key);
    append_store_resolve(js, cap, store_substr, export_names);
    {
        size_t cur = strlen(js);
        snprintf(js + cur, cap - cur,
            "if(store.persist&&store.persist.rehydrate)await store.persist.rehydrate();"
            "return JSON.stringify({ok:true,removed:'%s'});})()", key);
    }
    return js;
}

char *nx_payload_build_read_key(const char *key) {
    if (!key) return NULL;
    size_t cap = strlen(key) + 256;
    char *js = (char *)malloc(cap);
    if (!js) return NULL;
    snprintf(js, cap, "JSON.stringify({value:localStorage.getItem('%s')})", key);
    return js;
}
