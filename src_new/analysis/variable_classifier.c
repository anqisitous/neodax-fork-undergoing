#include "variable_classifier.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

// ============================================================================
// Variable Classification System Implementation
// 
// 変数分類システムの実装
// 将棋AIの変数をその役割と性質に基づいて分類
// ============================================================================

// ============================================================================
// Initialization and Cleanup
// ============================================================================

void var_classifier_init(var_classifier_t *classifier) {
    if (!classifier) return;
    
    memset(classifier, 0, sizeof(var_classifier_t));
    classifier->capacity = 1024;
    classifier->classifications = (var_classification_t *)calloc(
        classifier->capacity, sizeof(var_classification_t));
    classifier->num_classifications = 0;
    
    // Initialize statistics
    for (int i = 0; i < VAR_CAT_MAX; i++) {
        classifier->categories[i] = 0;
    }
    for (int i = 0; i < VAR_ASPECT_MAX; i++) {
        classifier->aspects[i] = 0;
    }
    for (int i = 0; i < VAR_ACCESS_MAX; i++) {
        classifier->access_patterns[i] = 0;
    }
    for (int i = 0; i < VAR_TYPE_MAX; i++) {
        classifier->data_types[i] = 0;
    }
}

void var_classifier_free(var_classifier_t *classifier) {
    if (!classifier) return;
    
    if (classifier->classifications) {
        free(classifier->classifications);
        classifier->classifications = NULL;
    }
    
    classifier->num_classifications = 0;
    classifier->capacity = 0;
}

// ============================================================================
// String Conversion Functions
// ============================================================================

const char *var_category_to_string(var_category_t category) {
    switch (category) {
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
    switch (aspect) {
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
        case VAR_ASPECT_SEQUENTIAL: return "Sequential";
        case VAR_ASPECT_RANDOM: return "Random";
        case VAR_ASPECT_INDEXED: return "Indexed";
        case VAR_ASPECT_HIERARCHICAL: return "Hierarchical";
        case VAR_ASPECT_LINEAR: return "Linear";
        case VAR_ASPECT_TREE: return "Tree";
        case VAR_ASPECT_GRAPH: return "Graph";
        case VAR_ASPECT_HASHED: return "Hashed";
        case VAR_ASPECT_COMPRESSED: return "Compressed";
        case VAR_ASPECT_ENCRYPTED: return "Encrypted";
        default: return "Unknown";
    }
}

const char *var_access_pattern_to_string(var_access_pattern_t pattern) {
    switch (pattern) {
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
    switch (type) {
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

// ============================================================================
// Classification Helper Functions
// ============================================================================

// Case-insensitive string comparison
static int strcasecmp_n(const char *s1, const char *s2, size_t n) {
    if (!s1 || !s2) return -1;
    
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)s1[i]) != tolower((unsigned char)s2[i])) {
            return tolower((unsigned char)s1[i]) - tolower((unsigned char)s2[i]);
        }
        if (s1[i] == '\0' || s2[i] == '\0') {
            return 0;
        }
    }
    return 0;
}

// Check if string contains pattern (case-insensitive)
static int strcontains(const char *str, const char *pattern) {
    if (!str || !pattern) return 0;
    
    size_t str_len = strlen(str);
    size_t pattern_len = strlen(pattern);
    
    if (pattern_len == 0) return 1;
    if (str_len < pattern_len) return 0;
    
    for (size_t i = 0; i <= str_len - pattern_len; i++) {
        if (strcasecmp_n(str + i, pattern, pattern_len) == 0) {
            return 1;
        }
    }
    return 0;
}

// Check if string starts with prefix (case-insensitive)
static int strstartswith(const char *str, const char *prefix) {
    if (!str || !prefix) return 0;
    return strcasecmp_n(str, prefix, strlen(prefix)) == 0;
}

// Check if string ends with suffix (case-insensitive)
static int strendswith(const char *str, const char *suffix) {
    if (!str || !suffix) return 0;
    
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    
    if (str_len < suffix_len) return 0;
    
    return strcasecmp_n(str + str_len - suffix_len, suffix, suffix_len) == 0;
}

// Apply classification rules
static var_category_t apply_rules(const char *name, const var_classification_rule_t *rules) {
    if (!name || !rules) return VAR_CAT_UNKNOWN;
    
    for (int i = 0; rules[i].pattern; i++) {
        if (strcontains(name, rules[i].pattern)) {
            size_t name_len = strlen(name);
            if (name_len >= (size_t)rules[i].min_length &&
                (rules[i].max_length == 0 || name_len <= (size_t)rules[i].max_length)) {
                return rules[i].category;
            }
        }
    }
    return VAR_CAT_UNKNOWN;
}

