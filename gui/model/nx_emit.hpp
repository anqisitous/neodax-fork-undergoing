#ifndef NX_EMIT_HPP
#define NX_EMIT_HPP

#include <string>

#include "core/macho_static.h"
#include "nx_graph.hpp"

namespace nx {

struct EmitOptions {
    bool include_bytes = true;
    bool include_instructions = true;
    size_t max_functions = 0;
};

bool write_cpp(const macho_binary_t &bin, const Graph &g, const std::string &path,
               const EmitOptions &opt, std::string *err);
bool write_hex(const macho_binary_t &bin, const std::string &path,
               const EmitOptions &opt, std::string *err);
bool write_nodes(const macho_binary_t &bin, const Graph &g, const std::string &path,
                 const EmitOptions &opt, std::string *err);

} // namespace nx

#endif
