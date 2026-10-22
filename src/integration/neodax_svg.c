#include "neodax_svg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* SVG color definitions */
static const char *svg_colors[] = {
    "#4e79a7",  /* Blue */
    "#f28e2b",  /* Orange */
    "#e15759",  /* Red */
    "#76b7b2",  /* Teal */
    "#59a14f",  /* Green */
    "#edc948",  /* Yellow */
    "#b07aa1",  /* Purple */
    "#ff9da7",  /* Pink */
    "#9c7aa1",  /* Brown */
    "#bab0ac"   /* Gray */
};

/* SVG header */
static void svg_write_header(FILE *fp, int width, int height) {
    fprintf(fp, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n");
    fprintf(fp, "<svg xmlns=\"http://www.w3.org/2000/svg\" "\n");
    fprintf(fp, "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "\n");
    fprintf(fp, "width=\"%d\" height=\"%d\" "\n", width, height);
    fprintf(fp, "viewBox=\"0 0 %d %d\" "\n", width, height);
    fprintf(fp, "xml:space=\"preserve\">\n");
    
    fprintf(fp, "<style type=\"text/css\">\n");
    fprintf(fp, "  .node { fill: #ffffff; stroke: #333333; stroke-width: 1.5; }\n");
    fprintf(fp, "  .node:hover { stroke: #000000; stroke-width: 2; }\n");
    fprintf(fp, "  .node.text { font-family: Arial, sans-serif; font-size: 12px; }\n");
    fprintf(fp, "  .edge { stroke: #999999; stroke-width: 1; }\n");
    fprintf(fp, "  .edge:hover { stroke: #000000; stroke-width: 2; }\n");
    fprintf(fp, "  .cluster { fill: none; stroke: #666666; stroke-width: 1; }\n");
    fprintf(fp, "  .title { font-family: Arial, sans-serif; font-size: 14px; font-weight: bold; }\n");
    fprintf(fp, "  .label { font-family: Arial, sans-serif; font-size: 10px; }\n");
    fprintf(fp, "</style>\n");
    
    fprintf(fp, "<defs>\n");
    fprintf(fp, "  <marker id=\"arrowhead\" markerWidth=\"10\" markerHeight=\"7\" "\n");
    fprintf(fp, "refX=\"0\" refY=\"3.5\" orient=\"auto\">\n");
    fprintf(fp, "    <polygon points=\"0 0, 10 3.5, 0 7\" fill=\"#666666\"/>\n");
    fprintf(fp, "  </marker>\n");
    fprintf(fp, "</defs>\n");
    fprintf(fp, "\n");
}

/* SVG footer */
static void svg_write_footer(FILE *fp) {
    fprintf(fp, "</svg>\n");
}

/* Escape special characters for SVG */
static void svg_escape(char *dest, const char *src, size_t dest_size) {
    size_t i = 0, j = 0;
    while (src[i] && j < dest_size - 1) {
        switch (src[i]) {
            case '&':  if (j + 5 < dest_size) { dest[j++] = '&'; dest[j++] = 'a'; dest[j++] = 'm'; dest[j++] = 'p'; dest[j++] = ';'; } break;
            case '<':  if (j + 4 < dest_size) { dest[j++] = '&'; dest[j++] = 'l'; dest[j++] = 't'; dest[j++] = ';'; } break;
            case '>':  if (j + 4 < dest_size) { dest[j++] = '&'; dest[j++] = 'g'; dest[j++] = 't'; dest[j++] = ';'; } break;
            case '"': if (j + 6 < dest_size) { dest[j++] = '&'; dest[j++] = 'q'; dest[j++] = 'u'; dest[j++] = 'o'; dest[j++] = 't'; dest[j++] = ';'; } break;
            case '\'': if (j + 5 < dest_size) { dest[j++] = '&'; dest[j++] = 'a'; dest[j++] = 'p'; dest[j++] = 'o'; dest[j++] = 's'; } break;
            default:   dest[j++] = src[i]; break;
        }
        i++;
    }
    dest[j] = '\0';
}

/* Generate SVG for dependency graph */
static void svg_generate_dependency_graph(integrated_binary_t *ibin, FILE *fp, 
                                         int width, int height) {
    if (ibin->dep_graph.node_count == 0) return;
    
    int margin = 50;
    int center_x = width / 2;
    int center_y = height / 2;
    int max_radius = MIN(width, height) / 2 - margin;
    int node_radius = 25;
    
    /* Draw title */
    fprintf(fp, "<g transform=\"translate(0, 20)\">\n");
    fprintf(fp, "<text x=\"%d\" y=\"20\" text-anchor=\"middle\" class=\"title\">" 
            "Dependency Graph (%d nodes, %d edges)</text>\n",
            width / 2, ibin->dep_graph.node_count, ibin->dep_graph.edge_count);
    fprintf(fp, "</g>\n");
    
    /* Draw nodes in circular layout */
    double angle_step = 2 * M_PI / ibin->dep_graph.node_count;
    for (int i = 0; i < ibin->dep_graph.node_count; i++) {
        double angle = i * angle_step;
        int x = center_x + (int)(max_radius * cos(angle));
        int y = center_y + (int)(max_radius * sin(angle));
        
        const char *color = svg_colors[i % 10];
        fprintf(fp, "<circle cx=\"%d\" cy=\"%d\" r=\"%d\" fill=\"%s\" class=\"node\"/>\n",
                x, y, node_radius, color);
        
        char escaped_name[256];
        svg_escape(escaped_name, ibin->dep_graph.nodes[i].name, sizeof(escaped_name));
        fprintf(fp, "<text x=\"%d\" y=\"%d\" text-anchor=\"middle\" " 
                "dominant-baseline=\"middle\" class=\"node.text\" font-size=\"10\">" 
                "%s</text>\n",
                x, y - 5, escaped_name);
    }
    
    /* Draw edges */
    for (int i = 0; i < ibin->dep_graph.edge_count; i++) {
        depflow_edge_t *edge = &ibin->dep_graph.edges[i];
        if (edge->from >= ibin->dep_graph.node_count || 
            edge->to >= ibin->dep_graph.node_count) continue;
        
        double angle_from = edge->from * angle_step;
        double angle_to = edge->to * angle_step;
        int x1 = center_x + (int)(max_radius * cos(angle_from));
        int y1 = center_y + (int)(max_radius * sin(angle_from));
        int x2 = center_x + (int)(max_radius * cos(angle_to));
        int y2 = center_y + (int)(max_radius * sin(angle_to));
        
        /* Adjust for node radius */
        double dx = x2 - x1;
        double dy = y2 - y1;
        double dist = sqrt(dx*dx + dy*dy);
        if (dist > 0) {
            x1 += (int)(node_radius * dx / dist);
            y1 += (int)(node_radius * dy / dist);
            x2 -= (int)(node_radius * dx / dist);
            y2 -= (int)(node_radius * dy / dist);
        }
        
        fprintf(fp, "<path d=\"M %d %d L %d %d\" stroke=\"#666666\" " 
                "stroke-width=\"1.5\" marker-end=\"url(#arrowhead)\" class=\"edge\"/>\n",
                x1, y1, x2, y2);
    }
}

/* Generate SVG for execution graph */
static void svg_generate_execution_graph(integrated_binary_t *ibin, FILE *fp,
                                          int width, int height) {
    if (ibin->exec_graph.node_count == 0) return;
    
    int margin = 50;
    int node_width = 80;
    int node_height = 40;
    int x_spacing = 120;
    int y_spacing = 60;
    
    /* Draw title */
    fprintf(fp, "<g transform=\"translate(0, 20)\">\n");
    fprintf(fp, "<text x=\"%d\" y=\"20\" text-anchor=\"middle\" class=\"title\">" 
            "Execution Graph (%d nodes, %d edges)</text>\n",
            width / 2, ibin->exec_graph.node_count, ibin->exec_graph.edge_count);
    fprintf(fp, "</g>\n");
    
    /* Draw nodes in grid layout */
    int cols = (int)sqrt(ibin->exec_graph.node_count) + 1;
    for (int i = 0; i < ibin->exec_graph.node_count; i++) {
        int row = i / cols;
        int col = i % cols;
        int x = margin + col * x_spacing;
        int y = margin + 40 + row * y_spacing;
        
        const char *color = svg_colors[i % 10];
        fprintf(fp, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" " 
                "fill=\"%s\" rx=\"5\" ry=\"5\" class=\"node\"/>\n",
                x, y, node_width, node_height, color);
        
        execution_node_t *node = &ibin->exec_graph.nodes[i];
        char escaped_name[256];
        svg_escape(escaped_name, node->name, sizeof(escaped_name));
        fprintf(fp, "<text x=\"%d\" y=\"%d\" text-anchor=\"middle\" " 
                "dominant-baseline=\"middle\" class=\"node.text\">%s</text>\n",
                x + node_width/2, y + node_height/2 + 5, escaped_name);
    }
    
    /* Draw edges */
    for (int i = 0; i < ibin->exec_graph.edge_count; i++) {
        exec_edge_t *edge = &ibin->exec_graph.edges[i];
        if (edge->from >= ibin->exec_graph.node_count || 
            edge->to >= ibin->exec_graph.node_count) continue;
        
        int x1 = margin + (edge->from % cols) * x_spacing + node_width;
        int y1 = margin + 40 + (edge->from / cols) * y_spacing + node_height/2;
        int x2 = margin + (edge->to % cols) * x_spacing;
        int y2 = margin + 40 + (edge->to / cols) * y_spacing + node_height/2;
        
        fprintf(fp, "<path d=\"M %d %d L %d %d\" stroke=\"#666666\" " 
                "stroke-width=\"1.5\" marker-end=\"url(#arrowhead)\" class=\"edge\"/>\n",
                x1, y1, x2, y2);
    }
}

/* Generate SVG for state graph */
static void svg_generate_state_graph(integrated_binary_t *ibin, FILE *fp,
                                     int width, int height) {
    if (ibin->state_tracker.checkpoint_count == 0) return;
    
    int margin = 50;
    int center_x = width / 2;
    int center_y = height / 2;
    int max_radius = MIN(width, height) / 2 - margin;
    int node_radius = 25;
    
    /* Draw title */
    fprintf(fp, "<g transform=\"translate(0, 20)\">\n");
    fprintf(fp, "<text x=\"%d\" y=\"20\" text-anchor=\"middle\" class=\"title\">" 
            "State Graph (%d checkpoints)</text>\n",
            width / 2, ibin->state_tracker.checkpoint_count);
    fprintf(fp, "</g>\n");
    
    /* Draw state nodes in circular layout */
    double angle_step = 2 * M_PI / ibin->state_tracker.checkpoint_count;
    for (int i = 0; i < ibin->state_tracker.checkpoint_count; i++) {
        double angle = i * angle_step;
        int x = center_x + (int)(max_radius * cos(angle));
        int y = center_y + (int)(max_radius * sin(angle));
        
        const char *color = svg_colors[i % 10];
        fprintf(fp, "<circle cx=\"%d\" cy=\"%d\" r=\"%d\" fill=\"%s\" class=\"node\"/>\n",
                x, y, node_radius, color);
        
        state_checkpoint_t *cp = &ibin->state_tracker.checkpoints[i];
        char escaped_desc[256];
        svg_escape(escaped_desc, cp->description, sizeof(escaped_desc));
        fprintf(fp, "<text x=\"%d\" y=\"%d\" text-anchor=\"middle\" " 
                "dominant-baseline=\"middle\" class=\"node.text\" font-size=\"10\">" 
                "%s</text>\n",
                x, y - 5, escaped_desc[0] ? escaped_desc : "state");
    }
    
    /* Draw transitions */
    for (int i = 0; i < ibin->state_tracker.checkpoint_count - 1; i++) {
        double angle_from = i * angle_step;
        double angle_to = (i + 1) * angle_step;
        int x1 = center_x + (int)(max_radius * cos(angle_from));
        int y1 = center_y + (int)(max_radius * sin(angle_from));
        int x2 = center_x + (int)(max_radius * cos(angle_to));
        int y2 = center_y + (int)(max_radius * sin(angle_to));
        
        fprintf(fp, "<path d=\"M %d %d L %d %d\" stroke=\"#666666\" " 
                "stroke-width=\"1\" marker-end=\"url(#arrowhead)\" class=\"edge\"/>\n",
                x1, y1, x2, y2);
    }
}

/* Generate SVG for classification */
static void svg_generate_classification(integrated_binary_t *ibin, FILE *fp,
                                          int width, int height) {
    if (!ibin->variables_classified) return;
    
    int margin = 50;
    int bar_height = 30;
    int bar_spacing = 40;
    int max_bars = (height - margin * 2) / bar_spacing;
    int start_idx = 0;
    
    fprintf(fp, "<g transform=\"translate(%d, %d)\">\n", margin, margin + 20);
    fprintf(fp, "<text x=\"0\" y=\"-20\" class=\"title\">Variable Classification</text>\n");
    
    /* Draw classification bars */
    for (int i = start_idx; i < ibin->classifier.classification_count && 
         (i - start_idx) < max_bars; i++) {
        var_classification_t *cls = &ibin->classifier.classifications[i];
        int y = (i - start_idx) * bar_spacing;
        int bar_width = 200;
        
        const char *color = svg_colors[i % 10];
        fprintf(fp, "<rect x=\"0\" y=\"%d\" width=\"%d\" height=\"%d\" " 
                "fill=\"%s\" rx=\"3\" ry=\"3\" class=\"node\"/>\n",
                y, bar_width, bar_height, color);
        
        char escaped_name[256];
        svg_escape(escaped_name, cls->name, sizeof(escaped_name));
        fprintf(fp, "<text x=\"10\" y=\"%d\" dominant-baseline=\"middle\" " 
                "class=\"node.text\">%s (%s)</text>\n",
                y + bar_height/2 + 5, escaped_name, var_category_str(cls->category));
        
        /* Confidence bar */
        int conf_width = (bar_width * cls->category_confidence) / 100;
        fprintf(fp, "<rect x=\"220\" y=\"%d\" width=\"%d\" height=\"%d\" " 
                "fill=\"%s\" opacity=\"0.7\" rx=\"3\" ry=\"3\"/>\n",
                y, conf_width, bar_height, color);
        fprintf(fp, "<text x=\"220\" y=\"%d\" dominant-baseline=\"middle\" " 
                "class=\"node.text\" font-size=\"10\">%d%%</text>\n",
                y + bar_height/2 + 5, cls->category_confidence);
    }
    fprintf(fp, "</g>\n");
}

/* Generate SVG for parallel analysis */
static void svg_generate_parallel_analysis(integrated_binary_t *ibin, FILE *fp,
                                          int width, int height) {
    if (!ibin->has_parallel_execution) return;
    
    int margin = 50;
    int center_x = width / 2;
    int y = margin + 20;
    
    fprintf(fp, "<g transform=\"translate(0, %d)\">\n", margin);
    fprintf(fp, "<text x=\"%d\" y=\"0\" text-anchor=\"middle\" class=\"title\">" 
            "Parallel Execution Analysis</text>\n", center_x);
    
    y += 40;
    
    /* Beam search patterns */
    if (ibin->parallel_detector.beam_search_count > 0) {
        fprintf(fp, "<g transform=\"translate(%d, %d)\">\n", margin, y);
        fprintf(fp, "<text x=\"0\" y=\"0\" class=\"title\" font-size=\"14\">" 
                "Beam Search Patterns: %d</text>\n",
                ibin->parallel_detector.beam_search_count);
        
        for (int i = 0; i < ibin->parallel_detector.beam_search_count; i++) {
            beam_search_info_t *beam = &ibin->parallel_detector.beam_searches[i];
            int box_y = 30 + i * 40;
            
            fprintf(fp, "<rect x=\"10\" y=\"%d\" width=\"300\" height=\"30\" " 
                    "fill=\"#f0f0f0\" stroke=\"#666666\" rx=\"3\" ry=\"3\"/>\n", box_y);
            
            char escaped_name[256];
            svg_escape(escaped_name, beam->dispatch_func_name, sizeof(escaped_name));
            fprintf(fp, "<text x=\"20\" y=\"%d\" dominant-baseline=\"middle\" " 
                    "class=\"node.text\">Dispatch: %s (0x%llx)</text>\n",
                    box_y + 20, escaped_name, (unsigned long long)beam->dispatch_address);
        }
        fprintf(fp, "</g>\n");
        y += 40 + ibin->parallel_detector.beam_search_count * 40;
    }
    
    /* Thread pool patterns */
    if (ibin->parallel_detector.thread_pool_count > 0) {
        fprintf(fp, "<g transform=\"translate(%d, %d)\">\n", margin, y);
        fprintf(fp, "<text x=\"0\" y=\"0\" class=\"title\" font-size=\"14\">" 
                "Thread Pool Patterns: %d</text>\n",
                ibin->parallel_detector.thread_pool_count);
        
        for (int i = 0; i < ibin->parallel_detector.thread_pool_count; i++) {
            thread_pool_info_t *pool = &ibin->parallel_detector.thread_pools[i];
            int box_y = 30 + i * 40;
            
            fprintf(fp, "<rect x=\"10\" y=\"%d\" width=\"300\" height=\"30\" " 
                    "fill=\"#f0f0f0\" stroke=\"#666666\" rx=\"3\" ry=\"3\"/>\n", box_y);
            
            char escaped_name[256];
            svg_escape(escaped_name, pool->create_func_name, sizeof(escaped_name));
            fprintf(fp, "<text x=\"20\" y=\"%d\" dominant-baseline=\"middle\" " 
                    "class=\"node.text\">%s (%d threads)</text>\n",
                    box_y + 20, escaped_name, pool->num_threads);
        }
        fprintf(fp, "</g>\n");
    }
    
    fprintf(fp, "</g>\n");
}

/* Main SVG generation function */
int neodax_generate_svg(integrated_binary_t *ibin,
                       static_analysis_opts_t *opts,
                       const char *filename) {
    if (!ibin || !filename) return -1;
    
    FILE *fp = fopen(filename, "w");
    if (!fp) return -1;
    
    int width = 1200;
    int height = 800;
    
    svg_write_header(fp, width, height);
    
    /* Generate title */
    fprintf(fp, "<g>\n");
    fprintf(fp, "<rect x=\"0\" y=\"0\" width=\"%d\" height=\"60\" fill=\"#2c3e50\"/>\n", width);
    fprintf(fp, "<text x=\"%d\" y=\"35\" text-anchor=\"middle\" " 
            "fill=\"#ffffff\" font-family=\"Arial\" font-size=\"20\" font-weight=\"bold\">" 
            "NeoDAX Static Analysis - SVG Visualization</text>\n", width/2);
    fprintf(fp, "</g>\n");
    
    /* Generate graphs based on options */
    int y_offset = 80;
    
    if (opts->show_dependency_graph && ibin->dep_graph.node_count > 0) {
        fprintf(fp, "<g transform=\"translate(0, %d)\">\n", y_offset);
        svg_generate_dependency_graph(ibin, fp, width, 350);
        fprintf(fp, "</g>\n");
        y_offset += 400;
    }
    
    if (opts->show_execution_graph && ibin->exec_graph.node_count > 0) {
        fprintf(fp, "<g transform=\"translate(0, %d)\">\n", y_offset);
        svg_generate_execution_graph(ibin, fp, width, 350);
        fprintf(fp, "</g>\n");
        y_offset += 400;
    }
    
    if (opts->show_state_graph && ibin->state_tracker.checkpoint_count > 0) {
        fprintf(fp, "<g transform=\"translate(0, %d)\">\n", y_offset);
        svg_generate_state_graph(ibin, fp, width, 350);
        fprintf(fp, "</g>\n");
        y_offset += 400;
    }
    
    if (opts->show_classification && ibin->variables_classified) {
        fprintf(fp, "<g transform=\"translate(0, %d)\">\n", y_offset);
        svg_generate_classification(ibin, fp, width, 200);
        fprintf(fp, "</g>\n");
        y_offset += 250;
    }
    
    if (ibin->has_parallel_execution && 
        (opts->detect_parallel || opts->beam_search_analysis)) {
        fprintf(fp, "<g transform=\"translate(0, %d)\">\n", y_offset);
        svg_generate_parallel_analysis(ibin, fp, width, 200);
        fprintf(fp, "</g>\n");
    }
    
    svg_write_footer(fp);
    fclose(fp);
    
    return 0;
}