// ============================================================================
// Main Classification Functions
// ============================================================================

int var_classify_all(var_classifier_t *classifier, macho_binary_t *bin, depflow_graph_t *dep_graph) {
    if (!classifier || !bin) return -1;
    
    // Classify all symbols in the binary
    for (int i = 0; i < bin->nsymbols; i++) {
        macho_symbol_t *sym = &bin->symbols[i];
        
        // Skip functions and undefined symbols
        if (sym->type == MACHO_SYMBOL_TYPE_FUNC || sym->is_undefined) {
            continue;
        }
        
        var_classification_t classification;
        memset(&classification, 0, sizeof(var_classification_t));
        
        // Basic information
        classification.var_id = i + 1;
        classification.address = sym->address;
        strncpy(classification.name, sym->name, sizeof(classification.name) - 1);
        classification.name[sizeof(classification.name) - 1] = '\0';
        if (sym->demangled[0]) {
            strncpy(classification.demangled, sym->demangled, sizeof(classification.demangled) - 1);
            classification.demangled[sizeof(classification.demangled) - 1] = '\0';
        } else {
            strncpy(classification.demangled, sym->name, sizeof(classification.demangled) - 1);
            classification.demangled[sizeof(classification.demangled) - 1] = '\0';
        }
        classification.size = sym->size;
        classification.is_global = sym->is_global;
        classification.is_static = 0; // Would need more info
        classification.is_constant = 0; // Would need more info
        classification.is_volatile = 0; // Would need more info
        
        // Classify by name
        var_classify_by_name(classifier, classification.demangled, &classification);
        
        // Analyze aspects
        var_analyze_aspects(classifier, &classification);
        
        // Analyze access patterns
        var_analyze_access_patterns(classifier, &classification);
        
        // Analyze data types
        var_analyze_data_types(classifier, &classification);
        
        // Store classification
        if (classifier->num_classifications < classifier->capacity) {
            classifier->classifications[classifier->num_classifications] = classification;
            classifier->num_classifications++;
            
            // Update statistics
            classifier->categories[classification.primary_category]++;
            for (int j = 0; j < classification.num_aspects; j++) {
                classifier->aspects[classification.aspects[j]]++;
            }
            classifier->access_patterns[classification.access_pattern]++;
            classifier->data_types[classification.data_type]++;
        }
    }
    
    // Shogi AI specific classification
    var_classify_shogi_variables(classifier);
    
    // Parallel execution classification
    var_classify_parallel_variables(classifier);
    
    return 0;
}

int var_classify_by_name(var_classifier_t *classifier, const char *name, var_classification_t *result) {
    if (!name || !result) return -1;
    
    // Try board patterns first
    result->primary_category = apply_rules(name, board_rules);
    if (result->primary_category != VAR_CAT_UNKNOWN) {
        result->category_confidence = 90;
        return 0;
    }
    
    // Try move tree patterns
    result->primary_category = apply_rules(name, move_tree_rules);
    if (result->primary_category != VAR_CAT_UNKNOWN) {
        result->category_confidence = 90;
        return 0;
    }
    
    // Try search tree patterns
    result->primary_category = apply_rules(name, search_tree_rules);
    if (result->primary_category != VAR_CAT_UNKNOWN) {
        result->category_confidence = 90;
        return 0;
    }
    
    // Try evaluation patterns
    result->primary_category = apply_rules(name, evaluation_rules);
    if (result->primary_category != VAR_CAT_UNKNOWN) {
        result->category_confidence = 90;
        return 0;
    }
    
    // Try depth patterns
    result->primary_category = apply_rules(name, depth_rules);
    if (result->primary_category != VAR_CAT_UNKNOWN) {
        result->category_confidence = 90;
        return 0;
    }
    
    // Try node patterns
    result->primary_category = apply_rules(name, node_rules);
    if (result->primary_category != VAR_CAT_UNKNOWN) {
        result->category_confidence = 90;
        return 0;
    }
    
    // Try move patterns
    result->primary_category = apply_rules(name, move_rules);
    if (result->primary_category != VAR_CAT_UNKNOWN) {
        result->category_confidence = 90;
        return 0;
    }
    
    // Try constant patterns
    result->primary_category = apply_rules(name, constant_rules);
    if (result->primary_category != VAR_CAT_UNKNOWN) {
        result->category_confidence = 95;
        return 0;
    }
    
    // Try database patterns
    result->primary_category = apply_rules(name, database_rules);
    if (result->primary_category != VAR_CAT_UNKNOWN) {
        result->category_confidence = 85;
        return 0;
    }
    
    // Try parallel patterns
    result->primary_category = apply_rules(name, parallel_rules);
    if (result->primary_category != VAR_CAT_UNKNOWN) {
        result->category_confidence = 85;
        return 0;
    }
    
    // Try message patterns
    result->primary_category = apply_rules(name, message_rules);
    if (result->primary_category != VAR_CAT_UNKNOWN) {
        result->category_confidence = 85;
        return 0;
    }
    
    // Try temporary patterns
    result->primary_category = apply_rules(name, temporary_rules);
    if (result->primary_category != VAR_CAT_UNKNOWN) {
        result->category_confidence = 80;
        return 0;
    }
    
    // Default to state
    result->primary_category = VAR_CAT_STATE;
    result->category_confidence = 50;
    
    return 0;
}

