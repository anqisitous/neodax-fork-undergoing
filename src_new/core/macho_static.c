#include "macho_static.h"
#include <stdio.h>
#include <stdlib.h>
#define min(a,b) (((a)<(b))?(a):(b))
#define LE16(p) (((uint16_t)(p)[0]) | ((uint16_t)(p)[1] << 8))
#include <string.h>
#include <stdint.h>

// ============================================================================
// Mach-O Static Analysis Implementation
// 
// 静的解析のためのMach-Oファイルパーサと解析エンジン
// ============================================================================

// Byte swap utilities
static uint32_t bswap32(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v & 0xFF0000) >> 8) | (v >> 24);
}

static uint64_t bswap64(uint64_t v) {
    return ((uint64_t)bswap32((uint32_t)(v & 0xFFFFFFFF)) << 32) |
           (uint64_t)bswap32((uint32_t)(v >> 32));
}

// ============================================================================
// Initialization and Cleanup
// ============================================================================

void macho_init(macho_binary_t *bin) {
    if (!bin) return;
    
    memset(bin, 0, sizeof(macho_binary_t));
    bin->arch = MACHO_ARCH_UNKNOWN;
    bin->filetype = 0;
    bin->cputype = 0;
    bin->cpusubtype = 0;
    bin->flags = 0;
    bin->entry = 0;
    bin->base = 0;
    bin->image_size = 0;
    bin->nsections = 0;
    bin->nsymbols = 0;
    bin->symbols_allocated = 0;
    bin->nfunctions = 0;
    bin->functions_allocated = 0;
    bin->nxrefs = 0;
    bin->xrefs_allocated = 0;
    bin->nblocks = 0;
    bin->blocks_allocated = 0;
    bin->ninsns = 0;
    bin->insns_allocated = 0;
    bin->ndependencies = 0;
    bin->dependencies_allocated = 0;
    bin->ncall_chains = 0;
    bin->call_chains_allocated = 0;
    bin->nexecution_paths = 0;
    bin->execution_paths_allocated = 0;
    bin->nparallel_info = 0;
    bin->parallel_info_allocated = 0;
    bin->nvariables = 0;
    bin->variables_allocated = 0;
    bin->nstate_checkpoints = 0;
    bin->state_checkpoints_allocated = 0;
    bin->is_analyzed = 0;
    bin->is_pie = 0;
    bin->is_stripped = 0;
    bin->has_debug = 0;
    bin->total_insns = 0;
    bin->code_size = 0;
    bin->data_size = 0;
}

void macho_free(macho_binary_t *bin) {
    if (!bin) return;
    
    if (bin->data) {
        free(bin->data);
        bin->data = NULL;
    }
    
    if (bin->symbols) {
        free(bin->symbols);
        bin->symbols = NULL;
    }
    
    if (bin->functions) {
        free(bin->functions);
        bin->functions = NULL;
    }
    
    if (bin->xrefs) {
        free(bin->xrefs);
        bin->xrefs = NULL;
    }
    
    if (bin->blocks) {
        free(bin->blocks);
        bin->blocks = NULL;
    }
    
    if (bin->insns) {
        free(bin->insns);
        bin->insns = NULL;
    }
    
    if (bin->dependencies) {
        free(bin->dependencies);
        bin->dependencies = NULL;
    }
    
    if (bin->call_chains) {
        free(bin->call_chains);
        bin->call_chains = NULL;
    }
    
    if (bin->execution_paths) {
        free(bin->execution_paths);
        bin->execution_paths = NULL;
    }
    
    if (bin->parallel_info) {
        free(bin->parallel_info);
        bin->parallel_info = NULL;
    }
    
    if (bin->variables) {
        free(bin->variables);
        bin->variables = NULL;
    }
    
    if (bin->state_checkpoints) {
        free(bin->state_checkpoints);
        bin->state_checkpoints = NULL;
    }
    
    bin->size = 0;
    bin->nsections = 0;
    bin->nsymbols = 0;
    bin->symbols_allocated = 0;
    bin->nfunctions = 0;
    bin->functions_allocated = 0;
}

int macho_is_valid(macho_binary_t *bin) {
    if (!bin) return 0;
    if (!bin->data) return 0;
    if (bin->size == 0) return 0;
    if (bin->arch == MACHO_ARCH_UNKNOWN) return 0;
    return 1;
}

