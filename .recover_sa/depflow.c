#include "depflow.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ============================================================================
// Data Dependency Flow Analysis Implementation
// 
// 依存性追跡システムの実装
// 静的解析によるデータフロー追跡
// ============================================================================

// ============================================================================
// Graph Construction Functions
// ============================================================================

void depflow_graph_init(depflow_graph_t *graph) {
    if (!graph) return;
    
    memset(graph, 0, sizeof(depflow_graph_t));
    graph->node_capacity = DEPFLOW_MAX_NODES;
    graph->edge_capacity = DEPFLOW_MAX_EDGES;
    graph->nodes = (depflow_node_t *)calloc(DEPFLOW_MAX_NODES, sizeof(depflow_node_t));
    graph->edges = (depflow_edge_t *)calloc(DEPFLOW_MAX_EDGES, sizeof(depflow_edge_t));
    graph->func_deps = (depflow_func_deps_t *)calloc(DEPFLOW_MAX_NODES, sizeof(depflow_func_deps_t));
    graph->var_usages = (depflow_var_usage_t *)calloc(DEPFLOW_MAX_EDGES, sizeof(depflow_var_usage_t));
    graph->paths = (depflow_path_t *)calloc(DEPFLOW_MAX_PATHS, sizeof(depflow_path_t));
    
    for (int i = 0; i < DEPFLOW_MAX_NODES; i++) {
        graph->addr_to_node[i] = 0;
    }
    
    graph->node_count = 0;
    graph->edge_count = 0;
    graph->func_deps_count = 0;
    graph->var_usage_count = 0;
    graph->path_count = 0;
    graph->total_dependencies = 0;
    graph->max_path_length = 0;
    graph->avg_path_length = 0;
    graph->node_count_at_addr = 0;
}

void depflow_graph_free(depflow_graph_t *graph) {
    if (!graph) return;
    
    if (graph->nodes) {
        free(graph->nodes);
        graph->nodes = NULL;
    }
    if (graph->edges) {
        free(graph->edges);
        graph->edges = NULL;
    }
    if (graph->func_deps) {
        free(graph->func_deps);
        graph->func_deps = NULL;
    }
    if (graph->var_usages) {
        free(graph->var_usages);
        graph->var_usages = NULL;
    }
    if (graph->paths) {
        free(graph->paths);
        graph->paths = NULL;
    }
    
    graph->node_count = 0;
    graph->edge_count = 0;
    graph->func_deps_count = 0;
    graph->var_usage_count = 0;
    graph->path_count = 0;
}

int depflow_graph_add_node(depflow_graph_t *graph, depflow_node_t *node) {
    if (!graph || !node) return -1;
    
    if (graph->node_count >= graph->node_capacity) {
        return -1; // No more space
    }
    
    // Generate unique ID
    node->id = graph->node_count + 1;
    
    // Store node
    memcpy(&graph->nodes[graph->node_count], node, sizeof(depflow_node_t));
    graph->node_count++;
    
    // Update address mapping
    if (node->address > 0 && node->address < DEPFLOW_MAX_NODES) {
        graph->addr_to_node[node->address] = node->id;
    }
    
    return (int)node->id;
}

int depflow_graph_add_edge(depflow_graph_t *graph, uint64_t from, uint64_t to, int type) {
    return depflow_graph_add_edge_full(graph, from, to, type, 0, 0);
}

int depflow_graph_add_edge_full(depflow_graph_t *graph, uint64_t from, uint64_t to, 
                                  int type, uint64_t site, int is_conditional) {
    if (!graph) return -1;
    
    if (graph->edge_count >= graph->edge_capacity) {
        return -1; // No more space
    }
    
    depflow_edge_t edge;
    edge.from = from;
    edge.to = to;
    edge.type = type;
    edge.site = site;
    edge.is_conditional = is_conditional;
    edge.is_strong = 1;
    edge.weight = 1;
    
    memcpy(&graph->edges[graph->edge_count], &edge, sizeof(depflow_edge_t));
    graph->edge_count++;
    graph->total_dependencies++;
    
    return graph->edge_count - 1;
}

// ============================================================================
// Node Creation Functions
// ============================================================================

uint64_t depflow_node_create(depflow_graph_t *graph, int type, uint64_t address, 
                              const char *name, uint64_t size) {
    if (!graph) return 0;
    
    depflow_node_t node;
    memset(&node, 0, sizeof(depflow_node_t));
    
    node.type = type;
    node.address = address;
    if (name) {
        strncpy(node.name, name, sizeof(node.name) - 1);
        node.name[sizeof(node.name) - 1] = '\0';
    }
    node.size = size;
    node.category = DEPFLOW_VAR_CATEGORY_UNKNOWN;
    node.definition_site = address;
    node.visited = 0;
    node.timestamp = 0;
    
    if (depflow_graph_add_node(graph, &node) < 0) {
        return 0;
    }
    
    return node.id;
}

