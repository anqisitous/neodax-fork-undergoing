#ifndef DAX_MACHO_H
#define DAX_MACHO_H

/*
 * include/macho.h — canonical Mach-O wire format (single source of truth).
 *
 * Both the NeoDAX engine (loader.c / macho.c) and the static-analysis
 * framework (src/core/macho_static.*) use these definitions by composition.
 * Do not redefine these structs or constants elsewhere.
 *
 * Magic values are expressed as the uint32 a little-endian CPU reads from
 * the raw file bytes:
 *   *_LE  -> native little-endian Mach-O (most common)
 *   bare  -> byte-swapped (big-endian) Mach-O
 */

#include <stdint.h>

/* Mach-O magic numbers */
#define MACHO_MAGIC_32      0xCEFAEDFEU   /* file bytes: FE ED FA CE  (32-bit BE) */
#define MACHO_MAGIC_32_LE   0xFEEDFACEU   /* file bytes: CE FA ED FE  (32-bit LE) */
#define MACHO_MAGIC_64      0xCFFAEDFEU   /* file bytes: FE ED FA CF  (64-bit BE) */
#define MACHO_MAGIC_64_LE   0xFEEDFACFU   /* file bytes: CF FA ED FE  (64-bit LE) */
#define MACHO_FAT_MAGIC     0xBEBAFECAU   /* file bytes: CA FE BA BE  (FAT)       */
#define MACHO_FAT_MAGIC_LE  0xCAFEBABEU   /* byte-swapped FAT alias               */

/* CPU types */
#define MACHO_CPU_X86_64    0x01000007
#define MACHO_CPU_ARM64     0x0100000C
#define MACHO_CPU_ARM64_32  0x0200000C
#define MACHO_CPU_X86       0x00000007
#define MACHO_CPU_ARM       0x0000000C

/* File types */
#define MACHO_MH_OBJECT     0x1
#define MACHO_MH_EXECUTE    0x2
#define MACHO_MH_FVMLIB     0x3
#define MACHO_MH_CORE       0x4
#define MACHO_MH_PRELOAD    0x5
#define MACHO_MH_DYLIB      0x6
#define MACHO_MH_DYLINKER   0x7
#define MACHO_MH_BUNDLE     0x8
#define MACHO_MH_DSYM       0xA

/* Header flags */
#define MACHO_MH_PIE        0x200000

/* Load command types */
#define MACHO_LC_SEGMENT            0x1
#define MACHO_LC_SYMTAB             0x2
#define MACHO_LC_SYMSEG             0x3
#define MACHO_LC_THREAD             0x4
#define MACHO_LC_UNIXTHREAD         0x5
#define MACHO_LC_DYSYMTAB           0xB
#define MACHO_LC_LOAD_DYLIB         0xC
#define MACHO_LC_ID_DYLIB           0xD
#define MACHO_LC_LOAD_DYLINKER      0xE
#define MACHO_LC_ID_DYLINKER        0xF
#define MACHO_LC_ROUTINES           0x11
#define MACHO_LC_SEGMENT_64         0x19
#define MACHO_LC_ROUTINES_64        0x1A
#define MACHO_LC_UUID               0x1B
#define MACHO_LC_RPATH              0x1C
#define MACHO_LC_CODE_SIGNATURE     0x1D
#define MACHO_LC_DYLD_INFO          0x22
#define MACHO_LC_DYLD_INFO_ONLY     0x23
#define MACHO_LC_VERSION_MIN_MACOSX 0x24
#define MACHO_LC_VERSION_MIN_IPHONEOS 0x25
#define MACHO_LC_FUNCTION_STARTS    0x26
#define MACHO_LC_DYLD_ENVIRONMENT   0x27
#define MACHO_LC_MAIN               0x80000028  /* 0x28 | LC_REQ_DYLD */
#define MACHO_LC_DATA_IN_CODE       0x29
#define MACHO_LC_SOURCE_VERSION     0x2A
#define MACHO_LC_ENCRYPTION_INFO_64 0x2C
#define MACHO_LC_LINKER_OPTION      0x2D
#define MACHO_LC_LINKER_OPTIMIZATION_HINT 0x2E
#define MACHO_LC_VERSION_MIN_TVOS   0x2F
#define MACHO_LC_VERSION_MIN_WATCHOS 0x30
#define MACHO_LC_NOTE               0x31
#define MACHO_LC_BUILD_VERSION      0x32

/* Section types (low byte of section flags) */
#define MACHO_S_REGULAR                 0x0
#define MACHO_S_ZEROFILL                0x1   /* no file bytes — offset may be past EOF */
#define MACHO_S_CSTRING_LITERALS        0x2
#define MACHO_S_4BYTE_LITERALS          0x3
#define MACHO_S_8BYTE_LITERALS          0x4
#define MACHO_S_LITERAL_POINTERS        0x5
#define MACHO_S_NON_LAZY_SYMBOL_POINTERS 0x6
#define MACHO_S_LAZY_SYMBOL_POINTERS    0x7
#define MACHO_S_SYMBOL_STUBS            0x8u
#define MACHO_S_MOD_INIT_FUNC_POINTERS  0x9
#define MACHO_S_MOD_TERM_FUNC_POINTERS  0xA
#define MACHO_S_COALESCED               0xB
#define MACHO_S_GB_ZEROFILL             0xC
#define MACHO_S_INTERPOSING             0xD
#define MACHO_S_16BYTE_LITERALS         0xE
#define MACHO_S_DTRACE_DOF              0xF
#define MACHO_S_LAZY_DYLIB_SYMBOL_POINTERS 0x10