int var_classify_by_address(var_classifier_t *classifier, uint64_t address, var_classification_t *result) {
    if (!classifier || !result) return -1;
    
    // Find classification by address
    for (int i = 0; i < classifier->num_classifications; i++) {
        if (classifier->classifications[i].address == address) {
            *result = classifier->classifications[i];
            return 0;
        }
    }
    
    return -1; // Not found
}

int var_classify_by_usage(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result) {
    if (!classifier || !result) return -1;
    
    // Find classification by ID
    for (int i = 0; i < classifier->num_classifications; i++) {
        if (classifier->classifications[i].var_id == var_id) {
            *result = classifier->classifications[i];
            return 0;
        }
    }
    
    return -1; // Not found
}

// ============================================================================
// Category-Specific Classification
// ============================================================================

int var_classify_board(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result) {
    if (!classifier || !result) return -1;
    
    // Find the variable
    for (int i = 0; i < classifier->num_classifications; i++) {
        if (classifier->classifications[i].var_id == var_id) {
            var_classification_t *classification = &classifier->classifications[i];
            
            // Check if it's a board variable
            if (classification->primary_category == VAR_CAT_BOARD ||
                classification->primary_category == VAR_CAT_BITBOARD) {
                *result = *classification;
                return 0;
            }
        }
    }
    
    return -1;
}

int var_classify_move(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result) {
    if (!classifier || !result) return -1;
    
    // Find the variable
    for (int i = 0; i < classifier->num_classifications; i++) {
        if (classifier->classifications[i].var_id == var_id) {
            var_classification_t *classification = &classifier->classifications[i];
            
            // Check if it's a move variable
            if (classification->primary_category == VAR_CAT_MOVE) {
                *result = *classification;
                return 0;
            }
        }
    }
    
    return -1;
}

int var_classify_move_tree(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result) {
    if (!classifier || !result) return -1;
    
    // Find the variable
    for (int i = 0; i < classifier->num_classifications; i++) {
        if (classifier->classifications[i].var_id == var_id) {
            var_classification_t *classification = &classifier->classifications[i];
            
            // Check if it's a move tree variable
            if (classification->primary_category == VAR_CAT_MOVE_TREE) {
                *result = *classification;
                return 0;
            }
        }
    }
    
    return -1;
}

int var_classify_evaluation(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result) {
    if (!classifier || !result) return -1;
    
    // Find the variable
    for (int i = 0; i < classifier->num_classifications; i++) {
        if (classifier->classifications[i].var_id == var_id) {
            var_classification_t *classification = &classifier->classifications[i];
            
            // Check if it's an evaluation variable
            if (classification->primary_category == VAR_CAT_EVALUATION ||
                classification->primary_category == VAR_CAT_SCORE) {
                *result = *classification;
                return 0;
            }
        }
    }
    
    return -1;
}

int var_classify_depth(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result) {
    if (!classifier || !result) return -1;
    
    // Find the variable
    for (int i = 0; i < classifier->num_classifications; i++) {
        if (classifier->classifications[i].var_id == var_id) {
            var_classification_t *classification = &classifier->classifications[i];
            
            // Check if it's a depth variable
            if (classification->primary_category == VAR_CAT_DEPTH) {
                *result = *classification;
                return 0;
            }
        }
    }
    
    return -1;
}

int var_classify_node(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result) {
    if (!classifier || !result) return -1;
    
    // Find the variable
    for (int i = 0; i < classifier->num_classifications; i++) {
        if (classifier->classifications[i].var_id == var_id) {
            var_classification_t *classification = &classifier->classifications[i];
            
            // Check if it's a node variable
            if (classification->primary_category == VAR_CAT_NODE) {
                *result = *classification;
                return 0;
            }
        }
    }
    
    return -1;
}

int var_classify_search_tree(var_classifier_t *classifier, uint64_t var_id, var_classification_t *result) {
    if (!classifier || !result) return -1;
    
    // Find the variable
    for (int i = 0; i < classifier->num_classifications; i++) {
        if (classifier->classifications[i].var_id == var_id) {
            var_classification_t *classification = &classifier->classifications[i];
            
            // Check if it's a search tree variable
            if (classification->primary_category == VAR_CAT_SEARCH_TREE) {
                *result = *classification;
                return 0;
            }
        }
    }
    
    return -1;
}

