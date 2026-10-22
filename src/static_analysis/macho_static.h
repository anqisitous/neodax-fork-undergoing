#ifndef MACHO_STATIC_H
#define MACHO_STATIC_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

// ============================================================================
// Mach-O Static Analysis Framework
// 
// 静的解析のためのMach-Oファイル構造体と関数定義
// 将棋AIなどのバイナリ解析に特化した静的フロー解析を提供
// ============================================================================

#define MAX_MACHO_SECTIONS 256
#define MAX_MACHO_SYMBOLS 16384
#define MAX_MACHO_FUNCTIONS 8192
#define MAX_MACHO_XREFS 32768
#define MAX_MACHO_BLOCKS 65536
#define MAX_CALL_DEPTH 32
#define MAX_PARALLEL_PATHS 1024

// Mach-O CPU types
#define MACHO_CPU_ARM64 0x0100000C
#define MACHO_CPU_X86_64 0x01000007

// Mach-O file types
#define MACHO_MH_EXECUTE 0x2
#define MACHO_MH_DYLIB 0x6
#define MACHO_MH_BUNDLE 0x8

// Mach-O magic numbers
#define MACHO_MAGIC_32 0xFEEDFACE
#define MACHO_MAGIC_32_LE 0xCEFAEDFE
#define MACHO_MAGIC_64 0xFEEDFACF
#define MACHO_MAGIC_64_LE 0xCFFAEDFE
#define MACHO_FAT_MAGIC 0xCAFEBABE
#define MACHO_FAT_MAGIC_LE 0xBEBAFECA

// Section types
#define MACHO_S_REGULAR 0x0
#define MACHO_S_ZEROFILL 0x1
#define MACHO_S_CSTRING_LITERALS 0x2
#define MACHO_S_4BYTE_LITERALS 0x3
#define MACHO_S_8BYTE_LITERALS 0x4
#define MACHO_S_LITERAL_POINTERS 0x5
#define MACHO_S_NON_LAZY_SYMBOL_POINTERS 0x6
#define MACHO_S_LAZY_SYMBOL_POINTERS 0x7
#define MACHO_S_SYMBOL_STUBS 0x8
#define MACHO_S_MOD_INIT_FUNC_POINTERS 0x9
#define MACHO_S_MOD_TERM_FUNC_POINTERS 0xA
#define MACHO_S_COALESCED 0xB
#define MACHO_S_GB_ZEROFILL 0xC
#define MACHO_S_INTERPOSING 0xD
#define MACHO_S_16BYTE_LITERALS 0xE
#define MACHO_S_DTRACE_DOF 0xF
#define MACHO_S_LAZY_DYLIB_SYMBOL_POINTERS 0x10

// Section flags
#define MACHO_S_ATTR_PURE_INSTRUCTIONS 0x80000000
#define MACHO_S_ATTR_NO_TOC 0x40000000
#define MACHO_S_ATTR_STRIP_STATIC_SYMS 0x20000000
#define MACHO_S_ATTR_NO_DEAD_STRIP 0x10000000
#define MACHO_S_ATTR_LIVE_SUPPORT 0x08000000
#define MACHO_S_ATTR_SELF_MODIFYING_CODE 0x04000000
#define MACHO_S_ATTR_DEBUG 0x02000000
#define MACHO_S_ATTR_SOME_INSTRUCTIONS 0x00000400
#define MACHO_S_ATTR_EXT_RELOC 0x00000200
#define MACHO_S_ATTR_LOC_RELOC 0x00000100

