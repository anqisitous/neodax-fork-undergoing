/*
 * main.c — NeoDAX command line entry point
 *
 * Multi-architecture binary analysis: disassembler, CFG, symbolic execution,
 * decompiler, emulator, obfuscation scanner — plus the advanced static
 * analysis pipeline (`-w` / `--advanced-analysis`).
 *
 * The advanced analysis is a single Regression Ring: the pipeline laps over
 * its phases (survey → classify → trace → predict → parallel → verify), each
 * lap gathers more evidence and the frontier moves outward (progression).
 * Groups sit on the ring, functions are the leaves inside a group.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "dax.h"
#include "dax_guard.h"
#include "static_analysis.h"

/* ============================================================================
 * Usage
 * ============================================================================ */

static void print_usage(const char *p) {
    printf("\n");
    printf("  NeoDAX %s — binary analysis, disassembler, CFG, decompiler, emulator\n",
           DAX_VERSION);
    printf("  x86-64 · AArch64 · RISC-V · ELF · PE · Mach-O\n\n");
    printf("  Usage: %s [options] <binary>\n\n", p);

    printf("  Output\n");
    printf("    -a              show hex bytes alongside instructions\n");
    printf("    -l              list sections\n");
    printf("    -n              no color\n");
    printf("    -v              verbose\n");
    printf("    -s <section>    target section (default .text)\n");
    printf("    -S              all executable sections\n");
    printf("    -A <addr>       start address (hex)\n");
    printf("    -E <addr>       end address (hex)\n");
    printf("    -o <file>       write .daxc snapshot\n");
    printf("    -c <file>       convert .daxc to .S\n");

    printf("\n  Analysis\n");
    printf("    -y              resolve symbols\n");
    printf("    -d              demangle C++ names\n");
    printf("    -f              detect functions\n");
    printf("    -g              instruction group coloring\n");
    printf("    -r              cross-references\n");
    printf("    -t              string reference annotations\n");
    printf("    -C              control flow graphs\n");
    printf("    -L              loop detection\n");
    printf("    -G              call graph\n");
    printf("    -W              switch / jump table detection\n");
    printf("    -u              unicode string scan\n");
    printf("    -x              all standard analysis\n");

    printf("\n  Advanced\n");
    printf("    -P              symbolic execution\n");
    printf("    -Q              NR lifting (NeoDAX Representation)\n");
    printf("    -D              decompile to pseudo-C\n");
    printf("    -I              emulate functions\n");
    printf("    -e              entropy analysis\n");
    printf("    -R              recursive descent disassembly\n");
    printf("    -V              instruction validity filter\n");
    printf("    --poly          polymorphic obfuscation map\n");
    printf("    --dsa           DSA (dynamic single assignment, all functions)\n");
    printf("    --aire          context-aware assisted analysis\n");
    printf("    --vm-trace      VM dispatch trace\n");
    printf("    -X              everything\n");

    printf("\n  Advanced static analysis\n");
    printf("    -w, --advanced-analysis   full static flow analysis (regression ring)\n");
    printf("    --ring-svg <file>         write the ring visualization as SVG\n");
    printf("    --flow-depth <n>          caller-chain trace depth (default 32)\n");
    printf("    --flow-var <name>         trace one variable's caller chain\n");
    printf("    --flow-func <name>        focus the flow report on one function\n");

    printf("\n  Interactive / plugins\n");
    printf("    -i              interactive shell\n");
    printf("    -eox <folder>   load plugins from folder\n");
    printf("    -eox-list       list loaded plugins\n");

    printf("\n  Misc\n");
    printf("    -h              this help\n");
    printf("\n  Examples\n");
    printf("    %s -x ./binary\n", p);
    printf("    %s -w --ring-svg flow.svg ./binary\n", p);
    printf("    %s -w --flow-var best_score ./binary\n", p);
    printf("\n");
}

/* ============================================================================
 * Section helpers
 * ============================================================================ */

static dax_section_t *find_section(dax_binary_t *bin, const char *name) {
    if (!bin || !name) return NULL;
    for (int i = 0; i < bin->nsections && i < DAX_MAX_SECTIONS; i++) {
        if (!strcmp(bin->sections[i].name, name)) return &bin->sections[i];
    }
    return NULL;
}

