#ifndef MACHO_STATIC_H
#define MACHO_STATIC_H

/*
 * macho_static.h — NeoDAX Static Analysis Framework compatibility header
 *
 * This header exposes the static analysis types (depflow, execution_order,
 * parallel_detection, state_tracking, variable_classifier) against the
 * NeoDAX core `dax_binary_t` / `dax_opts_t` types so that the src_new static
 * analysis modules can be called directly from the neodax CLI.
 */

#include "dax.h"
#include "macho.h"

/* Type compatibility: macho_binary_t / macho_opts_t are aliases for the
 * NeoDAX core types so existing src_new sources compile without changes. */
typedef dax_binary_t  macho_binary_t;
typedef dax_opts_t    macho_opts_t;

/* Analysis option aliases (mirror of dax_opts_t static fields) */
#define MACHO_OPT_DEPFLOW         (1 << 0)
#define MACHO_OPT_CLASSIFY_VARS   (1 << 1)
#define MACHO_OPT_PREDICT_EXEC    (1 << 2)
#define MACHO_OPT_DETECT_PARALLEL (1 << 3)
#define MACHO_OPT_TRACK_STATE     (1 << 4)
#define MACHO_OPT_BEAM_SEARCH     (1 << 5)
#define MACHO_OPT_SHOGI           (1 << 6)
#define MACHO_OPT_BEND            (1 << 7)
#define MACHO_OPT_PROOF_BASED     (1 << 8)
#define MACHO_OPT_PARALLEL_COMPLEMENT (1 << 9)

/* Arch type aliases (macho_static.c uses MACHO_ARCH_*, dax.h uses ARCH_*) */
#define MACHO_ARCH_UNKNOWN ARCH_UNKNOWN
#define MACHO_ARCH_X86_64  ARCH_X86_64
#define MACHO_ARCH_ARM64   ARCH_ARM64
#define MACHO_ARCH_RISCV64 ARCH_RISCV64

/* Section type aliases (macho_static.c uses MACHO_SEC_TYPE_*, dax.h uses SEC_TYPE_*) */
#define MACHO_SEC_TYPE_CODE   SEC_TYPE_CODE
#define MACHO_SEC_TYPE_DATA   SEC_TYPE_DATA
#define MACHO_SEC_TYPE_RODATA SEC_TYPE_RODATA
#define MACHO_SEC_TYPE_BSS    SEC_TYPE_BSS
#define MACHO_SEC_TYPE_PLT    SEC_TYPE_PLT
#define MACHO_SEC_TYPE_GOT    SEC_TYPE_GOT
#define MACHO_SEC_TYPE_DYNAMIC SEC_TYPE_DYNAMIC
#define MACHO_SEC_TYPE_DEBUG  SEC_TYPE_DEBUG
#define MACHO_SEC_TYPE_OTHER  SEC_TYPE_OTHER

/* Mach-O section attribute / type flags (used by macho_static.c) */
#define MACHO_S_REGULAR            0x0
#define MACHO_S_ZEROFILL           0x1
#define MACHO_S_CSTRING_LITERALS   0x2
#define MACHO_S_4BYTE_LITERALS     0x3
#define MACHO_S_8BYTE_LITERALS     0x4
#define MACHO_S_LITERAL_POINTERS   0x5
#define MACHO_S_NON_LAZY_SYMBOL_POINTERS 0x6
#define MACHO_S_LAZY_SYMBOL_POINTERS 0x7
#define MACHO_S_MOD_INIT_FUNC_POINTERS 0x9
#define MACHO_S_MOD_TERM_FUNC_POINTERS 0xA
#define MACHO_S_COALESCED          0xB
#define MACHO_S_GB_ZEROFILL        0xC
#define MACHO_S_INTERPOSING        0xD
#define MACHO_S_16BYTE_LITERALS    0xE
#define MACHO_S_DTRACE_DOF         0xF
#define MACHO_S_LAZY_DYLIB_SYMBOL_POINTERS 0x10

