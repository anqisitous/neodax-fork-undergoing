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
        "const seen=[];"
        "const add=u=>{if(u&&u.indexOf(subs)>=0&&seen.indexOf(u)<0)seen.push(u);};"
        "try{performance.getEntriesByType('resource').forEach(e=>add(e.name));}catch(e){}"
        "try{document.querySelectorAll('script[src],link[href]').forEach(el=>add(el.src||el.href));}catch(e){}"
        "if(!seen.length)return JSON.stringify({err:'store asset not loaded',subs});"
        "const mod=await import(seen[seen.length-1]);"
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
            "let stored=null;"
            "if(b64){const val=decodeURIComponent(escape(atob(b64)));"
            "try{stored=JSON.parse(val);}catch(e){stored=null;}"
            "localStorage.setItem('%s',val);}",
            b64, key);
    }
    append_store_resolve(js, cap, store_substr, export_names);
    {
        size_t cur = strlen(js);
        /* ここが肝。
           localStorage を書いて rehydrate() するだけでは足りない。
           rehydrate は localStorage を読み直すが、その後アプリが
           「メモリに残った古い状態」を次の persist で書き戻すので
           結局元に戻る (これが「消しても復活する」の本体)。
           メモリ側の state も同じ値にしないと永続化されない。 */
        snprintf(js + cur, cap - cur,
            "const cnt=st=>st.tabs?st.tabs.length:Object.keys(st).length;"
            "const before=cnt(store.getState());"
            "if(stored&&typeof stored.state==='object'&&store.setState){"
            "const s=stored.state;const patch={};"
            "for(const k of Object.keys(s))patch[k]=s[k];"
            "store.setState(patch,false);"
            "}"
            "if(store.persist&&store.persist.rehydrate)await store.persist.rehydrate();"
            "const after=cnt(store.getState());"
            "return JSON.stringify({ok:true,set:!!stored,before,after});"
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

char *nx_payload_build_stop_restore(const char *store_substr,
                                    const char *export_names,
                                    const char *key_prefix) {
    size_t cap = 8192;
    char *js = (char *)malloc(cap);
    if (!js) return NULL;
    const char *pfx = key_prefix ? key_prefix : "";

    /* 1) 復元に使われる localStorage キーを前方一致で除去する。
          後段が早期 return しても効果が残るよう、これを最初に行う。 */
    snprintf(js, cap,
        "(async()=>{const removed=[];"
        "try{for(let i=localStorage.length-1;i>=0;i--){const k=localStorage.key(i);"
        "if(k&&k.indexOf('%s')===0){localStorage.removeItem(k);removed.push(k);}}}"
        "catch(e){return JSON.stringify({ok:false,step:'localStorage',err:String(e)});}"
        "let cleared=false;",
        pfx);

    /* 2) ストアが特定できれば、その永続化内容も消す。 */
    if (store_substr && *store_substr) {
        append_store_resolve(js, cap, store_substr, export_names);
        size_t cur2 = strlen(js);
        snprintf(js + cur2, cap - cur2,
            "if(store&&store.persist&&store.persist.clearStorage){"
            "try{await store.persist.clearStorage();cleared=true;}catch(e){}}"
            "if(store&&store.persist&&store.persist.rehydrate){try{await store.persist.rehydrate();}catch(e){}}");
    }

    size_t cur = strlen(js);
    snprintf(js + cur, cap - cur,
        "return JSON.stringify({ok:true,removed,cleared,prefix:'%s'});})()", pfx);
    return js;
}

char *nx_payload_build_baseline_restore(const char *store_substr,
                                        const char *export_names,
                                        const char *key) {
    if (!store_substr) return NULL;
    size_t cap = 8192;
    char *js = (char *)malloc(cap);
    if (!js) return NULL;
    const char *k = key ? key : "";

    /* 1) 元の永続値を退避し、後で戻せるようにする。 */
    snprintf(js, cap,
        "(async()=>{const r={};"
        "try{r.before=localStorage.getItem('%s');"
        "if(r.before!==null)localStorage.setItem('%s.nx-backup',r.before);}"
        "catch(e){r.backupErr=String(e);}",
        k, k);

    /* 2) ストアを解決する。 */
    append_store_resolve(js, cap, store_substr, export_names);

    /* 3) 現在の状態から「何も開いていない」妥当な状態を作って適用・永続化する。
          スキーマを知らなくても、配列を空にしてセッション選択系を消せば済む。 */
    size_t cur = strlen(js);
    snprintf(js + cur, cap - cur,
        "const st=store.getState();const base={};"
        "for(const kk of Object.keys(st)){const v=st[kk];"
        "if(Array.isArray(v))base[kk]=[];else if(v&&typeof v==='object')base[kk]=v;"
        "else base[kk]=v;}"
        "for(const kk of Object.keys(base)){"
        "if(typeof base[kk]==='string'&&/^(active|selected|current|last|focused|opened)/i.test(kk))"
        "base[kk]='';}"
        "const baseStr=JSON.stringify(base);"
        "store.setState(base);"
        "if(store.persist&&store.persist.rehydrate){try{await store.persist.rehydrate();}catch(e){}}"
        "r.after=(function(){try{return localStorage.getItem('%s');}catch(e){return null;}})();"
        "r.persisted=r.after!==null;"
        "r.baselineLen=baseStr.length;"
        "r.baseline=baseStr.slice(0,600);"
        "r.before=r.before?r.before.slice(0,300):null;"
        "return JSON.stringify({ok:true,...r});})()",
        k);
    return js;
}