// ============================================================================
// Aspect Analysis
// ============================================================================

int var_analyze_aspects(var_classifier_t *classifier, var_classification_t *classification) {
    if (!classifier || !classification) return -1;
    
    const char *name = classification->demangled[0] ? classification->demangled : classification->name;
    
    // Check for static aspect
    if (apply_rules(name, static_aspect_rules) != VAR_ASPECT_UNKNOWN) {
        classification->aspects[classification->num_aspects++] = VAR_ASPECT_STATIC;
    }
    
    // Check for shared aspect
    if (apply_rules(name, shared_aspect_rules) != VAR_ASPECT_UNKNOWN) {
        classification->aspects[classification->num_aspects++] = VAR_ASPECT_SHARED;
    }
    
    // Check for private aspect
    if (apply_rules(name, private_aspect_rules) != VAR_ASPECT_UNKNOWN) {
        classification->aspects[classification->num_aspects++] = VAR_ASPECT_PRIVATE;
    }
    
    // Check for tree aspect
    if (apply_rules(name, tree_aspect_rules) != VAR_ASPECT_UNKNOWN) {
        classification->aspects[classification->num_aspects++] = VAR_ASPECT_TREE;
    }
    
    // Check for graph aspect
    if (apply_rules(name, graph_aspect_rules) != VAR_ASPECT_UNKNOWN) {
        classification->aspects[classification->num_aspects++] = VAR_ASPECT_GRAPH;
    }
    
    // Add aspect based on category
    switch (classification->primary_category) {
        case VAR_CAT_CONSTANT:
            classification->aspects[classification->num_aspects++] = VAR_ASPECT_STATIC;
            classification->aspects[classification->num_aspects++] = VAR_ASPECT_IMMUTABLE;
            break;
        case VAR_CAT_DATABASE:
            classification->aspects[classification->num_aspects++] = VAR_ASPECT_SEQUENTIAL;
            classification->aspects[classification->num_aspects++] = VAR_ASPECT_SHARED;
            break;
        case VAR_CAT_PARALLEL:
            classification->aspects[classification->num_aspects++] = VAR_ASPECT_SHARED;
            break;
        case VAR_CAT_TEMPORARY:
            classification->aspects[classification->num_aspects++] = VAR_ASPECT_TEMPORARY;
            break;
        case VAR_CAT_BOARD:
        case VAR_CAT_BITBOARD:
            classification->aspects[classification->num_aspects++] = VAR_ASPECT_HIERARCHICAL;
            classification->aspects[classification->num_aspects++] = VAR_ASPECT_INDEXED;
            break;
        case VAR_CAT_MOVE_TREE:
        case VAR_CAT_SEARCH_TREE:
            classification->aspects[classification->num_aspects++] = VAR_ASPECT_TREE;
            classification->aspects[classification->num_aspects++] = VAR_ASPECT_HIERARCHICAL;
            break;
        default:
            break;
    }
    
    // Set confidence
    classification->aspect_confidence = 80;
    
    return 0;
}

int var_analyze_access_patterns(var_classifier_t *classifier, var_classification_t *classification) {
    if (!classifier || !classification) return -1;
    
    // Determine access pattern based on category
    switch (classification->primary_category) {
        case VAR_CAT_BOARD:
        case VAR_CAT_BITBOARD:
            classification->access_pattern = VAR_ACCESS_RANDOM;
            classification->access_confidence = 90;
            break;
        case VAR_CAT_MOVE:
        case VAR_CAT_MOVE_TREE:
            classification->access_pattern = VAR_ACCESS_ITERATIVE;
            classification->access_confidence = 85;
            break;
        case VAR_CAT_DATABASE:
            classification->access_pattern = VAR_ACCESS_SEQUENTIAL;
            classification->access_confidence = 80;
            break;
        case VAR_CAT_SEARCH_TREE:
            classification->access_pattern = VAR_ACCESS_RECURSIVE;
            classification->access_confidence = 85;
            break;
        case VAR_CAT_PARALLEL:
            classification->access_pattern = VAR_ACCESS_PARALLEL;
            classification->access_confidence = 90;
            break;
        case VAR_CAT_CONSTANT:
            classification->access_pattern = VAR_ACCESS_SINGLE;
            classification->access_confidence = 95;
            break;
        case VAR_CAT_TEMPORARY:
            classification->access_pattern = VAR_ACCESS_FREQUENT;
            classification->access_confidence = 75;
            break;
        default:
            classification->access_pattern = VAR_ACCESS_MULTIPLE;
            classification->access_confidence = 50;
            break;
    }
    
    return 0;
}

