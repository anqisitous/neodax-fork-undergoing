// nx_restore_track.cpp — 復元コードを静的に逆trackする C++ クラス設計デコーダー
//
// 目的:
//   「どのバイナリの、どの関数が、状態を復元しているか」を、アプリを起動せず、
//   動的観測も使わず、バイナリだけで逆方向に追跡する。
//
// 追跡の向き:
//   復元に使われる外部シンボル (例: RestoreWorkspace / 設定読み出し) を起点に、
//   GOT → 参照命令 → 呼び出しサイト → 所属関数 と逆向きに辿る。
//   これにより「復元を起こしている主体」と「その直前の条件分岐」が特定できる。
//
// クラス設計:
//   MachOImage       ファイル/スライス/セグメント/VM→ファイル対応
//   ImportResolver   外部シンボル束縛 (chained fixups / dyld bind)
//   Arm64            命令デコード (追跡に必要な命令のみ厳密に)
//   XRefIndex        GOT参照・スタブ・直接BLから呼び出し関係を構築
//   BackTracker      起点シンボルから逆向きに呼び出し元/所属関数を解決
//   RestoreLocator   対象バイナリ群を走査して復元主体を列挙
//
// ビルド:
//   clang++ -std=c++17 -O2 nx_restore_track.cpp -o nx_restore_track
//
// 使い方:
//   nx_restore_track <path> [--filter SUBSTR]... [--symbols-only]
//
// 設計方針:
//   外部依存なし。Mach-O のロードコマンドは自前で歩く。
//   アプリ固有の名前はハードコードしない (--filter で与える)。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <memory>
#include <fstream>

#include <dirent.h>
#include <sys/stat.h>

#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/fat.h>
#include <mach-o/fixup-chains.h>

#include "track/learn.hpp"

extern "C" {
#include "../core/macho_static.h"
#include "../analysis/depflow.h"
#include "../analysis/variable_classifier.h"
}

namespace nx {

// ============================================================ MachOImage

struct Segment {
    std::string name;
    uint64_t vmaddr = 0, vmsize = 0, fileoff = 0, filesize = 0;
    uint32_t prot = 0;
    bool executable() const { return (prot & VM_PROT_EXECUTE) != 0; }
};

struct Section {
    std::string seg;
    std::string name;
    uint64_t addr = 0, size = 0;
    uint32_t flags = 0;
};

class MachOImage {
public:
    // ファイル全体を読み、arm64 スライスを選ぶ。失敗時 nullptr。
    static std::unique_ptr<MachOImage> open(const std::string &path);

    const std::string &path() const { return path_; }
    const std::string &arch() const { return arch_; }
    const std::vector<Segment> &segments() const { return segments_; }
    const std::vector<uint8_t> &data() const { return data_; }
    uint64_t entry() const { return entry_; }
    uint64_t slice_base() const { return slice_base_; }
    bool is_fat() const { return fat_; }

    const Segment *segment_at(uint64_t vm) const;
    const std::vector<Section> &sections() const { return sections_; }
    // スタブ本体 (__stubs / __objc_stubs) は「参照サイト」ではないので除外する
    bool is_stub_addr(uint64_t vm) const;
    const uint8_t *at(uint64_t vm, size_t need = 1) const;
    std::string string_at(uint64_t vm, size_t max = 256) const;
    bool vm_to_off(uint64_t vm, uint64_t *off) const;

    const symtab_command *symtab() const { return symtab_; }
    const dyld_info_command *dyld_info() const { return dyld_info_; }
    const linkedit_data_command *chained_fixups() const { return chained_; }
    const linkedit_data_command *function_starts() const { return fnstarts_; }

    // 自前シンボル (nlist) を名前→アドレスで返す
    const std::map<std::string, uint64_t> &defined_symbols() const { return defined_; }
    // アドレス → シンボル名 (内部関数への BL を解決するため)
    const std::string *name_at(uint64_t addr) const {
        auto it = name_by_addr_.find(addr);
        return it == name_by_addr_.end() ? nullptr : &it->second;
    }

    // リンクしている外部ライブラリ (LC_LOAD_DYLIB 等) と rpath。
    // otool -L 相当。依存の連鎖を辿るための入口になる。
    const std::vector<std::string> &dylibs() const { return dylibs_; }
    const std::vector<std::string> &rpaths() const { return rpaths_; }
    const std::string &dylib_id() const { return id_; }

private:
    std::string path_;
    std::vector<uint8_t> data_;
    std::vector<Segment> segments_;
    std::vector<Section> sections_;
    std::map<std::string, uint64_t> defined_;
    std::map<uint64_t, std::string> name_by_addr_;
    std::string arch_;
    uint64_t entry_ = 0;
    uint64_t slice_base_ = 0;
    bool fat_ = false;
    std::vector<std::string> dylibs_;
    std::vector<std::string> rpaths_;
    std::string id_;

    const symtab_command *symtab_ = nullptr;
    const dyld_info_command *dyld_info_ = nullptr;
    const linkedit_data_command *chained_ = nullptr;
    const linkedit_data_command *fnstarts_ = nullptr;

    bool parse(uint64_t base, const mach_header_64 *mh);
};

std::unique_ptr<MachOImage> MachOImage::open(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return nullptr; }
    auto img = std::unique_ptr<MachOImage>(new MachOImage());
    img->path_ = path;
    img->data_.resize((size_t)n);
    size_t got = fread(img->data_.data(), 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) return nullptr;

    const uint8_t *p = img->data_.data();
    if (img->data_.size() < 8) return nullptr;
    uint32_t be = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                  ((uint32_t)p[2] << 8) | (uint32_t)p[3];

    if (be == FAT_MAGIC || be == FAT_MAGIC_64) {
        img->fat_ = true;
        bool is64 = (be == FAT_MAGIC_64);
        uint32_t nfat = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
                        ((uint32_t)p[6] << 8) | (uint32_t)p[7];
        uint64_t chosen = 0;
        bool picked = false;
        for (uint32_t i = 0; i < nfat; i++) {
            size_t eo = 8 + (size_t)i * (is64 ? 32 : 20);
            if (eo + (is64 ? 32 : 20) > img->data_.size()) break;
            uint32_t cpu = ((uint32_t)p[eo] << 24) | ((uint32_t)p[eo+1] << 16) |
                           ((uint32_t)p[eo+2] << 8) | (uint32_t)p[eo+3];
            uint64_t off;
            if (is64) {
                off = 0;
                for (int k = 0; k < 8; k++) off = (off << 8) | p[eo + 8 + k];
            } else {
                off = ((uint32_t)p[eo+8] << 24) | ((uint32_t)p[eo+9] << 16) |
                      ((uint32_t)p[eo+10] << 8) | (uint32_t)p[eo+11];
            }
            if (cpu == CPU_TYPE_ARM64) { chosen = off; picked = true; break; }
            if (i == nfat - 1 && !picked) chosen = off;
        }
        img->slice_base_ = chosen;
    }

    const mach_header_64 *mh = (const mach_header_64 *)(p + img->slice_base_);
    if (img->slice_base_ + sizeof(mach_header_64) > img->data_.size()) return nullptr;
    if (mh->magic != MH_MAGIC_64) return nullptr;
    if (!img->parse(img->slice_base_, mh)) return nullptr;
    return img;
}

bool MachOImage::parse(uint64_t base, const mach_header_64 *mh) {
    arch_ = (mh->cputype == CPU_TYPE_ARM64) ? "arm64" : "other";

    size_t off = (size_t)base + sizeof(mach_header_64);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (off + sizeof(load_command) > data_.size()) break;
        const load_command *lc = (const load_command *)(data_.data() + off);
        if (lc->cmdsize < sizeof(load_command) || off + lc->cmdsize > data_.size()) break;
        switch (lc->cmd) {
            case LC_SEGMENT_64: {
                const segment_command_64 *sg = (const segment_command_64 *)lc;
                Segment s;
                s.name = std::string(sg->segname, strnlen(sg->segname, 16));
                s.vmaddr = sg->vmaddr;
                s.vmsize = sg->vmsize;
                // fileoff はスライス先頭基準。fat では base を足して絶対値にする。
                s.fileoff = base + sg->fileoff;
                s.filesize = sg->filesize;
                s.prot = sg->initprot;
                segments_.push_back(s);

                const section_64 *sec =
                    (const section_64 *)((const uint8_t *)sg + sizeof(segment_command_64));
                for (uint32_t k = 0; k < sg->nsects; k++, sec++) {
                    Section sc;
                    sc.seg = std::string(sec->segname, strnlen(sec->segname, 16));
                    sc.name = std::string(sec->sectname, strnlen(sec->sectname, 16));
                    sc.addr = sec->addr;
                    sc.size = sec->size;
                    sc.flags = sec->flags;
                    sections_.push_back(sc);
                }
                break;
            }
            case LC_SYMTAB:            symtab_ = (const symtab_command *)lc; break;
            case LC_DYLD_INFO:
            case LC_DYLD_INFO_ONLY:    dyld_info_ = (const dyld_info_command *)lc; break;
            case LC_DYLD_CHAINED_FIXUPS: chained_ = (const linkedit_data_command *)lc; break;
            case LC_FUNCTION_STARTS:   fnstarts_ = (const linkedit_data_command *)lc; break;
            case LC_MAIN: {
                const entry_point_command *e = (const entry_point_command *)lc;
                for (const auto &s : segments_)
                    if (s.name == "__TEXT") { entry_ = s.vmaddr + e->entryoff; break; }
                break;
            }
            case LC_LOAD_DYLIB:
            case LC_LOAD_WEAK_DYLIB:
            case LC_REEXPORT_DYLIB:
            case LC_LOAD_UPWARD_DYLIB:
            case LC_LAZY_LOAD_DYLIB: {
                const dylib_command *dc = (const dylib_command *)lc;
                if (dc->dylib.name.offset < lc->cmdsize) {
                    const char *nm = (const char *)lc + dc->dylib.name.offset;
                    dylibs_.push_back(std::string(nm, strnlen(nm, lc->cmdsize - dc->dylib.name.offset)));
                }
                break;
            }
            case LC_ID_DYLIB: {
                const dylib_command *dc = (const dylib_command *)lc;
                if (dc->dylib.name.offset < lc->cmdsize) {
                    const char *nm = (const char *)lc + dc->dylib.name.offset;
                    id_ = std::string(nm, strnlen(nm, lc->cmdsize - dc->dylib.name.offset));
                }
                break;
            }
            case LC_RPATH: {
                const rpath_command *rc = (const rpath_command *)lc;
                if (rc->path.offset < lc->cmdsize) {
                    const char *pth = (const char *)lc + rc->path.offset;
                    rpaths_.push_back(std::string(pth, strnlen(pth, lc->cmdsize - rc->path.offset)));
                }
                break;
            }
            default: break;
        }
        off += lc->cmdsize;
    }

    if (symtab_) {
        // symoff/stroff はスライス先頭基準。fat では base を足す。
        uint64_t sym_off = base + symtab_->symoff;
        uint64_t str_off = base + symtab_->stroff;
        if (sym_off >= data_.size() || str_off >= data_.size()) return true;
        const struct nlist_64 *nl = (const struct nlist_64 *)(data_.data() + sym_off);
        const char *strs = (const char *)(data_.data() + str_off);
        for (uint32_t i = 0; i < symtab_->nsyms; i++) {
            uint64_t need = sym_off + (uint64_t)(i + 1) * sizeof(struct nlist_64);
            if (need > data_.size()) break;
            uint32_t so = nl[i].n_un.n_strx;
            if (so >= symtab_->strsize) continue;
            if (str_off + so >= data_.size()) continue;
            std::string nm = strs + so;
            if (nm.empty() || (nl[i].n_type & N_STAB)) continue;
            if ((nl[i].n_type & N_TYPE) == N_SECT) {
                defined_[nm] = nl[i].n_value;
                if (!nm.empty() && nm[0] == '_')
                    name_by_addr_.emplace(nl[i].n_value, nm);
            }
        }
    }
    return true;
}

const Segment *MachOImage::segment_at(uint64_t vm) const {
    for (const auto &s : segments_)
        if (vm >= s.vmaddr && vm < s.vmaddr + s.vmsize) return &s;
    return nullptr;
}

bool MachOImage::is_stub_addr(uint64_t vm) const {
    for (const auto &sc : sections_) {
        if (sc.name != "__stubs" && sc.name != "__objc_stubs") continue;
        if (vm >= sc.addr && vm < sc.addr + sc.size) return true;
    }
    return false;
}

const uint8_t *MachOImage::at(uint64_t vm, size_t need) const {
    const Segment *s = segment_at(vm);
    if (!s || !s->filesize) return nullptr;
    uint64_t o = s->fileoff + (vm - s->vmaddr);
    if (o + need > data_.size()) return nullptr;
    return data_.data() + o;
}

bool MachOImage::vm_to_off(uint64_t vm, uint64_t *off) const {
    const Segment *s = segment_at(vm);
    if (!s) return false;
    uint64_t o = s->fileoff + (vm - s->vmaddr);
    if (o >= data_.size()) return false;
    *off = o;
    return true;
}

std::string MachOImage::string_at(uint64_t vm, size_t max) const {
    const Segment *s = segment_at(vm);
    if (!s) return {};
    const uint8_t *p = at(vm);
    if (!p) return {};
    uint64_t end_off = s->fileoff + s->filesize;
    uint64_t o = s->fileoff + (vm - s->vmaddr);
    size_t lim = (size_t)std::min<uint64_t>(end_off - o, max);
    std::string out;
    for (size_t i = 0; i < lim && p[i]; i++) out.push_back((char)p[i]);
    return out;
}

// ============================================================ ImportResolver

struct Binding {
    std::string symbol;
    uint64_t    slot = 0;   // GOT/ポインタ格納位置
};

class ImportResolver {
public:
    void parse(const MachOImage &img);
    const std::string *symbol_at_slot(uint64_t slot) const {
        auto it = slot_to_symbol_.find(slot);
        return it == slot_to_symbol_.end() ? nullptr : &it->second;
    }
    const std::vector<Binding> &bindings() const { return bindings_; }

private:
    void parse_chained(const MachOImage &img);
    void parse_dyld_info(const MachOImage &img);

    std::vector<Binding> bindings_;
    std::map<uint64_t, std::string> slot_to_symbol_;
};

void ImportResolver::parse(const MachOImage &img) {
    if (const auto *c = img.chained_fixups()) parse_chained(img);
    else if (const auto *d = img.dyld_info()) parse_dyld_info(img);

    // 同じスロットに複数シンボルが付く事故を防ぐ (最初の1件を正とする)
    bindings_.clear();
    for (const auto &kv : slot_to_symbol_)
        bindings_.push_back({kv.second, kv.first});
    std::sort(bindings_.begin(), bindings_.end(),
              [](const Binding &a, const Binding &b) { return a.slot < b.slot; });
}