static int is_code_section(dax_section_t *s) {
    return s && (s->type == SEC_TYPE_CODE || s->type == SEC_TYPE_PLT);
}

static int section_valid(dax_binary_t *bin, dax_section_t *s) {
    if (!bin || !s || s->size == 0) return 0;
    if (s->offset > bin->size) return 0;
    if (s->size > bin->size - s->offset) return 0;
    return 1;
}

/* ============================================================================
 * Core passes
 * ============================================================================ */

static void run_disasm(dax_binary_t *bin, dax_opts_t *opts, FILE *out) {
    if (!bin || !opts) return;

    if (opts->all_sections) {
        for (int i = 0; i < bin->nsections && i < DAX_MAX_SECTIONS; i++) {
            dax_section_t *s = &bin->sections[i];
            if (!is_code_section(s) || !section_valid(bin, s)) continue;
            dax_opts_t o = *opts;
            snprintf(o.section, sizeof(o.section), "%s", s->name);
            if (bin->arch == ARCH_X86_64)         dax_disasm_x86_64(bin, &o, out);
            else if (bin->arch == ARCH_ARM64)   dax_disasm_arm64(bin, &o, out);
            else if (bin->arch == ARCH_RISCV64) dax_disasm_riscv64(bin, &o, out);
        }
        return;
    }

    dax_section_t *s = find_section(bin, opts->section);
    if (!s) s = find_section(bin, ".text");
    if (!s || !section_valid(bin, s)) return;

    dax_opts_t o = *opts;
    snprintf(o.section, sizeof(o.section), "%s", s->name);

    if (bin->arch == ARCH_X86_64)         dax_disasm_x86_64(bin, &o, out);
    else if (bin->arch == ARCH_ARM64)   dax_disasm_arm64(bin, &o, out);
    else if (bin->arch == ARCH_RISCV64) dax_disasm_riscv64(bin, &o, out);
}

static void run_func_detect(dax_binary_t *bin, FILE *out) {
    if (!bin) return;
    for (int i = 0; i < bin->nsections && i < DAX_MAX_SECTIONS; i++) {
        dax_section_t *s = &bin->sections[i];
        if (!is_code_section(s) || !section_valid(bin, s)) continue;
        dax_func_detect(bin, bin->data + s->offset, (size_t)s->size, s->vaddr, s);
    }
    dax_clamp_counts(bin);
}

static void run_xref_build(dax_binary_t *bin) {
    if (!bin) return;
    dax_xref_build(bin);
}

static void run_cfg(dax_binary_t *bin, dax_opts_t *opts, FILE *out) {
    if (!bin || !opts) return;
    for (int fi = 0; fi < bin->nfunctions && fi < DAX_MAX_FUNCTIONS; fi++) {
        dax_section_t *s = NULL;
        for (int i = 0; i < bin->nsections && i < DAX_MAX_SECTIONS; i++) {
            dax_section_t *cand = &bin->sections[i];
            uint64_t fs = bin->functions[fi].start;
            if (fs >= cand->vaddr && fs < cand->vaddr + cand->size) { s = cand; break; }
        }
        if (!s || !section_valid(bin, s)) continue;
        dax_cfg_build(bin, bin->data + s->offset, (size_t)s->size, s->vaddr, fi);
    }
    dax_clamp_counts(bin);
    for (int fi = 0; fi < bin->nfunctions && fi < DAX_MAX_FUNCTIONS; fi++) {
        dax_cfg_print(bin, fi, opts, out);
    }
}

static void run_switch(dax_binary_t *bin, dax_opts_t *opts, FILE *out) {
    if (!bin || !opts) return;
    for (int i = 0; i < bin->nsections && i < DAX_MAX_SECTIONS; i++) {
        dax_section_t *s = &bin->sections[i];
        if (!is_code_section(s) || !section_valid(bin, s)) continue;
        dax_switch_detect(bin, opts, bin->data + s->offset, (size_t)s->size, s->vaddr, out);
    }
}

static void run_dsa(dax_binary_t *bin, dax_opts_t *opts, FILE *out) {
    if (!bin || !opts) return;
    void *df = calloc(1, DAX_DSA_FUNC_SIZE);
    if (!df) return;
    for (int fi = 0; fi < bin->nfunctions && fi < DAX_MAX_FUNCTIONS; fi++) {
        dax_dsa_build_func(bin, fi, (dsa_func_t *)df);
        dax_dsa_print(bin, fi, (dsa_func_t *)df, opts, out);
    }
    free(df);
}