// Load command types
#define MACHO_LC_SEGMENT 0x1
#define MACHO_LC_SYMTAB 0x2
#define MACHO_LC_SYMSEG 0x3
#define MACHO_LC_THREAD 0x4
#define MACHO_LC_UNIXTHREAD 0x5
#define MACHO_LC_LOADFVMLIB 0x6
#define MACHO_LC_IDFVMLIB 0x7
#define MACHO_LC_IDENT 0x8
#define MACHO_LC_FIDF 0x9
#define MACHO_LC_PREPAGE 0xA
#define MACHO_LC_DYSYMTAB 0xB
#define MACHO_LC_LOAD_DYLIB 0xC
#define MACHO_LC_ID_DYLIB 0xD
#define MACHO_LC_LOAD_DYLINKER 0xE
#define MACHO_LC_ID_DYLINKER 0xF
#define MACHO_LC_PREBOUND_DYLIB 0x10
#define MACHO_LC_ROUTINES 0x11
#define MACHO_LC_SUB_FRAMEWORK 0x12
#define MACHO_LC_SUB_UMBRELLA 0x13
#define MACHO_LC_SUB_CLIENT 0x14
#define MACHO_LC_SUB_LIBRARY 0x15
#define MACHO_LC_TWOLEVEL_HINTS 0x16
#define MACHO_LC_PREBIND_CKSUM 0x17
#define MACHO_LC_LOAD_WEAK_DYLIB 0x18
#define MACHO_LC_SEGMENT_64 0x19
#define MACHO_LC_ROUTINES_64 0x1A
#define MACHO_LC_UUID 0x1B
#define MACHO_LC_RPATH 0x1C
#define MACHO_LC_CODE_SIGNATURE 0x1D
#define MACHO_LC_SEGMENT_SPLIT_INFO 0x1E
#define MACHO_LC_REEXPORT_DYLIB 0x1F
#define MACHO_LC_LAZY_LOAD_DYLIB 0x20
#define MACHO_LC_ENCRYPTION_INFO 0x21
#define MACHO_LC_DYLD_INFO 0x22
#define MACHO_LC_DYLD_INFO_ONLY 0x23
#define MACHO_LC_LOAD_UPWARD_DYLIB 0x24
#define MACHO_LC_VERSION_MIN_MACOSX 0x25
#define MACHO_LC_VERSION_MIN_IPHONEOS 0x26
#define MACHO_LC_FUNCTION_STARTS 0x26
#define MACHO_LC_DYLD_ENVIRONMENT 0x27
#define MACHO_LC_MAIN 0x28
#define MACHO_LC_DATA_IN_CODE 0x29
#define MACHO_LC_SOURCE_VERSION 0x2A
#define MACHO_LC_DYLIB_CODE_SIGN_DRS 0x2B
#define MACHO_LC_ENCRYPTION_INFO_64 0x2C
#define MACHO_LC_LINKER_OPTION 0x2D
#define MACHO_LC_LINKER_OPTIMIZATION_HINT 0x2E
#define MACHO_LC_VERSION_MIN_TVOS 0x2F
#define MACHO_LC_VERSION_MIN_WATCHOS 0x30
#define MACHO_LC_NOTE 0x31
#define MACHO_LC_BUILD_VERSION 0x32

// Symbol table flags
#define MACHO_N_STAB 0x00FE
#define MACHO_N_PEXT 0x0010
#define MACHO_N_TYPE 0x000E
#define MACHO_N_EXT 0x0001

// Symbol types
#define MACHO_N_UNDF 0x0
#define MACHO_N_ABS 0x2
#define MACHO_N_SECT 0xE
#define MACHO_N_PBUD 0xC
#define MACHO_N_INDR 0xA

// ============================================================================
// Mach-O Header Structures
// ============================================================================

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
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
} macho_header32_t;

typedef struct {
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
    uint32_t reserved;
} macho_header64_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
} macho_load_cmd_t;

typedef struct {
    char segname[16];
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
    char segname[16];
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
    char sectname[16];
    char segname[16];
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
    char sectname[16];
    char segname[16];
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
    uint32_t symoff;
    uint32_t nsyms;
    uint32_t stroff;
    uint32_t strsize;
} macho_symtab_cmd_t;

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
    int32_t n_strx;
    uint8_t n_type;
    uint8_t n_sect;
    int16_t n_desc;
} macho_nlist32_t;

typedef struct {
    int32_t n_strx;
    uint8_t n_type;
    uint8_t n_sect;
    int16_t n_desc;
} macho_nlist64_t;

#pragma pack(pop)

// ============================================================================
// Static Analysis Data Structures
// ============================================================================

typedef enum {
    MACHO_ARCH_UNKNOWN = 0,
    MACHO_ARCH_X86_64,
    MACHO_ARCH_ARM64,
    MACHO_ARCH_RISCV64
} macho_arch_t;