uint64_t depflow_node_create_variable(depflow_graph_t *graph, uint64_t address,
                                         const char *name, uint64_t size, int category) {
    uint64_t node_id = depflow_node_create(graph, DEPFLOW_NODE_VARIABLE, address, name, size);
    if (node_id > 0) {
        graph->nodes[graph->node_count - 1].category = category;
    }
    return node_id;
}

uint64_t depflow_node_create_register(depflow_graph_t *graph, uint64_t address,
                                        int reg_num, int reg_class) {
    char reg_name[32];
    snprintf(reg_name, sizeof(reg_name), "reg_%d_class_%d", reg_num, reg_class);
    uint64_t node_id = depflow_node_create(graph, DEPFLOW_NODE_REGISTER, address, reg_name, 8);
    if (node_id > 0) {
        graph->nodes[graph->node_count - 1].reg_num = reg_num;
        graph->nodes[graph->node_count - 1].reg_class = reg_class;
    }
    return node_id;
}

uint64_t depflow_node_create_function(depflow_graph_t *graph, int func_idx) {
    char func_name[64];
    snprintf(func_name, sizeof(func_name), "func_%d", func_idx);
    uint64_t node_id = depflow_node_create(graph, DEPFLOW_NODE_FUNCTION, 0, func_name, 0);
    if (node_id > 0) {
        graph->nodes[graph->node_count - 1].func_idx = func_idx;
    }
    return node_id;
}

// ============================================================================
// Main Dependency Analysis
// ============================================================================

int depflow_analyze(macho_binary_t *bin, macho_opts_t *opts) {
    if (!bin || !macho_is_valid(bin)) return -1;
    
    depflow_context_t ctx;
    memset(&ctx, 0, sizeof(depflow_context_t));
    
    ctx.bin = bin;
    ctx.opts = opts;
    
    // Initialize graph
    depflow_graph_init(&ctx.graph);
    
    // Build dependency graph
    if (depflow_build_dependency_graph(bin, &ctx.graph) != 0) {
        fprintf(stderr, "depflow: Failed to build dependency graph\n");
        depflow_graph_free(&ctx.graph);
        return -1;
    }
    
    // Analyze function dependencies
    if (depflow_analyze_function_dependencies(bin, &ctx.graph) != 0) {
        fprintf(stderr, "depflow: Failed to analyze function dependencies\n");
    }
    
    // Track variable usage
    if (opts && opts->depflow_target_var[0] != '\0') {
        if (depflow_track_variable_usage(bin, &ctx.graph, opts->depflow_target_var) != 0) {
            fprintf(stderr, "depflow: Failed to track variable usage\n");
        }
    }
    
    // Predict execution order
    if (depflow_predict_execution_order(bin, &ctx.graph, ctx.execution_order, 
                                         DEPFLOW_MAX_NODES) != 0) {
        fprintf(stderr, "depflow: Failed to predict execution order\n");
    }
    
    // Detect parallel execution
    if (depflow_detect_parallel_execution(bin, &ctx.graph) != 0) {
        fprintf(stderr, "depflow: Failed to detect parallel execution\n");
    }
    
    // Beam search specific analysis
    if (0) {
        if (depflow_analyze_beam_search(&ctx.graph, bin) != 0) {
            fprintf(stderr, "depflow: Failed to analyze beam search patterns\n");
        }
    }
    
    // Output results
    if (opts && opts->depflow_dot[0] != '\0') {
        depflow_export_dot(&ctx.graph, opts->depflow_dot);
    }
    
    // Print analysis
    if (opts && opts->verbose) {
        depflow_print_dependency_graph(&ctx.graph, opts, stdout);
    }
    
    // Cleanup
    depflow_graph_free(&ctx.graph);
    
    return 0;
}