void ImportResolver::parse_chained(const MachOImage &img) {
    const auto *cmd = img.chained_fixups();
    if (!cmd || !cmd->datasize) return;
    uint64_t off = img.slice_base() + cmd->dataoff;
    if (off + cmd->datasize > img.data().size() || cmd->datasize < 32) return;

    const uint8_t *base = img.data().data() + off;
    const auto *hdr = (const dyld_chained_fixups_header *)base;
    if (hdr->fixups_version != 0) return;
    if (getenv("NX_DEBUG"))
        fprintf(stderr, "[chained] version=%u imports_count=%u imports_format=%u starts=%u\n",
                hdr->fixups_version, hdr->imports_count, hdr->imports_format,
                hdr->starts_offset);

    const auto *starts = (const dyld_chained_starts_in_image *)(base + hdr->starts_offset);
    const uint8_t *imp = base + hdr->imports_offset;
    const uint8_t *symbase = base + hdr->symbols_offset;

    auto sym_name = [&](uint32_t idx) -> std::string {
        if (idx >= hdr->imports_count) return {};
        switch (hdr->imports_format) {
            case DYLD_CHAINED_IMPORT: {
                const auto *t = (const dyld_chained_import *)imp;
                return std::string((const char *)(symbase + t[idx].name_offset));
            }
            case DYLD_CHAINED_IMPORT_ADDEND: {
                const auto *t = (const dyld_chained_import_addend *)imp;
                return std::string((const char *)(symbase + t[idx].name_offset));
            }
            case DYLD_CHAINED_IMPORT_ADDEND64: {
                const auto *t = (const dyld_chained_import_addend64 *)imp;
                return std::string((const char *)(symbase + t[idx].name_offset));
            }
            default: return {};
        }
    };

    const auto &segs = img.segments();
    for (uint32_t si = 0; si < starts->seg_count && si < segs.size(); si++) {
        uint16_t so = starts->seg_info_offset[si];
        if (so == 0) continue;
        const auto *seg = (const dyld_chained_starts_in_segment *)((const uint8_t *)starts + so);
        uint64_t seg_vm = segs[si].vmaddr;
        if (getenv("NX_DEBUG"))
            fprintf(stderr, "[chained]  si=%u name=%-14s vm=0x%llx pages=%u psize=%u fmt=%u\n",
                    si, segs[si].name.c_str(), (unsigned long long)seg_vm,
                    seg->page_count, seg->page_size, seg->pointer_format);

        for (uint32_t pi = 0; pi < seg->page_count; pi++) {
            uint16_t sp = seg->page_start[pi];
            if (sp == DYLD_CHAINED_PTR_START_NONE) continue;

            // チェーンを 1 本歩く。start_off はページ先頭からのバイト位置。
            auto walk = [&](uint16_t start_off) {
                uint64_t chain_vm = seg_vm + (uint64_t)pi * seg->page_size + start_off;
                for (int guard = 0; guard < 200000; guard++) {
                    uint64_t fo = 0;
                    if (!img.vm_to_off(chain_vm, &fo) || fo + 8 > img.data().size()) break;
                    uint64_t raw = *(const uint64_t *)(img.data().data() + fo);

                    uint64_t next = 0;
                    bool is_bind = false;
                    uint32_t ordinal = 0;

                    // ポインタ形式ごとに bind ビット・ordinal・next の位置が違う。
                    switch (seg->pointer_format) {
                        case DYLD_CHAINED_PTR_64:
                        case DYLD_CHAINED_PTR_64_OFFSET: {
                            dyld_chained_ptr_64_bind b;
                            memcpy(&b, &raw, 8);
                            if (b.bind) { is_bind = true; ordinal = b.ordinal; next = b.next; }
                            break;
                        }
                        case DYLD_CHAINED_PTR_ARM64E:
                        case DYLD_CHAINED_PTR_ARM64E_USERLAND: {
                            dyld_chained_ptr_arm64e_bind b;
                            memcpy(&b, &raw, 8);
                            if (b.bind) { is_bind = true; ordinal = b.ordinal; next = b.next; }
                            break;
                        }
                        case DYLD_CHAINED_PTR_ARM64E_USERLAND24: {
                            dyld_chained_ptr_arm64e_bind24 b;
                            memcpy(&b, &raw, 8);
                            if (b.bind) { is_bind = true; ordinal = b.ordinal; next = b.next; }
                            break;
                        }
                        default:
                            break;
                    }

                    if (is_bind) {
                        // 観測に合わせ 0 基準で引く (ord=0 が最初の import)
                        std::string nm = sym_name(ordinal);
                        if (!nm.empty() && slot_to_symbol_.find(chain_vm) == slot_to_symbol_.end())
                            slot_to_symbol_[chain_vm] = nm;
                    }
                    if (!next) break;
                    chain_vm += next * 4;
                }
            };

            if (sp & DYLD_CHAINED_PTR_START_MULTI) {
                // 1 ページに複数のチェーンがある。page_start[] の続きを索引として使う。
                uint32_t idx = sp & 0x7FFF;
                for (uint32_t guard = 0; guard < 4096; guard++) {
                    uint16_t v = seg->page_start[idx];
                    walk(v & 0x7FFF);
                    if (v & DYLD_CHAINED_PTR_START_LAST) break;
                    idx++;
                }
            } else {
                walk(sp);
            }
        }
    }
}

void ImportResolver::parse_dyld_info(const MachOImage &img) {
    const auto *di = img.dyld_info();
    if (!di || !di->bind_size) return;
    const uint8_t *p = img.data().data() + img.slice_base() + di->bind_off;
    const uint8_t *end = p + di->bind_size;
    uint64_t seg_index = 0, seg_offset = 0;
    std::string sym;
    int type = 0;
    const auto &segs = img.segments();

    while (p < end) {
        uint8_t op = *p & 0xB0, imm = *p & 0x0F;
        p++;
        switch (op) {
            case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                sym.clear();
                while (p < end && *p) sym.push_back((char)*p++);
                p++;
                break;
            case BIND_OPCODE_SET_TYPE_IMM: type = imm; break;
            case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: {
                seg_index = imm;
                uint64_t v = 0; int sh = 0;
                while (p < end) { uint8_t x = *p++; v |= (uint64_t)(x & 0x7F) << sh; sh += 7; if (!(x & 0x80)) break; }
                seg_offset = v;
                break;
            }
            case BIND_OPCODE_ADD_ADDR_ULEB: {
                uint64_t v = 0; int sh = 0;
                while (p < end) { uint8_t x = *p++; v |= (uint64_t)(x & 0x7F) << sh; sh += 7; if (!(x & 0x80)) break; }
                seg_offset += v;
                break;
            }
            case BIND_OPCODE_DO_BIND:
            case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
            case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
            case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: {
                if (seg_index < segs.size() && !sym.empty()) {
                    uint64_t slot = segs[seg_index].vmaddr + seg_offset;
                    if (slot_to_symbol_.find(slot) == slot_to_symbol_.end())
                        slot_to_symbol_[slot] = sym;
                }
                if (op == BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB) {
                    uint64_t v = 0; int sh = 0;
                    while (p < end) { uint8_t x = *p++; v |= (uint64_t)(x & 0x7F) << sh; sh += 7; if (!(x & 0x80)) break; }
                    seg_offset += v + 8;
                } else if (op == BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED) {
                    seg_offset += (uint64_t)imm * 8 + 8;
                } else if (op == BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB) {
                    uint64_t cnt = 0, skip = 0; int sh = 0;
                    while (p < end) { uint8_t x = *p++; cnt |= (uint64_t)(x & 0x7F) << sh; sh += 7; if (!(x & 0x80)) break; }
                    sh = 0;
                    while (p < end) { uint8_t x = *p++; skip |= (uint64_t)(x & 0x7F) << sh; sh += 7; if (!(x & 0x80)) break; }
                    seg_offset += cnt * (skip + 8);
                } else {
                    seg_offset += 8;
                }
                break;
            }
            default: break;
        }
    }
    (void)type;
}

// ============================================================ Arm64

class Arm64 {
public:
    enum class Op {
        Invalid, Nop, Ret, Br, Blr, B, Bl, BCond, Cbz, Cbnz, Tbz, Tbnz,
        Adrp, Add, Sub, Ldr, Str, Mov, Movz, Movk, Cmp, BlrReg, Unknown
    };
    struct Insn {
        uint64_t addr = 0;
        uint32_t raw = 0;
        Op op = Op::Invalid;
        int rd = -1, rn = -1, rt = -1;
        uint64_t target = 0;      // B/BL/ADRP の解決先
        bool target_known = false;
        uint64_t imm = 0;         // ADD/LDR の即値
        int cond = -1;
    };

    static bool decode(uint64_t pc, uint32_t w, Insn *out);
    static const char *op_name(Op op);
};

const char *Arm64::op_name(Op op) {
    switch (op) {
        case Op::Nop: return "nop";
        case Op::Ret: return "ret";
        case Op::Br:  return "br";
        case Op::Blr: return "blr";
        case Op::B:   return "b";
        case Op::Bl:  return "bl";
        case Op::BCond: return "b.cond";
        case Op::Cbz: return "cbz";
        case Op::Cbnz: return "cbnz";
        case Op::Tbz: return "tbz";
        case Op::Tbnz: return "tbnz";
        case Op::Adrp: return "adrp";
        case Op::Add: return "add";
        case Op::Sub: return "sub";
        case Op::Ldr: return "ldr";
        case Op::Str: return "str";
        case Op::Mov: return "mov";
        case Op::Movz: return "movz";
        case Op::Movk: return "movk";
        case Op::Cmp: return "cmp";
        default: return "?";
    }
}

bool Arm64::decode(uint64_t pc, uint32_t w, Insn *out) {
    *out = Insn();
    out->addr = pc;
    out->raw = w;

    if (w == 0xD503201Fu) { out->op = Op::Nop; return true; }
    if ((w & 0xFFFFFC1Fu) == 0xD65F0000u) { out->op = Op::Ret; out->rn = (w >> 5) & 31; return true; }
    if ((w & 0xFFFFFC1Fu) == 0xD61F0000u) { out->op = Op::Br; 
        out->rn = (w >> 5) & 31; return true; }
    if ((w & 0xFFFFFC1Fu) == 0xD63F0000u) { out->op = Op::Blr; out->rn = (w >> 5) & 31; return true; }

    // BL / B
    if ((w & 0xFC000000u) == 0x94000000u || (w & 0xFC000000u) == 0x14000000u) {
        int64_t imm = (int64_t)(w & 0x03FFFFFFu);
        if (imm & 0x02000000) imm |= ~0x03FFFFFFLL;
        out->op = ((w & 0xFC000000u) == 0x94000000u) ? Op::Bl : Op::B;
        out->target = pc + (uint64_t)(imm * 4);
        out->target_known = true;
        return true;
    }
    // B.cond
    if ((w & 0xFF000000u) == 0x54000000u) {
        int64_t imm = (int64_t)((w >> 5) & 0x7FFFF);
        if (imm & 0x40000) imm |= ~0x7FFFFLL;
        out->op = Op::BCond;
        out->cond = (int)(w & 0xF);
        out->target = pc + (uint64_t)(imm * 4);
        out->target_known = true;
        return true;
    }
    // CBZ / CBNZ
    if ((w & 0x7E000000u) == 0x34000000u) {
        int64_t imm = (int64_t)((w >> 5) & 0x7FFFF);
        if (imm & 0x40000) imm |= ~0x7FFFFLL;
        out->op = ((w >> 24) & 1) ? Op::Cbnz : Op::Cbz;
        out->rt = (int)(w & 31);
        out->target = pc + (uint64_t)(imm * 4);
        out->target_known = true;
        return true;
    }
    // TBZ / TBNZ
    if ((w & 0x7E000000u) == 0x36000000u) {
        int64_t imm = (int64_t)((w >> 5) & 0x3FFF);
        if (imm & 0x2000) imm |= ~0x3FFFLL;
        out->op = ((w >> 24) & 1) ? Op::Tbnz : Op::Tbz;
        out->rt = (int)(w & 31);
        out->target = pc + (uint64_t)(imm * 4);
        out->target_known = true;
        return true;
    }
    // ADRP
    if ((w & 0x9F000000u) == 0x90000000u) {
        int64_t immhi = (int64_t)((w >> 5) & 0x7FFFF);
        int64_t immlo = (int64_t)((w >> 29) & 3);
        int64_t imm = (immhi << 2) | immlo;
        if (imm & (1LL << 20)) imm |= ~((1LL << 21) - 1);
        out->op = Op::Adrp;
        out->rd = (int)(w & 31);
        out->target = (pc & ~0xFFFULL) + (uint64_t)(imm << 12);
        out->target_known = true;
        return true;
    }
    // ADD/SUB (immediate, 64bit)
    if ((w & 0x7F000000u) == 0x11000000u || (w & 0x7F000000u) == 0x51000000u) {
        out->op = ((w >> 30) & 1) ? Op::Sub : Op::Add;
        out->rd = (int)(w & 31);
        out->rn = (int)((w >> 5) & 31);
        out->imm = (uint64_t)((w >> 10) & 0xFFF);
        if ((w >> 22) & 1) out->imm <<= 12;
        return true;
    }
    // ADD/SUB shifted register (ADD xN, xM, xK)
    if ((w & 0x7F200000u) == 0x0B000000u) {
        out->op = Op::Add;
        out->rd = (int)(w & 31);
        out->rn = (int)((w >> 5) & 31);
        out->rt = (int)((w >> 16) & 31);
        return true;
    }
    // LDR/STR (unsigned offset, 64bit)
    if ((w & 0xFFC00000u) == 0xF9400000u || (w & 0xFFC00000u) == 0xF9000000u) {
        out->op = ((w & 0xFFC00000u) == 0xF9400000u) ? Op::Ldr : Op::Str;
        out->rt = (int)(w & 31);
        out->rn = (int)((w >> 5) & 31);
        out->imm = (uint64_t)((w >> 10) & 0xFFF) * 8;
        return true;
    }
    // MOVZ / MOVK
    if ((w & 0x7F800000u) == 0x52800000u || (w & 0x7F800000u) == 0x72800000u) {
        out->op = ((w >> 29) & 1) ? Op::Movk : Op::Movz;
        out->rd = (int)(w & 31);
        out->imm = (uint64_t)((w >> 5) & 0xFFFF);
        return true;
    }
    // ORR (register) = MOV alias
    if ((w & 0xFFE0FFE0u) == 0xAA0003E0u) {
        out->op = Op::Mov;
        out->rd = (int)(w & 31);
        out->rn = (int)((w >> 16) & 31);
        return true;
    }
    // SUBS (immediate) = CMP alias
    if ((w & 0xFF800000u) == 0xF1000000u) {
        out->op = Op::Cmp;
        out->rn = (int)((w >> 5) & 31);
        out->imm = (uint64_t)((w >> 10) & 0xFFF);
        return true;
    }
    out->op = Op::Unknown;
    return false;
}

// ============================================================ XRefIndex

struct CallSite {
    uint64_t    addr = 0;        // 参照/呼び出し命令のアドレス
    std::string target_symbol;   // 解決できた外部シンボル (空なら code への直接参照)
    uint64_t    target_vm = 0;   // 参照先 VM (GOT か code)
    bool        is_call = false; // BL なら true、ADRP+LDR (参照) なら false
};

class XRefIndex {
public:
    void build(const MachOImage &img, const ImportResolver &imports);

    // シンボル部分一致で呼び出し/参照サイトを集める
    std::vector<CallSite> sites_for(const std::string &substr) const;
    const std::vector<CallSite> &sites() const { return sites_; }

private:
    std::vector<CallSite> sites_;
    std::map<uint64_t, uint64_t> stub_to_slot_;
};

void XRefIndex::build(const MachOImage &img, const ImportResolver &imports) {
    const Segment *text = nullptr;
    for (const auto &s : img.segments())
        if (s.name == "__TEXT" && s.executable()) { text = &s; break; }
    if (!text) return;

    const uint8_t *tp = img.at(text->vmaddr, (size_t)text->filesize);
    if (!tp) {
        uint64_t fo = 0;
        if (img.vm_to_off(text->vmaddr, &fo)) tp = img.data().data() + fo;
    }
    if (!tp) return;
    const uint64_t tvm = text->vmaddr;
    const size_t tsz = (size_t)text->filesize;

    // --- 1) スタブ表: __stubs セクションを直接走査し、各スタブの GOT スロットを解く ---
    //   形式はツールチェインによって違う:
    //     (a) ldr x16, #lit          … リテラルロード 1 命令
    //     (b) adrp x16, page         … 2 命令
    //         ldr  x16, [x16, #off]
    //   両方を解かないと、一部のシンボルだけ「呼び出しサイト 0 件」になる。
    for (const auto &sc : img.sections()) {
        if (sc.name != "__stubs" && sc.name != "__objc_stubs") continue;
        const uint8_t *sp = img.at(sc.addr, (size_t)sc.size);
        if (!sp) continue;
        uint64_t prev_adrp_pc = 0, prev_page = 0;
        int prev_rd = -1;
        for (uint64_t o = 0; o + 4 <= sc.size; o += 4) {
            const uint64_t pc = sc.addr + o;
            const uint32_t w = *(const uint32_t *)(sp + o);

            // (a) ldr x16, #lit  (literal load)
            if ((w & 0xFF00001Fu) == 0x58000010u) {
                int64_t imm = (int64_t)((w >> 5) & 0x7FFFF) * 4;
                stub_to_slot_[pc] = pc + (uint64_t)imm;
                prev_rd = -1;
                continue;
            }

            Arm64::Insn in;
            Arm64::decode(pc, w, &in);
            if (in.op == Arm64::Op::Adrp && in.rd == 16) {
                prev_adrp_pc = pc; prev_page = in.target; prev_rd = 16;
                continue;
            }
            if (in.op == Arm64::Op::Ldr && in.rn == 16 && prev_rd == 16 &&
                pc == prev_adrp_pc + 4) {
                stub_to_slot_[prev_adrp_pc] = prev_page + in.imm;
                prev_rd = -1;
                continue;
            }
            prev_rd = -1;
        }
    }

    // --- 2) __text 走査: ADRP + LDR/ADD と BL ---
    uint64_t prev_adrp = 0;
    uint64_t prev_page = 0;
    int prev_rd = -1;

    for (size_t i = 0; i + 4 <= tsz; i += 4) {
        uint64_t pc = tvm + i;
        uint32_t w = *(const uint32_t *)(tp + i);
        Arm64::Insn in;
        bool ok = Arm64::decode(pc, w, &in);

        // スタブ本体は「参照サイト」ではない。スタブは上の表で解決済み。
        if (img.is_stub_addr(pc)) { prev_rd = -1; continue; }

        if (ok && in.op == Arm64::Op::Adrp) {
            prev_adrp = pc; prev_page = in.target; prev_rd = in.rd;
            continue;
        }

        // ADRP の直後 (数命令以内) の LDR/ADD を対応付ける
        if (prev_rd >= 0) {
            bool use = false;
            if (ok && in.op == Arm64::Op::Ldr && in.rn == prev_rd) {
                uint64_t slot = prev_page + in.imm;
                auto *sym = imports.symbol_at_slot(slot);
                if (sym)
                    sites_.push_back({prev_adrp, *sym, slot, false});
                use = true;
            } else if (ok && in.op == Arm64::Op::Add && in.rn == prev_rd) {
                uint64_t va = prev_page + in.imm;
                sites_.push_back({prev_adrp, std::string(), va, false});
                use = true;
            }
            (void)use;
            prev_rd = -1;
        }

        // BL: スタブ経由 / 直接の両方
        if (ok && in.op == Arm64::Op::Bl && in.target_known) {
            auto it = stub_to_slot_.find(in.target);
            if (it != stub_to_slot_.end()) {
                auto *sym = imports.symbol_at_slot(it->second);
                if (sym) sites_.push_back({pc, *sym, in.target, true});
            } else {
                // 同一バイナリ内の直接 BL は defined シンボルで解決する
                const std::string *nm2 = img.name_at(in.target);
                if (nm2) sites_.push_back({pc, *nm2, in.target, true});
            }
        }
    }

    std::sort(sites_.begin(), sites_.end(),
              [](const CallSite &a, const CallSite &b) { return a.addr < b.addr; });
}

