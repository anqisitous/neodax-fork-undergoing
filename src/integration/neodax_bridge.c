#include "neodax_bridge.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>

/* ================================================================== */
/* output helpers                                                      */
/* ================================================================== */

static pthread_mutex_t g_out_lock = PTHREAD_MUTEX_INITIALIZER;

static void safe_printf(FILE *out, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&g_out_lock);
    vfprintf(out ? out : stdout, fmt, ap);
    pthread_mutex_unlock(&g_out_lock);
    va_end(ap);
}

static const char *var_cat_str(int c)  { return var_category_to_string((var_category_t)c); }
static const char *var_asp_str(int a)  { return var_aspect_to_string((var_aspect_t)a); }
static const char *var_acc_str(int a)  { return var_access_pattern_to_string((var_access_pattern_t)a); }
static const char *var_typ_str(int t)  { return var_data_type_to_string((var_data_type_t)t); }

static const char *parallel_type_str(int t) {
    switch (t) {
        case PARALLEL_TYPE_BEAM_SEARCH:   return "beam-search";
        case PARALLEL_TYPE_THREAD_POOL:   return "thread-pool";
        case PARALLEL_TYPE_TASK_PARALLEL: return "task-parallel";
        case PARALLEL_TYPE_DATA_PARALLEL: return "data-parallel";
        case PARALLEL_TYPE_PIPELINE:      return "pipeline";
        case PARALLEL_TYPE_MAP_REDUCE:    return "map-reduce";
        default:                          return "unknown";
    }
}

static const char *dispatch_type_str(int t) {
    switch (t) {
        case PARALLEL_DISPATCH_INDIRECT:   return "indirect";
        case PARALLEL_DISPATCH_DYNAMIC:    return "dynamic";
        case PARALLEL_DISPATCH_WORK_QUEUE: return "work-queue";
        case PARALLEL_DISPATCH_BEAM:       return "beam";
        default:                           return "direct";
    }
}

static const char *sync_type_str(int t) {
    switch (t) {
        case PARALLEL_SYNC_BARRIER:   return "barrier";
        case PARALLEL_SYNC_LOCK:      return "lock";
        case PARALLEL_SYNC_SEMAPHORE: return "semaphore";
        case PARALLEL_SYNC_ATOMIC:    return "atomic";
        case PARALLEL_SYNC_FUTURE:    return "future";
        case PARALLEL_SYNC_MUTEX:     return "mutex";
        default:                      return "none";
    }
}

static const char *state_value_type_str(int t) {
    switch (t) {
        case STATE_VALUE_TYPE_BOARD:       return "board";
        case STATE_VALUE_TYPE_MOVE:        return "move";
        case STATE_VALUE_TYPE_SCORE:       return "score";
        case STATE_VALUE_TYPE_DEPTH:       return "depth";
        case STATE_VALUE_TYPE_NODE:        return "node";
        case STATE_VALUE_TYPE_SEARCH_TREE: return "search-tree";
        case STATE_VALUE_TYPE_PLY:         return "ply";
        case STATE_VALUE_TYPE_HASH:        return "hash";
        case STATE_VALUE_TYPE_EVAL:        return "eval";
        default:                           return "unknown";
    }
}

/* ================================================================== */
/* init / free                                                         */
/* ================================================================== */

int neodax_bridge_init(integrated_binary_t *ibin, const uint8_t *data, size_t size) {
    if (!ibin || !data || size == 0) return -1;

    memset(ibin, 0, sizeof(*ibin));
    macho_init(&ibin->macho_bin);

    /* macho_parse は data を保持しないので呼び出し側が設定する。
       macho_free が data を free する契約。 */
    ibin->macho_bin.data = (uint8_t *)data;
    ibin->macho_bin.size = size;

    if (macho_parse(&ibin->macho_bin, data, size) != 0) {
        ibin->macho_bin.data = NULL;   /* caller が free する */
        macho_free(&ibin->macho_bin);
        return -1;
    }

    /* ヘッダ表示/依存解析のために関数境界をここで確定 (冪等) */
    macho_detect_functions(&ibin->macho_bin);

    /* 自律ディスパッチ: 形式を検出し provider を選ぶ */
    ibin->bin = nx_macho_wrap(&ibin->macho_bin);
    if (!ibin->bin) return -1;
    ibin->backend_caps = ibin->bin->ops->caps;
    ibin->backend_name = ibin->bin->ops->name;

    ibin->engine = nx_engine_new(ibin->bin, 1, stdout);
    if (!ibin->engine) {
        nx_close(ibin->bin);
        ibin->bin = NULL;
        macho_free(&ibin->macho_bin);
        return -1;
    }
    return 0;
}