// ============================================================================
// Fat Binary Parsing
// ============================================================================

static int fat_find_slice(const uint8_t *data, size_t sz,
                         uint32_t want_cpu,
                         uint32_t *out_off, uint32_t *out_size) {
    if (sz < sizeof(macho_fat_header_t)) return -1;
    
    const macho_fat_header_t *fh = (const macho_fat_header_t *)data;
    uint32_t narch = bswap32(fh->nfat_arch);
    
    if (narch > 64) return -1;
    
    size_t base = sizeof(macho_fat_header_t);
    
    // First pass: try to find the exact CPU type
    for (uint32_t i = 0; i < narch; i++) {
        if (base + sizeof(macho_fat_arch_t) > sz) break;
        const macho_fat_arch_t *fa = (const macho_fat_arch_t *)(data + base);
        uint32_t cpu = bswap32(fa->cputype);
        uint32_t off = bswap32(fa->offset);
        uint32_t fsz = bswap32(fa->size);
        
        if (cpu == want_cpu && off + fsz <= sz) {
            *out_off = off;
            *out_size = fsz;
            return 0;
        }
        base += sizeof(macho_fat_arch_t);
    }
    
    // Second pass: fall back to first valid slice
    base = sizeof(macho_fat_header_t);
    for (uint32_t i = 0; i < narch; i++) {
        if (base + sizeof(macho_fat_arch_t) > sz) break;
        const macho_fat_arch_t *fa = (const macho_fat_arch_t *)(data + base);
        uint32_t off = bswap32(fa->offset);
        uint32_t fsz = bswap32(fa->size);
        if (off + fsz <= sz) {
            *out_off = off;
            *out_size = fsz;
            return 0;
        }
        base += sizeof(macho_fat_arch_t);
    }
    
    return -1;
}

int macho_parse_fat(macho_binary_t *bin, const uint8_t *data, size_t size) {
    if (size < sizeof(macho_fat_header_t)) return -1;
    
    uint32_t magic = *(const uint32_t *)data;
    
    if (magic != MACHO_FAT_MAGIC && magic != MACHO_FAT_MAGIC_LE) {
        return -1; // Not a fat binary
    }
    
    uint32_t off = 0, slice_sz = 0;
    
    // Try ARM64 first, then x86_64
    if (fat_find_slice(data, size, MACHO_CPU_ARM64, &off, &slice_sz) != 0) {
        if (fat_find_slice(data, size, MACHO_CPU_X86_64, &off, &slice_sz) != 0) {
            return -1; // No valid slice found
        }
    }
    
    if (off == 0 || off + slice_sz > size) {
        return -1;
    }
    
    // Recursively parse the selected slice
    return macho_parse_header(bin, data + off, slice_sz);
}

// ============================================================================
// Header Parsing
// ============================================================================

int macho_parse_header(macho_binary_t *bin, const uint8_t *data, size_t size) {
    if (size < sizeof(macho_header64_t)) {
        fprintf(stderr, "macho: Mach-O too small\n");
        return -1;
    }
    
    uint32_t magic = *(const uint32_t *)data;
    int is64 = 0;
    int swap = 0;
    
    if (magic == MACHO_MAGIC_64_LE) {
        is64 = 1; swap = 0;
    } else if (magic == MACHO_MAGIC_64) {
        is64 = 1; swap = 1;
    } else if (magic == MACHO_MAGIC_32_LE) {
        is64 = 0; swap = 0;
    } else if (magic == MACHO_MAGIC_32) {
        is64 = 0; swap = 1;
    } else {
        fprintf(stderr, "macho: unrecognized Mach-O magic 0x%08X\n", magic);
        return -1;
    }
    
    #define LE32(v) (swap ? bswap32(v) : (v))
    #define LE64(v) (swap ? bswap64(v) : (v))
    
    const macho_header64_t *hdr = (const macho_header64_t *)data;
    uint32_t cputype = LE32(hdr->cputype);
    uint32_t filetype = LE32(hdr->filetype);
    uint32_t ncmds = LE32(hdr->ncmds);
    uint32_t sizeofcmds = LE32(hdr->sizeofcmds);
    uint32_t flags = LE32(hdr->flags);
    
    // Set architecture
    switch (cputype) {
        case MACHO_CPU_ARM64:
            bin->arch = MACHO_ARCH_ARM64;
            break;
        case MACHO_CPU_X86_64:
            bin->arch = MACHO_ARCH_X86_64;
            break;
        default:
            bin->arch = MACHO_ARCH_UNKNOWN;
            break;
    }
    
    bin->filetype = filetype;
    bin->cputype = cputype;
    bin->flags = flags;
    bin->is_pie = (filetype == MACHO_MH_DYLIB) ? 1 : 0;
    
    // Parse load commands
    if (macho_parse_load_commands(bin, data, size) != 0) {
        return -1;
    }
    
    // Parse sections
    if (macho_parse_sections(bin, data, size) != 0) {
        return -1;
    }
    
    // Parse symbols
    if (macho_parse_symbols(bin, data, size) != 0) {
        // Non-fatal: symbols might be stripped
        bin->is_stripped = 1;
    }
    
    #undef LE32
    #undef LE64
    
    return 0;
}