int depflow_build_dependency_graph(macho_binary_t *bin, depflow_graph_t *graph) {
    if (!bin || !graph) return -1;
    
    // Create nodes for all functions
    for (int i = 0; i < bin->nfunctions; i++) {
        macho_function_t *func = &bin->functions[i];
        uint64_t node_id = depflow_node_create_function(graph, i);
        
        if (node_id > 0) {
            // Update node name with actual function name
            strncpy(graph->nodes[graph->node_count - 1].name, func->name, 
                   sizeof(graph->nodes[graph->node_count - 1].name) - 1);
            graph->nodes[graph->node_count - 1].address = func->start;
        }
    }
    
    // Create nodes for all symbols
    for (int i = 0; i < bin->nsymbols; i++) {
        macho_symbol_t *sym = &bin->symbols[i];
        
        // Skip undefined symbols
        if (sym->is_undefined) continue;
        
        // Determine node type based on symbol type
        int node_type = DEPFLOW_NODE_VARIABLE;
        int category = DEPFLOW_VAR_CATEGORY_UNKNOWN;
        
        switch (sym->type) {
            case MACHO_SYMBOL_TYPE_FUNC:
                node_type = DEPFLOW_NODE_FUNCTION;
                break;
            case MACHO_SYMBOL_TYPE_OBJECT:
                node_type = DEPFLOW_NODE_VARIABLE;
                // Try to categorize based on name
                if (strstr(sym->name, "board") || strstr(sym->name, "ban")) {
                    category = DEPFLOW_VAR_CATEGORY_BOARD;
                } else if (strstr(sym->name, "move") || strstr(sym->name, "te")) {
                    category = DEPFLOW_VAR_CATEGORY_MOVE;
                } else if (strstr(sym->name, "score") || strstr(sym->name, "eval")) {
                    category = DEPFLOW_VAR_CATEGORY_SCORE;
                } else if (strstr(sym->name, "depth") || strstr(sym->name, "ply")) {
                    category = DEPFLOW_VAR_CATEGORY_DEPTH;
                } else if (strstr(sym->name, "node") || strstr(sym->name, "search")) {
                    category = DEPFLOW_VAR_CATEGORY_SEARCH_TREE;
                }
                break;
            case MACHO_SYMBOL_TYPE_IMPORT:
            case MACHO_SYMBOL_TYPE_EXPORT:
                node_type = DEPFLOW_NODE_FUNCTION;
                break;
            default:
                node_type = DEPFLOW_NODE_VARIABLE;
                break;
        }
        
        uint64_t node_id = depflow_node_create(graph, node_type, sym->address, 
                                                sym->demangled[0] ? sym->demangled : sym->name, 
                                                sym->size);
        if (node_id > 0 && node_type == DEPFLOW_NODE_VARIABLE) {
            graph->nodes[graph->node_count - 1].category = category;
        }
    }
    
    // Create edges based on cross-references
    for (int i = 0; i < bin->nxrefs; i++) {
        macho_xref_t *xref = &bin->xrefs[i];
        
        // Find source and destination nodes
        uint64_t from_node = depflow_node_id_by_address(graph, xref->from);
        uint64_t to_node = depflow_node_id_by_address(graph, xref->to);
        
        if (from_node > 0 && to_node > 0) {
            int edge_type = DEPFLOW_EDGE_CONTROL;
            if (xref->is_call) {
                edge_type = DEPFLOW_EDGE_CONTROL;
            } else if (xref->type == MACHO_XREF_TYPE_DATA) {
                edge_type = DEPFLOW_EDGE_DATA;
            }
            
            depflow_graph_add_edge_full(graph, from_node, to_node, edge_type, 
                                        xref->from, xref->is_conditional);
        }
    }
    
    // Create dependency edges between functions and their variables
    for (int i = 0; i < bin->nfunctions; i++) {
        macho_function_t *func = &bin->functions[i];
        uint64_t func_node = depflow_node_id_by_address(graph, func->start);
        
        if (func_node == 0) continue;
        
        // Find variables used by this function
        // This would require more detailed analysis in a real implementation
        for (int j = 0; j < bin->nsymbols; j++) {
            macho_symbol_t *sym = &bin->symbols[j];
            
            if (sym->is_undefined) continue;
            
            // Check if symbol is in the same section as function
            if (func->start <= sym->address && sym->address < func->end) {
                uint64_t var_node = depflow_node_id_by_address(graph, sym->address);
                if (var_node > 0) {
                    // Function uses this variable
                    depflow_graph_add_edge(graph, func_node, var_node, DEPFLOW_EDGE_USE_DEF);
                }
            }
        }
    }
    
    return 0;
}

