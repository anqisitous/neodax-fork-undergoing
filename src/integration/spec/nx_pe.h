#ifndef NX_PE_H
#define NX_PE_H

/*
 * nx_pe — macOS 上で Windows PE (.exe/.dll) を解析する汎用リーダ。
 *
 * Mach-O 解析と同じ「同等のパイプライン」を PE にも適用する:
 *   - DOS / NT ヘッダ、OptionalHeader (PE32/PE32+)
 *   - セクション表
 *   - DataDirectory (Import / Export)
 *   - インポート DLL と関数名、エクスポート名
 *
 * spec パッケージの PE 構造 spec と組み合わせて、認識・C++ 生成にも載る。
 */

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     name[9];
    uint32_t virtual_size;
    uint32_t virtual_address;
    uint32_t raw_size;
    uint32_t raw_ptr;
    uint32_t characteristics;
} nx_pe_section_t;

typedef struct {
    int      is64;            /* PE32+ なら 1 */
    uint16_t machine;
    const char *machine_name;
    uint16_t num_sections;
    uint32_t timestamp;
    uint32_t entry_point;
    uint64_t image_base;
    uint16_t subsystem;
    const char *subsystem_name;
    uint32_t size_of_image;
    uint32_t size_of_headers;
    nx_pe_section_t *sections;
    int      n_sections;

    /* data directories */
    uint32_t dd_rva[16];
    uint32_t dd_size[16];
} nx_pe_image_t;

typedef struct {
    char dll[128];
    char **funcs;
    uint16_t *ords;
    int     n_funcs;
} nx_pe_import_t;

typedef struct {
    char **names;
    uint32_t *rvas;
    int      n;
} nx_pe_export_t;

/* PE を解析してイメージ情報を得る。失敗で NULL。 */
nx_pe_image_t *nx_pe_open(const char *path);
void           nx_pe_free(nx_pe_image_t *img);

/* 解析結果を可読出力。 */
void nx_pe_print(const nx_pe_image_t *img, FILE *out);

/* パスから一括解析して出力。成功 0。 */
int nx_pe_analyze(const char *path, FILE *out);

/* インポート/エクスポートを取得 (取得できれば *out に malloc 配列)。 */
nx_pe_import_t *nx_pe_read_imports(const char *path, int *n_out);
nx_pe_export_t *nx_pe_read_exports(const char *path, int *n_out);

/* PE かどうか (MZ + PE\0\0)。 */
int nx_pe_is_pe(const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* NX_PE_H */