int macho_parse(macho_binary_t *bin, const uint8_t *data, size_t size) {
    if (!bin || !data || size == 0) return -1;
    
    // Check for fat binary
    uint32_t magic = *(const uint32_t *)data;
    
    if (magic == MACHO_FAT_MAGIC || magic == MACHO_FAT_MAGIC_LE) {
        return macho_parse_fat(bin, data, size);
    }
    
    // Parse as regular Mach-O
    return macho_parse_header(bin, data, size);
}

// ============================================================================
// Load Command Parsing
// ============================================================================

int macho_parse_load_commands(macho_binary_t *bin, const uint8_t *data, size_t size) {
    if (size < sizeof(macho_header64_t)) return -1;
    
    uint32_t magic = *(const uint32_t *)data;
    int is64 = (magic == MACHO_MAGIC_64 || magic == MACHO_MAGIC_64_LE) ? 1 : 0;
    int swap = (magic == MACHO_MAGIC_64 || magic == MACHO_MAGIC_32) ? 1 : 0;
    
    #define LE32(v) (swap ? bswap32(v) : (v))
    #define LE64(v) (swap ? bswap64(v) : (v))
    
    const macho_header64_t *hdr = (const macho_header64_t *)data;
    uint32_t ncmds = LE32(hdr->ncmds);
    uint32_t sizeofcmds = LE32(hdr->sizeofcmds);
    
    size_t hdr_size = is64 ? sizeof(macho_header64_t) : sizeof(macho_header32_t);
    size_t cmd_off = hdr_size;
    
    // Symbol table info for indirect symbol resolution
    uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    uint32_t indirectoff = 0, nindirect = 0;
    
    for (uint32_t ci = 0; ci < ncmds; ci++) {
        if (cmd_off + sizeof(macho_load_cmd_t) > size) break;
        
        const macho_load_cmd_t *lc = (const macho_load_cmd_t *)(data + cmd_off);
        uint32_t cmd = LE32(lc->cmd);
        uint32_t cmdsize = LE32(lc->cmdsize);
        
        if (cmdsize < 8 || cmd_off + cmdsize > size) break;
        
        switch (cmd) {
            case MACHO_LC_SEGMENT_64: {
                if (!is64) break;
                const macho_segment64_t *seg = (const macho_segment64_t *)(data + cmd_off);
                uint64_t vmaddr = LE64(seg->vmaddr);
                uint64_t vmsize = LE64(seg->vmsize);
                uint32_t ns = LE32(seg->nsects);
                
                if (strncmp(seg->segname, "__TEXT", 6) == 0) {
                    bin->base = vmaddr;
                    bin->image_size = vmaddr + vmsize;
                }
                
                // Entry point detection
                if (strncmp(seg->segname, "__TEXT", 6) == 0) {
                    size_t sec_off = cmd_off + sizeof(macho_segment64_t);
                    for (uint32_t si = 0; si < ns; si++) {
                        if (sec_off + sizeof(macho_section64_t) > size) break;
                        const macho_section64_t *sec = (const macho_section64_t *)(data + sec_off);
                        
                        if (strncmp(sec->sectname, "__text", 6) == 0 ||
                            strncmp(sec->sectname, "__TEXT", 6) == 0) {
                            // Try to find entry point
                            // For now, use the start of __text section
                            if (bin->entry == 0) {
                                bin->entry = LE64(sec->addr);
                            }
                        }
                        sec_off += sizeof(macho_section64_t);
                    }
                }
                break;
            }
            
            case MACHO_LC_SEGMENT: {
                if (is64) break;
                const macho_segment32_t *seg = (const macho_segment32_t *)(data + cmd_off);
                uint32_t vmaddr = LE32(seg->vmaddr);
                uint32_t vmsize = LE32(seg->vmsize);
                
                if (strncmp(seg->segname, "__TEXT", 6) == 0) {
                    bin->base = vmaddr;
                    bin->image_size = vmaddr + vmsize;
                }
                break;
            }
            
            case MACHO_LC_SYMTAB: {
                const macho_symtab_cmd_t *symtab = (const macho_symtab_cmd_t *)(data + cmd_off);
                symoff = LE32(symtab->symoff);
                nsyms = LE32(symtab->nsyms);
                stroff = LE32(symtab->stroff);
                strsize = LE32(symtab->strsize);
                break;
            }
            
            case MACHO_LC_DYSYMTAB: {
                const macho_dysymtab_cmd_t *dysymtab = (const macho_dysymtab_cmd_t *)(data + cmd_off);
                indirectoff = LE32(dysymtab->indirectsymoff);
                nindirect = LE32(dysymtab->nindirectsyms);
                break;
            }
            
            case MACHO_LC_MAIN: {
                // Entry point from LC_MAIN
                if (is64) {
                    const uint64_t *entry_offset = (const uint64_t *)(data + cmd_off + 16);
                    bin->entry = LE64(*entry_offset);
                } else {
                    const uint32_t *entry_offset = (const uint32_t *)(data + cmd_off + 8);
                    bin->entry = LE32(*entry_offset);
                }
                break;
            }
            
            case MACHO_LC_UNIXTHREAD: {
                // Entry point from thread state
                if (is64) {
                    // For x86_64: rip is at offset 16 in the thread command
                    // For ARM64: pc is at offset 32
                    if (bin->arch == MACHO_ARCH_X86_64 && cmdsize >= 16 + 8) {
                        const uint64_t *rip = (const uint64_t *)(data + cmd_off + 16);
                        bin->entry = LE64(*rip);
                    } else if (bin->arch == MACHO_ARCH_ARM64 && cmdsize >= 32 + 8) {
                        const uint64_t *pc = (const uint64_t *)(data + cmd_off + 32);
                        bin->entry = LE64(*pc);
                    }
                }
                break;
            }
            
            default:
                break;
        }
        
        cmd_off += cmdsize;
    }
    
    #undef LE32
    #undef LE64
    
    return 0;
}