/* ============================================================================
 * Advanced static analysis (-w / --advanced-analysis)
 * ============================================================================ */

static void run_advanced(dax_binary_t *bin, dax_opts_t *opts, FILE *out) {
    nd_static_ctx_t ctx;

    nd_static_ctx_init(&ctx, bin, opts);
    ctx.advanced_analysis = 1;

    /* Depth for the caller-chain walk. */
    if (opts->flow_depth > 0) ctx.trace_depth = opts->flow_depth;

    /* The flow trace needs functions and call edges. */
    if (bin->nfunctions == 0) run_func_detect(bin, out);
    if (!bin->xrefs) run_xref_build(bin);
    if (!bin->symbols) dax_sym_load(bin);

    DAX_RUN_PASS("advanced", opts->color, nd_static_analyze(&ctx));

    /* Report */
    nd_ring_print(&ctx.ring, out);
    nd_static_print_order(&ctx, out);
    nd_static_print_parallel(&ctx, out);
    nd_static_print_flow(&ctx, out);

    /* Focus on one variable if requested. */
    if (opts->flow_var[0]) {
        int vi = nd_variable_find_by_name(&ctx, opts->flow_var);
        if (vi < 0) {
            fprintf(out, "\n  [!] variable '%s' not found\n", opts->flow_var);
        } else {
            fprintf(out, "\n=== Caller chain: %s ===\n", ctx.variables[vi].name);
            nd_variable_t *v = &ctx.variables[vi];
            fprintf(out, "  address : 0x%llx\n", (unsigned long long)v->address);
            fprintf(out, "  type    : %s\n",
                    v->type == ND_VAR_GLOBAL ? "global" :
                    v->type == ND_VAR_LOCAL  ? "local"  :
                    v->type == ND_VAR_ARGUMENT ? "argument" :
                    v->type == ND_VAR_RETURN ? "return" : "register");
            for (int i = 0; i < ctx.var_flows.count; i++) {
                nd_var_flow_t *fl = &ctx.var_flows.flows[i];
                if (fl->var_id != v->id) continue;
                fprintf(out, "  chain   : ");
                for (int k = 0; k < fl->chain_len; k++) {
                    for (int j = 0; j < ctx.nfunctions; j++) {
                        if (ctx.functions[j].id == fl->chain[k]) {
                            fprintf(out, "%s%s", k ? "  <-  " : "",
                                    ctx.functions[j].name);
                            break;
                        }
                    }
                }
                fprintf(out, "\n");
            }
        }
    }

    /* Focus on one function. */
    if (opts->flow_func[0]) {
        int fi = nd_function_find_by_name(&ctx, opts->flow_func);
        if (fi < 0) {
            fprintf(out, "\n  [!] function '%s' not found\n", opts->flow_func);
        } else {
            nd_function_t *f = &ctx.functions[fi];
            fprintf(out, "\n=== Function flow: %s ===\n", f->name);
            fprintf(out, "  range   : 0x%llx - 0x%llx\n",
                    (unsigned long long)f->start, (unsigned long long)f->end);
            fprintf(out, "  order   : %d\n", f->execution_order);
            if (f->group_id) {
                for (int g = 0; g < ctx.ngroups; g++) {
                    if (ctx.groups[g].id == f->group_id) {
                        fprintf(out, "  group   : %s\n", ctx.groups[g].name);
                        break;
                    }
                }
            }
            if (f->is_parallel)    fprintf(out, "  parallel: yes\n");
            if (f->is_conditional) fprintf(out, "  cond var: %s\n", f->condition_var);
            fprintf(out, "  callees :\n");
            for (int k = 0; k < f->ncalls; k++) {
                for (int j = 0; j < ctx.nfunctions; j++) {
                    if (ctx.functions[j].id == f->calls[k]) {
                        fprintf(out, "    -> %s\n", ctx.functions[j].name);
                        break;
                    }
                }
            }
        }
    }

    /* SVG output. */
    if (opts->ring_svg[0]) {
        if (nd_static_write_svg(&ctx, opts->ring_svg) == 0) {
            fprintf(out, "\n  Ring SVG written: %s\n", opts->ring_svg);
        }
    }

    nd_static_ctx_free(&ctx);
}

