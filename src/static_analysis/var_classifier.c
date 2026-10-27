#include "variable_classifier.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void var_classifier_init(var_classifier_t *classifier) {
    if (!classifier) return;
    memset(classifier, 0, sizeof(var_classifier_t));
    classifier->capacity = 1024;
    classifier->classifications = (var_classification_t *)calloc(classifier->capacity, sizeof(var_classification_t));
    classifier->num_classifications = 0;
}

void var_classifier_free(var_classifier_t *classifier) {
    if (!classifier) return;
    if (classifier->classifications) { free(classifier->classifications); classifier->classifications = NULL; }
    classifier->num_classifications = 0;
}

const char *var_category_to_string(var_category_t category) {
    switch(category) {
        case VAR_CAT_UNKNOWN: return "Unknown";
        case VAR_CAT_BOARD: return "Board";
        case VAR_CAT_MOVE: return "Move";
        case VAR_CAT_MOVE_TREE: return "Move Tree";
        case VAR_CAT_EVALUATION: return "Evaluation";
        case VAR_CAT_DEPTH: return "Depth";
        case VAR_CAT_NODE: return "Node";
        case VAR_CAT_SEARCH_TREE: return "Search Tree";
        case VAR_CAT_CONSTANT: return "Constant";
        case VAR_CAT_DATABASE: return "Database";
        case VAR_CAT_STATE: return "State";
        case VAR_CAT_MESSAGE: return "Message";
        case VAR_CAT_PARALLEL: return "Parallel";
        case VAR_CAT_TEMPORARY: return "Temporary";
        case VAR_CAT_GLOBAL_CONFIG: return "Global Config";
        case VAR_CAT_HASH: return "Hash";
        case VAR_CAT_BITBOARD: return "Bitboard";
        case VAR_CAT_PIECE: return "Piece";
        case VAR_CAT_SCORE: return "Score";
        case VAR_CAT_TIME: return "Time";
        case VAR_CAT_MEMORY: return "Memory";
        case VAR_CAT_IO: return "I/O";
        case VAR_CAT_UTILITY: return "Utility";
        default: return "Unknown";
    }
}

const char *var_aspect_to_string(var_aspect_t aspect) {
    switch(aspect) {
        case VAR_ASPECT_UNKNOWN: return "Unknown";
        case VAR_ASPECT_STATIC: return "Static";
        case VAR_ASPECT_DYNAMIC: return "Dynamic";
        case VAR_ASPECT_PERSISTENT: return "Persistent";
        case VAR_ASPECT_TEMPORARY: return "Temporary";
        case VAR_ASPECT_SHARED: return "Shared";
        case VAR_ASPECT_PRIVATE: return "Private";
        case VAR_ASPECT_READONLY: return "Read-Only";
        case VAR_ASPECT_WRITABLE: return "Writable";
        case VAR_ASPECT_IMMUTABLE: return "Immutable";
        case VAR_ASPECT_MUTABLE: return "Mutable";
        default: return "Unknown";
    }
}

const char *var_access_pattern_to_string(var_access_pattern_t pattern) {
    switch(pattern) {
        case VAR_ACCESS_UNKNOWN: return "Unknown";
        case VAR_ACCESS_SINGLE: return "Single";
        case VAR_ACCESS_MULTIPLE: return "Multiple";
        case VAR_ACCESS_SEQUENTIAL: return "Sequential";
        case VAR_ACCESS_RANDOM: return "Random";
        case VAR_ACCESS_ITERATIVE: return "Iterative";
        case VAR_ACCESS_RECURSIVE: return "Recursive";
        case VAR_ACCESS_PARALLEL: return "Parallel";
        case VAR_ACCESS_CONDITIONAL: return "Conditional";
        case VAR_ACCESS_FREQUENT: return "Frequent";
        case VAR_ACCESS_RARE: return "Rare";
        default: return "Unknown";
    }
}