// ============================================================================
// Section Parsing
// ============================================================================

int macho_parse_sections(macho_binary_t *bin, const uint8_t *data, size_t size) {
    if (size < sizeof(macho_header64_t)) return -1;
    
    uint32_t magic = *(const uint32_t *)data;
    int is64 = (magic == MACHO_MAGIC_64 || magic == MACHO_MAGIC_64_LE) ? 1 : 0;
    int swap = (magic == MACHO_MAGIC_64 || magic == MACHO_MAGIC_32) ? 1 : 0;
    
    #define LE32(v) (swap ? bswap32(v) : (v))
    #define LE64(v) (swap ? bswap64(v) : (v))
    
    const macho_header64_t *hdr = (const macho_header64_t *)data;
    uint32_t ncmds = LE32(hdr->ncmds);
    
    size_t hdr_size = is64 ? sizeof(macho_header64_t) : sizeof(macho_header32_t);
    size_t cmd_off = hdr_size;
    
    for (uint32_t ci = 0; ci < ncmds; ci++) {
        if (cmd_off + sizeof(macho_load_cmd_t) > size) break;
        
        const macho_load_cmd_t *lc = (const macho_load_cmd_t *)(data + cmd_off);
        uint32_t cmd = LE32(lc->cmd);
        uint32_t cmdsize = LE32(lc->cmdsize);
        
        if (cmdsize < 8 || cmd_off + cmdsize > size) break;
        
        if (cmd == MACHO_LC_SEGMENT_64 && is64) {
            const macho_segment64_t *seg = (const macho_segment64_t *)(data + cmd_off);
            uint32_t ns = LE32(seg->nsects);
            uint32_t ip = LE32(seg->initprot);
            
            size_t sec_off = cmd_off + sizeof(macho_segment64_t);
            
            for (uint32_t si = 0; si < ns && bin->nsections < MAX_MACHO_SECTIONS; si++) {
                if (sec_off + sizeof(macho_section64_t) > size) break;
                
                const macho_section64_t *sec = (const macho_section64_t *)(data + sec_off);
                
                macho_section_t *s = &bin->sections[bin->nsections++];
                
                // Copy section name
                strncpy(s->segname, seg->segname, 16);
                s->segname[16] = '\0';
                strncpy(s->name, sec->sectname, 16);
                s->name[16] = '\0';
                
                // If section name starts with __, remove the prefix
                if (s->name[0] == '_' && s->name[1] == '_') {
                    memmove(s->name, s->name + 2, strlen(s->name) - 1);
                }
                
                s->offset = (uint64_t)LE32(sec->offset);
                s->vaddr = LE64(sec->addr);
                s->size = LE64(sec->size);
                s->flags = LE32(sec->flags);
                s->file_offset = s->offset;
                s->insn_count = 0;
                
                // Determine section type
                uint32_t stype = s->flags & 0xFF;
                s->executable = (ip & 4) != 0;
                s->writable = (ip & 2) != 0;
                s->readable = (ip & 1) != 0;
                
                if (stype == MACHO_S_REGULAR || stype == MACHO_S_ZEROFILL) {
                    if (s->executable) {
                        s->type = MACHO_SEC_TYPE_CODE;
                    } else if (s->writable) {
                        s->type = MACHO_SEC_TYPE_DATA;
                    } else {
                        s->type = MACHO_SEC_TYPE_RODATA;
                    }
                } else if (stype == MACHO_S_CSTRING_LITERALS) {
                    s->type = MACHO_SEC_TYPE_RODATA;
                } else if (stype == MACHO_S_4BYTE_LITERALS ||
                           stype == MACHO_S_8BYTE_LITERALS ||
                           stype == MACHO_S_LITERAL_POINTERS) {
                    s->type = MACHO_SEC_TYPE_RODATA;
                } else if (stype == MACHO_S_NON_LAZY_SYMBOL_POINTERS ||
                           stype == MACHO_S_LAZY_SYMBOL_POINTERS) {
                    s->type = MACHO_SEC_TYPE_GOT;
                } else if (stype == MACHO_S_SYMBOL_STUBS) {
                    s->type = MACHO_SEC_TYPE_PLT;
                } else {
                    s->type = MACHO_SEC_TYPE_OTHER;
                }
                
                sec_off += sizeof(macho_section64_t);
            }
        } else if (cmd == MACHO_LC_SEGMENT && !is64) {
            const macho_segment32_t *seg = (const macho_segment32_t *)(data + cmd_off);
            uint32_t ns = LE32(seg->nsects);
            uint32_t ip = LE32(seg->initprot);
            
            size_t sec_off = cmd_off + sizeof(macho_segment32_t);
            
            for (uint32_t si = 0; si < ns && bin->nsections < MAX_MACHO_SECTIONS; si++) {
                if (sec_off + sizeof(macho_section32_t) > size) break;
                
                const macho_section32_t *sec = (const macho_section32_t *)(data + sec_off);
                
                macho_section_t *s = &bin->sections[bin->nsections++];
                
                strncpy(s->segname, seg->segname, 16);
                s->segname[16] = '\0';
                strncpy(s->name, sec->sectname, 16);
                s->name[16] = '\0';
                
                if (s->name[0] == '_' && s->name[1] == '_') {
                    memmove(s->name, s->name + 2, strlen(s->name) - 1);
                }
                
                s->offset = (uint64_t)LE32(sec->offset);
                s->vaddr = LE32(sec->addr);
                s->size = LE32(sec->size);
                s->flags = LE32(sec->flags);
                s->file_offset = s->offset;
                s->insn_count = 0;
                
                uint32_t stype = s->flags & 0xFF;
                s->executable = (ip & 4) != 0;
                s->writable = (ip & 2) != 0;
                s->readable = (ip & 1) != 0;
                
                if (stype == MACHO_S_REGULAR || stype == MACHO_S_ZEROFILL) {
                    if (s->executable) {
                        s->type = MACHO_SEC_TYPE_CODE;
                    } else if (s->writable) {
                        s->type = MACHO_SEC_TYPE_DATA;
                    } else {
                        s->type = MACHO_SEC_TYPE_RODATA;
                    }
                } else {
                    s->type = MACHO_SEC_TYPE_OTHER;
                }
                
                sec_off += sizeof(macho_section32_t);
            }
        }
        
        cmd_off += cmdsize;
    }
    
    #undef LE32
    #undef LE64
    
    return 0;
}