/* ============================================================================
 * main
 * ============================================================================ */

#ifndef NEODAX_NODE

int main(int argc, char **argv) {
    dax_binary_t  bin;
    dax_opts_t    opts;
    dax_plugin_registry_t plugins;
    const char   *path = NULL;
    int           i;

    memset(&bin, 0, sizeof(bin));
    memset(&opts, 0, sizeof(opts));
    memset(&plugins, 0, sizeof(plugins));

    opts.color = 1;
    opts.show_addr = 1;
    opts.symbols = 1;
    opts.start_addr = 0;
    opts.end_addr = (uint64_t)-1;
    snprintf(opts.section, sizeof(opts.section), "%s", ".text");
    opts.flow_depth = 32;

    dax_config_load("config.dax-ng");

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            print_usage(argv[0]);
            return 0;
        }
        else if (!strcmp(a, "-a"))          opts.show_bytes = 1;
        else if (!strcmp(a, "-l"))          opts.list_plugins = opts.list_plugins; /* handled below */
        else if (!strcmp(a, "-n"))          opts.color = 0;
        else if (!strcmp(a, "-v"))          opts.verbose = 1;
        else if (!strcmp(a, "-S"))          opts.all_sections = 1;
        else if (!strcmp(a, "-y"))          opts.symbols = 1;
        else if (!strcmp(a, "-d"))          opts.demangle = 1;
        else if (!strcmp(a, "-f"))          opts.funcs = 1;
        else if (!strcmp(a, "-g"))          opts.groups = 1;
        else if (!strcmp(a, "-r"))          opts.xrefs = 1;
        else if (!strcmp(a, "-t"))          opts.strings = 1;
        else if (!strcmp(a, "-C"))          opts.cfg = 1;
        else if (!strcmp(a, "-L"))          opts.loops = 1;
        else if (!strcmp(a, "-G"))          opts.callgraph = 1;
        else if (!strcmp(a, "-W"))          opts.switches = 1;
        else if (!strcmp(a, "-u"))          opts.unicode = 1;
        else if (!strcmp(a, "-P"))          opts.symexec = 1;
        else if (!strcmp(a, "-Q"))          opts.ssa = 1;
        else if (!strcmp(a, "-D"))          opts.decompile = 1;
        else if (!strcmp(a, "-I"))          opts.emulate = 1;
        else if (!strcmp(a, "-e"))          opts.entropy = 1;
        else if (!strcmp(a, "-R"))          opts.rda = 1;
        else if (!strcmp(a, "-V"))          opts.ivf = 1;
        else if (!strcmp(a, "--poly"))      opts.poly_map = 1;
        else if (!strcmp(a, "--dsa"))       opts.dsa = 1;
        else if (!strcmp(a, "--aire"))      opts.aire = 1;
        else if (!strcmp(a, "--vm-trace"))  opts.vm_trace = 1;
        else if (!strcmp(a, "-i"))          opts.interactive = 1;

        /* -l is "list sections"; it is not a plugin-list flag. */
        else if (!strcmp(a, "-eox-list"))   opts.list_plugins = 1;
        else if (!strcmp(a, "-eox")) {
            if (i + 1 < argc) {
                snprintf(opts.plugin_dir, sizeof(opts.plugin_dir), "%s", argv[++i]);
            }
        }
        else if (!strcmp(a, "-s") || !strcmp(a, "--section")) {
            if (i + 1 < argc) snprintf(opts.section, sizeof(opts.section), "%s", argv[++i]);
        }
        else if (!strcmp(a, "-A")) {
            if (i + 1 < argc) opts.start_addr = strtoull(argv[++i], NULL, 16);
        }
        else if (!strcmp(a, "-E")) {
            if (i + 1 < argc) opts.end_addr = strtoull(argv[++i], NULL, 16);
        }
        else if (!strcmp(a, "-o") || !strcmp(a, "--output")) {
            if (i + 1 < argc) snprintf(opts.output_daxc, sizeof(opts.output_daxc), "%s", argv[++i]);
        }
        else if (!strcmp(a, "-c") || !strcmp(a, "--convert")) {
            if (i + 1 < argc) {
                const char *daxc = argv[++i];
                char asm_out[600];
                snprintf(asm_out, sizeof(asm_out), "%s", daxc);
                size_t al = strlen(asm_out);
                if (al > 2 && !strcmp(asm_out + al - 2, ".daxc")) {
                    asm_out[al - 5] = '\0';
                    snprintf(asm_out + al - 5, sizeof(asm_out) - al + 5, "%s.S", daxc);
                } else {
                    snprintf(asm_out + al, sizeof(asm_out) - al, ".S");
                }
                return dax_daxc_to_asm(daxc, asm_out, opts.color) == 0 ? 0 : 1;
            }
        }
        else if (!strcmp(a, "-w") || !strcmp(a, "--advanced-analysis") ||
                 !strcmp(a, "--advanced")) {
            opts.advanced_analysis = 1;
        }
        else if (!strcmp(a, "--ring-svg")) {
            if (i + 1 < argc) {
                snprintf(opts.ring_svg, sizeof(opts.ring_svg), "%s", argv[++i]);
                opts.advanced_analysis = 1;
            }
        }
        else if (!strcmp(a, "--flow-depth")) {
            if (i + 1 < argc) opts.flow_depth = atoi(argv[++i]);
            opts.advanced_analysis = 1;
        }
        else if (!strcmp(a, "--flow-var")) {
            if (i + 1 < argc) snprintf(opts.flow_var, sizeof(opts.flow_var), "%s", argv[++i]);
            opts.advanced_analysis = 1;
        }
        else if (!strcmp(a, "--flow-func")) {
            if (i + 1 < argc) snprintf(opts.flow_func, sizeof(opts.flow_func), "%s", argv[++i]);
            opts.advanced_analysis = 1;
        }
        else if (!strcmp(a, "-x")) {
            opts.funcs = opts.xrefs = opts.symbols = 1;
            opts.cfg = opts.loops = opts.callgraph = opts.switches = 1;
            opts.strings = opts.groups = opts.unicode = 1;
        }
        else if (!strcmp(a, "-X")) {
            opts.funcs = opts.xrefs = opts.symbols = 1;
            opts.cfg = opts.loops = opts.callgraph = opts.switches = 1;
            opts.strings = opts.groups = opts.unicode = 1;
            opts.symexec = opts.ssa = opts.decompile = opts.emulate = 1;
            opts.entropy = opts.rda = opts.ivf = 1;
            opts.poly_map = opts.aire = opts.vm_trace = opts.dsa = 1;
            opts.advanced_analysis = 1;
        }
        else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "neodax: unknown option '%s'\n", a);
            print_usage(argv[0]);
            return 1;
        }
        else {
            path = a;
            break;
        }
    }

    if (!path) {
        print_usage(argv[0]);
        return 1;
    }

    /* ── Load ───────────────────────────────────────────────────────── */
    if (dax_load_binary(path, &bin) != 0) {
        fprintf(stderr, "neodax: failed to load '%s'\n", path);
        return 1;
    }

    dax_compute_sha256(&bin);
    dax_harden_all(&bin, opts.color);

    /* ── Plugins ────────────────────────────────────────────────────── */
    if (opts.plugin_dir[0]) {
        dax_plugin_load_dir(&plugins, opts.plugin_dir);
    }
    if (opts.list_plugins) {
        dax_plugin_list(&plugins, stdout, opts.color);
    }

    /* ── Standard pipeline ──────────────────────────────────────────── */
    DAX_RUN_PASS("symbols", opts.color, dax_sym_load(&bin));
    dax_clamp_counts(&bin);

    if (opts.funcs || opts.cfg || opts.loops || opts.callgraph || opts.switches ||
        opts.advanced_analysis) {
        DAX_RUN_PASS("functions", opts.color, run_func_detect(&bin, stdout));
    }

    if (opts.xrefs || opts.cfg || opts.callgraph || opts.advanced_analysis) {
        DAX_RUN_PASS("xrefs", opts.color, run_xref_build(&bin));
    }

    if (opts.advanced_analysis) {
        /* The ring analysis runs its own passes; it needs the CFG blocks
         * only for loop detection, which it derives itself. */
        run_advanced(&bin, &opts, stdout);
    }

    /* Banner */
    if (!opts.advanced_analysis) {
        dax_print_banner(&bin, &opts);
    }

    if (opts.list_plugins) {
        dax_plugin_list(&plugins, stdout, opts.color);
    }

    /* Sections */
    if (opts.list_sections) dax_print_sections(&bin, &opts);

    /* Disassembly */
    if (!opts.advanced_analysis) {
        DAX_RUN_PASS("disasm", opts.color, run_disasm(&bin, &opts, stdout));
    }

    /* CFG */
    if (opts.cfg) {
        DAX_RUN_PASS("cfg", opts.color, run_cfg(&bin, &opts, stdout));
    }

    /* Loops */
    if (opts.loops) {
        DAX_RUN_PASS("loops", opts.color, dax_loop_print_all(&bin, &opts, stdout));
    }

    /* Call graph */
    if (opts.callgraph) {
        DAX_RUN_PASS("callgraph", opts.color, dax_callgraph_print(&bin, &opts, stdout));
    }

    /* Switch tables */
    if (opts.switches) {
        DAX_RUN_PASS("switches", opts.color, run_switch(&bin, &opts, stdout));
    }

    /* Unicode */
    if (opts.unicode) {
        DAX_RUN_PASS("unicode", opts.color,
                     do { dax_scan_unicode(&bin);
                          dax_print_unicode_strings(&bin, &opts, stdout); } while (0));
    }

    /* Symbolic execution */
    if (opts.symexec) {
        DAX_RUN_PASS("symexec", opts.color, dax_symexec_all(&bin, &opts, stdout));
    }

    /* NR lifting */
    if (opts.ssa) {
        DAX_RUN_PASS("nr", opts.color, dax_ssa_lift_all(&bin, &opts, stdout));
    }

    /* Decompiler */
    if (opts.decompile) {
        DAX_RUN_PASS("decompile", opts.color, dax_decompile_all(&bin, &opts, stdout));
    }

    /* Emulator */
    if (opts.emulate) {
        DAX_RUN_PASS("emulate", opts.color, dax_emulate_all(&bin, &opts, stdout));
    }

    /* DSA */
    if (opts.dsa) {
        DAX_RUN_PASS("dsa", opts.color, run_dsa(&bin, &opts, stdout));
    }

    /* Entropy */
    if (opts.entropy) {
        DAX_RUN_PASS("entropy", opts.color, dax_entropy_scan(&bin, &opts, stdout));
    }

    /* Recursive descent */
    if (opts.rda) {
        DAX_RUN_PASS("rda", opts.color, dax_rda_all(&bin, &opts, stdout));
    }

    /* IVF */
    if (opts.ivf) {
        DAX_RUN_PASS("ivf", opts.color, dax_ivf_scan(&bin, &opts, stdout));
    }

    /* Polymorphic map */
    if (opts.poly_map) {
        DAX_RUN_PASS("poly", opts.color, dax_poly_map(&bin, &opts, stdout));
    }

    /* AIRE */
    if (opts.aire) {
        DAX_RUN_PASS("aire", opts.color, dax_aire_analyze(&bin, &opts, stdout));
    }

    /* VM trace */
    if (opts.vm_trace) {
        DAX_RUN_PASS("vmtrace", opts.color, dax_vm_trace(&bin, &opts, stdout));
    }

    /* Plugin hooks */
    dax_plugin_run_post_load(&plugins, &bin, &opts);
    dax_plugin_run_pre_disasm(&plugins, &bin, &opts, stdout);
    dax_plugin_run_post_disasm(&plugins, &bin, &opts, stdout);
    dax_plugin_run_ivf(&plugins, &bin, &opts, stdout);

    /* .daxc snapshot */
    if (opts.output_daxc[0]) {
        DAX_RUN_PASS("daxc", opts.color,
                     dax_daxc_write(&bin, &opts, opts.output_daxc));
        if (opts.output_daxc[0]) {
            fprintf(stdout, "\n  Snapshot written: %s\n", opts.output_daxc);
        }
    }

    /* Interactive shell */
    if (opts.interactive) {
        dax_interactive(&bin, &opts);
    }

    dax_plugin_free(&plugins);
    dax_free_binary_full(&bin);
    return 0;
}

#endif /* NEODAX_NODE */