void neodax_bridge_free(integrated_binary_t *ibin) {
    if (!ibin) return;

    if (ibin->dep_graph) { depflow_graph_free(ibin->dep_graph); free(ibin->dep_graph); }
    if (ibin->classifier) { var_classifier_free(ibin->classifier); free(ibin->classifier); }
    if (ibin->exec_graph) { exec_order_graph_free(ibin->exec_graph); free(ibin->exec_graph); }
    if (ibin->parallel)   { parallel_analysis_free(ibin->parallel); free(ibin->parallel); }
    if (ibin->state)      { state_graph_free(ibin->state); free(ibin->state); }

    if (ibin->engine) nx_engine_free(ibin->engine);
    if (ibin->bin)    nx_close(ibin->bin);      /* borrow なので bin 本体は free しない */

    /* data の所有権は呼び出し側 (main_integrated の file_data) にある。
       ここで macho_free に渡すと二重解放になるので切り離してから配列だけ解放。 */
    ibin->macho_bin.data = NULL;
    macho_free(&ibin->macho_bin);
    memset(ibin, 0, sizeof(*ibin));
}

/* ================================================================== */
/* pipeline                                                            */
/* ================================================================== */

static void harvest_artifacts(integrated_binary_t *ibin) {
    nx_engine_t *e = ibin->engine;
    if (!e) return;
    if (e->art[NX_SLOT_DEPS])     ibin->dep_graph  = (depflow_graph_t *)e->art[NX_SLOT_DEPS];
    if (e->art[NX_SLOT_VARS])     { ibin->classifier = (var_classifier_t *)e->art[NX_SLOT_VARS];
                                   ibin->variables_classified = 1; }
    if (e->art[NX_SLOT_EXEC])     { ibin->exec_graph = (exec_order_graph_t *)e->art[NX_SLOT_EXEC];
                                   ibin->execution_order_determined = 1; }
    if (e->art[NX_SLOT_PARALLEL]) { ibin->parallel   = (parallel_analysis_t *)e->art[NX_SLOT_PARALLEL];
                                   ibin->has_parallel_execution = ibin->parallel->group_count > 0;
                                   ibin->has_beam_search = ibin->parallel->beam_search_count > 0; }
    if (e->art[NX_SLOT_STATE])    ibin->state = (state_graph_t *)e->art[NX_SLOT_STATE];
}

/* provider が宣言した pass を nx エンジンで実行 (直列/並列は自動決定) */
static int run_engine(integrated_binary_t *ibin, int nthreads) {
    nx_engine_t *e = ibin->engine;
    if (!e || !ibin->bin || !ibin->bin->ops->passes) return -1;
    if (e->produced & NX_ART_DEPS) { harvest_artifacts(ibin); return 0; }  /* 実行済み */

    int np = 0;
    const nx_pass_t *passes = ibin->bin->ops->passes(&np);
    e->nthreads = nthreads > 0 ? nthreads : 1;

    int rc = nx_engine_run(e, passes, np);
    harvest_artifacts(ibin);
    return rc;
}

int neodax_run_static_analysis(integrated_binary_t *ibin,
                               static_analysis_opts_t *opts, FILE *output) {
    if (!ibin || !opts) return -1;

    safe_printf(output, "\n=== Static Mach-O Analysis ===\n");
    safe_printf(output, "  backend: %s (caps=0x%x)\n",
                ibin->backend_name ? ibin->backend_name : "?", ibin->backend_caps);

    int rc = run_engine(ibin, opts->num_threads);
    if (rc != 0) {
        safe_printf(output, "  ERROR: analysis pipeline failed (rc=%d)\n", rc);
        return rc;
    }

    safe_printf(output, "  dependency graph : %d nodes / %d edges\n",
                ibin->dep_graph ? ibin->dep_graph->node_count : 0,
                ibin->dep_graph ? ibin->dep_graph->edge_count : 0);
    safe_printf(output, "  execution graph  : %d nodes / %d edges\n",
                ibin->exec_graph ? ibin->exec_graph->node_count : 0,
                ibin->exec_graph ? ibin->exec_graph->edge_count : 0);
    safe_printf(output, "  parallel groups  : %d (beam=%d)\n",
                ibin->parallel ? ibin->parallel->group_count : 0,
                ibin->parallel ? ibin->parallel->beam_search_count : 0);
    safe_printf(output, "  state checkpoints: %d\n",
                ibin->state ? ibin->state->checkpoint_count : 0);

    if (opts->workflow_output[0]) {
        if (neodax_emit_workflow(ibin, opts->workflow_output) == 0)
            safe_printf(output, "  workflow code    : %s\n", opts->workflow_output);
    }
    safe_printf(output, "=== Static Analysis Complete ===\n");
    return 0;
}