// ============================================================================
// Symbol Parsing
// ============================================================================

static int is_mangled(const char *name) {
    if (!name || name[0] == '\0') return 0;
    
    // C++ mangled names typically start with _Z
    if (name[0] == '_' && (name[1] == 'Z' || name[1] == '_')) {
        return 1;
    }
    
    return 0;
}

static void demangle_name(char *demangled, const char *name, size_t demangled_size) {
    if (!name || !demangled || demangled_size == 0) {
        if (demangled && demangled_size > 0) demangled[0] = '\0';
        return;
    }
    
    // Simple demangling for common patterns
    // For full C++ demangling, we'd need a proper demangler
    
    if (strncmp(name, "_Z", 2) == 0) {
        // Itanium ABI mangled name
        // _ZN3Foo3BarEv -> Foo::Bar()
        // _Z7myfuncv -> myfunc()
        const char *p = name + 2;
        int len = 0;
        
        if (*p == 'N') {
            // Nested name: _ZN<name>E
            p++; // skip 'N'
            while (*p && *p != 'E') {
                if (len < (int)demangled_size - 1) {
                    demangled[len++] = *p;
                }
                p++;
            }
            demangled[len] = '\0';
        } else if (*p >= '0' && *p <= '9') {
            // Simple function name with length prefix
            int num_len = 0;
            while (*p >= '0' && *p <= '9') {
                num_len = num_len * 10 + (*p - '0');
                p++;
            }
            if (num_len > 0) {
                strncpy(demangled, p, min(num_len, (int)demangled_size - 1));
                demangled[min(num_len, (int)demangled_size - 1)] = '\0';
            }
        } else {
            strncpy(demangled, name, demangled_size - 1);
            demangled[demangled_size - 1] = '\0';
        }
    } else {
        strncpy(demangled, name, demangled_size - 1);
        demangled[demangled_size - 1] = '\0';
    }
}