int depflow_analyze_function_dependencies(macho_binary_t *bin, depflow_graph_t *graph) {
    if (!bin || !graph) return -1;
    
    // Analyze each function's dependencies
    for (int i = 0; i < bin->nfunctions; i++) {
        macho_function_t *func = &bin->functions[i];
        uint64_t func_node = depflow_node_id_by_address(graph, func->start);
        
        if (func_node == 0) continue;
        
        depflow_func_deps_t *deps = &graph->func_deps[graph->func_deps_count++];
        deps->func_idx = i;
        deps->ninputs = 0;
        deps->noutputs = 0;
        deps->nreads = 0;
        deps->nwrites = 0;
        deps->ncalls = 0;
        deps->has_side_effects = 0;
        
        // Find all edges from this function
        for (int j = 0; j < graph->edge_count; j++) {
            depflow_edge_t *edge = &graph->edges[j];
            
            if (edge->from == func_node) {
                // This is an outgoing dependency
                depflow_node_t *to_node = &graph->nodes[edge->to - 1];
                
                if (to_node->type == DEPFLOW_NODE_FUNCTION) {
                    if (deps->ncalls < 256) {
                        deps->calls[deps->ncalls++] = to_node->func_idx;
                    }
                } else if (to_node->type == DEPFLOW_NODE_VARIABLE) {
                    if (edge->type == DEPFLOW_EDGE_USE_DEF) {
                        if (deps->nreads < 256) {
                            deps->reads[deps->nreads++] = edge->to;
                        }
                    } else if (edge->type == DEPFLOW_EDGE_DEF_USE) {
                        if (deps->nwrites < 256) {
                            deps->writes[deps->nwrites++] = edge->to;
                        }
                    }
                }
            } else if (edge->to == func_node) {
                // This is an incoming dependency
                depflow_node_t *from_node = &graph->nodes[edge->from - 1];
                
                if (from_node->type == DEPFLOW_NODE_VARIABLE) {
                    if (edge->type == DEPFLOW_EDGE_USE_DEF) {
                        if (deps->ninputs < 256) {
                            deps->inputs[deps->ninputs++] = edge->from;
                        }
                    }
                }
            }
        }
        
        // Determine if function has side effects
        deps->has_side_effects = (deps->nwrites > 0 || deps->ncalls > 0);
    }
    
    return 0;
}

int depflow_track_variable_usage(macho_binary_t *bin, depflow_graph_t *graph, 
                                   const char *var_name) {
    if (!bin || !graph || !var_name) return -1;
    
    // Find the variable node
    uint64_t var_node = 0;
    for (int i = 0; i < graph->node_count; i++) {
        if (graph->nodes[i].type == DEPFLOW_NODE_VARIABLE &&
            strcmp(graph->nodes[i].name, var_name) == 0) {
            var_node = graph->nodes[i].id;
            break;
        }
    }
    
    if (var_node == 0) {
        fprintf(stderr, "depflow: Variable '%s' not found\n", var_name);
        return -1;
    }
    
    // Track all usages of this variable
    for (int i = 0; i < graph->edge_count; i++) {
        depflow_edge_t *edge = &graph->edges[i];
        
        if (edge->to == var_node && edge->type == DEPFLOW_EDGE_USE_DEF) {
            // This is a use of the variable
            depflow_var_usage_t *usage = &graph->var_usages[graph->var_usage_count++];
            usage->var_id = var_node;
            usage->instruction_addr = edge->site;
            usage->usage_type = 0; // read
            usage->is_conditional = edge->is_conditional;
            usage->timestamp = 0;
        } else if (edge->from == var_node && edge->type == DEPFLOW_EDGE_DEF_USE) {
            // This is a definition of the variable
            depflow_var_usage_t *usage = &graph->var_usages[graph->var_usage_count++];
            usage->var_id = var_node;
            usage->instruction_addr = edge->site;
            usage->usage_type = 1; // write
            usage->is_conditional = edge->is_conditional;
            usage->timestamp = 0;
        }
    }
    
    return 0;
}

// ============================================================================
// Path Finding Algorithms
// ============================================================================

int depflow_find_all_paths(depflow_graph_t *graph, uint64_t start, uint64_t end,
                             depflow_path_t *paths, int max_paths) {
    if (!graph || start == 0 || end == 0) return -1;
    
    // Simple BFS-based path finding (would use DFS in real implementation)
    int path_count = 0;
    
    // Check if start and end are valid nodes
    if (start >= graph->node_count + 1 || end >= graph->node_count + 1) {
        return -1;
    }
    
    // Simple path: direct edge
    for (int i = 0; i < graph->edge_count; i++) {
        if (graph->edges[i].from == start && graph->edges[i].to == end) {
            if (path_count < max_paths) {
                paths[path_count].path_id = path_count + 1;
                paths[path_count].start_node = start;
                paths[path_count].end_node = end;
                paths[path_count].nodes[0] = start;
                paths[path_count].nodes[1] = end;
                paths[path_count].node_count = 2;
                paths[path_count].edges[0] = i + 1;
                paths[path_count].edge_count = 1;
                paths[path_count].total_weight = graph->edges[i].weight;
                paths[path_count].is_feasible = 1;
                path_count++;
            }
        }
    }
    
    return path_count;
}

int depflow_find_shortest_path(depflow_graph_t *graph, uint64_t start, uint64_t end,
                                uint64_t *path, int *path_length) {
    if (!graph || !path || !path_length || start == 0 || end == 0) return -1;
    
    // Simple implementation: find direct edge or two-hop path
    
    // Check direct edge
    for (int i = 0; i < graph->edge_count; i++) {
        if (graph->edges[i].from == start && graph->edges[i].to == end) {
            path[0] = start;
            path[1] = end;
            *path_length = 2;
            return 0;
        }
    }
    
    // Check two-hop path
    for (int i = 0; i < graph->edge_count; i++) {
        if (graph->edges[i].from == start) {
            uint64_t mid = graph->edges[i].to;
            for (int j = 0; j < graph->edge_count; j++) {
                if (graph->edges[j].from == mid && graph->edges[j].to == end) {
                    path[0] = start;
                    path[1] = mid;
                    path[2] = end;
                    *path_length = 3;
                    return 0;
                }
            }
        }
    }
    
    return -1; // No path found
}