typedef enum {
    MACHO_SEC_TYPE_UNKNOWN = 0,
    MACHO_SEC_TYPE_CODE,
    MACHO_SEC_TYPE_DATA,
    MACHO_SEC_TYPE_RODATA,
    MACHO_SEC_TYPE_BSS,
    MACHO_SEC_TYPE_PLT,
    MACHO_SEC_TYPE_GOT,
    MACHO_SEC_TYPE_DYNAMIC,
    MACHO_SEC_TYPE_DEBUG,
    MACHO_SEC_TYPE_OTHER
} macho_sec_type_t;

typedef enum {
    MACHO_SYMBOL_TYPE_UNKNOWN = 0,
    MACHO_SYMBOL_TYPE_FUNC,
    MACHO_SYMBOL_TYPE_OBJECT,
    MACHO_SYMBOL_TYPE_IMPORT,
    MACHO_SYMBOL_TYPE_EXPORT,
    MACHO_SYMBOL_TYPE_WEAK,
    MACHO_SYMBOL_TYPE_LOCAL
} macho_symbol_type_t;

typedef enum {
    MACHO_XREF_TYPE_CALL = 0,
    MACHO_XREF_TYPE_BRANCH,
    MACHO_XREF_TYPE_DATA,
    MACHO_XREF_TYPE_JUMP_TABLE
} macho_xref_type_t;

typedef enum {
    MACHO_BLOCK_TYPE_REGULAR = 0,
    MACHO_BLOCK_TYPE_ENTRY,
    MACHO_BLOCK_TYPE_EXIT,
    MACHO_BLOCK_TYPE_CONDITIONAL,
    MACHO_BLOCK_TYPE_LOOP_HEADER,
    MACHO_BLOCK_TYPE_LOOP_BODY,
    MACHO_BLOCK_TYPE_LOOP_EXIT,
    MACHO_BLOCK_TYPE_SWITCH,
    MACHO_BLOCK_TYPE_POLYMORPHIC
} macho_block_type_t;

typedef enum {
    MACHO_EDGE_TYPE_FALL = 0,
    MACHO_EDGE_TYPE_JUMP,
    MACHO_EDGE_TYPE_COND_TRUE,
    MACHO_EDGE_TYPE_COND_FALSE,
    MACHO_EDGE_TYPE_CALL,
    MACHO_EDGE_TYPE_RET,
    MACHO_EDGE_TYPE_INDIRECT
} macho_edge_type_t;

// Section information
typedef struct {
    char name[64];
    uint64_t offset;
    uint64_t vaddr;
    uint64_t size;
    uint32_t flags;
    macho_sec_type_t type;
    uint32_t insn_count;
    uint64_t file_offset;
    int executable;
    int writable;
    int readable;
} macho_section_t;

// Symbol information
typedef struct {
    uint64_t address;
    char name[512];
    char demangled[512];
    macho_symbol_type_t type;
    uint64_t size;
    int is_entry;
    int is_global;
    int is_weak;
    int is_undefined;
    uint32_t section_idx;
} macho_symbol_t;

// Cross-reference information
typedef struct {
    uint64_t from;
    uint64_t to;
    macho_xref_type_t type;
    int is_call;
    int is_conditional;
    int is_indirect;
    uint64_t resolved_target;
} macho_xref_t;

// Function information
typedef struct {
    uint64_t start;
    uint64_t end;
    char name[512];
    int sym_idx;
    uint32_t insn_count;
    uint32_t block_count;
    uint8_t has_loops;
    uint8_t has_calls;
    uint8_t has_branches;
    uint8_t has_indirect_calls;
    uint64_t stack_frame_size;
    uint64_t local_vars_size;
    uint64_t arg_count;
    uint64_t return_type;
} macho_function_t;

// Basic block information
typedef struct {
    uint64_t start;
    uint64_t end;
    int func_idx;
    int id;
    macho_block_type_t block_type;
    int succ[16];
    int nsucc;
    int pred[16];
    int npred;
    macho_edge_type_t edge_type[16];
    int is_entry;
    int is_exit;
    int is_visited;
    uint64_t execution_count;
} macho_block_t;