int macho_parse_symbols(macho_binary_t *bin, const uint8_t *data, size_t size) {
    if (size < sizeof(macho_header64_t)) return -1;
    
    uint32_t magic = *(const uint32_t *)data;
    int is64 = (magic == MACHO_MAGIC_64 || magic == MACHO_MAGIC_64_LE) ? 1 : 0;
    int swap = (magic == MACHO_MAGIC_64 || magic == MACHO_MAGIC_32) ? 1 : 0;
    
    #define LE32(v) (swap ? bswap32(v) : (v))
    #define LE64(v) (swap ? bswap64(v) : (v))
    
    const macho_header64_t *hdr = (const macho_header64_t *)data;
    uint32_t ncmds = LE32(hdr->ncmds);
    
    size_t hdr_size = is64 ? sizeof(macho_header64_t) : sizeof(macho_header32_t);
    size_t cmd_off = hdr_size;
    
    uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    
    // Find symbol table
    for (uint32_t ci = 0; ci < ncmds; ci++) {
        if (cmd_off + sizeof(macho_load_cmd_t) > size) break;
        
        const macho_load_cmd_t *lc = (const macho_load_cmd_t *)(data + cmd_off);
        uint32_t cmd = LE32(lc->cmd);
        uint32_t cmdsize = LE32(lc->cmdsize);
        
        if (cmdsize < 8 || cmd_off + cmdsize > size) break;
        
        if (cmd == MACHO_LC_SYMTAB) {
            const macho_symtab_cmd_t *symtab = (const macho_symtab_cmd_t *)(data + cmd_off);
            symoff = LE32(symtab->symoff);
            nsyms = LE32(symtab->nsyms);
            stroff = LE32(symtab->stroff);
            strsize = LE32(symtab->strsize);
            break;
        }
        
        cmd_off += cmdsize;
    }
    
    if (symoff == 0 || nsyms == 0 || stroff >= size) {
        return -1; // No symbol table
    }
    
    // Allocate symbol array
    if (nsyms > MAX_MACHO_SYMBOLS) {
        nsyms = MAX_MACHO_SYMBOLS;
    }
    
    bin->symbols = (macho_symbol_t *)calloc(nsyms, sizeof(macho_symbol_t));
    if (!bin->symbols) return -1;
    bin->symbols_allocated = nsyms;
    
    const uint8_t *nlist_data = data + symoff;
    const uint8_t *str_data = data + stroff;
    
    for (uint32_t i = 0; i < nsyms; i++) {
        const macho_nlist64_t *nlist = (const macho_nlist64_t *)(nlist_data + i * sizeof(macho_nlist64_t));
        
        uint32_t str_idx = LE32(nlist->n_strx);
        uint8_t n_type = nlist->n_type;
        uint8_t n_sect = nlist->n_sect;
        int16_t n_desc = nlist->n_desc;
        
        if (str_idx >= strsize) continue;
        
        const char *name = (const char *)str_data + str_idx;
        
        macho_symbol_t *sym = &bin->symbols[bin->nsymbols++];
        
        sym->address = is64 ? LE64(nlist->n_value) : LE32(nlist->n_value);
        strncpy(sym->name, name, sizeof(sym->name) - 1);
        sym->name[sizeof(sym->name) - 1] = '\0';
        
        // Demangle if needed
        if (is_mangled(name)) {
            demangle_name(sym->demangled, name, sizeof(sym->demangled));
        } else {
            strncpy(sym->demangled, name, sizeof(sym->demangled) - 1);
            sym->demangled[sizeof(sym->demangled) - 1] = '\0';
        }
        
        // Determine symbol type
        if (n_type & MACHO_N_STAB) {
            // Debug symbol, skip for now
            bin->nsymbols--;
            continue;
        }
        
        switch (n_type & MACHO_N_TYPE) {
            case MACHO_N_UNDF:
                sym->type = MACHO_SYMBOL_TYPE_UNKNOWN;
                if ((n_type & MACHO_N_EXT) && (n_desc & 0x10)) {
                    sym->type = MACHO_SYMBOL_TYPE_WEAK;
                }
                sym->is_undefined = 1;
                break;
            case MACHO_N_ABS:
                sym->type = MACHO_SYMBOL_TYPE_OBJECT;
                break;
            case MACHO_N_SECT:
                if (n_sect > 0 && n_sect <= bin->nsections) {
                    // Check if this is in a code section
                    macho_section_t *sec = &bin->sections[n_sect - 1];
                    if (sec->type == MACHO_SEC_TYPE_CODE) {
                        sym->type = MACHO_SYMBOL_TYPE_FUNC;
                    } else {
                        sym->type = MACHO_SYMBOL_TYPE_OBJECT;
                    }
                } else {
                    sym->type = MACHO_SYMBOL_TYPE_OBJECT;
                }
                break;
            case MACHO_N_INDR:
                sym->type = MACHO_SYMBOL_TYPE_IMPORT;
                sym->is_undefined = 1;
                break;
            default:
                sym->type = MACHO_SYMBOL_TYPE_UNKNOWN;
                break;
        }
        
        sym->is_global = (n_type & MACHO_N_EXT) != 0;
        sym->is_weak = (n_desc & 0x10) != 0;
        sym->is_entry = 0;
        
        // Check if this is the entry point
        if (sym->address == bin->entry && sym->type == MACHO_SYMBOL_TYPE_FUNC) {
            sym->is_entry = 1;
        }
    }
    
    #undef LE32
    #undef LE64
    
    return 0;
}