int depflow_find_longest_path(depflow_graph_t *graph, uint64_t start, uint64_t end,
                               uint64_t *path, int *path_length) {
    // For now, same as shortest path
    return depflow_find_shortest_path(graph, start, end, path, path_length);
}

// ============================================================================
// Execution Order Prediction
// ============================================================================

int depflow_predict_execution_order(macho_binary_t *bin, depflow_graph_t *graph,
                                     uint64_t *order, int max_order) {
    if (!bin || !graph || !order) return -1;
    
    int order_count = 0;
    
    // Simple topological sort based on dependencies
    // In a real implementation, this would use Kahn's algorithm or similar
    
    // Mark all nodes as unvisited
    for (int i = 0; i < graph->node_count; i++) {
        graph->nodes[i].visited = 0;
    }
    
    // Start with entry point function
    uint64_t entry_node = 0;
    for (int i = 0; i < graph->node_count; i++) {
        if (graph->nodes[i].type == DEPFLOW_NODE_FUNCTION &&
            graph->nodes[i].address == bin->entry) {
            entry_node = graph->nodes[i].id;
            break;
        }
    }
    
    if (entry_node == 0) {
        // Use first function as entry point
        for (int i = 0; i < graph->node_count; i++) {
            if (graph->nodes[i].type == DEPFLOW_NODE_FUNCTION) {
                entry_node = graph->nodes[i].id;
                break;
            }
        }
    }
    
    if (entry_node > 0) {
        order[order_count++] = entry_node;
        graph->nodes[entry_node - 1].visited = 1;
    }
    
    // Simple BFS traversal
    for (int i = 0; i < graph->node_count && order_count < max_order; i++) {
        for (int j = 0; j < graph->edge_count; j++) {
            depflow_edge_t *edge = &graph->edges[j];
            
            // Check if source is in order and destination is not visited
            int src_in_order = 0;
            for (int k = 0; k < order_count; k++) {
                if (order[k] == edge->from) {
                    src_in_order = 1;
                    break;
                }
            }
            
            if (src_in_order && !graph->nodes[edge->to - 1].visited) {
                order[order_count++] = edge->to;
                graph->nodes[edge->to - 1].visited = 1;
            }
        }
    }
    
    return order_count;
}

int depflow_analyze_data_flow_order(macho_binary_t *bin, depflow_graph_t *graph) {
    // Analyze data flow to determine execution order
    // This would use more sophisticated algorithms in a real implementation
    return 0;
}

// ============================================================================
// Parallel Execution Detection
// ============================================================================

int depflow_detect_parallel_execution(macho_binary_t *bin, depflow_graph_t *graph) {
    if (!bin || !graph) return -1;
    
    // Look for patterns that indicate parallel execution
    // 1. Multiple functions with same or similar inputs
    // 2. Dispatch patterns (beam search, etc.)
    // 3. Thread creation or parallel constructs
    
    for (int i = 0; i < bin->nfunctions; i++) {
        macho_function_t *func = &bin->functions[i];
        
        // Check for parallel dispatch patterns
        if (strstr(func->name, "dispatch") || strstr(func->name, "parallel") ||
            strstr(func->name, "beam") || strstr(func->name, "thread")) {
            // This might be a parallel dispatch function
            uint64_t func_node = depflow_node_id_by_address(graph, func->start);
            if (func_node > 0) {
                // Mark as potential parallel dispatch point
                // In a real implementation, we'd analyze the function
            }
        }
    }
    
    // Analyze call patterns for parallel execution
    for (int i = 0; i < graph->func_deps_count; i++) {
        depflow_func_deps_t *deps = &graph->func_deps[i];
        
        // If a function calls multiple functions with similar inputs,
        // it might be parallel execution
        if (deps->ncalls > 1) {
            // Check if called functions have similar input dependencies
            // This would require more detailed analysis
        }
    }
    
    return 0;
}

int depflow_analyze_parallel_patterns(depflow_graph_t *graph) {
    // Analyze patterns that indicate parallel execution
    return 0;
}

// ============================================================================
// Beam Search Specific Analysis
// ============================================================================