std::vector<CallSite> XRefIndex::sites_for(const std::string &substr) const {
    std::vector<CallSite> out;
    for (const auto &s : sites_)
        if (!s.target_symbol.empty() &&
            s.target_symbol.find(substr) != std::string::npos)
            out.push_back(s);
    return out;
}

// ============================================================ BackTracker

class BackTracker {
public:
    BackTracker(const MachOImage &img, const XRefIndex &xr) : img_(img), xr_(xr) { build_functions(); }

    // 指定アドレスを含む関数の先頭を返す (無ければ addr 自身)
    uint64_t function_containing(uint64_t addr) const;
    std::string function_name(uint64_t addr) const;

    // 起点シンボルへ逆向きに辿り、呼び出し元関数と近傍文字列を出す
    struct Finding {
        std::string symbol;
        uint64_t    site = 0;
        bool        is_call = false;
        uint64_t    func = 0;
        std::string func_name;
        std::vector<std::string> nearby_strings;
    };
    std::vector<Finding> track(const std::string &substr, size_t max_strings = 3) const;

    // 関数が参照する文字列を広く集める (ファイルパス抽出用)。上限付き。
    std::vector<std::string> strings_in_function(uint64_t func_addr,
                                                 size_t max = 24) const;

    // ファイル読み出し系 API かどうか
    static bool is_file_api(const std::string &s);
    // パスらしい文字列かどうか
    static bool looks_like_path(const std::string &s);

    // 関数先頭の一覧 (昇順)。関数の範囲を決めるのに使う。
    const std::vector<uint64_t> &func_starts() const { return funcs_; }

private:
    void build_functions();
    std::vector<std::string> scan_string_refs(uint64_t around, size_t max) const;

    const MachOImage &img_;
    const XRefIndex &xr_;
    std::vector<uint64_t> funcs_;                        // 昇順
    std::map<uint64_t, std::string> addr_to_name_;
};

void BackTracker::build_functions() {
    // LC_FUNCTION_STARTS (ULEB128 差分) を読む。無ければ nlist から作る。
    if (const auto *fs = img_.function_starts()) {
        const uint8_t *p = img_.data().data() + img_.slice_base() + fs->dataoff;
        const uint8_t *end = p + fs->datasize;
        uint64_t addr = 0;
        while (p < end) {
            uint64_t delta = 0; int sh = 0;
            bool done = false;
            while (p < end) {
                uint8_t b = *p++;
                delta |= (uint64_t)(b & 0x7F) << sh;
                sh += 7;
                if (!(b & 0x80)) { done = true; break; }
            }
            if (!done || delta == 0) break;
            addr += delta;
            funcs_.push_back(addr);
        }
    }
    for (const auto &kv : img_.defined_symbols()) {
        if (kv.second) {
            funcs_.push_back(kv.second);
            addr_to_name_[kv.second] = kv.first;
        }
    }
    std::sort(funcs_.begin(), funcs_.end());
    funcs_.erase(std::unique(funcs_.begin(), funcs_.end()), funcs_.end());
}

uint64_t BackTracker::function_containing(uint64_t addr) const {
    if (funcs_.empty()) return addr;
    auto it = std::upper_bound(funcs_.begin(), funcs_.end(), addr);
    if (it == funcs_.begin()) return addr;
    return *(--it);
}

std::string BackTracker::function_name(uint64_t addr) const {
    auto it = addr_to_name_.find(addr);
    if (it != addr_to_name_.end()) return it->second;
    return {};
}

// 参照サイトの近傍で ADRP+ADD が指す文字列を拾う (何と言って復元しているか)
std::vector<std::string> BackTracker::scan_string_refs(uint64_t around, size_t max) const {
    std::vector<std::string> out;
    auto printable = [](const std::string &s) {
        if (s.size() < 3) return false;
        int good = 0;
        for (unsigned char c : s) {
            if (c == '\t' || c == '\n' || c == '\r') { good++; continue; }
            if (c < 0x20 || c == 0x7F) return false;
            good++;
        }
        return good >= 3;
    };

    const Segment *text = nullptr;
    for (const auto &s : img_.segments())
        if (s.name == "__TEXT" && s.executable()) { text = &s; break; }
    if (!text) return out;

    uint64_t start = around > 256 ? around - 256 : 0;
    uint64_t prev_page = 0;
    int prev_rd = -1;
    for (uint64_t pc = start; pc <= around + 16 && out.size() < max; pc += 4) {
        const uint8_t *p = img_.at(pc, 4);
        if (!p) continue;
        uint32_t w = *(const uint32_t *)p;
        Arm64::Insn in;
        if (!Arm64::decode(pc, w, &in)) continue;
        if (in.op == Arm64::Op::Adrp) { prev_page = in.target; prev_rd = in.rd; continue; }
        if (prev_rd >= 0 && in.op == Arm64::Op::Add && in.rn == prev_rd) {
            std::string s = img_.string_at(prev_page + in.imm, 160);
            if (printable(s) && s.find_first_not_of(" \t\r\n") != std::string::npos)
                out.push_back(s);
            prev_rd = -1;
        }
    }
    return out;
}

// 関数が参照する文字列を広く集める。ADRP+ADD の組を関数範囲で走査する。
std::vector<std::string> BackTracker::strings_in_function(uint64_t f, size_t max) const {
    std::vector<std::string> out;
    const Segment *text = nullptr;
    for (const auto &s : img_.segments())
        if (s.name == "__TEXT" && s.executable()) { text = &s; break; }
    if (!text) return out;

    uint64_t end = f + 0x8000;
    for (size_t i = 0; i < funcs_.size(); i++)
        if (funcs_[i] > f) { end = funcs_[i]; break; }
    if (end > text->vmaddr + text->filesize) end = text->vmaddr + text->filesize;

    uint64_t prev_page = 0;
    int prev_rd = -1;
    for (uint64_t pc = f; pc + 4 <= end && out.size() < max; pc += 4) {
        const uint8_t *p = img_.at(pc, 4);
        if (!p) break;
        uint32_t w = *(const uint32_t *)p;
        Arm64::Insn in;
        if (!Arm64::decode(pc, w, &in)) continue;
        if (in.op == Arm64::Op::Adrp) { prev_page = in.target; prev_rd = in.rd; continue; }
        if (prev_rd >= 0 && in.op == Arm64::Op::Add && in.rn == prev_rd) {
            std::string s = img_.string_at(prev_page + in.imm, 200);
            if (s.size() >= 2 && s.find_first_not_of(" \t\r\n") != std::string::npos)
                out.push_back(s);
            prev_rd = -1;
        }
    }
    return out;
}

bool BackTracker::is_file_api(const std::string &s) {
    // 部分一致は危険。例えば "read" は "Thread" に一致してしまう。
    // C API のインポート名は先頭に '_' が付くので、それを外して完全一致で見る。
    static const char *kApi[] = {
        "open", "openat", "fopen", "freopen", "stat", "statfs", "lstat", "access",
        "read", "pread", "readlink", "opendir", "readdir", "scandir",
        "realpath", "glob", "mmap", "getxattr", "dlopen",
        "sqlite3_open", "sqlite3_open_v2", "sqlite3_prepare", "sqlite3_prepare_v2",
        "sqlite3_exec", "sqlite3_close",
        nullptr
    };
    const char *p = s.c_str();
    if (p[0] == '_') p++;
    for (int i = 0; kApi[i]; i++)
        if (std::strcmp(p, kApi[i]) == 0) return true;
    return false;
}

bool BackTracker::looks_like_path(const std::string &s) {
    if (s.size() < 3) return false;
    if (s[0] == '/' || s[0] == '~') return true;
    if (s.compare(0, 4, "$$$/") == 0) return true;
    if (s.find('/') != std::string::npos && s.find(' ') == std::string::npos) return true;
    static const char *ext[] = {".txt", ".json", ".plist", ".xml", ".db", ".sqlite",
                                 ".ini", ".cfg", ".conf", ".bin", ".log", ".dat",
                                 ".xmp", ".aex", ".plugin", ".framework", ".dylib", ".so",
                                 ".png", ".jpg", ".ttf", ".aegp", ".pbg", nullptr};
    for (int i = 0; ext[i]; i++) {
        if (s.size() > strlen(ext[i]) &&
            s.compare(s.size() - strlen(ext[i]), strlen(ext[i]), ext[i]) == 0) return true;
    }
    return false;
}

std::vector<BackTracker::Finding> BackTracker::track(const std::string &substr,
                                                     size_t max_strings) const {
    std::vector<Finding> out;
    for (const auto &s : xr_.sites_for(substr)) {
        Finding f;
        f.symbol = s.target_symbol;
        f.site = s.addr;
        f.is_call = s.is_call;
        f.func = function_containing(s.addr);
        f.func_name = function_name(f.func);
        f.nearby_strings = scan_string_refs(s.addr, max_strings);
        out.push_back(std::move(f));
    }
    return out;
}

// ============================================================ RingAnalyzer
//
// depflow_build_rings と同じ概念の環 (call-graph SCC) を、静的な呼び出し関係から作る。
//
// 狙い:
//   「待ち」と「動かないブロック」は同じ形をしている。
//     ・待ち   = 相手の完了を待つエッジ (WaitForProcess / semaphore_wait / join)
//     ・ブロック = 条件が成立するまで進めないエッジ (lock / gate / notify 待ち)
//   どちらも「ノードが別ノードに依存して止まる」エッジなので、
//   呼び出しグラフの SCC を取れば同一の機構で検出できる。
//
//   環の中で待ちエッジが閉じている場合、その環は
//   「待っても何もできない」状態になる。切るべきはその待ちエッジ1本。

class RingAnalyzer {
public:
    struct Node {
        uint64_t addr = 0;          // 内部関数の先頭。外部は 0
        std::string name;
        std::vector<uint64_t> callees;   // 内部: 関数先頭 / 外部: 0 (ext_label で区別)
        std::vector<std::string> callee_ext;
        bool is_wait = false;       // 相手の完了を待つ
        bool is_spawn = false;      // 別プロセス/別スレッドを起こす
        int scc = -1;
    };

    void build(const MachOImage &img, const XRefIndex &xr);
    void find_rings();                       // Tarjan SCC

    struct StallRing {
        std::vector<std::string> members;
        std::vector<std::string> wait_edges;  // 切る候補
        std::vector<std::string> spawn_edges;
    };
    std::vector<StallRing> stall_rings() const;
    const std::vector<Node> &nodes() const { return nodes_; }
    size_t ring_count() const { return rings_; }

    static bool is_wait_symbol(const std::string &s);
    static bool is_spawn_symbol(const std::string &s);

private:
    std::vector<Node> nodes_;
    std::map<uint64_t, size_t> addr_to_node_;
    std::map<std::string, size_t> ext_to_node_;
    size_t rings_ = 0;

    const MachOImage *img_ = nullptr;
    std::vector<uint64_t> func_starts_;
    uint64_t func_of(uint64_t addr) const;
};

bool RingAnalyzer::is_wait_symbol(const std::string &s) {
    // 注意: "Block"/"block" は「コードブロック」の意でも使われるため除外する。
    //        "lock" 小文字は "block" に部分一致するので使わない。
    static const char *pat[] = {
        "Wait", "wait", "Sleep", "sleep", "Join", "join", "Semaphore", "semaphore",
        "CondWait", "cond_wait", "pthread_cond", "pthread_mutex", "RunLoop", "RunModal",
        "DispatchGroup", "WaitForProcess", "waitpid", "Mutex", "Barrier", "Gate",
        "Acquire", "AcquireLock", "Lock", "Waiter",
        NULL
    };
    for (int i = 0; pat[i]; i++)
        if (s.find(pat[i]) != std::string::npos) return true;
    return false;
}

bool RingAnalyzer::is_spawn_symbol(const std::string &s) {
    static const char *pat[] = {
        "Spawn", "spawn", "Launch", "launch", "Relaunch", "relaunch",
        "CreateProcess", "NSTask", "Fork", "fork", "Exec", "exec",
        "OpenApplication", "LSOpen", "XPC", NULL
    };
    for (int i = 0; pat[i]; i++)
        if (s.find(pat[i]) != std::string::npos) return true;
    return false;
}

uint64_t RingAnalyzer::func_of(uint64_t addr) const {
    if (func_starts_.empty()) return addr;
    auto it = std::upper_bound(func_starts_.begin(), func_starts_.end(), addr);
    if (it == func_starts_.begin()) return addr;
    return *(--it);
}

void RingAnalyzer::build(const MachOImage &img, const XRefIndex &xr) {
    img_ = &img;

    // 関数一覧 (LC_FUNCTION_STARTS + defined シンボル)
    if (const auto *fs = img.function_starts()) {
        const uint8_t *p = img.data().data() + img.slice_base() + fs->dataoff;
        const uint8_t *end = p + fs->datasize;
        uint64_t a = 0;
        while (p < end) {
            uint64_t d = 0; int sh = 0; bool done = false;
            while (p < end) { uint8_t b = *p++; d |= (uint64_t)(b & 0x7F) << sh; sh += 7; if (!(b & 0x80)) { done = true; break; } }
            if (!done || d == 0) break;
            a += d;
            func_starts_.push_back(a);
        }
    }
    for (const auto &kv : img.defined_symbols())
        if (kv.second) func_starts_.push_back(kv.second);
    std::sort(func_starts_.begin(), func_starts_.end());
    func_starts_.erase(std::unique(func_starts_.begin(), func_starts_.end()), func_starts_.end());

    auto node_for = [&](uint64_t addr, const std::string &name) -> size_t {
        auto it = addr_to_node_.find(addr);
        if (it != addr_to_node_.end()) return it->second;
        Node n;
        n.addr = addr;
        n.name = name;
        n.is_wait = is_wait_symbol(name);
        n.is_spawn = is_spawn_symbol(name);
        nodes_.push_back(n);
        size_t idx = nodes_.size() - 1;
        addr_to_node_[addr] = idx;
        return idx;
    };
    auto ext_node = [&](const std::string &sym) -> size_t {
        auto it = ext_to_node_.find(sym);
        if (it != ext_to_node_.end()) return it->second;
        Node n;
        n.addr = 0;
        n.name = sym;
        n.is_wait = is_wait_symbol(sym);
        n.is_spawn = is_spawn_symbol(sym);
        nodes_.push_back(n);
        size_t idx = nodes_.size() - 1;
        ext_to_node_[sym] = idx;
        return idx;
    };

    for (const auto &s : xr.sites()) {
        if (!s.is_call || s.target_symbol.empty()) continue;
        uint64_t caller = func_of(s.addr);
        const std::string *cn = img.name_at(caller);
        size_t ci = node_for(caller, cn ? *cn : std::string());

        bool internal = false;
        const std::string *tn = s.target_vm ? img.name_at(s.target_vm) : nullptr;
        if (tn) internal = true;
        if (internal && tn) {
            size_t ti = node_for(s.target_vm, *tn);
            nodes_[ci].callees.push_back(nodes_[ti].addr);
        } else {
            size_t ti = ext_node(s.target_symbol);
            nodes_[ci].callee_ext.push_back(nodes_[ti].name);
        }
    }
}