/* Mach-O section attributes */
#define MACHO_S_ATTR_PURE_INSTRUCTIONS 0x80000000
#define MACHO_S_ATTR_NO_TOC          0x40000000
#define MACHO_S_ATTR_STRIP_STATIC_SYMS 0x20000000
#define MACHO_S_ATTR_NO_DEAD_STRIP   0x10000000
#define MACHO_S_ATTR_LIVE_SUPPORT    0x08000000
#define MACHO_S_ATTR_SELF_MODIFYING_CODE 0x04000000
#define MACHO_S_ATTR_DEBUG           0x02000000
#define MACHO_S_ATTR_SOME_INSTRUCTIONS 0x00000400
#define MACHO_S_ATTR_EXT_RELOC       0x00000200
#define MACHO_S_ATTR_LOC_RELOC       0x00000100

/* Mach-O parsing functions (implemented in macho_static.c) */
int  macho_parse(macho_binary_t *bin, const uint8_t *data, size_t size);
int  macho_parse_fat(macho_binary_t *bin, const uint8_t *data, size_t size);
int  macho_parse_header(macho_binary_t *bin, const uint8_t *data, size_t size);
int  macho_parse_load_commands(macho_binary_t *bin, const uint8_t *data, size_t size);
int  macho_parse_sections(macho_binary_t *bin, const uint8_t *data, size_t size);
int  macho_parse_symbols(macho_binary_t *bin, const uint8_t *data, size_t size);
void macho_init(macho_binary_t *bin);
void macho_free(macho_binary_t *bin);

/* Basic analysis (implemented in macho_static.c) */
int  macho_analyze(macho_binary_t *bin, macho_opts_t *opts);
int  macho_detect_functions(macho_binary_t *bin);
int  macho_build_cfg(macho_binary_t *bin);
int  macho_build_callgraph(macho_binary_t *bin);
int  macho_detect_loops(macho_binary_t *bin);
int  macho_detect_switch_tables(macho_binary_t *bin);
int  macho_analyze_dependencies(macho_binary_t *bin, macho_opts_t *opts);
int  macho_track_variable_usage(macho_binary_t *bin, const char *var_name);
int  macho_build_dependency_graph(macho_binary_t *bin);
int  macho_is_valid(macho_binary_t *bin);

/* Cross-module analysis wrappers (implemented in macho_static.c) */
int  exec_order_analyze(macho_binary_t *bin);
int  parallel_detect_analyze(macho_binary_t *bin);
int  parallel_detect_beam_search(macho_binary_t *bin);

/* 32-bit segment type used by macho_static.c */
#pragma pack(push, 1)
typedef struct {
    char     segname[16];
    uint32_t vmaddr;
    uint32_t vmsize;
    uint32_t fileoff;
    uint32_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
    uint32_t nsects;
    uint32_t flags;
} macho_segment32_t;
#pragma pack(pop)

/* 32-bit section type used by macho_static.c */
#pragma pack(push, 1)
typedef struct {
    char     sectname[16];
    char     segname[16];
    uint32_t addr;
    uint32_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
} macho_section32_t;
#pragma pack(pop)

/* MAX sections constant */
#ifndef MAX_MACHO_SECTIONS
#define MAX_MACHO_SECTIONS DAX_MAX_SECTIONS
#endif

#ifndef MAX_MACHO_SYMBOLS
#define MAX_MACHO_SYMBOLS DAX_MAX_SYMBOLS
#endif

/* Mach-O nlist type flags (used by macho_static.c) */
#define MACHO_N_STAB  0x00FE
#define MACHO_N_PEXT  0x0010
#define MACHO_N_TYPE  0x000E
#define MACHO_N_EXT   0x0001
#define MACHO_N_UNDF  0x0
#define MACHO_N_ABS   0x2
#define MACHO_N_SECT  0xE
#define MACHO_N_PBUD  0xC
#define MACHO_N_INDR  0xA

#endif /* MACHO_STATIC_H */