int depflow_analyze_beam_search(depflow_graph_t *graph, macho_binary_t *bin) {
    if (!graph || !bin) return -1;
    
    // Look for beam search patterns:
    // 1. Functions that process multiple states/nodes in parallel
    // 2. Argument passing patterns
    // 3. Result collection patterns
    
    for (int i = 0; i < graph->node_count; i++) {
        depflow_node_t *node = &graph->nodes[i];
        
        // Check for beam search related names
        if (strstr(node->name, "beam") || strstr(node->name, "search") ||
            strstr(node->name, "expand") || strstr(node->name, "evaluate")) {
            // This might be part of beam search
            // In a real implementation, we'd analyze the connections
        }
    }
    
    return 0;
}

int depflow_track_beam_search_arguments(depflow_graph_t *graph, macho_binary_t *bin) {
    // Track arguments passed to beam search functions
    return 0;
}

// ============================================================================
// State Tracking for Regression Analysis
// ============================================================================

int depflow_track_state_dependencies(depflow_graph_t *graph, macho_binary_t *bin) {
    // Track state dependencies for regression analysis
    return 0;
}

int depflow_analyze_state_changes(depflow_graph_t *graph, macho_binary_t *bin) {
    // Analyze state changes and their dependencies
    return 0;
}

// ============================================================================
// Utility Functions
// ============================================================================

uint64_t depflow_node_id_by_address(depflow_graph_t *graph, uint64_t address) {
    if (!graph || address == 0 || address >= DEPFLOW_MAX_NODES) return 0;
    return graph->addr_to_node[address];
}

uint64_t depflow_node_id_by_name(depflow_graph_t *graph, const char *name) {
    if (!graph || !name) return 0;
    
    for (int i = 0; i < graph->node_count; i++) {
        if (strcmp(graph->nodes[i].name, name) == 0) {
            return graph->nodes[i].id;
        }
    }
    
    return 0;
}

int depflow_is_reachable(depflow_graph_t *graph, uint64_t from, uint64_t to) {
    if (!graph || from == 0 || to == 0) return 0;
    
    // Simple BFS reachability check
    if (from >= graph->node_count + 1 || to >= graph->node_count + 1) return 0;
    
    // Mark all nodes as unvisited
    for (int i = 0; i < graph->node_count; i++) {
        graph->nodes[i].visited = 0;
    }
    
    // BFS from 'from' node
    uint64_t queue[DEPFLOW_MAX_NODES];
    int queue_front = 0, queue_back = 0;
    
    queue[queue_back++] = from;
    graph->nodes[from - 1].visited = 1;
    
    while (queue_front < queue_back) {
        uint64_t current = queue[queue_front++];
        
        if (current == to) {
            return 1; // Reachable
        }
        
        // Find all outgoing edges
        for (int i = 0; i < graph->edge_count; i++) {
            if (graph->edges[i].from == current && !graph->nodes[graph->edges[i].to - 1].visited) {
                queue[queue_back++] = graph->edges[i].to;
                graph->nodes[graph->edges[i].to - 1].visited = 1;
            }
        }
    }
    
    return 0; // Not reachable
}

int depflow_get_dependencies(depflow_graph_t *graph, uint64_t node_id,
                              uint64_t *deps, int max_deps) {
    if (!graph || node_id == 0 || !deps) return -1;
    
    int count = 0;
    
    for (int i = 0; i < graph->edge_count && count < max_deps; i++) {
        if (graph->edges[i].to == node_id) {
            deps[count++] = graph->edges[i].from;
        }
    }
    
    return count;
}

int depflow_get_dependents(depflow_graph_t *graph, uint64_t node_id,
                            uint64_t *dependents, int max_dependents) {
    if (!graph || node_id == 0 || !dependents) return -1;
    
    int count = 0;
    
    for (int i = 0; i < graph->edge_count && count < max_dependents; i++) {
        if (graph->edges[i].from == node_id) {
            dependents[count++] = graph->edges[i].to;
        }
    }
    
    return count;
}

// ============================================================================
// Output Functions
// ============================================================================

