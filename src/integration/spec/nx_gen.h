#ifndef NX_GEN_H
#define NX_GEN_H

/*
 * nx_gen — nx_spec の宣言モデルから C++ のクラス/構造体コードを生成する。
 *
 * spec は「構造の記述」だけ。生成器はそれを読み、
 *   - 各フィールドに対応する C++ メンバ
 *   - enum (NX_D_ENUM のフィールド)
 *   - parse(data,size)   … バイト列からメンバへ
 *   - serialize()        … メンバからバイト列へ
 * を持つ struct を出力する。WebSocket でも何でも、spec があれば生成できる。
 */

#include <stdio.h>
#include "nx_spec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* spec 1 つから C++ クラスを生成。
   - class_name が NULL なら spec->name を CamelCase 化して使う
   - only_decl=1 なら宣言のみ (実装は serialize/parse のインライン)
   - out に出力。 */
int nx_gen_cpp(const nx_spec_t *spec, const char *class_name,
               int inline_impl, FILE *out);

/* 登録済み全 spec をまとめて生成。 */
int nx_gen_cpp_all(int inline_impl, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_GEN_H */