/* Section attribute flags */
#define MACHO_S_ATTR_PURE_INSTRUCTIONS  0x80000000
#define MACHO_S_ATTR_NO_TOC             0x40000000
#define MACHO_S_ATTR_STRIP_STATIC_SYMS  0x20000000
#define MACHO_S_ATTR_NO_DEAD_STRIP      0x10000000
#define MACHO_S_ATTR_LIVE_SUPPORT       0x08000000
#define MACHO_S_ATTR_SELF_MODIFYING_CODE 0x04000000
#define MACHO_S_ATTR_DEBUG              0x02000000
#define MACHO_S_ATTR_SOME_INSTRUCTIONS  0x00000400
#define MACHO_S_ATTR_EXT_RELOC          0x00000200
#define MACHO_S_ATTR_LOC_RELOC          0x00000100

/* nlist n_type masks */
#define MACHO_N_STAB 0xE0
#define MACHO_N_PEXT 0x10
#define MACHO_N_TYPE 0x0E
#define MACHO_N_EXT  0x01

/* nlist symbol types (n_type & MACHO_N_TYPE) */
#define MACHO_N_UNDF 0x0
#define MACHO_N_ABS  0x2
#define MACHO_N_SECT 0xE
#define MACHO_N_PBUD 0xC
#define MACHO_N_INDR 0xA

#define MACHO_INDIRECT_SYMBOL_LOCAL 0x80000000u
#define MACHO_INDIRECT_SYMBOL_ABS   0x40000000u

#pragma pack(push, 1)

typedef struct {
    uint32_t magic;
    uint32_t nfat_arch;
} macho_fat_header_t;

typedef struct {
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t offset;
    uint32_t size;
    uint32_t align;
} macho_fat_arch_t;

typedef struct {
    uint32_t magic;
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
} macho_header32_t;

typedef struct {
    uint32_t magic;
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
    uint32_t reserved;   /* 64-bit only */
} macho_header64_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
} macho_load_cmd_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
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

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    char     segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
    uint32_t nsects;
    uint32_t flags;
} macho_segment64_t;

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

typedef struct {
    char     sectname[16];
    char     segname[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
} macho_section64_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint64_t entryoff;   /* offset from __TEXT base to entry point */
    uint64_t stacksize;
} macho_entry_cmd_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t symoff;
    uint32_t nsyms;
    uint32_t stroff;
    uint32_t strsize;
} macho_symtab_cmd_t;

/* LC_DYSYMTAB — dynamic symbol info incl. the indirect symbol table
 * used to name __stubs entries. */
typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t ilocalsym;
    uint32_t nlocalsym;
    uint32_t iextdefsym;
    uint32_t nextdefsym;
    uint32_t iundefsym;
    uint32_t nundefsym;
    uint32_t tocoff;
    uint32_t ntoc;
    uint32_t modtaboff;
    uint32_t nmodtab;
    uint32_t extrefsymoff;
    uint32_t nextrefsyms;
    uint32_t indirectsymoff;
    uint32_t nindirectsyms;
    uint32_t extreloff;
    uint32_t nextrel;
    uint32_t locreloff;
    uint32_t nlocrel;
} macho_dysymtab_cmd_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint64_t dataoff;
    uint64_t datasize;
} macho_dyld_info_cmd_t;

typedef struct {
    int32_t  n_strx;
    uint8_t  n_type;
    uint8_t  n_sect;
    int16_t  n_desc;
    uint32_t n_value;
} macho_nlist32_t;

typedef struct {
    int32_t  n_strx;
    uint8_t  n_type;
    uint8_t  n_sect;
    int16_t  n_desc;
    uint64_t n_value;
} macho_nlist64_t;

#pragma pack(pop)

/* Compile-time layout locks (wincomp-style): the parser reads fields by
 * offset for both widths, so these must never drift. */
typedef char macho_chk_header32 [sizeof(macho_header32_t)  == 28 ? 1 : -1];
typedef char macho_chk_header64 [sizeof(macho_header64_t)  == 32 ? 1 : -1];
typedef char macho_chk_seg32    [sizeof(macho_segment32_t) == 56 ? 1 : -1];
typedef char macho_chk_seg64    [sizeof(macho_segment64_t) == 72 ? 1 : -1];
typedef char macho_chk_sec32    [sizeof(macho_section32_t) == 72 ? 1 : -1];
typedef char macho_chk_sec64    [sizeof(macho_section64_t) == 80 ? 1 : -1];
typedef char macho_chk_nlist32  [sizeof(macho_nlist32_t)   == 12 ? 1 : -1];
typedef char macho_chk_nlist64  [sizeof(macho_nlist64_t)   == 16 ? 1 : -1];
typedef char macho_chk_entry    [sizeof(macho_entry_cmd_t) == 24 ? 1 : -1];

#endif /* DAX_MACHO_H */