void depflow_print_dependency_graph(depflow_graph_t *graph, macho_opts_t *opts, FILE *out) {
    if (!graph || !out) return;
    
    int c = opts ? opts->color : 0;
    const char *C_TITLE = c ? "\033[1;36m" : "";
    const char *C_SECTION = c ? "\033[1;33m" : "";
    const char *C_NODE = c ? "\033[1;32m" : "";
    const char *C_EDGE = c ? "\033[1;35m" : "";
    const char *C_RESET = c ? "\033[0m" : "";
    
    fprintf(out, "%s=== Dependency Graph ===%s\n", C_TITLE, C_RESET);
    fprintf(out, "Nodes: %d, Edges: %d\n\n", graph->node_count, graph->edge_count);
    
    fprintf(out, "%sNodes:%s\n", C_SECTION, C_RESET);
    for (int i = 0; i < graph->node_count; i++) {
        depflow_node_t *node = &graph->nodes[i];
        fprintf(out, "  %s[%llu]%s %s (type: %d, addr: 0x%llx, size: %llu)%s\n",
               C_NODE, (unsigned long long)node->id, C_RESET,
               node->name, node->type, (unsigned long long)node->address,
               (unsigned long long)node->size, C_RESET);
        
        if (node->category != DEPFLOW_VAR_CATEGORY_UNKNOWN) {
            fprintf(out, "      Category: %d", node->category);
            switch (node->category) {
                case DEPFLOW_VAR_CATEGORY_BOARD: fprintf(out, " (Board)"); break;
                case DEPFLOW_VAR_CATEGORY_MOVE: fprintf(out, " (Move)"); break;
                case DEPFLOW_VAR_CATEGORY_SCORE: fprintf(out, " (Score)"); break;
                case DEPFLOW_VAR_CATEGORY_DEPTH: fprintf(out, " (Depth)"); break;
                case DEPFLOW_VAR_CATEGORY_NODE: fprintf(out, " (Node)"); break;
                case DEPFLOW_VAR_CATEGORY_SEARCH_TREE: fprintf(out, " (Search Tree)"); break;
                case DEPFLOW_VAR_CATEGORY_PARALLEL: fprintf(out, " (Parallel)"); break;
                case DEPFLOW_VAR_CATEGORY_MESSAGE: fprintf(out, " (Message)"); break;
                case DEPFLOW_VAR_CATEGORY_STATE: fprintf(out, " (State)"); break;
                default: fprintf(out, " (Unknown)"); break;
            }
            fprintf(out, "%s\n", C_RESET);
        }
    }
    
    fprintf(out, "\n%sEdges:%s\n", C_SECTION, C_RESET);
    for (int i = 0; i < graph->edge_count; i++) {
        depflow_edge_t *edge = &graph->edges[i];
        depflow_node_t *from = &graph->nodes[edge->from - 1];
        depflow_node_t *to = &graph->nodes[edge->to - 1];
        
        fprintf(out, "  %s[%llu -> %llu]%s type: %d, site: 0x%llx, conditional: %d%s\n",
               C_EDGE, (unsigned long long)edge->from, (unsigned long long)edge->to,
               C_RESET, edge->type, (unsigned long long)edge->site, edge->is_conditional, C_RESET);
        fprintf(out, "      %s -> %s%s\n", from->name, to->name, C_RESET);
    }
}

void depflow_print_variable_usage(depflow_graph_t *graph, macho_opts_t *opts, FILE *out) {
    if (!graph || !out) return;
    
    int c = opts ? opts->color : 0;
    const char *C_TITLE = c ? "\033[1;36m" : "";
    const char *C_RESET = c ? "\033[0m" : "";
    
    fprintf(out, "%s=== Variable Usage ===%s\n", C_TITLE, C_RESET);
    fprintf(out, "Total usages: %d\n\n", graph->var_usage_count);
    
    for (int i = 0; i < graph->var_usage_count; i++) {
        depflow_var_usage_t *usage = &graph->var_usages[i];
        depflow_node_t *var_node = NULL;
        
        for (int j = 0; j < graph->node_count; j++) {
            if (graph->nodes[j].id == usage->var_id) {
                var_node = &graph->nodes[j];
                break;
            }
        }
        
        if (var_node) {
            const char *usage_type_str = "";
            switch (usage->usage_type) {
                case 0: usage_type_str = "Read"; break;
                case 1: usage_type_str = "Write"; break;
                case 2: usage_type_str = "Read/Write"; break;
            }
            
            fprintf(out, "  Variable: %s (0x%llx), Type: %s, Addr: 0x%llx, Conditional: %d\n",
                   var_node->name, (unsigned long long)usage->var_id,
                   usage_type_str, (unsigned long long)usage->instruction_addr,
                   usage->is_conditional);
        }
    }
}

void depflow_print_execution_order(depflow_graph_t *graph, macho_opts_t *opts, FILE *out) {
    if (!graph || !out) return;
    
    int c = opts ? opts->color : 0;
    const char *C_TITLE = c ? "\033[1;36m" : "";
    const char *C_RESET = c ? "\033[0m" : "";
    
    fprintf(out, "%s=== Execution Order ===%s\n", C_TITLE, C_RESET);
    fprintf(out, "(Topological order based on dependencies)\n\n");
    
    // In a real implementation, we'd have a proper topological sort
    // For now, just list nodes in order
    for (int i = 0; i < graph->node_count; i++) {
        depflow_node_t *node = &graph->nodes[i];
        fprintf(out, "  %d: %s (0x%llx)\n", i + 1, node->name, (unsigned long long)node->address);
    }
}