const char *var_data_type_to_string(var_data_type_t type) {
    switch(type) {
        case VAR_TYPE_UNKNOWN: return "Unknown";
        case VAR_TYPE_SCALAR: return "Scalar";
        case VAR_TYPE_ARRAY: return "Array";
        case VAR_TYPE_STRUCT: return "Struct";
        case VAR_TYPE_UNION: return "Union";
        case VAR_TYPE_POINTER: return "Pointer";
        case VAR_TYPE_FUNCTION_PTR: return "Function Pointer";
        case VAR_TYPE_BITFIELD: return "Bitfield";
        case VAR_TYPE_STRING: return "String";
        case VAR_TYPE_LIST: return "List";
        case VAR_TYPE_TREE: return "Tree";
        case VAR_TYPE_GRAPH: return "Graph";
        case VAR_TYPE_HASH_TABLE: return "Hash Table";
        case VAR_TYPE_STACK: return "Stack";
        case VAR_TYPE_QUEUE: return "Queue";
        case VAR_TYPE_HEAP: return "Heap";
        case VAR_TYPE_MATRIX: return "Matrix";
        case VAR_TYPE_VECTOR: return "Vector";
        default: return "Unknown";
    }
}

int var_classify_all(var_classifier_t *classifier, macho_binary_t *bin, depflow_graph_t *dep_graph) {
    if (!classifier || !bin || !dep_graph) return -1;
    classifier->num_classifications = 0;
    for (int i = 0; i < bin->nsymbols && classifier->num_classifications < classifier->capacity; i++) {
        macho_symbol_t *sym = &bin->symbols[i];
        if (sym->is_undefined) continue;
        if (sym->type == SYM_UNKNOWN) continue;
        var_classification_t cls;
        memset(&cls, 0, sizeof(cls));
        cls.var_id = sym->address;
        cls.address = sym->address;
        strncpy(cls.name, sym->demangled[0] ? sym->demangled : sym->name, sizeof(cls.name)-1);
        cls.size = sym->size;
        cls.category_confidence = 60;
        if (strstr(cls.name, "board") || strstr(cls.name, "ban")) cls.primary_category = VAR_CAT_BOARD;
        else if (strstr(cls.name, "move") || strstr(cls.name, "te")) cls.primary_category = VAR_CAT_MOVE;
        else if (strstr(cls.name, "score") || strstr(cls.name, "eval")) cls.primary_category = VAR_CAT_EVALUATION;
        else if (strstr(cls.name, "depth") || strstr(cls.name, "ply")) cls.primary_category = VAR_CAT_DEPTH;
        else if (strstr(cls.name, "node") || strstr(cls.name, "search")) cls.primary_category = VAR_CAT_SEARCH_TREE;
        else cls.primary_category = VAR_CAT_UNKNOWN;
        classifier->classifications[classifier->num_classifications++] = cls;
    }
    return 0;
}

int var_classify_shogi_variables(var_classifier_t *classifier) {
    if (!classifier) return -1;
    int c = 0;
    for (int i = 0; i < classifier->num_classifications; i++) {
        if (classifier->classifications[i].primary_category == VAR_CAT_BOARD ||
            classifier->classifications[i].primary_category == VAR_CAT_MOVE ||
            classifier->classifications[i].primary_category == VAR_CAT_EVALUATION) {
            classifier->classifications[i].is_shogi_specific = 1;
            c++;
        }
    }
    return c;
}

int var_identify_shogi_roles(var_classifier_t *classifier) { return 0; }
void var_classifier_print(var_classifier_t *classifier, macho_opts_t *opts, FILE *out) {}
void var_classifier_print_summary(var_classifier_t *classifier, macho_opts_t *opts, FILE *out) {}
void var_classifier_print_detailed(var_classifier_t *classifier, macho_opts_t *opts, FILE *out) {}
int var_analyze_aspects(var_classifier_t *classifier, var_classification_t *classification) { return 0; }
int var_analyze_access_patterns(var_classifier_t *classifier, var_classification_t *classification) { return 0; }
int var_analyze_data_types(var_classifier_t *classifier, var_classification_t *classification) { return 0; }
int var_classify_by_name(var_classifier_t *classifier, const char *name, var_classification_t *result) { return -1; }
int var_classify_by_address(var_classifier_t *classifier, uint64_t address, var_classification_t *result) { return -1; }
int var_classify_by_usage(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result) { return -1; }
int var_classify_board(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result) { return -1; }
int var_classify_move(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result) { return -1; }
int var_classify_move_tree(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result) { return -1; }
int var_classify_evaluation(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result) { return -1; }
int var_classify_depth(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result) { return -1; }
int var_classify_node(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result) { return -1; }
int var_classify_search_tree(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result) { return -1; }
int var_classify_parallel_variables(var_classifier_t *classifier) { return 0; }
int var_identify_shared_variables(var_classifier_t *classifier) { return 0; }
void var_classifier_export_dot(var_classifier_t *classifier, const char *filename) {}
void var_classifier_export_category_dot(var_classifier_t *classifier, const char *filename) {}