void RingAnalyzer::find_rings() {
    // Tarjan SCC (反復実装: 深い呼び出しでも再帰を作らない)
    const size_t N = nodes_.size();
    std::vector<int> index(N, -1), low(N, 0), onstk(N, 0);
    std::vector<size_t> stk;
    int idx = 0;

    struct Frame { size_t v; size_t ei; };
    std::vector<Frame> call;

    std::map<uint64_t, size_t> addr_index;
    for (size_t i = 0; i < N; i++)
        if (nodes_[i].addr) addr_index[nodes_[i].addr] = i;

    for (size_t root = 0; root < N; root++) {
        if (index[root] != -1) continue;
        call.push_back({root, 0});
        index[root] = low[root] = idx++;
        stk.push_back(root); onstk[root] = 1;

        while (!call.empty()) {
            Frame &f = call.back();
            Node &v = nodes_[f.v];
            if (f.ei < v.callees.size()) {
                uint64_t ca = v.callees[f.ei++];
                auto it = addr_index.find(ca);
                if (it == addr_index.end()) continue;
                size_t w = it->second;
                if (index[w] == -1) {
                    index[w] = low[w] = idx++;
                    stk.push_back(w); onstk[w] = 1;
                    call.push_back({w, 0});
                } else if (onstk[w]) {
                    if (index[w] < low[f.v]) low[f.v] = index[w];
                }
            } else {
                size_t vv = f.v;
                call.pop_back();
                if (!call.empty()) {
                    size_t parent = call.back().v;
                    if (low[vv] < low[parent]) low[parent] = low[vv];
                }
                if (low[vv] == index[vv]) {
                    int comp = 0;
                    std::vector<size_t> members;
                    while (true) {
                        size_t w = stk.back(); stk.pop_back(); onstk[w] = 0;
                        members.push_back(w); comp++;
                        if (w == vv) break;
                    }
                    if (comp > 1) {
                        int id = (int)rings_++;
                        for (size_t w : members) nodes_[w].scc = id;
                    } else {
                        nodes_[vv].scc = -1;
                    }
                }
            }
        }
    }
}

std::vector<RingAnalyzer::StallRing> RingAnalyzer::stall_rings() const {
    std::map<int, StallRing> by_scc;
    for (const auto &n : nodes_) {
        if (n.scc < 0) continue;
        StallRing &r = by_scc[n.scc];
        if (!n.name.empty())
            r.members.push_back(n.name + (n.is_wait ? "   [WAIT]" : (n.is_spawn ? "   [SPAWN]" : "")));
        for (const auto &e : n.callee_ext) {
            if (is_wait_symbol(e)) r.wait_edges.push_back(n.name + "  --wait-->  " + e);
            if (is_spawn_symbol(e)) r.spawn_edges.push_back(n.name + "  --spawn--> " + e);
        }
    }
    std::vector<StallRing> out;
    for (auto &kv : by_scc) {
        // 待ちエッジを持つ環を先に、持たない環も含めて返す。
        // 持たない環は「健全な相互再帰」、持つ環が「待ちが閉じた環」の候補。
        out.push_back(kv.second);
    }
    std::sort(out.begin(), out.end(), [](const StallRing &a, const StallRing &b) {
        size_t aw = a.wait_edges.size() + a.spawn_edges.size();
        size_t bw = b.wait_edges.size() + b.spawn_edges.size();
        if (aw != bw) return aw > bw;
        return a.members.size() > b.members.size();
    });
    return out;
}

// ============================================================ EdgeClassifier
//
// 「分析を断ち切る」ための層。
//
// 問題:
//   環 (SCC) をそのまま再帰的に辿ると、進展の無い相互再帰で
//   「待っても何もできない」状態に落ちる。解析が停止しないか、
//   意味の無いノードで膨らむ。
//
// 方針:
//   環の中で辿る際に「新しく得られた事実」の有無でエッジを分類する。
//     Necessary    : 新しい到達先を得た。解析に必要な線
//     Obstructive  : 新しいものを何も生まずに環を閉じる線。邪魔する線
//     Irrelevant   : 到達先が既知だが環も閉じない。どうでもいい線
//   進展ゼロのエッジでは再帰しない (そこで分析を断ち切る)。
//
// 状態依存:
//   待ち/起動エッジは「状態によって動く・動かない」を決める線なので
//   state_dependent として印を付ける。分類は状態ではなく進展で決めるため、
//   状態の値そのものを知らなくても同じ機構で扱える。

class EdgeClassifier {
public:
    enum class Cls { Necessary, Obstructive, Irrelevant };
    struct EdgeInfo {
        std::string from, to;
        Cls cls = Cls::Irrelevant;
        bool gained = false;        // 新しい到達先を得たか
        bool wait = false, spawn = false;
        bool internal = false;
    };

    void run(const RingAnalyzer &ra, uint64_t entry);
    const std::vector<EdgeInfo> &edges() const { return edges_; }
    size_t cuts() const { return cuts_; }
    size_t reached() const { return reached_; }
    static const char *cls_name(Cls c);

private:
    std::vector<EdgeInfo> edges_;
    size_t cuts_ = 0;
    size_t reached_ = 0;
};

const char *EdgeClassifier::cls_name(Cls c) {
    switch (c) {
        case Cls::Necessary:   return "necessary";
        case Cls::Obstructive: return "obstructive";
        case Cls::Irrelevant:  return "irrelevant";
    }
    return "?";
}

void EdgeClassifier::run(const RingAnalyzer &ra, uint64_t entry) {
    const auto &ns = ra.nodes();
    if (ns.empty()) return;

    std::map<uint64_t, size_t> by_addr;
    std::map<std::string, size_t> by_name;
    for (size_t i = 0; i < ns.size(); i++) {
        if (ns[i].addr) by_addr[ns[i].addr] = i;
        else by_name[ns[i].name] = i;
    }

    size_t start = 0;
    auto sit = by_addr.find(entry);
    if (sit != by_addr.end()) start = sit->second;
    else {
        for (size_t i = 0; i < ns.size(); i++)
            if (ns[i].addr) { start = i; break; }
    }

    // DFS。現在の経路 (onpath) を保つことで
    //   祖先へ戻る線 = 環を閉じる線 (obstructive)
    //   既訪だが祖先でない線 = 交差する線 (irrelevant)
    // を区別する。進展が無いのでどちらも再帰しない (分析を断ち切る)。
    std::vector<char> visited(ns.size(), 0), onpath(ns.size(), 0);
    struct Frame { size_t u; size_t ei; };
    std::vector<Frame> st;
    st.push_back({start, 0});
    visited[start] = 1; onpath[start] = 1; reached_ = 1;

    while (!st.empty()) {
        size_t u = st.back().u;
        size_t k = st.back().ei++;
        const auto &nu = ns[u];
        size_t nint = nu.callees.size();
        size_t total = nint + nu.callee_ext.size();
        if (k >= total) {
            onpath[u] = 0;
            st.pop_back();
            continue;
        }

        if (k < nint) {
            auto it = by_addr.find(nu.callees[k]);
            if (it == by_addr.end()) continue;
            size_t v = it->second;
            EdgeInfo e;
            e.from = nu.name;
            e.to = ns[v].name;
            e.internal = true;
            e.wait = ns[v].is_wait;
            e.spawn = ns[v].is_spawn;
            if (!visited[v]) {
                e.gained = true;
                e.cls = Cls::Necessary;
                visited[v] = 1; onpath[v] = 1; reached_++;
                edges_.push_back(std::move(e));
                st.push_back({v, 0});
                continue;
            }
            if (onpath[v]) { e.cls = Cls::Obstructive; cuts_++; }   // 環を閉じる
            else { e.cls = Cls::Irrelevant; }                       // 交差
            edges_.push_back(std::move(e));
        } else {
            const std::string &sym = nu.callee_ext[k - nint];
            EdgeInfo e;
            e.from = nu.name;
            e.to = sym;
            e.internal = false;
            e.wait = RingAnalyzer::is_wait_symbol(sym);
            e.spawn = RingAnalyzer::is_spawn_symbol(sym);
            // 外部の待ち/起動は「状態で動く・動かない」を決めるので必要線として残す。
            e.cls = (e.wait || e.spawn) ? Cls::Necessary : Cls::Irrelevant;
            edges_.push_back(std::move(e));
        }
    }
}

// ============================================================ Importance
//
// 「重要な関数に到達して、意味のある結果になるか」を再現する。
//
// 時間や件数では切らない。代わりに呼び出しグラフを入口から辿り、
//   ・入口から届くか (到達性)
//   ・環 (相互再帰) の中にあるか
//   ・他から呼ばれ / 他を呼ぶ数が多いか
// で重要度を決め、解析結果が重要な関数に乗っているかを見る。

class Importance {
public:
    void build(const RingAnalyzer &ra, uint64_t entry);
    bool reaches(uint64_t f) const { return reachable_.count(f) != 0; }
    bool important(uint64_t f) const { return important_.count(f) != 0; }
    size_t reachable_count() const { return reachable_.size(); }
    size_t important_count() const { return important_.size(); }

private:
    std::set<uint64_t> reachable_;
    std::set<uint64_t> important_;
};

void Importance::build(const RingAnalyzer &ra, uint64_t entry) {
    const auto &ns = ra.nodes();
    std::map<uint64_t, size_t> by_addr;
    for (size_t i = 0; i < ns.size(); i++)
        if (ns[i].addr) by_addr[ns[i].addr] = i;

    // 入口から辿れる範囲を求める
    std::vector<size_t> q;
    auto start = by_addr.find(entry);
    if (start != by_addr.end()) { q.push_back(start->second); reachable_.insert(entry); }
    else {
        // 入口が無ければ、定義のある関数から始める
        for (size_t i = 0; i < ns.size() && q.empty(); i++)
            if (ns[i].addr) { q.push_back(i); reachable_.insert(ns[i].addr); }
    }
    for (size_t h = 0; h < q.size(); h++) {
        const auto &n = ns[q[h]];
        for (uint64_t ca : n.callees) {
            if (reachable_.count(ca)) continue;
            auto it = by_addr.find(ca);
            if (it == by_addr.end()) continue;
            reachable_.insert(ca);
            q.push_back(it->second);
        }
    }

    // 重要度: 届く & (環の中 か 呼び出し数が多い)
    for (const auto &kv : by_addr) {
        const auto &n = ns[kv.second];
        if (!reachable_.count(n.addr)) continue;
        size_t deg = n.callees.size() + n.callee_ext.size();
        if (n.scc >= 0 || deg >= 2) important_.insert(n.addr);
    }
}

// ============================================================ PathFinder
//
// 「重要な関数に乗ったか」ではなく、「入口から具体的な効果まで経路が実在するか」を再現する。
// 重要な関数に乗っていても、そこが行き止まりなら意味はない。
// 効果とは、ファイル読み書き・終了・待ち・状態変更のような具体的な終端である。

class PathFinder {
public:
    struct Found {
        std::vector<std::string> chain;   // 入口からの関数名の並び
        std::string effect;               // 終端の効果
    };

    void build(const RingAnalyzer &ra) {
        ns_ = &ra.nodes();
        for (size_t i = 0; i < ns_->size(); i++)
            if ((*ns_)[i].addr) by_addr_[(*ns_)[i].addr] = i;
    }

    // 入口から、効果を呼ぶ節点までの経路を 1 本見つける。
    bool find_effect(uint64_t entry, const std::vector<std::string> &pats,
                     Found *out) const;

private:
    const std::vector<RingAnalyzer::Node> *ns_ = nullptr;
    std::map<uint64_t, size_t> by_addr_;
};

bool PathFinder::find_effect(uint64_t entry, const std::vector<std::string> &pats,
                             Found *out) const {
    if (!ns_ || !out) return false;
    auto start = by_addr_.find(entry);
    if (start == by_addr_.end()) return false;

    std::vector<size_t> q{start->second};
    std::map<size_t, size_t> prev;               // 子 -> 親
    std::set<size_t> seen{start->second};
    size_t hit = (size_t)-1;
    std::string effect;

    for (size_t h = 0; h < q.size() && hit == (size_t)-1; h++) {
        const auto &n = (*ns_)[q[h]];
        // この節点が効果を呼んでいるか
        for (const auto &e : n.callee_ext) {
            for (const auto &p : pats) {
                if (e.find(p) != std::string::npos) { hit = q[h]; effect = e; break; }
            }
            if (hit != (size_t)-1) break;
        }
        if (hit != (size_t)-1) break;

        for (uint64_t ca : n.callees) {
            auto it = by_addr_.find(ca);
            if (it == by_addr_.end()) continue;
            if (!seen.insert(it->second).second) continue;
            prev[it->second] = q[h];
            q.push_back(it->second);
        }
    }
    if (hit == (size_t)-1) return false;

    // 経路を組み立てる
    std::vector<std::string> chain;
    for (size_t cur = hit;; cur = prev[cur]) {
        chain.push_back((*ns_)[cur].name.empty() ? "(unnamed)" : (*ns_)[cur].name);
        auto it = prev.find(cur);
        if (it == prev.end()) break;
    }
    std::reverse(chain.begin(), chain.end());
    out->chain = chain;
    out->effect = effect;
    return true;
}

// ============================================================ RestoreLocator

    struct Options {
        std::vector<std::string> filters;
        bool symbols_only = false;
        bool verbose = false;
        bool rings = false;      // 環 (SCC) 解析モード
        bool prune = false;      // 分析断ち切り (エッジ分類) モード
        bool files = false;      // ファイルアクセス解析モード
        bool widget = false;     // ウィジェット構成のシミュレート
        uint64_t entry = 0;      // 断ち切り分析の起点 (0 なら自動)
    };

class RestoreLocator {
public:
    explicit RestoreLocator(const Options &o) : opt_(o) {}

    int run(const std::string &path, FILE *out);

private:
    Options opt_;
};

namespace {

// ---- 分析の打ち切り ----------------------------------------------------
// 「いつまでも終わらない」を防ぐための共通の番人。
//   ・全体の時間予算 (deadline_ms)
//   ・件数上限 (ops_budget) / 入力サイズ上限 (max_bytes)
// どれかを超えたら、以降の重い処理をすべて即座に飛ばす。
class Deadline {
public:
    explicit Deadline(int ms, size_t ops)
        : t0_(std::chrono::steady_clock::now()),
          limit_ms_(ms), ops_limit_(ops) {}

    bool expired() const {
        if (ops_used_ >= ops_limit_) return true;
        if (limit_ms_ <= 0) return false;
        auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0_).count();
        return dt >= limit_ms_;
    }
    void charge(size_t n = 1) { ops_used_ += n; }

private:
    std::chrono::steady_clock::time_point t0_;
    int    limit_ms_;
    size_t ops_limit_;
    size_t ops_used_ = 0;
};

} // namespace