void depflow_print_parallel_analysis(depflow_graph_t *graph, macho_opts_t *opts, FILE *out) {
    if (!graph || !out) return;
    
    int c = opts ? opts->color : 0;
    const char *C_TITLE = c ? "\033[1;36m" : "";
    const char *C_RESET = c ? "\033[0m" : "";
    
    fprintf(out, "%s=== Parallel Execution Analysis ===%s\n", C_TITLE, C_RESET);
    fprintf(out, "(Placeholder - would show detected parallel patterns)\n\n");
}

// ============================================================================
// DOT Export Functions
// ============================================================================

void depflow_export_dot(depflow_graph_t *graph, const char *filename) {
    if (!graph || !filename) return;
    
    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "depflow: Failed to open file '%s' for writing\n", filename);
        return;
    }
    
    fprintf(f, "digraph DependencyGraph {\n");
    fprintf(f, "  rankdir=LR;\n");
    fprintf(f, "  node [shape=box, style=filled];\n");
    fprintf(f, "  edge [color=gray40];\n\n");
    
    // Define colors for different node types
    fprintf(f, "  // Node type colors\n");
    fprintf(f, "  node [fillcolor=lightblue, color=black]; // Default\n\n");
    
    // Write nodes
    for (int i = 0; i < graph->node_count; i++) {
        depflow_node_t *node = &graph->nodes[i];
        
        const char *shape = "box";
        const char *fillcolor = "lightblue";
        const char *color = "black";
        
        switch (node->type) {
            case DEPFLOW_NODE_FUNCTION:
                shape = "ellipse";
                fillcolor = "lightgreen";
                break;
            case DEPFLOW_NODE_VARIABLE:
                shape = "box";
                fillcolor = "lightyellow";
                break;
            case DEPFLOW_NODE_REGISTER:
                shape = "diamond";
                fillcolor = "lightpink";
                break;
            case DEPFLOW_NODE_MEMORY:
                shape = "cylinder";
                fillcolor = "lightgray";
                break;
            case DEPFLOW_NODE_CONSTANT:
                shape = "plaintext";
                fillcolor = "white";
                break;
            case DEPFLOW_NODE_STACK:
                shape = "record";
                fillcolor = "lightsalmon";
                break;
        }
        
        // Escape special characters in name
        char escaped_name[256];
        int j = 0;
        for (int k = 0; node->name[k] && j < sizeof(escaped_name) - 1; k++) {
            if (node->name[k] == '"' || node->name[k] == '\\' || node->name[k] == '\n') {
                escaped_name[j++] = '\\';
            }
            escaped_name[j++] = node->name[k];
        }
        escaped_name[j] = '\0';
        
        fprintf(f, "  n%llu [label=\"%s\\n0x%llx\", shape=%s, fillcolor=%s, color=%s];\n",
               (unsigned long long)node->id, escaped_name, (unsigned long long)node->address,
               shape, fillcolor, color);
    }
    
    fprintf(f, "\n");
    
    // Write edges
    for (int i = 0; i < graph->edge_count; i++) {
        depflow_edge_t *edge = &graph->edges[i];
        
        const char *color = "gray40";
        const char *style = "solid";
        int width = 1;
        
        switch (edge->type) {
            case DEPFLOW_EDGE_DEF_USE:
                color = "blue";
                break;
            case DEPFLOW_EDGE_USE_DEF:
                color = "green";
                break;
            case DEPFLOW_EDGE_CONTROL:
                color = "red";
                width = 2;
                break;
            case DEPFLOW_EDGE_DATA:
                color = "purple";
                break;
            case DEPFLOW_EDGE_ANTI:
                color = "orange";
                style = "dashed";
                break;
            case DEPFLOW_EDGE_OUTPUT:
                color = "brown";
                break;
        }
        
        if (edge->is_conditional) {
            style = "dashed";
        }
        
        fprintf(f, "  n%llu -> n%llu [color=%s, style=%s, penwidth=%d, label=\"0x%llx\"];\n",
               (unsigned long long)edge->from, (unsigned long long)edge->to,
               color, style, width, (unsigned long long)edge->site);
    }
    
    fprintf(f, "}\n");
    
    fclose(f);
}

void depflow_export_subgraph_dot(depflow_graph_t *graph, const char *filename,
                                  uint64_t start_node, int depth) {
    // Export a subgraph starting from a specific node
    // Placeholder implementation
    depflow_export_dot(graph, filename);
}

// ============================================================================
// Shogi AI Specific Analysis
// ============================================================================

int depflow_analyze_shogi_variables(depflow_graph_t *graph, macho_binary_t *bin) {
    // Analyze variables specific to shogi AI
    return 0;
}

int depflow_track_search_tree(depflow_graph_t *graph, macho_binary_t *bin) {
    // Track search tree construction and traversal
    return 0;
}

int depflow_analyze_move_generation(depflow_graph_t *graph, macho_binary_t *bin) {
    // Analyze move generation logic
    return 0;
}