/* Recovered workflow -> compilable C++ (function = step, call = transition,
 * guard = then/now, external/indirect/store/parallel as stubs). */
int neodax_emit_workflow(integrated_binary_t *ibin, const char *filename) {
    if (!ibin || !filename || !filename[0]) return -1;
    if (!ibin->dep_graph) return -1;
    depflow_export_workflow(&ibin->macho_bin, ibin->dep_graph, filename);
    return 0;
}

/* ================================================================== */
/* autonomous scheduling demo / complement                             */
/* ================================================================== */

int neodax_parallel_complement(integrated_binary_t *ibin,
                               static_analysis_opts_t *opts, FILE *output) {
    if (!ibin || !opts) return -1;

    safe_printf(output, "\n=== Autonomous Schedule (直列/並列の自動決定) ===\n");

    int np = 0;
    const nx_pass_t *passes = ibin->bin && ibin->bin->ops->passes
                            ? ibin->bin->ops->passes(&np) : NULL;
    if (!passes || np <= 0) {
        safe_printf(output, "  no passes available for backend\n");
        return 0;
    }

    nx_pass_plan_t plan[256];
    int nplan = nx_plan(passes, np, ibin->engine ? ibin->engine->produced : 0,
                        plan, 256);
    int wave = -1;
    for (int i = 0; i < nplan; i++) {
        if (plan[i].wave != wave) {
            wave = plan[i].wave;
            safe_printf(output, "  wave %d (%s):\n", wave,
                        plan[i].parallel ? "parallel" : "serial");
        }
        safe_printf(output, "    - %-16s reads=0x%02x writes=0x%02x\n",
                    plan[i].pass->name, plan[i].pass->reads, plan[i].pass->writes);
    }
    ibin->plan_waves = wave + 1;

    int rc = run_engine(ibin, opts->num_threads);
    safe_printf(output, "  pipeline %s\n", rc == 0 ? "complete" : "failed");
    return rc;
}

/* ================================================================== */
/* variable classification                                             */
/* ================================================================== */

int neodax_classify_variables(integrated_binary_t *ibin,
                              static_analysis_opts_t *opts, FILE *output) {
    if (!ibin || !opts) return -1;
    safe_printf(output, "\n=== Variable Classification ===\n");

    if (!ibin->variables_classified || !ibin->classifier) {
        safe_printf(output, "  variables not classified (run --classify-vars)\n");
        return 0;
    }

    var_classifier_t *vc = ibin->classifier;
    for (int i = 0; i < vc->num_classifications; i++) {
        var_classification_t *c = &vc->classifications[i];
        safe_printf(output, "  %s (0x%llx, size=%llu)\n",
                    c->name, (unsigned long long)c->address,
                    (unsigned long long)c->size);
        safe_printf(output, "    category : %s (%d%%)\n",
                    var_cat_str(c->primary_category), c->category_confidence);
        safe_printf(output, "    data type: %s\n", var_typ_str(c->data_type));
        safe_printf(output, "    access   : %s\n", var_acc_str(c->access_pattern));
        if (c->num_aspects > 0) {
            safe_printf(output, "    aspects  :");
            for (int j = 0; j < c->num_aspects; j++)
                safe_printf(output, " %s", var_asp_str(c->aspects[j]));
            safe_printf(output, "\n");
        }
        if (c->is_shogi_specific)
            safe_printf(output, "    role: %d\n", c->shogi_role);
    }

    safe_printf(output, "  total=%d board=%d move=%d eval=%d search-tree=%d\n",
                vc->num_classifications,
                vc->categories[VAR_CAT_BOARD] + vc->categories[VAR_CAT_BITBOARD],
                vc->categories[VAR_CAT_MOVE] + vc->categories[VAR_CAT_MOVE_TREE],
                vc->categories[VAR_CAT_EVALUATION] + vc->categories[VAR_CAT_SCORE],
                vc->categories[VAR_CAT_SEARCH_TREE] + vc->categories[VAR_CAT_NODE]);
    return 0;
}

