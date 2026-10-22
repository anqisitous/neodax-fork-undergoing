#ifndef DAX_NR_TYPES_H
#define DAX_NR_TYPES_H
/*
 * nr_types.h — NR (NeoDAX Representation) の型定義を decomp.c と
 * depflow.c で共有するためのヘッダ。中身は decomp.c にあった定義と
 * ビット単位で同一。片方だけ直して食い違う事故を避けるため、
 * decomp.c 側もこのヘッダを include する形に揃えている。
 */
#include <stdint.h>

typedef enum {
    NR_ASSIGN=0,
    NR_LOAD, NR_STORE, NR_MEMCPY,
    NR_ADD, NR_SUB, NR_MUL, NR_DIV, NR_NEG,
    NR_AND, NR_OR, NR_XOR, NR_NOT,
    NR_SHL, NR_SHR, NR_ROL, NR_ROR,
    NR_CAST, NR_SEXT, NR_ZEXT,
    NR_CMP, NR_BRANCH, NR_COND_BR,
    NR_PHI, NR_UNDEF,
    NR_CALL, NR_INDIRECT_CALL, NR_SYSCALL, NR_RET,
    NR_NOP, NR_LABEL
} nr_op_t;

typedef enum {
    NRT_UNKNOWN=0,
    NRT_BOOL,
    NRT_U8, NRT_U16, NRT_U32, NRT_U64,
    NRT_PTR, NRT_PTR_CODE, NRT_FLAGS
} nr_type_t;

#define NR_MAX_VARS    768
#define NR_MAX_STMTS  3072
#define NR_MAX_CALLS   256

typedef struct {
    char      base[20];
    int       version;
    nr_type_t type;
    uint8_t   bits;
    uint64_t  def_addr;
    int       func_idx;
} nr_var_t;

typedef struct {
    nr_op_t   op;
    int       dst, src1, src2, src3;
    uint64_t  imm;
    int       has_imm;
    uint64_t  addr;
    int       callee_func_idx;
    char      callee_name[48];
    char      cond[12];
    uint64_t  branch_target;
} nr_stmt_t;

typedef struct {
    nr_var_t   vars[NR_MAX_VARS];
    int        nvars;
    nr_stmt_t  stmts[NR_MAX_STMTS];
    int        nstmts;
    int        version_map[64];
    char       last_cmp_lhs[20];
    char       last_cmp_rhs[20];
    int        last_cmp_rhs_imm;
    uint64_t   last_cmp_rhs_val;
    int        func_idx;
} nr_ctx_t;

typedef struct {
    int      caller_func;
    int      callee_func;
    uint64_t call_site;
    char     callee_name[48];
} nr_call_edge_t;

#endif /* DAX_NR_TYPES_H */