// ============================================================================
// Basic Analysis Functions
// ============================================================================

int macho_analyze(macho_binary_t *bin, macho_opts_t *opts) {
    if (!bin || !macho_is_valid(bin)) return -1;
    
    // Detect functions
    if (macho_detect_functions(bin) != 0) {
        fprintf(stderr, "macho: Failed to detect functions\n");
        return -1;
    }
    
    // Build control flow graph
    if (macho_build_cfg(bin) != 0) {
        fprintf(stderr, "macho: Failed to build CFG\n");
        return -1;
    }
    
    // Build call graph
    if (macho_build_callgraph(bin) != 0) {
        fprintf(stderr, "macho: Failed to build call graph\n");
        return -1;
    }
    
    // Detect loops
    if (macho_detect_loops(bin) != 0) {
        fprintf(stderr, "macho: Failed to detect loops\n");
        // Non-fatal
    }
    
    // Detect switch tables
    if (macho_detect_switch_tables(bin) != 0) {
        fprintf(stderr, "macho: Failed to detect switch tables\n");
        // Non-fatal
    }
    
    // Data dependency analysis
    if (opts && opts->depflow) {
        if (macho_analyze_dependencies(bin, opts) != 0) {
            fprintf(stderr, "macho: Failed to analyze dependencies\n");
            // Non-fatal
        }
    }
    
    // Execution order analysis
    if (opts && opts->execution_order) {
        if (macho_analyze_execution_order(bin) != 0) {
            fprintf(stderr, "macho: Failed to analyze execution order\n");
            // Non-fatal
        }
    }
    
    // Parallel execution detection
    if (opts && opts->parallel_detection) {
        if (macho_detect_parallel_execution(bin) != 0) {
            fprintf(stderr, "macho: Failed to detect parallel execution\n");
            // Non-fatal
        }
    }
    
    // Beam search pattern analysis
    if (opts && opts->beam_search_analysis) {
        if (macho_analyze_beam_search_patterns(bin) != 0) {
            fprintf(stderr, "macho: Failed to analyze beam search patterns\n");
            // Non-fatal
        }
    }
    
    bin->is_analyzed = 1;
    return 0;
}