/* ================================================================== */
/* execution order                                                     */
/* ================================================================== */

int neodax_predict_execution_order(integrated_binary_t *ibin,
                                   static_analysis_opts_t *opts, FILE *output) {
    if (!ibin || !opts) return -1;
    safe_printf(output, "\n=== Execution Order Prediction ===\n");

    if (!ibin->exec_graph || ibin->exec_graph->node_count == 0) {
        safe_printf(output, "  execution graph not built\n");
        return 0;
    }
    exec_order_graph_t *g = ibin->exec_graph;
    safe_printf(output, "  %d nodes / %d edges\n", g->node_count, g->edge_count);

    uint64_t order[EXEC_ORDER_MAX_NODES];
    int n = exec_order_topological_sort(g, order, EXEC_ORDER_MAX_NODES);
    if (n > 0) {
        safe_printf(output, "  topological order (first %d):\n", n < 32 ? n : 32);
        for (int i = 0; i < n && i < 32; i++) {
            exec_node_t *node = NULL;
            for (int k = 0; k < g->node_count; k++)
                if (g->nodes[k].id == order[i]) { node = &g->nodes[k]; break; }
            if (node)
                safe_printf(output, "    %2d: %s (0x%llx)\n",
                            i + 1, node->name, (unsigned long long)node->address);
        }
    }
    exec_order_critical_path_analysis(g);
    safe_printf(output, "  critical path length: %llu\n",
                (unsigned long long)g->critical_path_length);
    ibin->execution_order_determined = 1;
    return 0;
}

/* ================================================================== */
/* parallel detection                                                  */
/* ================================================================== */

int neodax_detect_parallel(integrated_binary_t *ibin,
                           static_analysis_opts_t *opts, FILE *output) {
    if (!ibin || !opts) return -1;
    safe_printf(output, "\n=== Parallel Execution Detection ===\n");

    if (!ibin->parallel) { safe_printf(output, "  not analyzed\n"); return 0; }
    parallel_analysis_t *pa = ibin->parallel;

    safe_printf(output, "  groups=%d dispatch=%d beam=%d\n",
                pa->group_count, pa->dispatch_count, pa->beam_search_count);

    for (int i = 0; i < pa->group_count; i++) {
        parallel_group_t *gr = &pa->groups[i];
        safe_printf(output, "  group #%d %s: type=%s dispatch=%s sync=%s workers=%d\n",
                    i + 1, gr->name, parallel_type_str(gr->type),
                    dispatch_type_str(gr->dispatch_type), sync_type_str(gr->sync_type),
                    gr->worker_count);
    }
    for (int i = 0; i < pa->beam_search_count; i++) {
        beam_search_info_t *b = &pa->beam_searches[i];
        safe_printf(output, "  beam #%d: %s (0x%llx) width=%llu depth=%llu\n",
                    i + 1, b->dispatch_function, (unsigned long long)b->dispatch_addr,
                    (unsigned long long)b->beam_width, (unsigned long long)b->max_depth);
    }
    return 0;
}

/* ================================================================== */
/* state tracking                                                      */
/* ================================================================== */

int neodax_track_state(integrated_binary_t *ibin,
                       static_analysis_opts_t *opts, FILE *output) {
    if (!ibin || !opts) return -1;
    safe_printf(output, "\n=== State Tracking ===\n");

    if (!ibin->state) { safe_printf(output, "  not analyzed\n"); return 0; }
    state_graph_t *sg = ibin->state;
    safe_printf(output, "  checkpoints=%d variables=%d changes=%d regressions=%d\n",
                sg->checkpoint_count, sg->variable_count,
                sg->change_count, sg->regression_count);

    for (int i = 0; i < sg->checkpoint_count && i < 16; i++) {
        state_checkpoint_t *cp = &sg->checkpoints[i];
        safe_printf(output, "  cp #%d %s (0x%llx)%s%s\n",
                    i + 1, cp->name, (unsigned long long)cp->address,
                    cp->is_entry ? " [entry]" : "",
                    cp->is_exit ? " [exit]" : "");
    }
    for (int i = 0; i < sg->regression_count; i++) {
        regression_result_t *r = &sg->regressions[i];
        safe_printf(output, "  regression #%d type=%d sig=%d: %s\n",
                    i + 1, r->regression_type, r->is_significant, r->description);
    }
    return 0;
}

/* ================================================================== */
/* shogi AI analysis                                                   */
/* ================================================================== */