int var_analyze_data_types(var_classifier_t *classifier, var_classification_t *classification) {
    if (!classifier || !classification) return -1;
    
    // Determine data type based on category
    switch (classification->primary_category) {
        case VAR_CAT_BOARD:
        case VAR_CAT_BITBOARD:
            classification->data_type = VAR_TYPE_MATRIX;
            classification->type_confidence = 90;
            break;
        case VAR_CAT_MOVE:
            classification->data_type = VAR_TYPE_STRUCT;
            classification->type_confidence = 85;
            break;
        case VAR_CAT_MOVE_TREE:
        case VAR_CAT_SEARCH_TREE:
            classification->data_type = VAR_TYPE_TREE;
            classification->type_confidence = 90;
            break;
        case VAR_CAT_EVALUATION:
        case VAR_CAT_SCORE:
            classification->data_type = VAR_TYPE_SCALAR;
            classification->type_confidence = 85;
            break;
        case VAR_CAT_DEPTH:
            classification->data_type = VAR_TYPE_SCALAR;
            classification->type_confidence = 90;
            break;
        case VAR_CAT_NODE:
            classification->data_type = VAR_TYPE_STRUCT;
            classification->type_confidence = 85;
            break;
        case VAR_CAT_DATABASE:
            classification->data_type = VAR_TYPE_HASH_TABLE;
            classification->type_confidence = 75;
            break;
        case VAR_CAT_CONSTANT:
            classification->data_type = VAR_TYPE_SCALAR;
            classification->type_confidence = 95;
            break;
        case VAR_CAT_PARALLEL:
            classification->data_type = VAR_TYPE_STRUCT;
            classification->type_confidence = 80;
            break;
        case VAR_CAT_MESSAGE:
            classification->data_type = VAR_TYPE_STRING;
            classification->type_confidence = 85;
            break;
        case VAR_CAT_TEMPORARY:
            classification->data_type = VAR_TYPE_SCALAR;
            classification->type_confidence = 70;
            break;
        default:
            classification->data_type = VAR_TYPE_UNKNOWN;
            classification->type_confidence = 50;
            break;
    }
    
    return 0;
}

// ============================================================================
// Shogi AI Specific Classification
// ============================================================================

int var_classify_shogi_variables(var_classifier_t *classifier) {
    if (!classifier) return -1;
    
    for (int i = 0; i < classifier->num_classifications; i++) {
        var_classification_t *classification = &classifier->classifications[i];
        
        // Identify Shogi-specific variables
        const char *name = classification->demangled[0] ? classification->demangled : classification->name;
        
        // Check for Shogi-specific patterns
        if (strcontains(name, "shogi") || strcontains(name, "usi")) {
            classification->is_shogi_specific = 1;
        }
        
        // Identify Shogi roles
        switch (classification->primary_category) {
            case VAR_CAT_BOARD:
            case VAR_CAT_BITBOARD:
                classification->shogi_role = 1; // Board
                break;
            case VAR_CAT_MOVE:
                classification->shogi_role = 2; // Move
                break;
            case VAR_CAT_MOVE_TREE:
            case VAR_CAT_SEARCH_TREE:
                classification->shogi_role = 3; // Search Tree
                break;
            case VAR_CAT_EVALUATION:
            case VAR_CAT_SCORE:
                classification->shogi_role = 4; // Evaluation
                break;
            case VAR_CAT_DEPTH:
                classification->shogi_role = 5; // Depth
                break;
            case VAR_CAT_NODE:
                classification->shogi_role = 6; // Node
                break;
            case VAR_CAT_PIECE:
                classification->shogi_role = 7; // Piece
                break;
            default:
                classification->shogi_role = 0; // Unknown
                break;
        }
    }
    
    return 0;
}

int var_identify_shogi_roles(var_classifier_t *classifier) {
    if (!classifier) return -1;
    
    // Shogi roles are already identified in var_classify_shogi_variables
    return 0;
}

// ============================================================================
// Parallel Execution Classification
// ============================================================================

int var_classify_parallel_variables(var_classifier_t *classifier) {
    if (!classifier) return -1;
    
    for (int i = 0; i < classifier->num_classifications; i++) {
        var_classification_t *classification = &classifier->classifications[i];
        
        // Check if this is a parallel variable
        if (classification->primary_category == VAR_CAT_PARALLEL) {
            // Add parallel-specific aspects
            classification->aspects[classification->num_aspects++] = VAR_ASPECT_SHARED;
            
            // If it's a beam search related variable
            const char *name = classification->demangled[0] ? classification->demangled : classification->name;
            if (strcontains(name, "beam") || strcontains(name, "search")) {
                classification->aspects[classification->num_aspects++] = VAR_ASPECT_DYNAMIC;
            }
        }
    }
    
    return 0;
}