// ============================================================================
// Function Detection (Stub)
// ============================================================================

int macho_detect_functions(macho_binary_t *bin) {
    // This is a placeholder - actual implementation would use
    // symbol information and heuristic analysis
    
    if (bin->nsymbols > 0) {
        // Allocate function array
        bin->functions_allocated = bin->nsymbols;
        bin->functions = (macho_function_t *)calloc(bin->nsymbols, sizeof(macho_function_t));
        if (!bin->functions) return -1;
        
        for (int i = 0; i < bin->nsymbols; i++) {
            macho_symbol_t *sym = &bin->symbols[i];
            
            if (sym->type == MACHO_SYMBOL_TYPE_FUNC) {
                macho_function_t *func = &bin->functions[bin->nfunctions++];
                
                func->start = sym->address;
                func->end = sym->address + sym->size; // Size might not be accurate
                strncpy(func->name, sym->demangled[0] ? sym->demangled : sym->name, sizeof(func->name) - 1);
                func->name[sizeof(func->name) - 1] = '\0';
                func->sym_idx = i;
                func->insn_count = 0;
                func->block_count = 0;
                func->has_loops = 0;
                func->has_calls = 0;
                func->has_branches = 0;
                func->has_indirect_calls = 0;
            }
        }
    }
    
    return 0;
}

// ============================================================================
// CFG Building (Stub)
// ============================================================================

int macho_build_cfg(macho_binary_t *bin) {
    // Placeholder - actual implementation would disassemble
    // and build control flow graph
    return 0;
}

// ============================================================================
// Call Graph Building (Stub)
// ============================================================================

int macho_build_callgraph(macho_binary_t *bin) {
    // Placeholder - actual implementation would analyze cross-references
    return 0;
}

// ============================================================================
// Loop Detection (Stub)
// ============================================================================

int macho_detect_loops(macho_binary_t *bin) {
    // Placeholder
    return 0;
}

// ============================================================================
// Switch Table Detection (Stub)
// ============================================================================

int macho_detect_switch_tables(macho_binary_t *bin) {
    // Placeholder
    return 0;
}

// ============================================================================
// Data Dependency Analysis (Stub)
// ============================================================================

int macho_analyze_dependencies(macho_binary_t *bin, macho_opts_t *opts) {
    // Placeholder
    return 0;
}

// ============================================================================
// Execution Order Analysis (Stub)
// ============================================================================

int macho_analyze_execution_order(macho_binary_t *bin) {
    // Placeholder
    return 0;
}

// ============================================================================
// Parallel Execution Detection (Stub)
// ============================================================================

int macho_detect_parallel_execution(macho_binary_t *bin) {
    // Placeholder
    return 0;
}

// ============================================================================
// Beam Search Pattern Analysis (Stub)
// ============================================================================

int macho_analyze_beam_search_patterns(macho_binary_t *bin) {
    // Placeholder
    return 0;
}

// ============================================================================
// State Tracking (Stub)
// ============================================================================

int macho_track_state_changes(macho_binary_t *bin) {
    // Placeholder
    return 0;
}

// ============================================================================
// Regression Analysis (Stub)
// ============================================================================

int macho_analyze_regression(macho_binary_t *bin) {
    // Placeholder
    return 0;
}

// ============================================================================
// State Checkpoint Building (Stub)
// ============================================================================

int macho_build_state_checkpoints(macho_binary_t *bin) {
    // Placeholder
    return 0;
}