int RestoreLocator::run(const std::string &path, FILE *out) {
    auto img = MachOImage::open(path);
    if (!img) {
        fprintf(out, "[skip] not a 64-bit Mach-O: %s\n", path.c_str());
        return 1;
    }
    if (img->arch() != "arm64") {
        fprintf(out, "[skip] arch=%s: %s\n", img->arch().c_str(), path.c_str());
        return 1;
    }

    ImportResolver imports;
    imports.parse(*img);

    // 学んだことを「解析の仕方」として貯める。対象を特定する情報は入れない。
    // 終了時に自動保存するので、どの経路で抜けても学習は残る。
    nx::SkillStore store;
    store.load(nx::SkillStore::default_path());
    struct SaveOnExit {
        nx::SkillStore *s;
        ~SaveOnExit() { s->save(nx::SkillStore::default_path()); }
    } save_guard{&store};

    store.observe("arch", img->arch());
    store.observe("fat", img->is_fat() ? "fat" : "thin");
    store.observe("chained_fixups", img->chained_fixups() ? "あり" : "なし");
    store.observe("imports_bucket", std::to_string(imports.bindings().size() / 64));
    store.observe("defined_bucket", std::to_string(img->defined_symbols().size() / 256));
    store.observe("sections_bucket", std::to_string(img->sections().size()));

    // 部分結果の位置は構造だけから決める (パスも名前も使わない)。
    // 一本化: 済んだ手順は同じ脳に「癖」として残す。
    store.observe("shape", std::to_string(img->segments().size()) + " segments, " +
                             std::to_string(img->sections().size()) + " sections");

    XRefIndex xr;
    xr.build(*img, imports);

    store.observe("step", "xref_index まで到達");

    BackTracker bt(*img, xr);

    // 重要な関数に届くかを再現する (時間や件数では切らない)
    RingAnalyzer rings;
    rings.build(*img, xr);
    rings.find_rings();
    Importance imp;
    imp.build(rings, img->entry());
    store.observe("reachable_bucket", std::to_string(imp.reachable_count() / 64));
    store.observe("important_bucket", std::to_string(imp.important_count() / 64));

    // ファイルアクセス解析: このモジュールが「どのファイルを読むか」を洗い出す。
    // 見つからなければ何も出力せずに切り上げる (探索を長引かせない)。
    if (opt_.files) {
        std::map<uint64_t, std::set<std::string>> paths_by_func;
        std::set<std::string> apis;
        size_t nsite = 0;
        for (const auto &s : xr.sites()) {
            if (!s.is_call || s.target_symbol.empty()) continue;
            if (!BackTracker::is_file_api(s.target_symbol)) continue;
            nsite++;
            apis.insert(s.target_symbol);
            uint64_t f = bt.function_containing(s.addr);
            for (const auto &str : bt.strings_in_function(f, 32))
                if (BackTracker::looks_like_path(str))
                    paths_by_func[f].insert(str);
        }
        if (nsite == 0 || paths_by_func.empty()) {
            fprintf(out, "できませんでした\n");
            return 0;
        }

        fprintf(out, "=== %s ===\n", path.c_str());
        fprintf(out, "  このモジュールが読むファイルは、次のとおりです。\n");
        for (const auto &kv : paths_by_func) {
            const std::string *nm = img->name_at(kv.first);
            fprintf(out, "  --- %s (0x%llx) ---\n",
                    nm ? nm->c_str() : "(unnamed)", (unsigned long long)kv.first);
            for (const auto &p : kv.second) fprintf(out, "      %s\n", p.c_str());
        }
        fprintf(out, "  使われる file API は、");
        for (const auto &a : apis) fprintf(out, "%s ", a.c_str());
        fprintf(out, "です (呼び出しは %zu 件)。\n\n", nsite);
        return 0;
    }

    fprintf(out, "=== %s ===\n", path.c_str());
    fprintf(out, "  arch=%s fat=%d imports=%zu defined=%zu entry=0x%llx\n",
            img->arch().c_str(), (int)img->is_fat(), imports.bindings().size(),
            img->defined_symbols().size(), (unsigned long long)img->entry());

    // 分析を断ち切る: 線を 必要 / 邪魔 / どうでもいい に分類し、進展ゼロで探索を打ち切る
    if (opt_.prune) {
        RingAnalyzer ra;
        ra.build(*img, xr);
        ra.find_rings();

        EdgeClassifier ec;
        uint64_t entry = opt_.entry ? opt_.entry : img->entry();
        if (!entry && !ra.nodes().empty()) entry = ra.nodes()[0].addr;
        ec.run(ra, entry);

        size_t n_nec = 0, n_obs = 0, n_irr = 0, n_state = 0;
        for (const auto &e : ec.edges()) {
            if (e.cls == EdgeClassifier::Cls::Necessary) n_nec++;
            else if (e.cls == EdgeClassifier::Cls::Obstructive) n_obs++;
            else n_irr++;
            if (e.wait || e.spawn) n_state++;
        }
        fprintf(out, "  entry=0x%llx  reached=%zu / nodes=%zu\n",
                (unsigned long long)entry, ec.reached(), ra.nodes().size());
        fprintf(out, "  lines: necessary=%zu  obstructive=%zu  irrelevant=%zu\n",
                n_nec, n_obs, n_irr);
        fprintf(out, "  state-dependent (wait/spawn): %zu\n", n_state);
        fprintf(out, "  analysis cut at obstructive lines: %zu\n", ec.cuts());

        // 邪魔する線 (=閉じている環) を先に出して、切る場所を示す
        fprintf(out, "\n  --- 邪魔する線 (ここで分析を断ち切る) ---\n");
        size_t shown = 0;
        for (const auto &e : ec.edges()) {
            if (e.cls != EdgeClassifier::Cls::Obstructive) continue;
            if (shown++ >= 12) { fprintf(out, "      ...\n"); break; }
            fprintf(out, "      %s\n        -> %s\n",
                    e.from.empty() ? "(unnamed)" : e.from.c_str(),
                    e.to.empty() ? "(unnamed)" : e.to.c_str());
        }
        if (!shown) fprintf(out, "      (なし: 進展の無い環は検出されなかった)\n");

        fprintf(out, "\n  --- 状態で動く/動かないの線 (待ち・起動) ---\n");
        shown = 0;
        for (const auto &e : ec.edges()) {
            if (!e.wait && !e.spawn) continue;
            if (shown++ >= 12) { fprintf(out, "      ...\n"); break; }
            fprintf(out, "      [%s] %s -> %s\n", e.wait ? "wait" : "spawn",
                    e.from.empty() ? "(unnamed)" : e.from.c_str(), e.to.c_str());
        }
        if (!shown) fprintf(out, "      (なし)\n");
        fprintf(out, "\n");
        return 0;
    }

    // 環 (SCC) 解析: 「待ち」と「ブロック」を同一機構で検出する
    if (opt_.rings) {
        RingAnalyzer ra;
        ra.build(*img, xr);
        ra.find_rings();
        auto rings = ra.stall_rings();
        (void)rings;
        fprintf(out, "  nodes=%zu  rings(SCC>1)=%zu\n",
                ra.nodes().size(), ra.ring_count());
        size_t shown = 0;
        for (const auto &r : ra.stall_rings()) {
            if (++shown > 12) break;
            bool has_wait = !r.wait_edges.empty() || !r.spawn_edges.empty();
            fprintf(out, "  --- 環 #%zu  members=%zu %s ---\n", shown, r.members.size(),
                    has_wait ? "[待ち/起動が環内]" : "[健全: 待ちなし]");
            for (const auto &ed : r.wait_edges)
                fprintf(out, "      wait  %s\n", ed.c_str());
            for (const auto &ed : r.spawn_edges)
                fprintf(out, "      spawn %s\n", ed.c_str());
            size_t m = 0;
            for (const auto &mem : r.members) {
                if (m++ >= 6) { fprintf(out, "      ... (+%zu)\n", r.members.size() - 6); break; }
                fprintf(out, "      %s\n", mem.c_str());
            }
        }
        fprintf(out, "\n");
        return 0;
    }

    // 既定の探索語: 復元・復旧・セッション・設定の読み出しに関わる一般的な語
    std::vector<std::string> filters = opt_.filters;
    if (filters.empty()) {
        filters = {"Restore", "restore", "Recover", "recover", "Reopen", "reopen",
                   "LastSession", "LastState", "AutoSave", "autosave",
                   "LoadWorkspace", "LoadSession", "Resume", "resume",
                   "quit.blend", "Session", "session"};
    }
    // 既定語は「外部シンボル」ではなく「定義済みシンボル」にも当てる。
    // 復元の実体はそのバイナリ内の関数であることが多いため。
    size_t total = 0;
    for (const auto &f : filters) {
        // (a) 外部シンボル経由の呼び出しを逆track
        auto findings = bt.track(f, 2);
        for (auto &fd : findings) {
            fprintf(out, "  [import] %-52s <- site 0x%08llx %s\n",
                    fd.symbol.c_str(), (unsigned long long)fd.site,
                    fd.is_call ? "(call)" : "(ref)");
            fprintf(out, "           in function 0x%08llx %s\n",
                    (unsigned long long)fd.func,
                    fd.func_name.empty() ? "(unnamed)" : fd.func_name.c_str());
            for (auto &s : fd.nearby_strings)
                fprintf(out, "           str: \"%s\"\n", s.c_str());
            total++;
        }
        // (b) 定義済みシンボル自体が復元関数である場合を報告
        for (const auto &kv : img->defined_symbols()) {
            if (kv.first.find(f) == std::string::npos) continue;
            if (opt_.symbols_only == false) {
                fprintf(out, "  [define] %-52s @ 0x%08llx\n",
                        kv.first.c_str(), (unsigned long long)kv.second);
            }
            total++;
        }
    }
    // 意味のある結果になったかを再現する。
    // 件数や時間ではなく、「入口から具体的な効果までの経路が実在するか」で判定する。
    PathFinder pf;
    pf.build(rings);
    std::vector<std::string> effects;
    for (const auto &f : filters) effects.push_back(f);
    for (const char *e : {"open", "fopen", "access", "stat", "exit", "abort",
                          "terminate", "Wait", "Sleep", "read", "write"})
        effects.push_back(e);

    PathFinder::Found found_path;
    bool meaningful = pf.find_effect(img->entry(), effects, &found_path);

    if (total == 0 || !meaningful) {
        store.observe("meaning", "入口から効果までの経路が実在しなかった");
        fprintf(out, "意味がありませんでした\n");
        return 0;
    }
    store.observe("meaning", "入口から効果までの経路が実在した");
    fprintf(out, "入口から効果までの経路が実在します。長さは %zu です。\n", found_path.chain.size());
    for (size_t i = 0; i < found_path.chain.size() && i < 8; i++)
        fprintf(out, "  %s%s\n", std::string(i, ' ').c_str(), found_path.chain[i].c_str());
    fprintf(out, "終端の効果は %s です。\n", found_path.effect.c_str());
    fprintf(out, "\n");
    return 0;
}

} // namespace nx

// ============================================================ C 入口
// neodax_static (C の CLI) から呼ぶための薄いラッパ。
// mode: 0=既定(逆track) 1=--rings 2=--prune 3=--files