int var_identify_shared_variables(var_classifier_t *classifier) {
    if (!classifier) return -1;
    
    for (int i = 0; i < classifier->num_classifications; i++) {
        var_classification_t *classification = &classifier->classifications[i];
        
        // Check if this variable is shared
        if (classification->is_global) {
            // Add shared aspect if not already present
            int has_shared = 0;
            for (int j = 0; j < classification->num_aspects; j++) {
                if (classification->aspects[j] == VAR_ASPECT_SHARED) {
                    has_shared = 1;
                    break;
                }
            }
            if (!has_shared) {
                classification->aspects[classification->num_aspects++] = VAR_ASPECT_SHARED;
            }
        }
    }
    
    return 0;
}

// ============================================================================
// Output Functions
// ============================================================================

void var_classifier_print(var_classifier_t *classifier, macho_opts_t *opts, FILE *out) {
    if (!classifier || !out) return;
    
    int c = opts ? opts->color : 0;
    const char *C_TITLE = c ? "\033[1;36m" : "";
    const char *C_SECTION = c ? "\033[1;33m" : "";
    const char *C_CAT = c ? "\033[1;32m" : "";
    const char *C_ASPECT = c ? "\033[1;35m" : "";
    const char *C_ACCESS = c ? "\033[1;36m" : "";
    const char *C_TYPE = c ? "\033[1;34m" : "";
    const char *C_SHOGI = c ? "\033[1;33m" : "";
    const char *C_RESET = c ? "\033[0m" : "";
    
    fprintf(out, "%s=== Variable Classification ===%s\n", C_TITLE, C_RESET);
    fprintf(out, "Total Variables: %d\n\n", classifier->num_classifications);
    
    var_classifier_print_summary(classifier, opts, out);
    fprintf(out, "\n");
    var_classifier_print_detailed(classifier, opts, out);
}

void var_classifier_print_summary(var_classifier_t *classifier, macho_opts_t *opts, FILE *out) {
    if (!classifier || !out) return;
    
    int c = opts ? opts->color : 0;
    const char *C_SECTION = c ? "\033[1;33m" : "";
    const char *C_CAT = c ? "\033[1;32m" : "";
    const char *C_ASPECT = c ? "\033[1;35m" : "";
    const char *C_ACCESS = c ? "\033[1;36m" : "";
    const char *C_TYPE = c ? "\033[1;34m" : "";
    const char *C_RESET = c ? "\033[0m" : "";
    
    fprintf(out, "%sCategory Distribution:%s\n", C_SECTION, C_RESET);
    for (int i = 1; i < VAR_CAT_MAX; i++) {
        if (classifier->categories[i] > 0) {
            fprintf(out, "  %s%-15s%s: %d (%3.1f%%)\n", 
                   C_CAT, var_category_to_string(i), C_RESET,
                   classifier->categories[i],
                   100.0 * classifier->categories[i] / classifier->num_classifications);
        }
    }
    
    fprintf(out, "\n%sAspect Distribution:%s\n", C_SECTION, C_RESET);
    for (int i = 1; i < VAR_ASPECT_MAX; i++) {
        if (classifier->aspects[i] > 0) {
            fprintf(out, "  %s%-15s%s: %d\n", 
                   C_ASPECT, var_aspect_to_string(i), C_RESET,
                   classifier->aspects[i]);
        }
    }
    
    fprintf(out, "\n%sAccess Pattern Distribution:%s\n", C_SECTION, C_RESET);
    for (int i = 1; i < VAR_ACCESS_MAX; i++) {
        if (classifier->access_patterns[i] > 0) {
            fprintf(out, "  %s%-15s%s: %d\n", 
                   C_ACCESS, var_access_pattern_to_string(i), C_RESET,
                   classifier->access_patterns[i]);
        }
    }
    
    fprintf(out, "\n%sData Type Distribution:%s\n", C_SECTION, C_RESET);
    for (int i = 1; i < VAR_TYPE_MAX; i++) {
        if (classifier->data_types[i] > 0) {
            fprintf(out, "  %s%-15s%s: %d\n", 
                   C_TYPE, var_data_type_to_string(i), C_RESET,
                   classifier->data_types[i]);
        }
    }
}