// Instruction information
typedef struct {
    uint64_t address;
    uint8_t bytes[15];
    uint8_t length;
    char mnemonic[32];
    char operands[256];
    uint8_t is_call;
    uint8_t is_branch;
    uint8_t is_conditional;
    uint8_t is_indirect;
    uint8_t is_return;
    uint8_t is_nop;
    uint8_t affects_flags;
    uint8_t reads_memory;
    uint8_t writes_memory;
} macho_insn_t;

// Data dependency information
typedef struct {
    uint64_t address;
    int type;  // 0: register, 1: memory, 2: stack
    char name[64];
    uint64_t size;
    int is_input;
    int is_output;
    int is_modified;
} macho_dependency_t;

// Function call chain
typedef struct {
    uint64_t call_site;
    uint64_t caller;
    uint64_t callee;
    int depth;
    uint64_t timestamp;
    int is_recursive;
    int is_parallel;
    uint64_t parallel_group_id;
} macho_call_chain_t;

// Execution path
typedef struct {
    uint64_t path_id;
    uint64_t start_addr;
    uint64_t end_addr;
    uint64_t entry_point;
    int block_count;
    uint64_t blocks[MAX_CALL_DEPTH];
    int is_feasible;
    uint64_t probability;
    uint64_t execution_order;
} macho_execution_path_t;

// Parallel execution information
typedef struct {
    uint64_t dispatch_addr;
    uint64_t function_addr;
    uint64_t thread_count;
    uint64_t group_id;
    char dispatch_type[32];  // "beam_search", "parallel_for", etc.
    uint64_t argument_base;
    uint64_t argument_size;
    int is_dynamic;
    uint64_t synchronization_point;
} macho_parallel_info_t;

// Variable tracking information
typedef struct {
    uint64_t address;
    char name[128];
    uint64_t size;
    int type;  // 0: local, 1: global, 2: argument, 3: return
    uint64_t definition_site;
    uint64_t use_sites[64];
    int use_count;
    int is_modified;
    int is_read;
    int is_written;
    uint64_t lifetime_start;
    uint64_t lifetime_end;
} macho_variable_t;

// State tracking for regression analysis
typedef struct {
    uint64_t address;
    uint64_t value_hash;
    uint64_t timestamp;
    int is_checkpoint;
    char description[256];
} macho_state_checkpoint_t;

// Main binary structure
typedef struct {
    // File information
    uint8_t *data;
    size_t size;
    char filepath[1024];
    char sha256[65];
    
    // Architecture and format
    macho_arch_t arch;
    uint32_t filetype;
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t flags;
    
    // Entry point
    uint64_t entry;
    uint64_t base;
    uint64_t image_size;
    
    // Sections
    macho_section_t sections[MAX_MACHO_SECTIONS];
    int nsections;
    
    // Symbols
    macho_symbol_t *symbols;
    int nsymbols;
    int symbols_allocated;
    
    // Functions
    macho_function_t *functions;
    int nfunctions;
    int functions_allocated;
    
    // Cross-references
    macho_xref_t *xrefs;
    int nxrefs;
    int xrefs_allocated;
    
    // Basic blocks
    macho_block_t *blocks;
    int nblocks;
    int blocks_allocated;
    
    // Instructions
    macho_insn_t *insns;
    int ninsns;
    int insns_allocated;
    
    // Dependencies
    macho_dependency_t *dependencies;
    int ndependencies;
    int dependencies_allocated;
    
    // Call chains
    macho_call_chain_t *call_chains;
    int ncall_chains;
    int call_chains_allocated;
    
    // Execution paths
    macho_execution_path_t *execution_paths;
    int nexecution_paths;
    int execution_paths_allocated;
    
    // Parallel execution info
    macho_parallel_info_t *parallel_info;
    int nparallel_info;
    int parallel_info_allocated;
    
    // Variables
    macho_variable_t *variables;
    int nvariables;
    int variables_allocated;
    
    // State checkpoints
    macho_state_checkpoint_t *state_checkpoints;
    int nstate_checkpoints;
    int state_checkpoints_allocated;
    
    // Analysis flags
    int is_analyzed;
    int is_pie;
    int is_stripped;
    int has_debug;
    
    // Statistics
    uint64_t total_insns;
    uint64_t code_size;
    uint64_t data_size;
    
} macho_binary_t;