extern "C" int nx_track_run(const char *path, int mode, uint64_t entry,
                           const char *filter, int symbols_only, FILE *out) {
    nx::Options opt;
    opt.symbols_only = symbols_only != 0;
    opt.entry = entry;
    // filter はカンマ区切りで複数語を受け取る。
    if (filter && *filter) {
        std::string s(filter), cur;
        for (char c : s) {
            if (c == ',') { if (!cur.empty()) opt.filters.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        if (!cur.empty()) opt.filters.push_back(cur);
    }
    switch (mode) {
        case 1: opt.rings = true; break;
        case 2: opt.prune = true; break;
        case 3: opt.files = true; break;
        default: break;
    }
    if (!out) out = stdout;
    nx::RestoreLocator loc(opt);
    return loc.run(path, out);
}

// ---------------------------------------------------------------- 環の報告
//
// neodax 本来の環は depflow_build_rings が作る。
// 戻り値由来の呼び出し辺 (DEPFLOW_ORIGIN_CALLEE_RET) だけを
// Kosaraju SCC でまとめたもの。生の call 命令は使わない。
// ここはその結果 (summaries[i].ring_id) を読むだけで、作り直さない。
//
extern "C" int nx_rings_report(macho_binary_t *bin, depflow_graph_t *g, FILE *out) {
    if (!out) out = stdout;
    // どこで環が生まれなかったかが分かるように、まず数を出す。
    std::fprintf(out, "関数は %d 個で、シンボルは %d 個で、xref は %d 個で、要約は %d 個で、環は %d 個です。\n",
                 bin ? bin->nfunctions : 0, bin ? bin->nsymbols : 0,
                 bin ? bin->nxrefs : 0,
                 g ? g->summary_count : 0, g ? g->ring_count : 0);
    if (!bin || !g || !g->summaries || g->summary_count <= 0) {
        std::fprintf(out, "意味がありませんでした\n");
        return 0;
    }

    // 環は辺から自分で求める。depflow の ring_id は戻り値依存辺しか見ておらず、
    // 相互再帰を取り逃すためである。呼び出し辺 (CONTROL, 非間接) を使う。
    std::vector<std::vector<int>> adj(bin->nfunctions);
    int n_ctrl = 0, n_mapped = 0;
    // 実アドレス順の索引を持つ。値は bin->functions の実インデックスで、
    // 住所の並び (start) と関数の並びをここで揃える。
    std::vector<std::pair<uint64_t, int>> fstarts;
    fstarts.reserve((size_t)bin->nfunctions);
    for (int i = 0; i < bin->nfunctions; i++)
        fstarts.push_back({bin->functions[i].start, i});
    std::sort(fstarts.begin(), fstarts.end());
    auto fof = [&](uint64_t a) -> int {
        auto it = std::upper_bound(
            fstarts.begin(), fstarts.end(), a,
            [](uint64_t v, const std::pair<uint64_t, int> &p) { return v < p.first; });
        if (it == fstarts.begin()) return -1;
        return (--it)->second;
    };
    for (int x = 0; x < bin->nxrefs; x++) {
        const macho_xref_t *xr = &bin->xrefs[x];
        if (!xr->is_call || xr->is_indirect) continue;
        n_ctrl++;
        int u = fof(xr->from), v = fof(xr->to);
        if (u >= 0 && v >= 0 && u != v) { adj[u].push_back(v); n_mapped++; }
    }
    std::fprintf(out, "呼び出し xref は %d 個で、関数に結び付いたのは %d 個です。\n",
                 n_ctrl, n_mapped);

    // Tarjan SCC (反復実装)
    int N = bin->nfunctions;
    std::vector<int> index(N, -1), low(N, 0), onstk(N, 0), comp(N, -1);
    std::vector<int> stk;
    struct Fr { int v; size_t ei; };
    std::vector<Fr> call;
    int idx = 0, ncomp = 0;
    std::vector<std::vector<int>> comps;
    for (int r = 0; r < N; r++) {
        if (index[r] != -1) continue;
        call.push_back({r, 0});
        index[r] = low[r] = idx++;
        stk.push_back(r); onstk[r] = 1;
        while (!call.empty()) {
            Fr &f = call.back();
            if (f.ei < adj[f.v].size()) {
                int w = adj[f.v][f.ei++];
                if (index[w] == -1) {
                    index[w] = low[w] = idx++;
                    stk.push_back(w); onstk[w] = 1;
                    call.push_back({w, 0});
                } else if (onstk[w]) {
                    if (index[w] < low[f.v]) low[f.v] = index[w];
                }
            } else {
                int v = f.v;
                call.pop_back();
                if (!call.empty()) {
                    int p = call.back().v;
                    if (low[v] < low[p]) low[p] = low[v];
                }
                if (low[v] == index[v]) {
                    std::vector<int> members;
                    while (true) {
                        int w = stk.back(); stk.pop_back(); onstk[w] = 0;
                        members.push_back(w);
                        if (w == v) break;
                    }
                    if (members.size() >= 2) {
                        for (int w : members) comp[w] = ncomp;
                        comps.push_back(members);
                        ncomp++;
                    }
                }
            }
        }
    }

    if (comps.empty()) {
        std::fprintf(out, "相互再帰の環は見つかりませんでした。\n");
        return 0;
    }

    std::sort(comps.begin(), comps.end(),
              [](const std::vector<int> &a, const std::vector<int> &b) {
                  return a.size() > b.size();
              });

    std::fprintf(out, "このモジュールの環は %zu 個です。\n", comps.size());
    size_t shown = 0;
    for (const auto &r : comps) {
        if (++shown > 8) break;
        std::fprintf(out, "環の要素は %zu 個です。\n", r.size());
        size_t m = 0;
        for (int fi : r) {
            if (m++ >= 5) { std::fprintf(out, "ほか %zu 個\n", r.size() - 5); break; }
            const char *nm = bin->functions[fi].name;
            std::fprintf(out, "%s\n", (nm && *nm) ? nm : "(無名)");
        }
    }
    return 0;
}

// ---------------------------------------------------------------- 状態の絞り込み
//
// 環を検証する前に、とりうる状態だけに絞る。
// dylib では関数検出が空になるため、シンボルから関数集合を作る。
//   ・アドレスを持つ
//   ・未定義でない
//   ・関数シンボルである
// この 3 つの条件を満たすものだけを採用する。
// これで「あり得ない状態」が最初から混ざらない。
//
extern "C" int nx_narrow_functions(macho_binary_t *bin) {
    if (!bin) return 0;
    if (bin->nfunctions > 0) return bin->nfunctions;   // 既にあるなら触らない

    std::vector<const macho_symbol_t *> fs;
    for (int i = 0; i < bin->nsymbols; i++) {
        const macho_symbol_t *s = &bin->symbols[i];
        if (s->is_undefined || s->address == 0) continue;
        if (s->name[0] == '\0') continue;
        // 型が未設定のことがあるので型では絞らない。
        // アドレスを持ち未定義でないものだけが「とりうる状態」である。
        fs.push_back(s);
    }
    if (fs.empty()) return 0;

    std::sort(fs.begin(), fs.end(),
              [](const macho_symbol_t *a, const macho_symbol_t *b) {
                  return a->address < b->address;
              });

    if (!bin->functions || bin->functions_allocated < (int)fs.size() + 1) {
        // 領域が足りなければ作り直す。古いものはここで開放してよい
        // (所有は bin に一本化されており、macho_free は新しい方を開放する)。
        free(bin->functions);
        bin->functions = (macho_function_t *)calloc(fs.size() + 1, sizeof(macho_function_t));
        if (!bin->functions) { bin->functions_allocated = 0; return 0; }
        bin->functions_allocated = (int)fs.size() + 1;
    }

    for (size_t i = 0; i < fs.size(); i++) {
        macho_function_t *f = &bin->functions[i];
        memset(f, 0, sizeof(*f));
        f->start = fs[i]->address;
        uint64_t end = (i + 1 < fs.size()) ? fs[i + 1]->address : 0;
        if (fs[i]->size) end = fs[i]->address + fs[i]->size;
        f->end = end;
        snprintf(f->name, sizeof(f->name), "%s", fs[i]->name);
        f->sym_idx = -1;
    }
    bin->nfunctions = (int)fs.size();
    return bin->nfunctions;
}

static const char *名_category(int i) {
    static const char *t[] = {"不明","盤面","着手","着手木","評価値","探索深さ","節点",
        "探索木","定数","データベース","状態","通知","並列","一時変数","全体設定",
        "ハッシュ","ビット盤面","駒","点数","時間","記憶域","入出力","補助"};
    return (i >= 0 && i < (int)(sizeof(t)/sizeof(t[0]))) ? t[i] : "不明";
}
static const char *名_aspect(int i) {
    static const char *t[] = {"不明","静的","動的","持続","一時的","共有","専有",
        "読み出し専用","書き込み可","不変","可変","逐次","乱順","添字","階層",
        "線形","木","グラフ","ハッシュ","圧縮","暗号化"};
    return (i >= 0 && i < (int)(sizeof(t)/sizeof(t[0]))) ? t[i] : "不明";
}
static const char *名_access(int i) {
    static const char *t[] = {"不明","単一","複数","逐次","乱順","反復","再帰",
        "並列","条件付き","頻繁","稀"};
    return (i >= 0 && i < (int)(sizeof(t)/sizeof(t[0]))) ? t[i] : "不明";
}
static const char *名_type(int i) {
    static const char *t[] = {"不明","単一値","配列","構造体","共用体","ポインタ",
        "関数ポインタ","ビット欄","文字列","並び","木","グラフ","ハッシュ表",
        "積み","待ち行列","山","行列","ベクトル"};
    return (i >= 0 && i < (int)(sizeof(t)/sizeof(t[0]))) ? t[i] : "不明";
}
static const char *名_role(const char *r) {
    if (!r) return "不明";
    if (!std::strcmp(r, "Board")) return "盤面";
    if (!std::strcmp(r, "MoveGen")) return "着手生成";
    if (!std::strcmp(r, "Evaluator")) return "評価";
    if (!std::strcmp(r, "Search")) return "探索";
    if (!std::strcmp(r, "Node")) return "節点";
    if (!std::strcmp(r, "Dispatcher")) return "配分";
    if (!std::strcmp(r, "Worker")) return "作業";
    if (!std::strcmp(r, "Sync")) return "同期";
    if (!std::strcmp(r, "BeamSearch")) return "ビーム探索";
    if (!std::strcmp(r, "Decoder")) return "復号";
    if (!std::strcmp(r, "Loader")) return "読み込み";
    return r;
}

// 分類器と役割と C++ クラス設計をまとめて出す。1 つの手法に偏らないため。
extern "C" int nx_classify_report(macho_binary_t *bin, depflow_graph_t *g,
                                  const char *workflow_out, FILE *out) {
    if (!out) out = stdout;
    if (!bin || !g) { std::fprintf(out, "できませんでした\n"); return 0; }

    var_classifier_t vc;
    var_classifier_init(&vc);
    if (var_classify_all(&vc, bin, g) != 0) {
        std::fprintf(out, "できませんでした\n");
        var_classifier_free(&vc);
        return 0;
    }
    std::fprintf(out, "変数は %d 個です。\n", vc.num_classifications);

    std::fprintf(out, "カテゴリは次のとおりです。\n");
    for (int i = 1; i < VAR_CAT_MAX; i++)
        if (vc.categories[i])
            std::fprintf(out, "%s %d\n", 名_category(i), vc.categories[i]);

    std::fprintf(out, "側面は次のとおりです。\n");
    for (int i = 1; i < VAR_ASPECT_MAX; i++)
        if (vc.aspects[i])
            std::fprintf(out, "%s %d\n", 名_aspect(i), vc.aspects[i]);

    int max_acc = 0, tot_acc = 0;
    for (int i = 1; i < VAR_ACCESS_MAX; i++) { max_acc = std::max(max_acc, vc.access_patterns[i]); tot_acc += vc.access_patterns[i]; }
    // ほとんど全部が同じなら、その項目は不要なので出さない。
    if (tot_acc == 0 || max_acc * 10 < tot_acc * 9) {
        std::fprintf(out, "アクセスは次のとおりです。\n");
        for (int i = 1; i < VAR_ACCESS_MAX; i++)
            if (vc.access_patterns[i])
                std::fprintf(out, "%s %d\n", 名_access(i), vc.access_patterns[i]);
    } else {
        std::fprintf(out, "アクセスはほとんど同じなので、出しません。\n");
    }

    std::fprintf(out, "型は次のとおりです。\n");
    for (int i = 1; i < VAR_TYPE_MAX; i++)
        if (vc.data_types[i])
            std::fprintf(out, "%s %d\n", 名_type(i), vc.data_types[i]);

    std::map<std::string, int> roles;
    for (int i = 0; i < bin->nfunctions; i++) {
        char role[64];
        if (depflow_function_role(bin, g, i, role, sizeof(role))) roles[名_role(role)]++;
    }
    // 実アドレス順の索引。値は bin->functions の実インデックス。
    // 住所の並びと関数の並びをここで揃える。
    std::vector<std::pair<uint64_t, int>> starts;
    starts.reserve((size_t)bin->nfunctions);
    for (int i = 0; i < bin->nfunctions; i++)
        starts.push_back({bin->functions[i].start, i});
    std::sort(starts.begin(), starts.end());
    auto func_of = [&](uint64_t a) -> int {
        auto it = std::upper_bound(
            starts.begin(), starts.end(), a,
            [](uint64_t v, const std::pair<uint64_t, int> &p) { return v < p.first; });
        if (it == starts.begin()) return -1;
        return (--it)->second;
    };

    std::set<uint64_t> wanted;
    for (int i = 0; i < vc.num_classifications; i++)
        if (vc.classifications[i].address) wanted.insert(vc.classifications[i].address);

    std::map<uint64_t, std::set<int>> var_refs;
    {
        int n_dec = 0, n_pairs = 0, n_hit = 0;
        int cur_func = -2;
        // 実アドレスを保持しているレジスタ。ADRP → ADD / MOV → LDR/STR と
        // 最大深さまで辿る。関数をまたぐと意味が無いので関数ごとに捨てる。
        uint64_t reg_addr[32] = {0};
        bool     reg_valid[32] = {false};

        for (int ii = 0; ii < bin->ninsns; ii++) {
            macho_insn_t *mi = &bin->insns[ii];
            uint32_t w = 0;
            memcpy(&w, mi->bytes, 4);
            nx::Arm64::Insn in;
            nx::Arm64::decode(mi->address, w, &in);
            if (in.op != nx::Arm64::Op::Unknown) n_dec++;

            int fi = func_of(mi->address);
            if (fi != cur_func) {
                cur_func = fi;
                memset(reg_addr, 0, sizeof(reg_addr));
                memset(reg_valid, 0, sizeof(reg_valid));
            }

            auto note = [&](uint64_t a) {
                n_pairs++;
                if (!wanted.count(a)) return;
                if (fi >= 0) { var_refs[a].insert(fi); n_hit++; }
            };

            // ADRP: ページ先頭 (実アドレス) を保持する
            if (in.op == nx::Arm64::Op::Adrp) {
                if (in.rd >= 0) { reg_addr[in.rd] = in.target; reg_valid[in.rd] = true; }
                continue;
            }
            // ADD: 保持している実アドレス + 即値。結果も辿れるようにする。
            if (in.op == nx::Arm64::Op::Add && in.rn >= 0 && reg_valid[in.rn]) {
                uint64_t a = reg_addr[in.rn] + in.imm;
                note(a);
                if (in.rd >= 0) { reg_addr[in.rd] = a; reg_valid[in.rd] = true; }
                continue;
            }
            // MOV: レジスタ間でそのまま持ち回る
            if (in.op == nx::Arm64::Op::Mov && in.rn >= 0 && in.rd >= 0 &&
                reg_valid[in.rn]) {
                reg_addr[in.rd] = reg_addr[in.rn];
                reg_valid[in.rd] = true;
                continue;
            }
            // LDR/STR: 保持している実アドレス (+即値) を参照先とする
            if ((in.op == nx::Arm64::Op::Ldr || in.op == nx::Arm64::Op::Str) &&
                in.rn >= 0 && reg_valid[in.rn]) {
                note(reg_addr[in.rn] + in.imm);
                if (in.op == nx::Arm64::Op::Ldr && in.rt >= 0) reg_valid[in.rt] = false;
                continue;
            }
            // ここで追跡できない命令がレジスタを書いたら、その追跡を捨てる。
            for (int r = 0; r < 32; r++)
                if (reg_valid[r] &&
                    ((in.rd >= 0 && r == in.rd) || (in.rt >= 0 && r == in.rt)))
                    reg_valid[r] = false;
        }
        std::fprintf(out, "命令を %d 個読み、参照の候補は %d 個で、変数に当たったのは %d 個です。\n",
                     n_dec, n_pairs, n_hit);
    }

    // 群の番号。環があればそれを使い、無ければ振る舞いの役割を使う。
    // 変数の住所 (データ) と関数の住所 (コード) は別の領域なので、
    // 変数の住所を関数の住所として引いてはいけない。参照する関数側で揃える。
    auto group_of = [&](int fi) -> int {
        if (fi < 0) return -1;
        if (g->summaries && fi < g->summary_count && g->summaries[fi].ring_id >= 0)
            return g->summaries[fi].ring_id;
        char role[64];
        if (!depflow_function_role(bin, g, fi, role, sizeof(role))) return -1;
        int hv = 0;
        for (const char *p = role; *p; p++) hv = hv * 131 + (unsigned char)*p;
        return 100000 + (hv & 0xffff);
    };

    std::fprintf(out, "状態の参照の仕方を四つに分けます。\n");
    {
        int total = 0, solo_in = 0, solo_out = 0, grp_in = 0, grp_out = 0, none = 0;
        for (int i = 0; i < vc.num_classifications; i++) {
            const var_classification_t *c = &vc.classifications[i];
            if (c->primary_category != VAR_CAT_STATE) continue;
            total++;
            std::set<int> fs;
            auto vit = var_refs.find(c->address);
            if (vit != var_refs.end()) fs = vit->second;
            if (fs.empty()) { none++; continue; }
            // 参照する関数が属する群を、同じ基準 (実アドレス) でまとめる。
            std::set<int> gs;
            for (int fi : fs) gs.insert(group_of(fi));
            const bool in_one_group = (gs.size() == 1);
            if (fs.size() <= 2) {
                if (in_one_group) solo_in++; else solo_out++;
            } else {
                if (in_one_group) grp_in++; else grp_out++;
            }
        }
        std::fprintf(out, "状態は %d 個です。\n", total);
        std::fprintf(out, "単独で群の中を参照するものは %d 個です。\n", solo_in);
        std::fprintf(out, "単独で群の外を参照するものは %d 個です。\n", solo_out);
        std::fprintf(out, "群で群の中だけを参照するものは %d 個です。\n", grp_in);
        std::fprintf(out, "群で群の外へ出るものは %d 個です。\n", grp_out);
        std::fprintf(out, "参照が無いものは %d 個です。\n", none);
    }

    std::fprintf(out, "\n役割は次のとおりです。\n");
    for (const auto &kv : roles) std::fprintf(out, "%s %d\n", kv.first.c_str(), kv.second);

    var_classifier_free(&vc);

    if (workflow_out && *workflow_out) {
        depflow_export_workflow(bin, g, workflow_out);
        std::fprintf(out, "C++ クラス設計を %s に書きました。\n", workflow_out);
    }
    return 0;
}

// 学んだことの表示と消去。
// mode: 1=表示 2=消去 3=部分結果の一覧
extern "C" int nx_learn_run(int mode, FILE *out) {
    if (!out) out = stdout;
    nx::SkillStore s;
    std::string p = nx::SkillStore::default_path();
    s.load(p);
    if (mode == 2) {
        s.clear();
        s.save(p);
        std::fprintf(out, "学んだことを消しました。\n");
        return 0;
    }
    s.dump(out);
    return 0;
}


// ============================================================ guard
//
// 起動を止める「検知」を静的に消す。止める側のプロセスを触らないので、
// 止められたこと自体をアプリに検知させない (fail-safe)。
//
//   1. 対象文字列 (例 "DVA_Wrkspce") を参照する命令と所属関数を探す
//   2. その関数内の条件分岐 (cbz/cbnz/tbz/tbnz/b.cond) を候補にする
//   3. 文字列参照に一番近い = 一番簡単に消せる候補を選ぶ
//   4. --apply のときだけ実バイナリを書き換える。その前に <file>.nx-backup を取る
//   5. 書いた後に読み直して確認し、違えば backup から戻す
//
// 動的解析は一切使わない。

namespace {

struct nx_guard_cand {
    uint64_t func = 0;
    uint64_t site = 0;      // 条件分岐のアドレス
    uint64_t target = 0;    // 分岐先
    uint32_t raw = 0;
    int      dist = 0;      // 関数先頭からの距離
    const char *mn = "";
    std::string fname;
};

void nx_enc_b(uint64_t pc, uint64_t target, uint8_t out[4]) {
    int64_t imm = (int64_t)(target - pc) >> 2;
    uint32_t w = 0x14000000u | ((uint32_t)imm & 0x03FFFFFFu);
    out[0] = (uint8_t)(w & 0xff); out[1] = (uint8_t)((w >> 8) & 0xff);
    out[2] = (uint8_t)((w >> 16) & 0xff); out[3] = (uint8_t)((w >> 24) & 0xff);
}

bool nx_copy_file(const std::string &from, const std::string &to) {
    std::ifstream in(from, std::ios::binary);
    if (!in) return false;
    std::ofstream o(to, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o << in.rdbuf();
    return (bool)o;
}

} // namespace

// .app やディレクトリなら Contents/MacOS の実行ファイルを指す。
static std::string nx_resolve_app(const std::string &dir) {
    DIR *d = opendir(dir.c_str());
    if (!d) return {};
    std::string best;
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string p = dir + "/" + e->d_name;
        if (p.size() >= 4 && p.compare(p.size() - 4, 4, ".app") == 0) { best = p; break; }
    }
    closedir(d);
    return best;
}

// .app の Info.plist から CFBundleExecutable を読み、本体を一意に決める。
// これが無いと MacOS/ 内の補助実行ファイル (crashpad_handler 等) を
// 本体と誤認する。
static std::string nx_bundle_exe(const std::string &app) {
    std::ifstream f(app + "/Contents/Info.plist");
    if (!f) return {};
    std::string all((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    size_t k = all.find("CFBundleExecutable");
    if (k == std::string::npos) return {};
    size_t s = all.find("<string>", k);
    if (s == std::string::npos) return {};
    s += 8;
    size_t e = all.find("</string>", s);
    if (e == std::string::npos || e <= s) return {};
    std::string name = all.substr(s, e - s);
    // 前後の空白を落とす
    size_t a = name.find_first_not_of(" \t\r\n");
    size_t b = name.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    name = name.substr(a, b - a + 1);
    std::string p = app + "/Contents/MacOS/" + name;
    struct stat st;
    return (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode)) ? p : std::string();
}

static std::string nx_resolve_target(const std::string &path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return path;
    std::string app = path;
    if (app.size() < 4 || app.compare(app.size() - 4, 4, ".app") != 0)
        app = nx_resolve_app(path);   // ディレクトリ内の .app を探す
    if (app.empty()) return path;

    // 1) Info.plist の CFBundleExecutable を最優先。
    std::string declared = nx_bundle_exe(app);
    if (!declared.empty()) return declared;

    // 2) 無ければ MacOS/ 内で最大の実行ファイル (本体は普通いちばん大きい)。
    std::string macos = app + "/Contents/MacOS";
    DIR *d = opendir(macos.c_str());
    if (!d) return path;
    std::string best, first;
    uint64_t best_sz = 0;
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string p = macos + "/" + e->d_name;
        struct stat s2;
        if (stat(p.c_str(), &s2) == 0 && S_ISREG(s2.st_mode) && (s2.st_mode & 0111)) {
            if (first.empty()) first = p;
            if ((uint64_t)s2.st_size > best_sz) { best_sz = (uint64_t)s2.st_size; best = p; }
        }
    }
    closedir(d);
    if (!best.empty()) return best;
    return first.empty() ? path : first;
}

// Mach-O の魔法だけを先頭で確認する (中身は読まない)。
static bool nx_is_macho(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    unsigned char m[4] = {0, 0, 0, 0};
    f.read((char *)m, 4);
    uint32_t w = ((uint32_t)m[0] << 24) | ((uint32_t)m[1] << 16) |
                 ((uint32_t)m[2] << 8) | (uint32_t)m[3];
    // FEEDFACE / FEEDFACF / CAFEBABE / CAFEBABF (BE 読み) とその LE 版
    return w == 0xFEEDFACFu || w == 0xFEEDFACEu || w == 0xCEFAEDFEu ||
           w == 0xCFFAEDFEu || w == 0xCAFEBABEu || w == 0xBEBAFECAu ||
           w == 0xCAFEBABFu || w == 0xBEBAFECFu;
}

// 文字列参照を 1 つの Mach-O について調べ、見つけた数を返す。
static int nx_find_string_one(const std::string &target, const char *needle, FILE *out) {
    auto img = nx::MachOImage::open(target);
    if (!img) return 0;
    nx::ImportResolver imports; imports.parse(*img);
    nx::XRefIndex xr; xr.build(*img, imports);
    nx::BackTracker bt(*img, xr);
    int n = 0;
    for (const auto &s : xr.sites()) {
        if (s.is_call || !s.target_symbol.empty()) continue;
        std::string str = img->string_at(s.target_vm, 256);
        if (str.find(needle) == std::string::npos) continue;
        uint64_t f = bt.function_containing(s.addr);
        std::string fn = bt.function_name(f);
        std::fprintf(out, "  参照 site=0x%llx  func=0x%llx  %s  \"%s\"\n",
                     (unsigned long long)s.addr, (unsigned long long)f,
                     fn.empty() ? "(無名)" : fn.c_str(), str.c_str());
        n++;
    }
    return n;
}

static int nx_walk_macho(const std::string &dir, std::vector<std::string> &out, int depth) {
    if (depth > 10) return 0;
    DIR *d = opendir(dir.c_str());
    if (!d) return 0;
    int n = 0;
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string p = dir + "/" + e->d_name;
        struct stat st;
        if (stat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { n += nx_walk_macho(p, out, depth + 1); continue; }
        if (!S_ISREG(st.st_mode)) continue;
        if (nx_is_macho(p)) { out.push_back(p); n++; }
    }
    closedir(d);
    return n;
}

// 文字列を参照する箇所と所属関数を一覧する (静的のみ)。
// ディレクトリ/.app を渡すと、中の Mach-O をすべて探す。
extern "C" int nx_find_string(const char *path, const char *needle, FILE *out) {
    if (!out) out = stdout;
    if (!path || !needle || !*needle) return 2;
    nx::SkillStore brain;
    brain.load(nx::SkillStore::default_path());
    brain.observe("find_needle", needle);
    brain.observe("find_habit", "束を静的に走査し、文字列参照から関数を逆引きする");
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        std::vector<std::string> files;
        nx_walk_macho(path, files, 0);
        std::fprintf(out, "%s の中の Mach-O は %zu 個です。\n", path, files.size());
        int total = 0, hitfiles = 0;
        for (const auto &f : files) {
            int n = nx_find_string_one(f, needle, out);
            if (n > 0) {
                std::fprintf(out, "  -> %s (%d 箇所)\n", f.c_str(), n);
                total += n; hitfiles++;
            }
        }
        std::fprintf(out, "%s を参照するファイルは %d 個で、箇所は %d 個です。\n",
                     needle, hitfiles, total);
        brain.observe("find_hit_files_bucket", std::to_string(hitfiles));
        brain.observe("find_result", hitfiles ? "見つかった" : "見つからなかった");
        brain.save(nx::SkillStore::default_path());
        return hitfiles ? 0 : 1;
    }
    std::string target = nx_resolve_target(path);
    int n = nx_find_string_one(target, needle, out);
    std::fprintf(out, "%s を参照する箇所は %d 個です。\n", needle, n);
    brain.observe("find_result", n ? "見つかった" : "見つからなかった");
    brain.save(nx::SkillStore::default_path());
    return n ? 0 : 1;
}

// 生の文字列を走査する (strings + grep 相当)。
// --find-string が探すのは「文字列への参照」だが、Adobe のライセンス
// ライブラリのようにシンボルもリフレッシュ情報も strip されたバイナリでは
// 参照サイトが取れない。その場合は中身のバイトを直接見るしかない。
// ディレクトリ/.app を渡すと中の Mach-O を全て走査する。
extern "C" int nx_scan_strings(const char *path, const char *needle, FILE *out) {
    if (!out) out = stdout;
    if (!path || !needle || !*needle) return 2;
    size_t nlen = std::strlen(needle);
    struct stat st;
    std::vector<std::string> files;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        nx_walk_macho(path, files, 0);
    else
        files.push_back(path);
    if (files.empty()) { std::fprintf(out, "ファイルがありません: %s\n", path); return 1; }
    std::fprintf(out, "%zu 個の Mach-O を走査します。\n", files.size());
    int total = 0, hitfiles = 0;
    for (const auto &f : files) {
        std::ifstream in(f, std::ios::binary);
        if (!in) continue;
        std::string data((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        size_t pos = 0; int n = 0;
        while ((pos = data.find(needle, pos)) != std::string::npos) {
            // 前後の印字可能範囲を切り出す。
            size_t a = pos;
            while (a > 0 && data[a - 1] >= 0x20 && data[a - 1] < 0x7f) a--;
            size_t b = pos + nlen;
            while (b < data.size() && data[b] >= 0x20 && data[b] < 0x7f) b++;
            std::string s = data.substr(a, std::min<size_t>(b - a, 200));
            std::fprintf(out, "  %s  off=0x%llx  \"%s\"\n", f.c_str(),
                         (unsigned long long)a, s.c_str());
            n++;
            pos += nlen;
            if (n >= 40) break;
        }
        if (n > 0) { hitfiles++; total += n; }
    }
    std::fprintf(out, "%s を含む文字列は %d 個のファイルに %d 個です。\n",
                 needle, hitfiles, total);
    return total ? 0 : 1;
}

// 指定アドレス周辺を静的に逆アセンブルする (neodax 自身のデコーダのみ)。
extern "C" int nx_disasm(const char *path, uint64_t addr, int n, FILE *out) {
    if (!out) out = stdout;
    if (!path) return 2;
    std::string target = nx_resolve_target(path);
    auto img = nx::MachOImage::open(target);
    if (!img) { std::fprintf(out, "[skip] not a 64-bit Mach-O: %s\n", target.c_str()); return 1; }
    if (n <= 0) {
        // count を「関数の長さ」にびったり合わせる。
        // 含まれる関数の先頭から、次の関数の先頭までを全部出す。
        nx::ImportResolver imports; imports.parse(*img);
        nx::XRefIndex xr; xr.build(*img, imports);
        nx::BackTracker bt(*img, xr);
        uint64_t f = bt.function_containing(addr);
        const auto &starts = bt.func_starts();
        auto up = std::upper_bound(starts.begin(), starts.end(), f);
        uint64_t end = (up == starts.end()) ? f + 0x2000 : *up;
        if (end <= f) end = f + 0x1000;
        std::string fn = bt.function_name(f);
        std::fprintf(out, "; function 0x%llx %s  [0x%llx, 0x%llx)  %llu insn\n",
                     (unsigned long long)f, fn.empty() ? "(無名)" : fn.c_str(),
                     (unsigned long long)f, (unsigned long long)end,
                     (unsigned long long)((end - f) / 4));
        addr = f;
        n = (int)((end - f) / 4);
    }
    if (n <= 0 || n > 4096) n = 64;
    for (int i = 0; i < n; i++) {
        uint64_t pc = addr + (uint64_t)i * 4;
        const uint8_t *p = img->at(pc, 4);
        if (!p) break;
        uint32_t w = *(const uint32_t *)p;
        nx::Arm64::Insn in;
        nx::Arm64::decode(pc, w, &in);
        std::fprintf(out, "0x%08llx  %-7s", (unsigned long long)pc, nx::Arm64::op_name(in.op));
        using Op = nx::Arm64::Op;
        if (in.op == Op::Adrp) std::fprintf(out, " x%d, 0x%llx", in.rd, (unsigned long long)in.target);
        else if (in.op == Op::Add || in.op == Op::Sub)
            std::fprintf(out, " x%d, x%d, #0x%llx", in.rd, in.rn, (unsigned long long)in.imm);
        else if (in.op == Op::Ldr || in.op == Op::Str)
            std::fprintf(out, " x%d, [x%d, #0x%llx]", in.rt, in.rn, (unsigned long long)in.imm);
        else if (in.op == Op::B || in.op == Op::Bl || in.op == Op::BCond ||
                 in.op == Op::Cbz || in.op == Op::Cbnz || in.op == Op::Tbz || in.op == Op::Tbnz)
            std::fprintf(out, " 0x%llx", (unsigned long long)in.target);
        else if (in.op == Op::Ret || in.op == Op::Br || in.op == Op::Blr)
            std::fprintf(out, " x%d", in.rn);
        else if (in.op == Op::Mov) std::fprintf(out, " x%d, x%d", in.rd, in.rn);
        else if (in.op == Op::Movz || in.op == Op::Movk)
            std::fprintf(out, " x%d, #0x%llx", in.rd, (unsigned long long)in.imm);
        else if (in.op == Op::Cmp) std::fprintf(out, " x%d, #0x%llx", in.rn, (unsigned long long)in.imm);
        std::fprintf(out, "\n");
    }
    return 0;
}

// 定義シンボルを名前の部分一致で一覧する (静的のみ)。
// ディレクトリ/.app を渡すと、中の Mach-O をすべて調べ、
// 「どのファイルがそのシンボルを定義しているか」まで返す。
extern "C" int nx_list_symbols(const char *path, const char *needle, FILE *out) {
    if (!out) out = stdout;
    if (!path || !needle || !*needle) return 2;
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        std::vector<std::string> files;
        nx_walk_macho(path, files, 0);
        std::fprintf(out, "%s の中の Mach-O は %zu 個です。\n", path, files.size());
        int total = 0, hitfiles = 0;
        for (const auto &f : files) {
            auto img = nx::MachOImage::open(f);
            if (!img) continue;
            int n = 0;
            for (const auto &kv : img->defined_symbols()) {
                if (kv.first.find(needle) == std::string::npos) continue;
                std::fprintf(out, "  0x%llx  %-60s  %s\n",
                             (unsigned long long)kv.second, kv.first.c_str(), f.c_str());
                n++;
            }
            if (n > 0) { hitfiles++; total += n; }
        }
        std::fprintf(out, "%s を含む定義シンボルは %d 個のファイルに %d 個です。\n",
                     needle, hitfiles, total);
        return total ? 0 : 1;
    }
    std::string target = nx_resolve_target(path);
    auto img = nx::MachOImage::open(target);
    if (!img) { std::fprintf(out, "[skip] not a 64-bit Mach-O: %s\n", target.c_str()); return 1; }
    int n = 0;
    for (const auto &kv : img->defined_symbols()) {
        if (kv.first.find(needle) == std::string::npos) continue;
        std::fprintf(out, "  0x%llx  %s\n", (unsigned long long)kv.second, kv.first.c_str());
        n++;
    }
    std::fprintf(out, "%s を含む定義シンボルは %d 個です。\n", needle, n);
    return n ? 0 : 1;
}

// ---------------------------------------------------------------- 依存グラフ
// otool -L 相当。LC_LOAD_DYLIB / LC_RPATH を @rpath・@loader_path・
// @executable_path 展開込みで実パスに解決し、依存の連鎖を辿る。
// 「このモジュールは何に依存し、その依存はどのフレームワークが担当か」を
// 起動せずに決められる。

static std::string nx_dirname(const std::string &p) {
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? std::string(".") : p.substr(0, s);
}

static std::string nx_join_path(const std::string &a, const std::string &b) {
    if (b.empty()) return a;
    if (b[0] == '/') return b;
    if (a.empty()) return b;
    return a.back() == '/' ? a + b : a + "/" + b;
}

static bool nx_path_exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

// loader: 依存を要求している Mach-O。exe: ルート実行ファイル (rpath 基準)。
static std::string nx_resolve_dep(const std::string &loader, const std::string &exe,
                                  const std::vector<std::string> &rpaths,
                                  const std::string &dep) {
    std::string ldir = nx_dirname(loader), edir = nx_dirname(exe);
    auto expand = [&](std::string out) {
        auto rep = [&](const std::string &from, const std::string &to) {
            size_t pos;
            while ((pos = out.find(from)) != std::string::npos)
                out.replace(pos, from.size(), to);
        };
        rep("@loader_path", ldir);
        rep("@executable_path", edir);
        return out;
    };
    if (dep.rfind("@rpath/", 0) == 0) {
        std::string rest = dep.substr(7);
        for (const auto &rp : rpaths) {
            std::string cand = nx_join_path(expand(rp), rest);
            if (nx_path_exists(cand)) return cand;
        }
        return {};
    }
    if (dep.rfind("@loader_path/", 0) == 0)
        return nx_join_path(ldir, dep.substr(13));
    if (dep.rfind("@executable_path/", 0) == 0)
        return nx_join_path(edir, dep.substr(17));
    if (!dep.empty() && dep[0] == '/') return dep;
    return nx_join_path(ldir, dep);
}

// パスの . / .. を畳む (表示と存在確認のため)。絶対パスのみ対象。
static std::string nx_normpath(const std::string &in) {
    if (in.empty() || in[0] != '/') return in;
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < in.size()) {
        size_t j = in.find('/', i);
        if (j == std::string::npos) j = in.size();
        std::string seg = in.substr(i, j - i);
        if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (seg != "." && !seg.empty()) parts.push_back(seg);
        i = j + 1;
    }
    std::string out;
    for (const auto &s : parts) { out += "/"; out += s; }
    return out.empty() ? std::string("/") : out;
}

// リンクしているライブラリを列挙する。recursive=1 で依存の連鎖を辿る。
extern "C" int nx_list_deps(const char *path, int recursive, FILE *out) {
    if (!out) out = stdout;
    if (!path) return 2;
    std::string root = nx_resolve_target(path);
    if (!nx_path_exists(root)) { std::fprintf(out, "見つかりません: %s\n", root.c_str()); return 1; }

    std::set<std::string> seen;
    std::vector<std::pair<int, std::string>> queue;
    queue.push_back({0, root});
    int total = 0, unresolved = 0, files = 0;
    // dyld は @rpath を「読み込み連鎖全体の rpath」で解決する。
    // ローダ単体の rpath だけでは足りないので、走査した全イメージの
    // rpath を集めて使う (ルートを最初に処理するので順序も保たれる)。
    std::vector<std::string> all_rpaths;
    while (!queue.empty()) {
        int depth = queue.front().first;
        std::string p = queue.front().second;
        queue.erase(queue.begin());
        if (!seen.insert(p).second) continue;
        if (recursive && depth > 8) continue;
        auto img = nx::MachOImage::open(p);
        if (!img) continue;
        files++;
        std::string pad((size_t)depth * 2, ' ');
        std::fprintf(out, "%s%s\n", pad.c_str(), p.c_str());
        if (!img->dylib_id().empty())
            std::fprintf(out, "%s  id: %s\n", pad.c_str(), img->dylib_id().c_str());
        for (const auto &rp : img->rpaths()) {
            std::fprintf(out, "%s  rpath: %s\n", pad.c_str(), rp.c_str());
            if (std::find(all_rpaths.begin(), all_rpaths.end(), rp) == all_rpaths.end())
                all_rpaths.push_back(rp);
        }
        for (const auto &d : img->dylibs()) {
            total++;
            // 絶対パス (/System, /usr/lib 等) は dyld が解決する。
            // 実体は共有キャッシュ内にあることが多く、ファイルは存在しない。
            // 存在しないことを「未解決」とは呼ばない。
            bool absolute = !d.empty() && d[0] == '/';
            std::string r = nx_normpath(nx_resolve_dep(p, root, all_rpaths, d));
            if (absolute) {
                std::fprintf(out, "%s  -> %s\n", pad.c_str(), r.c_str());
                if (recursive && nx_path_exists(r) && !seen.count(r))
                    queue.push_back({depth + 1, r});
                continue;
            }
            if (r.empty() || !nx_path_exists(r)) {
                unresolved++;
                std::fprintf(out, "%s  - %s  (未解決)\n", pad.c_str(), d.c_str());
                continue;
            }
            std::fprintf(out, "%s  -> %s\n", pad.c_str(), r.c_str());
            if (recursive && !seen.count(r)) queue.push_back({depth + 1, r});
        }
        if (!recursive) break;
    }
    std::fprintf(out, "ライブラリ数 %d (未解決 %d) / 走査ファイル %d\n", total, unresolved, files);
    return 0;
}

// ---------------------------------------------------------------- 状態の一貫性回復
// 一つの永続手続き。「良い状態」をファイルの値で定義し、毎回同じ手順で揃える。
// NGL の SSOWatcher は .adobelicnotification を読み、SIGNOUT なら
// ローカルサインアウトしてトークンを消す (→ プロファイル 404 → AE 終了)。
// Mach-O には触らない。変更前の値は *.nx-backup に退避する。
struct nx_set_invariant { const char *file; const char *val; };
struct nx_state_rule {
    const char *name;
    const char *rel_dir;
    nx_set_invariant items[2];
};

static const nx_state_rule k_state_rules[] = {
    { "adobe", "Library/Application Support/Adobe",
      { { ".adobestatusnotification", "IMS_ACTIVE" },
        { ".adobelicnotification",   "SIGNIN" } } },
};

static std::string nx_trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, b - a + 1);
}