int neodax_shogi_analysis(integrated_binary_t *ibin,
                          static_analysis_opts_t *opts, FILE *output) {
    if (!ibin || !opts) return -1;
    safe_printf(output, "\n=== Pattern Analysis ===\n");

    if (ibin->classifier) {
        var_classifier_t *vc = ibin->classifier;
        safe_printf(output, "  board=%d move=%d eval=%d search-tree=%d\n",
                    vc->categories[VAR_CAT_BOARD] + vc->categories[VAR_CAT_BITBOARD],
                    vc->categories[VAR_CAT_MOVE] + vc->categories[VAR_CAT_MOVE_TREE],
                    vc->categories[VAR_CAT_EVALUATION] + vc->categories[VAR_CAT_SCORE],
                    vc->categories[VAR_CAT_SEARCH_TREE] + vc->categories[VAR_CAT_NODE]);
    }
    if (ibin->parallel) {
        for (int i = 0; i < ibin->parallel->beam_search_count; i++) {
            beam_search_info_t *b = &ibin->parallel->beam_searches[i];
            safe_printf(output, "  beam search: %s state=%s node=%s move=%s\n",
                        b->dispatch_function, b->state_type, b->node_type, b->move_type);
        }
    }
    if (ibin->state) {
        for (int i = 0; i < ibin->state->variable_count; i++) {
            state_variable_t *v = &ibin->state->variables[i];
            if (v->value_type != STATE_VALUE_TYPE_UNKNOWN)
                safe_printf(output, "  state var %s: %s\n",
                            v->name, state_value_type_str(v->value_type));
        }
    }
    return 0;
}

/* ================================================================== */
/* bend-inspired analysis                                              */
/* ================================================================== */

int neodax_bend_analysis(integrated_binary_t *ibin,
                         static_analysis_opts_t *opts, FILE *output) {
    if (!ibin || !opts) return -1;
    safe_printf(output, "\n=== Bend-Inspired Analysis ===\n");

    if (ibin->parallel) {
        parallel_analysis_t *pa = ibin->parallel;
        if (pa->beam_search_count > 0) ibin->parallel_type = BEND_PARALLEL_BEAM_SEARCH;
        else if (pa->group_count > 0)  ibin->parallel_type = BEND_PARALLEL_TASK;
        safe_printf(output, "  parallel type: %d (groups=%d dispatch=%d)\n",
                    ibin->parallel_type, pa->group_count, pa->dispatch_count);
        for (int i = 0; i < pa->dispatch_count && i < 16; i++)
            safe_printf(output, "    dispatch %s (0x%llx)\n",
                        pa->dispatch_points[i].func_name,
                        (unsigned long long)pa->dispatch_points[i].addr);
    }
    if (opts->use_bend_concepts && ibin->state) {
        safe_printf(output, "  proof-based regressions: %d\n",
                    ibin->state->regression_count);
        ibin->has_proof_based_analysis = 1;
    }
    return 0;
}

/* ================================================================== */
/* DOT output                                                          */
/* ================================================================== */

int neodax_generate_dot(integrated_binary_t *ibin,
                        static_analysis_opts_t *opts, const char *filename) {
    if (!ibin || !filename) return -1;
    char buf[1024];

    if (opts->show_dependency_graph && ibin->dep_graph) {
        depflow_export_dot(ibin->dep_graph, filename);
        safe_printf(stdout, "  wrote %s (dependency)\n", filename);
    }
    if (opts->show_execution_graph && ibin->exec_graph) {
        snprintf(buf, sizeof(buf), "%s.exec", filename);
        exec_order_export_dot(ibin->exec_graph, buf);
        safe_printf(stdout, "  wrote %s (execution)\n", buf);
    }
    if (opts->show_state_graph && ibin->state) {
        snprintf(buf, sizeof(buf), "%s.state", filename);
        state_export_dot(ibin->state, buf);
        safe_printf(stdout, "  wrote %s (state)\n", buf);
    }
    if (opts->show_classification && ibin->classifier) {
        snprintf(buf, sizeof(buf), "%s.vars", filename);
        var_classifier_export_dot(ibin->classifier, buf);
        safe_printf(stdout, "  wrote %s (variables)\n", buf);
    }
    return 0;
}

/* ================================================================== */
/* legacy neodax integration                                           */
/* ================================================================== */

int neodax_integrate_with_binary(void *dax_bin,
                                 static_analysis_opts_t *opts, FILE *output) {
    (void)opts;
    return nx_dax_integrate(dax_bin, output);
}