void var_classifier_print_detailed(var_classifier_t *classifier, macho_opts_t *opts, FILE *out) {
    if (!classifier || !out) return;
    
    int c = opts ? opts->color : 0;
    const char *C_SECTION = c ? "\033[1;33m" : "";
    const char *C_VAR = c ? "\033[1;37m" : "";
    const char *C_CAT = c ? "\033[1;32m" : "";
    const char *C_ASPECT = c ? "\033[1;35m" : "";
    const char *C_ACCESS = c ? "\033[1;36m" : "";
    const char *C_TYPE = c ? "\033[1;34m" : "";
    const char *C_SHOGI = c ? "\033[1;33m" : "";
    const char *C_RESET = c ? "\033[0m" : "";
    
    fprintf(out, "%sDetailed Classification:%s\n", C_SECTION, C_RESET);
    
    for (int i = 0; i < classifier->num_classifications; i++) {
        var_classification_t *classification = &classifier->classifications[i];
        
        fprintf(out, "\n  %sVariable %d: %s%s (0x%llx, size: %llu)%s\n",
               C_VAR, classification->var_id, 
               classification->demangled[0] ? classification->demangled : classification->name,
               C_RESET,
               (unsigned long long)classification->address,
               (unsigned long long)classification->size,
               C_RESET);
        
        fprintf(out, "    Category: %s%s (confidence: %d%%)\n",
               C_CAT, var_category_to_string(classification->primary_category), C_RESET,
               classification->category_confidence);
        
        if (classification->num_secondary_categories > 0) {
            fprintf(out, "    Secondary Categories: ");
            for (int j = 0; j < classification->num_secondary_categories; j++) {
                fprintf(out, "%s", var_category_to_string(classification->secondary_categories[j]));
                if (j < classification->num_secondary_categories - 1) fprintf(out, ", ");
            }
            fprintf(out, "\n");
        }
        
        fprintf(out, "    Aspects: ");
        for (int j = 0; j < classification->num_aspects; j++) {
            fprintf(out, "%s%s", 
                   j > 0 ? ", " : "",
                   var_aspect_to_string(classification->aspects[j]));
        }
        fprintf(out, " %s(confidence: %d%%)\n", C_RESET, classification->aspect_confidence);
        
        fprintf(out, "    Access Pattern: %s%s (confidence: %d%%)\n",
               C_ACCESS, var_access_pattern_to_string(classification->access_pattern), C_RESET,
               classification->access_confidence);
        
        fprintf(out, "    Data Type: %s%s (confidence: %d%%)\n",
               C_TYPE, var_data_type_to_string(classification->data_type), C_RESET,
               classification->type_confidence);
        
        if (classification->is_shogi_specific) {
            fprintf(out, "    Shogi Role: %s", C_SHOGI);
            switch (classification->shogi_role) {
                case 1: fprintf(out, "Board"); break;
                case 2: fprintf(out, "Move"); break;
                case 3: fprintf(out, "Search Tree"); break;
                case 4: fprintf(out, "Evaluation"); break;
                case 5: fprintf(out, "Depth"); break;
                case 6: fprintf(out, "Node"); break;
                case 7: fprintf(out, "Piece"); break;
                default: fprintf(out, "Unknown"); break;
            }
            fprintf(out, "%s\n", C_RESET);
        }
        
        if (classification->is_global) fprintf(out, "    [GLOBAL]\n");
        if (classification->is_static) fprintf(out, "    [STATIC]\n");
        if (classification->is_constant) fprintf(out, "    [CONSTANT]\n");
    }
}

// ============================================================================
// DOT Export Functions
// ============================================================================