static bool nx_read_small(const std::string &p, std::string &out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

static bool nx_write_small(const std::string &p, const std::string &v) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << v;
    return (bool)f;
}

extern "C" int nx_state_heal(const char *name, int apply, FILE *out) {
    if (!out) out = stdout;
    const char *h = std::getenv("HOME");
    std::string home = h ? h : "";
    int handled = 0;
    for (const auto &r : k_state_rules) {
        if (name && *name && std::string(name) != r.name) continue;
        handled++;
        std::string dir = nx_join_path(home, r.rel_dir);
        std::fprintf(out, "=== %s ===\n  dir: %s\n", r.name, dir.c_str());

        struct Item { std::string path; const char *want; bool ok; bool exists; };
        std::vector<Item> items;
        bool all_ok = true;
        for (const auto &in : r.items) {
            if (!in.file) continue;
            Item it;
            it.path = nx_join_path(dir, in.file);
            it.want = in.val;
            std::string v;
            it.exists = nx_read_small(it.path, v);
            it.ok = it.exists && nx_trim(v) == it.want;
            std::fprintf(out, "  %-28s = \"%s\" (期待 \"%s\")%s\n",
                         in.file, it.exists ? nx_trim(v).c_str() : "(無し)",
                         it.want, it.ok ? "" : "  ← 不一致");
            if (!it.ok) all_ok = false;
            items.push_back(it);
        }

        if (all_ok) {
            std::fprintf(out, "  一貫しています。変更なし。\n\n");
            continue;
        }
        if (!apply) {
            std::fprintf(out, "  [dry-run] 良い状態に揃える手順:\n");
            for (const auto &it : items)
                if (!it.ok)
                    std::fprintf(out, "    - %s を \"%s\" に\n", it.path.c_str(), it.want);
            std::fprintf(out, "  --apply で適用します。\n\n");
            continue;
        }
        int changed = 0;
        for (const auto &it : items) {
            if (it.ok) continue;
            if (it.exists) {
                std::string bak = it.path + ".nx-backup";
                if (!nx_path_exists(bak)) nx_copy_file(it.path, bak);
            }
            if (nx_write_small(it.path, it.want)) {
                changed++;
                std::fprintf(out, "  揃えた: %s = \"%s\"\n", it.path.c_str(), it.want);
            } else {
                std::fprintf(out, "  書けなかった: %s\n", it.path.c_str());
            }
        }
        bool verify = true;
        for (const auto &it : items) {
            std::string v;
            if (!nx_read_small(it.path, v) || nx_trim(v) != it.want) verify = false;
        }
        std::fprintf(out, "  結果: %s (変更 %d)\n\n",
                     verify ? "良い状態" : "未完了", changed);
    }
    if (handled == 0) {
        std::fprintf(out, "対象がありません: %s\n", name ? name : "");
        return 1;
    }
    return 0;
}