// Analysis options
typedef struct {
    int show_bytes;
    int show_addr;
    int color;
    int verbose;
    int symbols;
    int xrefs;
    int groups;
    int strings;
    int funcs;
    int demangle;
    int all_sections;
    int cfg;
    int loops;
    int callgraph;
    int switches;
    int interactive;
    int unicode;
    int symexec;
    int ssa;
    int decompile;
    int emulate;
    int entropy;
    int rda;
    int ivf;
    
    // New analysis options for static flow
    int depflow;                    // Data dependency flow analysis
    char depflow_target_func[512]; // Target function for dependency analysis
    char depflow_target_var[256];  // Target variable for tracking
    int depflow_depth;              // Maximum depth for dependency tracking
    char depflow_dot[1024];         // Output file for DOT format
    
    int execution_order;           // Analyze execution order
    int parallel_detection;        // Detect parallel execution patterns
    int state_tracking;            // Track state changes
    int regression_analysis;       // Analyze regression in state
    int beam_search_analysis;      // Special analysis for beam search patterns
    
    char output_dir[1024];         // Output directory for analysis results
    char analysis_report[1024];    // Report file path
    
} macho_opts_t;

// ============================================================================
// Function Prototypes
// ============================================================================

// Mach-O parsing
int macho_parse(macho_binary_t *bin, const uint8_t *data, size_t size);
int macho_parse_fat(macho_binary_t *bin, const uint8_t *data, size_t size);
int macho_parse_header(macho_binary_t *bin, const uint8_t *data, size_t size);
int macho_parse_load_commands(macho_binary_t *bin, const uint8_t *data, size_t size);
int macho_parse_sections(macho_binary_t *bin, const uint8_t *data, size_t size);
int macho_parse_symbols(macho_binary_t *bin, const uint8_t *data, size_t size);

// Static analysis
int macho_analyze(macho_binary_t *bin, macho_opts_t *opts);
int macho_detect_functions(macho_binary_t *bin);
int macho_build_cfg(macho_binary_t *bin);
int macho_build_callgraph(macho_binary_t *bin);
int macho_detect_loops(macho_binary_t *bin);
int macho_detect_switch_tables(macho_binary_t *bin);

// Data dependency analysis
int macho_analyze_dependencies(macho_binary_t *bin, macho_opts_t *opts);
int macho_track_variable_usage(macho_binary_t *bin, const char *var_name);
int macho_build_dependency_graph(macho_binary_t *bin);

// Execution flow analysis
int macho_analyze_execution_order(macho_binary_t *bin);
int macho_detect_parallel_execution(macho_binary_t *bin);
int macho_analyze_beam_search_patterns(macho_binary_t *bin);
int macho_predict_execution_sequence(macho_binary_t *bin);

// State tracking
int macho_track_state_changes(macho_binary_t *bin);
int macho_analyze_regression(macho_binary_t *bin);
int macho_build_state_checkpoints(macho_binary_t *bin);

// Utility functions
void macho_free(macho_binary_t *bin);
void macho_init(macho_binary_t *bin);
int macho_is_valid(macho_binary_t *bin);

// Output functions
void macho_print_analysis_report(macho_binary_t *bin, macho_opts_t *opts, FILE *out);
void macho_print_dependency_graph(macho_binary_t *bin, macho_opts_t *opts, FILE *out);
void macho_print_execution_flow(macho_binary_t *bin, macho_opts_t *opts, FILE *out);
void macho_print_parallel_analysis(macho_binary_t *bin, macho_opts_t *opts, FILE *out);
void macho_print_state_tracking(macho_binary_t *bin, macho_opts_t *opts, FILE *out);

// DOT output for visualization
void macho_export_cfg_dot(macho_binary_t *bin, const char *filename);
void macho_export_callgraph_dot(macho_binary_t *bin, const char *filename);
void macho_export_dependency_dot(macho_binary_t *bin, const char *filename);

#endif // MACHO_STATIC_H