void var_classifier_export_dot(var_classifier_t *classifier, const char *filename) {
    if (!classifier || !filename) return;
    
    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "var_classifier: Failed to open file '%s' for writing\n", filename);
        return;
    }
    
    fprintf(f, "digraph VariableClassification {\n");
    fprintf(f, "  rankdir=TB;\n");
    fprintf(f, "  node [shape=box, style=filled];\n");
    fprintf(f, "  edge [color=gray40];\n\n");
    
    // Define colors for categories
    const char *colors[] = {
        "lightgray",   // UNKNOWN
        "lightblue",   // BOARD
        "lightgreen",  // MOVE
        "lightyellow", // MOVE_TREE
        "lightpink",   // EVALUATION
        "lightsalmon", // DEPTH
        "lightcoral",  // NODE
        "lightpurple", // SEARCH_TREE
        "lightbrown",   // CONSTANT
        "lightorange", // DATABASE
        "lightcyan",   // STATE
        "lightgold",   // MESSAGE
        "lightsteelblue", // PARALLEL
        "lightgray",   // TEMPORARY
        "lightgray",   // GLOBAL_CONFIG
        "lightgray",   // HASH
        "lightgray",   // BITBOARD
        "lightgray",   // PIECE
        "lightgray",   // SCORE
        "lightgray",   // TIME
        "lightgray",   // MEMORY
        "lightgray",   // IO
        "lightgray"    // UTILITY
    };
    
    // Create nodes
    for (int i = 0; i < classifier->num_classifications; i++) {
        var_classification_t *classification = &classifier->classifications[i];
        
        const char *color = colors[classification->primary_category % 21];
        const char *shape = "box";
        
        switch (classification->primary_category) {
            case VAR_CAT_BOARD:
            case VAR_CAT_BITBOARD:
                shape = "cylinder";
                break;
            case VAR_CAT_MOVE:
            case VAR_CAT_NODE:
                shape = "ellipse";
                break;
            case VAR_CAT_MOVE_TREE:
            case VAR_CAT_SEARCH_TREE:
                shape = "folder";
                break;
            case VAR_CAT_CONSTANT:
                shape = "plaintext";
                break;
            default:
                shape = "box";
                break;
        }
        
        // Escape special characters in name
        char escaped_name[256];
        int j = 0;
        for (int k = 0; classification->demangled[k] && j < sizeof(escaped_name) - 1; k++) {
            if (classification->demangled[k] == '"' || classification->demangled[k] == '\\' || classification->demangled[k] == '\n') {
                escaped_name[j++] = '\\';
            }
            escaped_name[j++] = classification->demangled[k];
        }
        escaped_name[j] = '\0';
        
        fprintf(f, "  n%llu [label=\"%s\\n0x%llx\\n%s\", shape=%s, fillcolor=%s];\n",
               (unsigned long long)classification->var_id,
               escaped_name,
               (unsigned long long)classification->address,
               var_category_to_string(classification->primary_category),
               shape, color);
    }
    
    fprintf(f, "\n");
    
    // Create subgraphs for categories
    for (int cat = 1; cat < VAR_CAT_MAX; cat++) {
        int count = 0;
        for (int i = 0; i < classifier->num_classifications; i++) {
            if (classifier->classifications[i].primary_category == cat) {
                count++;
            }
        }
        
        if (count > 0) {
            fprintf(f, "  subgraph cluster_%d {\n", cat);
            fprintf(f, "    label=\"%s\\n(%d variables)\";\n", 
                   var_category_to_string(cat), count);
            fprintf(f, "    style=filled;\n");
            fprintf(f, "    color=%s;\n", colors[cat % 21]);
            fprintf(f, "    node [style=filled, fillcolor=white];\n\n");
            
            for (int i = 0; i < classifier->num_classifications; i++) {
                if (classifier->classifications[i].primary_category == cat) {
                    fprintf(f, "    n%llu;\n", 
                           (unsigned long long)classifier->classifications[i].var_id);
                }
            }
            
            fprintf(f, "  }\n\n");
        }
    }
    
    fprintf(f, "}\n");
    
    fclose(f);
}

void var_classifier_export_category_dot(var_classifier_t *classifier, const char *filename) {
    if (!classifier || !filename) return;
    
    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "var_classifier: Failed to open file '%s' for writing\n", filename);
        return;
    }
    
    fprintf(f, "digraph CategoryClassification {\n");
    fprintf(f, "  rankdir=TB;\n");
    fprintf(f, "  node [shape=box, style=filled];\n");
    fprintf(f, "  edge [color=gray40];\n\n");
    
    // Create category nodes
    for (int cat = 1; cat < VAR_CAT_MAX; cat++) {
        if (classifier->categories[cat] > 0) {
            fprintf(f, "  cat_%d [label=\"%s\\n%d variables\", fillcolor=lightblue];\n",
                   cat, var_category_to_string(cat), classifier->categories[cat]);
        }
    }
    
    fprintf(f, "\n");
    
    // Create variable nodes grouped by category
    for (int cat = 1; cat < VAR_CAT_MAX; cat++) {
        if (classifier->categories[cat] > 0) {
            for (int i = 0; i < classifier->num_classifications; i++) {
                if (classifier->classifications[i].primary_category == cat) {
                    var_classification_t *classification = &classifier->classifications[i];
                    
                    // Escape special characters in name
                    char escaped_name[256];
                    int j = 0;
                    for (int k = 0; classification->demangled[k] && j < sizeof(escaped_name) - 1; k++) {
                        if (classification->demangled[k] == '"' || classification->demangled[k] == '\\' || classification->demangled[k] == '\n') {
                            escaped_name[j++] = '\\';
                        }
                        escaped_name[j++] = classification->demangled[k];
                    }
                    escaped_name[j] = '\0';
                    
                    fprintf(f, "  var_%llu [label=\"%s\", fillcolor=lightyellow];\n",
                           (unsigned long long)classification->var_id, escaped_name);
                    
                    fprintf(f, "  cat_%d -> var_%llu;\n", cat, (unsigned long long)classification->var_id);
                }
            }
        }
    }
    
    fprintf(f, "}\n");
    
    fclose(f);
}