// 起動を止める検知を静的に消す。apply=0 なら提示のみ (ドライラン)。
// force_taken=1 は分岐を「取る側」(無条件 b)、0 は「取らない側」(nop) に固定。
extern "C" int nx_guard_run(const char *path, const char *needle, int apply, int pick,
                            int force_taken, FILE *out) {
    if (!out) out = stdout;
    if (!path || !needle || !*needle) return 2;
    std::string target = nx_resolve_target(path);
    auto img = nx::MachOImage::open(target);
    if (!img) { std::fprintf(out, "[skip] not a 64-bit Mach-O: %s\n", target.c_str()); return 1; }

    nx::ImportResolver imports; imports.parse(*img);
    nx::XRefIndex xr; xr.build(*img, imports);
    nx::BackTracker bt(*img, xr);

    // needle が 0x... なら、そのアドレスを含む関数を直接対象にする。
    std::map<uint64_t, int> nearest;   // 関数 -> 文字列参照 (または起点) までの距離
    if (std::strncmp(needle, "0x", 2) == 0 || std::strncmp(needle, "0X", 2) == 0) {
        uint64_t a = std::strtoull(needle, nullptr, 0);
        uint64_t f = bt.function_containing(a);
        std::string fn = bt.function_name(f);
        std::fprintf(out, "  起点 0x%llx の属する関数は 0x%llx %s です。\n",
                     (unsigned long long)a, (unsigned long long)f,
                     fn.empty() ? "(無名)" : fn.c_str());
        nearest[f] = 0;
    } else {
        for (const auto &s : xr.sites()) {
            if (s.is_call || !s.target_symbol.empty()) continue;
            std::string str = img->string_at(s.target_vm, 256);
            if (str.find(needle) == std::string::npos) continue;
            uint64_t f = bt.function_containing(s.addr);
            uint64_t d = s.addr > f ? s.addr - f : f - s.addr;
            auto it = nearest.find(f);
            if (it == nearest.end() || (int)d < it->second) nearest[f] = (int)d;
        }
        if (nearest.empty()) {
            std::fprintf(out, "%s を参照する箇所が見つかりませんでした。\n", needle);
            return 1;
        }
    }

    const auto &starts = bt.func_starts();
    std::vector<nx_guard_cand> cands;
    for (const auto &kv : nearest) {
        uint64_t f = kv.first;
        auto up = std::upper_bound(starts.begin(), starts.end(), f);
        uint64_t end = (up == starts.end()) ? f + 0x8000 : *up;
        if (end <= f) end = f + 0x8000;
        std::string fn = bt.function_name(f);
        for (uint64_t pc = f; pc + 4 <= end; pc += 4) {
            const uint8_t *p = img->at(pc, 4);
            if (!p) break;
            uint32_t w = *(const uint32_t *)p;
            nx::Arm64::Insn in;
            nx::Arm64::decode(pc, w, &in);
            const char *mn = nullptr;
            switch (in.op) {
                case nx::Arm64::Op::Cbz:   mn = "cbz";    break;
                case nx::Arm64::Op::Cbnz:  mn = "cbnz";   break;
                case nx::Arm64::Op::Tbz:   mn = "tbz";    break;
                case nx::Arm64::Op::Tbnz:  mn = "tbnz";   break;
                case nx::Arm64::Op::BCond: mn = "b.cond"; break;
                default: break;
            }
            if (!mn) continue;
            nx_guard_cand c;
            c.func = f; c.site = pc; c.target = in.target;
            c.raw = w; c.mn = mn; c.fname = fn;
            c.dist = (int)(pc > f ? pc - f : f - pc);
            cands.push_back(c);
        }
    }
    // 文字列参照に近い関数・近い分岐を優先 = 一番簡単な候補
    std::sort(cands.begin(), cands.end(),
              [&](const nx_guard_cand &a, const nx_guard_cand &b) {
                  if (nearest[a.func] != nearest[b.func]) return nearest[a.func] < nearest[b.func];
                  return a.dist < b.dist;
              });

    std::fprintf(out, "%s を参照する関数は %zu 個で、条件分岐の候補は %zu 個です。\n",
                 needle, nearest.size(), cands.size());
    int shown = 0;
    for (const auto &c : cands) {
        std::fprintf(out, "  [%d] %s  site=0x%llx -> 0x%llx  in %s (0x%llx)\n",
                     shown, c.mn, (unsigned long long)c.site,
                     (unsigned long long)c.target,
                     c.fname.empty() ? "(無名)" : c.fname.c_str(),
                     (unsigned long long)c.func);
        if (++shown >= 12) break;
    }
    if (cands.empty()) return 1;

    if (pick < 0 || pick >= (int)cands.size()) pick = 0;
    const nx_guard_cand &c = cands[pick];
    uint8_t nb[4];
    if (force_taken) nx_enc_b(c.site, c.target, nb);
    else { nb[0] = 0x1F; nb[1] = 0x20; nb[2] = 0x03; nb[3] = 0xD5; }  // nop
    std::fprintf(out, "選んだ候補 [%d]: 0x%llx の %s を %s に固定します。\n",
                 pick, (unsigned long long)c.site, c.mn,
                 force_taken ? "取る側 (b)" : "取らない側 (nop)");

    // やり方 (クセ) を脳に残す。対象を特定する情報は入れない。
    nx::SkillStore brain;
    brain.load(nx::SkillStore::default_path());
    brain.observe("guard_needle", needle);
    brain.observe("guard_habit", force_taken ? "検知の条件分岐を取る側に固定する"
                                             : "検知の条件分岐を取らない側 (nop) に固定する");
    brain.observe("guard_where", "文字列参照の近くの条件分岐");

    if (!apply) {
        std::fprintf(out, "[dry-run] 実バイナリは変更していません。--apply で適用します。\n");
        brain.observe("guard_result", "提示のみ");
        brain.save(nx::SkillStore::default_path());
        return 0;
    }

    uint64_t off = 0;
    if (!img->vm_to_off(c.site, &off)) {
        std::fprintf(out, "ファイル位置に変換できませんでした。\n");
        return 1;
    }
    std::string bak = target + ".nx-backup";
    {
        std::ifstream probe(bak, std::ios::binary);
        if (probe.good()) {
            std::fprintf(out, "backup が既にあります: %s\n", bak.c_str());
            return 1;
        }
    }
    if (!nx_copy_file(target, bak)) {
        std::fprintf(out, "backup を作れませんでした: %s\n", bak.c_str());
        return 1;
    }
    bool ok = false;
    {
        std::fstream f(target, std::ios::binary | std::ios::in | std::ios::out);
        if (f) {
            f.seekp((std::streamoff)off);
            f.write((const char *)nb, 4);
            f.seekg((std::streamoff)off);
            uint8_t chk[4] = {0, 0, 0, 0};
            f.read((char *)chk, 4);
            ok = (std::memcmp(chk, nb, 4) == 0);
        }
    }
    if (!ok) {
        nx_copy_file(bak, target);   // fail-safe: 戻す
        std::fprintf(out, "書き込みに失敗したので backup から戻しました。\n");
        brain.observe("guard_result", "失敗して戻した");
        brain.save(nx::SkillStore::default_path());
        return 1;
    }
    std::fprintf(out, "書き換えました: %s  offset=0x%llx  backup=%s\n",
                 target.c_str(), (unsigned long long)off, bak.c_str());
    brain.observe("guard_result", "適用した");
    brain.observe("guard_bytes", "4");
    brain.save(nx::SkillStore::default_path());
    return 0;
}

// ---------------------------------------------------------------- fork
// アプリのコピーを作り、そのコピーだけを解析・改変・再署名する。元アプリには触れない。
// 自律性: 各 op は probe(適用可否) と apply(実行) を持ち、実行時に適用可否を
// 判断して順に走らせる。op の並びはデータ (レジストリ) で決まり、コードは分岐しない。
// 統合: 既存の nx_guard_run / nx_bundle_exe / MachOImage をそのまま呼ぶ。
struct nx_fork_ctx {
    std::string src, dst, exe;
    std::vector<std::string> needles;
    int launch = 0;
    int bin_modified = 0;   // バイナリを実際に変えたか (resign の判断に使う)
    FILE *out = nullptr;
};

struct nx_fork_op {
    const char *name;
    int (*probe)(nx_fork_ctx *);
    int (*apply)(nx_fork_ctx *);
};

static std::string nx_shq(const std::string &s) {
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    o += "'";
    return o;
}

static int nx_sh(const std::string &cmd, FILE *out) {
    if (out) std::fprintf(out, "  $ %s\n", cmd.c_str());
    return std::system(cmd.c_str());
}

static int op_clone_probe(nx_fork_ctx *c) {
    if (c->src.empty() || c->dst.empty() || c->src == c->dst) return 0;
    return nx_path_exists(c->src) && !nx_path_exists(c->dst);
}
static int op_clone_apply(nx_fork_ctx *c) {
    // APFS の clonefile で複製する。CoW なので一瞬で、追加容量はほぼ 0。
    // 変えたファイルだけが実体を持つ。clonefile 不可なら ditto に落とす。
    std::string cmd = "cp -cR " + nx_shq(c->src) + " " + nx_shq(c->dst) +
                      " 2>/dev/null || ditto " + nx_shq(c->src) + " " + nx_shq(c->dst);
    if (nx_sh(cmd, c->out) != 0) return -1;
    c->exe = nx_bundle_exe(c->dst);
    return 0;
}

static int op_guard_probe(nx_fork_ctx *c) { return !c->needles.empty() && !c->exe.empty(); }
static int op_guard_apply(nx_fork_ctx *c) {
    for (const auto &n : c->needles) {
        std::fprintf(c->out, "  [guard] %s\n", n.c_str());
        if (nx_guard_run(c->exe.c_str(), n.c_str(), 1, 0, 0, c->out) == 0) {
            c->bin_modified = 1;
            // 退避はバンドルの外へ出す。中に残すと CodeResources の
            // 余計な nested code になり、外側の署名検証が壊れる。
            std::string bak = c->exe + ".nx-backup";
            if (nx_path_exists(bak)) {
                std::string out_bak = c->dst + ".nx-backup";
                std::remove(out_bak.c_str());
                std::rename(bak.c_str(), out_bak.c_str());
            }
        } else {
            std::fprintf(c->out, "  [guard] 適用できませんでした: %s\n", n.c_str());
        }
    }
    return 0;
}

// AE の終了原因 (SSO フラグ SIGNOUT) を、neodax の状態回復関数で良い状態に揃える。
// AE は外部 Plug-ins / Presets / Scripts を .app の「親ディレクトリ」基準で探す。
// fork を別の場所に置くとこれらを見失い、MetaPal.plugin (Metadata Browser) などが
// 読まれない。元の製品フォルダの兄弟を symlink で用意して環境ごと複製する。
static const char *k_sibling_res[] = { "Plug-ins", "Presets", "Scripts" };
static int op_resources_probe(nx_fork_ctx *c) {
    return !c->src.empty() && !c->dst.empty() ? 1 : 0;
}
static int op_resources_apply(nx_fork_ctx *c) {
    std::string sp = nx_dirname(c->src), dp = nx_dirname(c->dst);
    if (sp == dp) return 0;
    for (const char *n : k_sibling_res) {
        std::string s = nx_join_path(sp, n), d = nx_join_path(dp, n);
        if (!nx_path_exists(s) || nx_path_exists(d)) continue;
        nx_sh("ln -s " + nx_shq(s) + " " + nx_shq(d), c->out);
    }
    return 0;
}

static int op_state_probe(nx_fork_ctx *c) { (void)c; return 1; }
static int op_state_apply(nx_fork_ctx *c) {
    return nx_state_heal(NULL, 1, c->out);
}

static int op_resign_probe(nx_fork_ctx *c) { return c->bin_modified && !c->dst.empty(); }
static int op_resign_apply(nx_fork_ctx *c) {
    // 変えた実行ファイルだけ、そのあと外側バンドルだけを署名する。
    // --deep は中の全フレームワークを舐めるので署名には使わない。
    if (!c->exe.empty())
        nx_sh("codesign --force --sign - " + nx_shq(c->exe), c->out);
    if (nx_sh("codesign --force --sign - " + nx_shq(c->dst), c->out) != 0) {
        std::fprintf(c->out, "  [resign] codesign に失敗\n");
        return -1;
    }
    int rc = nx_sh("codesign --verify --deep --strict " + nx_shq(c->dst), c->out);
    std::fprintf(c->out, "  [resign] verify rc=%d%s\n", rc, rc == 0 ? "" : " (署名が不正)");
    return rc == 0 ? 0 : -1;
}

static int op_launch_probe(nx_fork_ctx *c) { return c->launch && !c->dst.empty(); }
static int op_launch_apply(nx_fork_ctx *c) {
    // LaunchServices は同じ bundle id を既存側に寄せたり、早々に終了させることが
    // ある。fork の実体を直接起動する方が確実 (実測で安定)。
    std::string exe = c->exe.empty() ? nx_bundle_exe(c->dst) : c->exe;
    if (exe.empty()) return -1;
    nx_sh("nohup " + nx_shq(exe) + " >/dev/null 2>&1 &", c->out);
    return 0;
}

static const nx_fork_op k_fork_ops[] = {
    { "clone",  op_clone_probe,  op_clone_apply  },
    { "res",    op_resources_probe, op_resources_apply },
    { "state",  op_state_probe,  op_state_apply  },
    { "guard",  op_guard_probe,  op_guard_apply  },
    { "resign", op_resign_probe, op_resign_apply },
    { "launch", op_launch_probe, op_launch_apply },
};

extern "C" int nx_fork_run(const char *src, const char *dst,
                           const char *needles_csv, int launch, FILE *out) {
    if (!out) out = stdout;
    if (!src || !dst || !*dst) {
        std::fprintf(out, "使い方: neodax --fork <src.app> --to <dst.app> "
                          "[--fork-patch S] [--fork-launch]\n");
        return 2;
    }
    nx_fork_ctx c;
    c.src = src; c.dst = dst; c.launch = launch; c.out = out;
    if (needles_csv && *needles_csv) {
        std::string s(needles_csv), cur;
        for (char ch : s) {
            if (ch == '\x1f') { if (!cur.empty()) c.needles.push_back(cur); cur.clear(); }
            else cur.push_back(ch);
        }
        if (!cur.empty()) c.needles.push_back(cur);
    }
    std::fprintf(out, "=== fork ===\n  src: %s\n  dst: %s\n", src, dst);
    int ran = 0;
    for (const auto &op : k_fork_ops) {
        int applicable = op.probe ? op.probe(&c) : 0;
        std::fprintf(out, "  [op] %-7s probe=%s\n", op.name, applicable ? "apply" : "skip");
        if (!applicable) continue;
        if (op.apply && op.apply(&c) != 0)
            std::fprintf(out, "  [op] %s failed\n", op.name);
        ran++;
    }
    std::fprintf(out, "実行した op: %d\n", ran);
    return ran ? 0 : 1;
}
