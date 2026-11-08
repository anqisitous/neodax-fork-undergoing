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
#include <libproc.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CFNetwork/CFNetwork.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ApplicationServices/ApplicationServices.h>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <Carbon/Carbon.h>
#include <Security/Authorization.h>
#include <Security/AuthorizationTags.h>

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

// ============================================================ AnnulusAnalyzer
//
// depflow_build_annuli と同じ概念の環 (call-graph SCC) を、静的な呼び出し関係から作る。
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

class AnnulusAnalyzer {
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
    void find_annuli();                       // Tarjan SCC

    struct StallAnnulus {
        std::vector<std::string> members;
        std::vector<std::string> wait_edges;  // 切る候補
        std::vector<std::string> spawn_edges;
    };
    std::vector<StallAnnulus> stall_annuli() const;
    const std::vector<Node> &nodes() const { return nodes_; }
    size_t annulus_count() const { return annuli_; }

    static bool is_wait_symbol(const std::string &s);
    static bool is_spawn_symbol(const std::string &s);

private:
    std::vector<Node> nodes_;
    std::map<uint64_t, size_t> addr_to_node_;
    std::map<std::string, size_t> ext_to_node_;
    size_t annuli_ = 0;

    const MachOImage *img_ = nullptr;
    std::vector<uint64_t> func_starts_;
    uint64_t func_of(uint64_t addr) const;
};

bool AnnulusAnalyzer::is_wait_symbol(const std::string &s) {
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

bool AnnulusAnalyzer::is_spawn_symbol(const std::string &s) {
    static const char *pat[] = {
        "Spawn", "spawn", "Launch", "launch", "Relaunch", "relaunch",
        "CreateProcess", "NSTask", "Fork", "fork", "Exec", "exec",
        "OpenApplication", "LSOpen", "XPC", NULL
    };
    for (int i = 0; pat[i]; i++)
        if (s.find(pat[i]) != std::string::npos) return true;
    return false;
}

uint64_t AnnulusAnalyzer::func_of(uint64_t addr) const {
    if (func_starts_.empty()) return addr;
    auto it = std::upper_bound(func_starts_.begin(), func_starts_.end(), addr);
    if (it == func_starts_.begin()) return addr;
    return *(--it);
}

void AnnulusAnalyzer::build(const MachOImage &img, const XRefIndex &xr) {
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

void AnnulusAnalyzer::find_annuli() {
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
                        int id = (int)annuli_++;
                        for (size_t w : members) nodes_[w].scc = id;
                    } else {
                        nodes_[vv].scc = -1;
                    }
                }
            }
        }
    }
}

std::vector<AnnulusAnalyzer::StallAnnulus> AnnulusAnalyzer::stall_annuli() const {
    std::map<int, StallAnnulus> by_scc;
    for (const auto &n : nodes_) {
        if (n.scc < 0) continue;
        StallAnnulus &r = by_scc[n.scc];
        if (!n.name.empty())
            r.members.push_back(n.name + (n.is_wait ? "   [WAIT]" : (n.is_spawn ? "   [SPAWN]" : "")));
        for (const auto &e : n.callee_ext) {
            if (is_wait_symbol(e)) r.wait_edges.push_back(n.name + "  --wait-->  " + e);
            if (is_spawn_symbol(e)) r.spawn_edges.push_back(n.name + "  --spawn--> " + e);
        }
    }
    std::vector<StallAnnulus> out;
    for (auto &kv : by_scc) {
        // 待ちエッジを持つ環を先に、持たない環も含めて返す。
        // 持たない環は「健全な相互再帰」、持つ環が「待ちが閉じた環」の候補。
        out.push_back(kv.second);
    }
    std::sort(out.begin(), out.end(), [](const StallAnnulus &a, const StallAnnulus &b) {
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

    void run(const AnnulusAnalyzer &ra, uint64_t entry);
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

void EdgeClassifier::run(const AnnulusAnalyzer &ra, uint64_t entry) {
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
            e.wait = AnnulusAnalyzer::is_wait_symbol(sym);
            e.spawn = AnnulusAnalyzer::is_spawn_symbol(sym);
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
    void build(const AnnulusAnalyzer &ra, uint64_t entry);
    bool reaches(uint64_t f) const { return reachable_.count(f) != 0; }
    bool important(uint64_t f) const { return important_.count(f) != 0; }
    size_t reachable_count() const { return reachable_.size(); }
    size_t important_count() const { return important_.size(); }

private:
    std::set<uint64_t> reachable_;
    std::set<uint64_t> important_;
};

void Importance::build(const AnnulusAnalyzer &ra, uint64_t entry) {
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

    void build(const AnnulusAnalyzer &ra) {
        ns_ = &ra.nodes();
        for (size_t i = 0; i < ns_->size(); i++)
            if ((*ns_)[i].addr) by_addr_[(*ns_)[i].addr] = i;
    }

    // 入口から、効果を呼ぶ節点までの経路を 1 本見つける。
    bool find_effect(uint64_t entry, const std::vector<std::string> &pats,
                     Found *out) const;

private:
    const std::vector<AnnulusAnalyzer::Node> *ns_ = nullptr;
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
        bool annuli = false;      // 環 (SCC) 解析モード
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
    AnnulusAnalyzer annuli;
    annuli.build(*img, xr);
    annuli.find_annuli();
    Importance imp;
    imp.build(annuli, img->entry());
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
        AnnulusAnalyzer ra;
        ra.build(*img, xr);
        ra.find_annuli();

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
    if (opt_.annuli) {
        AnnulusAnalyzer ra;
        ra.build(*img, xr);
        ra.find_annuli();
        auto annuli = ra.stall_annuli();
        (void)annuli;
        fprintf(out, "  nodes=%zu  annuli(SCC>1)=%zu\n",
                ra.nodes().size(), ra.annulus_count());
        size_t shown = 0;
        for (const auto &r : ra.stall_annuli()) {
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
    pf.build(annuli);
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
// mode: 0=既定(逆track) 1=--annuli 2=--prune 3=--files

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
        case 1: opt.annuli = true; break;
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
// neodax 本来の環は depflow_build_annuli が作る。
// 戻り値由来の呼び出し辺 (DEPFLOW_ORIGIN_CALLEE_RET) だけを
// Kosaraju SCC でまとめたもの。生の call 命令は使わない。
// ここはその結果 (summaries[i].annulus_id) を読むだけで、作り直さない。
//
extern "C" int nx_annuli_report(macho_binary_t *bin, depflow_graph_t *g, FILE *out) {
    if (!out) out = stdout;
    // どこで環が生まれなかったかが分かるように、まず数を出す。
    std::fprintf(out, "関数は %d 個で、シンボルは %d 個で、xref は %d 個で、要約は %d 個で、環は %d 個です。\n",
                 bin ? bin->nfunctions : 0, bin ? bin->nsymbols : 0,
                 bin ? bin->nxrefs : 0,
                 g ? g->summary_count : 0, g ? g->annulus_count : 0);
    if (!bin || !g || !g->summaries || g->summary_count <= 0) {
        std::fprintf(out, "意味がありませんでした\n");
        return 0;
    }

    // 環は辺から自分で求める。depflow の annulus_id は戻り値依存辺しか見ておらず、
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
        if (g->summaries && fi < g->summary_count && g->summaries[fi].annulus_id >= 0)
            return g->summaries[fi].annulus_id;
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
    auto t0 = std::chrono::steady_clock::now();
    for (const auto &f : files) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count() > 5000) {
            std::fprintf(out, "[5秒上限] 残りは打ち切り\n");
            break;
        }
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
    struct stat rst;
    if (stat(root.c_str(), &rst) == 0 && S_ISDIR(rst.st_mode)) {
        std::vector<std::string> roots;
        nx_walk_macho(root, roots, 0);
        std::fprintf(out, "ディレクトリ: %zu 個の Mach-O を起点にします。\n", roots.size());
        for (const auto &r : roots) queue.push_back({0, r});
    } else {
        queue.push_back({0, root});
    }
    int total = 0, unresolved = 0, files = 0;
    auto t0 = std::chrono::steady_clock::now();
    // dyld は @rpath を「読み込み連鎖全体の rpath」で解決する。
    // ローダ単体の rpath だけでは足りないので、走査した全イメージの
    // rpath を集めて使う (ルートを最初に処理するので順序も保たれる)。
    std::vector<std::string> all_rpaths;
    while (!queue.empty()) {
        int depth = queue.front().first;
        std::string p = queue.front().second;
        queue.erase(queue.begin());
        if (!seen.insert(p).second) continue;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count() > 10000) {
            std::fprintf(out, "[10秒上限] 打ち切り\n");
            break;
        }
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
    // NGL は Group Container 側にも V2 の通知フラグを持つ。
    // アプリ本体側だけ揃えても、こちらが SIGNOUT のままだとサインアウトされる。
    { "ngl", NULL,
      { { ".adobelicnotificationV2", "SIGNIN" } } },
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

// Group Containers の "<TEAM>.<suffix>" を探す (TEAM は環境で変わる)。
static std::string nx_find_suffix_in(const std::string &dir, const std::string &suffix) {
    DIR *d = opendir(dir.c_str());
    if (!d) return {};
    std::string found;
    while (struct dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() >= suffix.size() &&
            n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) {
            found = nx_join_path(dir, n);
            break;
        }
    }
    closedir(d);
    return found;
}

static std::string nx_find_group_container(const std::string &home, const std::string &suffix) {
    return nx_find_suffix_in(nx_join_path(home, "Library/Group Containers"), suffix);
}

extern "C" int nx_state_heal(const char *name, int apply, FILE *out) {
    if (!out) out = stdout;
    const char *h = std::getenv("HOME");
    std::string home = h ? h : "";
    int handled = 0;
    for (const auto &r : k_state_rules) {
        if (name && *name && std::string(name) != r.name) continue;
        handled++;
        std::string dir;
        if (r.rel_dir && *r.rel_dir) {
            dir = nx_join_path(home, r.rel_dir);
        } else {
            // rel_dir が無い規則は Group Container を名前で探す。
            std::string gc = nx_find_group_container(home, "com.adobe.NGL");
            if (gc.empty()) {
                std::fprintf(out, "=== %s ===\n  Group Container が見つかりません\n\n", r.name);
                continue;
            }
            dir = nx_join_path(gc, "licflags");
        }
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

// NGL の状態 (Group Container / Application Scripts / ~/Library/NGL) を
// アクティブな場所から退避する。削除ではなく rename で戻せる形にする。
// apply=0 は提示のみ。
extern "C" int nx_ngl_reset(int apply, FILE *out) {
    if (!out) out = stdout;
    const char *h = std::getenv("HOME");
    std::string home = h ? h : "";

    std::vector<std::string> targets;
    std::string gc = nx_find_group_container(home, "com.adobe.NGL");
    if (!gc.empty()) targets.push_back(gc);
    std::string scripts = nx_find_suffix_in(
        nx_join_path(home, "Library/Application Scripts"), "com.adobe.NGL");
    if (!scripts.empty()) targets.push_back(scripts);
    targets.push_back(nx_join_path(home, "Library/NGL"));

    std::vector<std::string> existing;
    for (const auto &p : targets)
        if (nx_path_exists(p)) existing.push_back(p);

    if (existing.empty()) {
        std::fprintf(out, "NGL の状態は見つかりませんでした。\n");
        return 1;
    }

    char ts[32] = {0};
    std::time_t now = std::time(NULL);
    std::strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", std::localtime(&now));

    for (const auto &p : existing) {
        std::string bak = p + ".nx-backup-" + ts;
        if (!apply) {
            std::fprintf(out, "[dry-run] %s -> %s\n", p.c_str(), bak.c_str());
            continue;
        }
        if (std::rename(p.c_str(), bak.c_str()) == 0) {
            std::fprintf(out, "退避: %s -> %s\n", p.c_str(), bak.c_str());
        } else {
            std::fprintf(out, "退避できません: %s\n", p.c_str());
        }
    }
    if (!apply) std::fprintf(out, "--apply で退避します。\n");
    return 0;
}

// ログイン/ライセンスダイアログの静的予測。
//
// back-track (実測した呼び出し鎖):
//   NGLWMessageManager::_getSUSIURL
//     -> NglController::SignInWithoutSpinner
//     -> NGL auth workflow -> CEF
//     -> Contents/Resources/ngl/resources/ui/index.html ("Adobe Licensing")
//   aelicensing::HaveUser -> GetIMSUserID
//   aelicensing::OnLicenseChanged -> licensingglue::GetNGLProfileAsJSON
//     -> AccessProfileData::Deserialize("NGL_ACCESSPROFILE")
//
// 予測式: login = !profile || !sso
//   profile = Group Container の ASNPsv2 に NGL アクセスプロファイルがある
//   sso     = .adobelicnotification == SIGNIN かつ .adobestatusnotification == IMS_ACTIVE
static bool nx_dir_has_entries(const std::string &p) {
    DIR *d = opendir(p.c_str());
    if (!d) return false;
    bool any = false;
    while (struct dirent *e = readdir(d)) {
        if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) { any = true; break; }
    }
    closedir(d);
    return any;
}

extern "C" int nx_predict_login(FILE *out) {
    if (!out) out = stdout;
    const char *h = std::getenv("HOME");
    std::string home = h ? h : "";

    std::string oobe = nx_join_path(home, "Library/Application Support/Adobe/OOBE");
    std::string prefs;
    nx_read_small(nx_join_path(oobe, "com.adobe.acc.container.default.prefs"), prefs);
    bool logged_out = prefs.find("LoggedOutModeEnabled\">true") != std::string::npos;
    bool logged_in_flag = prefs.find("LoggedOutModeEnabled\">false") != std::string::npos;

    std::string gc = nx_find_group_container(home, "com.adobe.NGL");
    bool have_gc = !gc.empty();

    std::string adobe = nx_join_path(home, "Library/Application Support/Adobe");
    std::string st, lic, v2;
    nx_read_small(nx_join_path(adobe, ".adobestatusnotification"), st);
    nx_read_small(nx_join_path(adobe, ".adobelicnotification"), lic);
    if (have_gc) nx_read_small(nx_join_path(gc, "licflags/.adobelicnotificationV2"), v2);
    st = nx_trim(st); lic = nx_trim(lic); v2 = nx_trim(v2);
    bool sso = (st == "IMS_ACTIVE") && (lic == "SIGNIN") && (v2 == "SIGNIN");

    // 判定は「起動時点で持続している状態」だけを使う。
    // NGL コンテナの有無は on-demand に作り直されるので判定に使わない。
    const char *verdict;
    const char *why;
    if (logged_out) {
        verdict = "YES"; why = "OOBE LoggedOutModeEnabled=true (持続マーカー)";
    } else if (logged_in_flag && sso) {
        verdict = "NO";  why = "LoggedOutMode=false かつ SSO/V2 が SIGNIN";
    } else {
        verdict = "UNKNOWN"; why = "持続マーカーが無い (NGL は起動時に生成されるため現在の不在は根拠にならない)";
    }

    char ts[32] = {0};
    std::time_t now = std::time(NULL);
    std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
    std::fprintf(out, "=== login dialog prediction (static, observed_at=%s) ===\n", ts);
    std::fprintf(out, "  OOBE prefs           : %s\n", prefs.empty() ? "(無し)" : "present");
    std::fprintf(out, "  LoggedOutModeEnabled : %s\n", logged_out ? "true" : logged_in_flag ? "false" : "(不明)");
    std::fprintf(out, "  SSO status           : \"%s\"\n", st.c_str());
    std::fprintf(out, "  SSO lic notification : \"%s\"\n", lic.c_str());
    std::fprintf(out, "  SSO V2 notification  : \"%s\"\n", v2.empty() ? "(無し)" : v2.c_str());
    std::fprintf(out, "  sso (3 つ揃ったか)   : %s\n", sso ? "true" : "false");
    std::fprintf(out, "  NGL container        : %s (informative only; on-demand)\n",
                 have_gc ? "present" : "absent now");
    std::fprintf(out, "LOGIN_DIALOG=%s\n", verdict);
    std::fprintf(out, "  reason: %s\n", why);
    if (logged_out) {
        std::fprintf(out, "  chain: aelicensing::HaveUser -> GetIMSUserID (空)\n");
        std::fprintf(out, "         _getSUSIURL -> NglController::SignInWithoutSpinner -> NGL UI\n");
    }
    return logged_out ? 1 : (logged_in_flag && sso ? 0 : 2);
}

// 再帰削除 (purge 用)。シンボリックリンクは辿らない。
static bool nx_rm_rf(const std::string &p) {
    struct stat st;
    if (lstat(p.c_str(), &st) != 0) return false;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p.c_str());
        if (d) {
            while (struct dirent *e = readdir(d)) {
                if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
                nx_rm_rf(nx_join_path(p, e->d_name));
            }
            closedir(d);
        }
        return rmdir(p.c_str()) == 0;
    }
    return unlink(p.c_str()) == 0;
}

// AE のログイン/アクセス状態とログを、アクティブな場所から退避する。
// purge=1 なら、これまでに作った .nx-purged-* / .nx-backup* も完全削除する。
extern "C" int nx_ae_login_purge(int apply, int purge, FILE *out) {
    if (!out) out = stdout;
    const char *h = std::getenv("HOME");
    std::string home = h ? h : "";

    char ts[32] = {0};
    std::time_t now = std::time(NULL);
    std::strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", std::localtime(&now));

    std::vector<std::string> targets;
    std::string gc = nx_find_group_container(home, "com.adobe.NGL");
    if (!gc.empty()) targets.push_back(gc);
    std::string scripts = nx_find_suffix_in(
        nx_join_path(home, "Library/Application Scripts"), "com.adobe.NGL");
    if (!scripts.empty()) targets.push_back(scripts);
    targets.push_back(nx_join_path(home, "Library/NGL"));

    const char *rel[] = {
        "Library/Application Support/Adobe/.adobelicnotification",
        "Library/Application Support/Adobe/.adobelicnotification.nx-backup",
        "Library/Application Support/Adobe/.adobestatusnotification",
        "Library/Application Support/Adobe/.adobestatusnotification.nx-backup",
        "Library/Application Support/Adobe/.adobefeatureflagnotification",
        "Library/Application Support/Adobe/OOBE/logged_out_guid",
        "Library/Application Support/Adobe/OOBE/logged_out_guid.nx-backup",
        "Library/Application Support/Adobe/OOBE/temp_ins_lbs_wid",
        "Library/Application Support/Adobe/OOBE/temp_lbs_wid",
        "Library/Application Support/Adobe/OOBE/com.adobe.acc.container.default.prefs",
        "Library/Application Support/Adobe/OOBE/com.adobe.acc.container.default.prefs.nx-backup",
        "Library/Application Support/Adobe/OOBE/com.adobe.acc.container/loggedOutFeatures/AnonymousUser",
        "Library/Application Support/Adobe/UPI",
        "Library/Logs/Adobe/Adobe After Effects 2026",
        "Library/Logs/Adobe/AdobeCRCommon",
        "Library/Logs/Adobe/AdobeGCClient",
        "Library/Logs/Adobe/UPI",
        "Library/Logs/Adobe/com.adobe.dunamis",
        "Library/Logs/Adobe/GrowthSDK/Production",
    };
    for (const char *r : rel) targets.push_back(nx_join_path(home, r));

    int moved = 0, missing = 0;
    for (const auto &p : targets) {
        if (!nx_path_exists(p)) { missing++; continue; }
        std::string bak = p + ".nx-purged-" + ts;
        if (!apply) {
            std::fprintf(out, "[dry-run] %s -> %s\n", p.c_str(), bak.c_str());
            moved++;
            continue;
        }
        if (std::rename(p.c_str(), bak.c_str()) == 0) {
            std::fprintf(out, "退避: %s -> %s\n", p.c_str(), bak.c_str());
            moved++;
        } else {
            std::fprintf(out, "退避できません: %s\n", p.c_str());
        }
    }

    int purged = 0;
    if (apply && purge) {
        const char *dirs[] = {
            "Library/Group Containers",
            "Library/Application Scripts",
            "Library",
            "Library/Application Support/Adobe",
            "Library/Application Support/Adobe/OOBE",
            "Library/Logs/Adobe",
            "Library/Logs/Adobe/GrowthSDK",
        };
        for (const char *rd : dirs) {
            std::string dir = nx_join_path(home, rd);
            DIR *d = opendir(dir.c_str());
            if (!d) continue;
            std::vector<std::string> kill;
            while (struct dirent *e = readdir(d)) {
                std::string n = e->d_name;
                if (n.find(".nx-purged-") != std::string::npos ||
                    n.find(".nx-backup") != std::string::npos)
                    kill.push_back(nx_join_path(dir, n));
            }
            closedir(d);
            for (const auto &k : kill) {
                if (nx_rm_rf(k)) {
                    std::fprintf(out, "削除: %s\n", k.c_str());
                    purged++;
                }
            }
        }
    }

    std::fprintf(out, "AE login purge: %s 退避 %d 件 / 完全削除 %d 件 (存在しない %d 件)\n",
                 apply ? "実行" : "dry-run", moved, purged, missing);
    if (!apply) std::fprintf(out, "--apply で退避します。 --purge も付けるとバックアップを完全削除します。\n");
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
    int move = 0;           // 1=コピーせず移動する (元は解体)
    int bin_modified = 0;      // Mach-O を変えたか (実行ファイルを再署名)
    int bundle_modified = 0;   // Info.plist を変えたか (外側を再署名)
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
    nx_sh("mkdir -p " + nx_shq(nx_dirname(c->dst)), c->out);
    if (c->move) {
        // 移動: 同一ボリュームなら rename で一瞬。元は残らない (=解体)。
        if (nx_sh("mv " + nx_shq(c->src) + " " + nx_shq(c->dst), c->out) != 0) return -1;
        if (nx_path_exists(c->src)) {
            std::fprintf(c->out, "  [clone] 移動後も元が残っています: %s\n", c->src.c_str());
            return -1;
        }
    } else {
        // 複製: APFS の clonefile で CoW 複製。一瞬で、追加容量はほぼ 0。
        // 変えたファイルだけが実体を持つ。clonefile 不可なら ditto に落とす。
        std::string cmd = "cp -cR " + nx_shq(c->src) + " " + nx_shq(c->dst) +
                          " 2>/dev/null || ditto " + nx_shq(c->src) + " " + nx_shq(c->dst);
        if (nx_sh(cmd, c->out) != 0) return -1;
    }
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
        // move モードでは兄弟も「移動」する。元の製品フォルダは空になり解体される。
        // copy モードでは元を残すので symlink で参照させる。
        if (c->move)
            nx_sh("mv " + nx_shq(s) + " " + nx_shq(d), c->out);
        else
            nx_sh("ln -s " + nx_shq(s) + " " + nx_shq(d), c->out);
    }
    return 0;
}

// メニューバー等に出るアプリ名は CFBundleName / CFBundleDisplayName 由来。
// fork は元と同じ名前になるので、コピー側だけ dst 名に変える。Info.plist は
// 署名の封印対象なので、変えたら外側を再署名する。
static std::string nx_plist_get(const std::string &pl, const char *key) {
    std::string cmd = "/usr/libexec/PlistBuddy -c " +
                      nx_shq(std::string("Print :") + key) + " " + nx_shq(pl) +
                      " 2>/dev/null";
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) return {};
    std::string out;
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    pclose(p);
    return nx_trim(out);
}

static int op_name_probe(nx_fork_ctx *c) {
    return !c->dst.empty() && nx_path_exists(c->dst + "/Contents/Info.plist");
}
static int op_name_apply(nx_fork_ctx *c) {
    std::string pl = c->dst + "/Contents/Info.plist";
    std::string base = c->dst;
    size_t sl = base.find_last_of('/');
    if (sl != std::string::npos) base = base.substr(sl + 1);
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".app") == 0)
        base = base.substr(0, base.size() - 4);
    if (base.empty()) return 0;
    bool changed = false;
    for (const char *k : { "CFBundleName", "CFBundleDisplayName" }) {
        if (nx_plist_get(pl, k) == base) continue;
        std::string setcmd = "/usr/libexec/PlistBuddy -c " +
                             nx_shq(std::string("Set :") + k + " " + base) + " " +
                             nx_shq(pl);
        if (nx_sh( setcmd, c->out) == 0) changed = true;
    }
    if (changed) {
        c->bundle_modified = 1;
        std::fprintf(c->out, "  [name] CFBundleName/DisplayName -> \"%s\"\n", base.c_str());
    }
    return 0;
}

static int op_state_probe(nx_fork_ctx *c) { (void)c; return 1; }
static int op_state_apply(nx_fork_ctx *c) {
    return nx_state_heal(NULL, 1, c->out);
}

static int op_resign_probe(nx_fork_ctx *c) {
    return (c->bin_modified || c->bundle_modified) && !c->dst.empty();
}
static int op_resign_apply(nx_fork_ctx *c) {
    // 変えた実行ファイルだけ、そのあと外側バンドルだけを署名する。
    // --deep は中の全フレームワークを舐めるので署名には使わない。
    if (c->bin_modified && !c->exe.empty())
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
    { "name",   op_name_probe,   op_name_apply   },
    { "state",  op_state_probe,  op_state_apply  },
    { "guard",  op_guard_probe,  op_guard_apply  },
    { "resign", op_resign_probe, op_resign_apply },
    { "launch", op_launch_probe, op_launch_apply },
};

extern "C" int nx_fork_run(const char *src, const char *dst,
                           const char *needles_csv, int launch, int move, FILE *out) {
    if (!out) out = stdout;
    if (!src || !dst || !*dst) {
        std::fprintf(out, "使い方: neodax --fork <src.app> --to <dst.app> "
                          "[--fork-patch S] [--fork-launch]\n");
        return 2;
    }
    nx_fork_ctx c;
    c.src = src; c.dst = dst; c.launch = launch; c.move = move; c.out = out;
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

// ---------------------------------------------------------------- release
// 止まったプロセスを neodax 自身で解放する。実行ファイルパスの部分一致で対象を選び、
// --apply のときだけ SIGTERM を送る。外部ツールは使わない (libproc + kill)。
// サインイン窓や更新ワークフローがアプリの終了を妨げているときのための手続き。
extern "C" int nx_release_proc(const char *needle, int apply, FILE *out) {
    if (!out) out = stdout;
    if (!needle || !*needle) {
        std::fprintf(out, "使い方: neodax --term <path-substr> [--apply]\n");
        return 2;
    }
    int npid = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    if (npid <= 0) return -1;
    std::vector<pid_t> pids((size_t)npid / sizeof(pid_t) + 64);
    npid = proc_listpids(PROC_ALL_PIDS, 0, pids.data(),
                         (int)(pids.size() * sizeof(pid_t)));
    if (npid <= 0) return -1;
    int count = npid / (int)sizeof(pid_t);
    pid_t self = getpid();
    int hits = 0;
    for (int i = 0; i < count; i++) {
        pid_t pid = pids[i];
        if (pid <= 1 || pid == self) continue;
        char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
        if (proc_pidpath(pid, path, sizeof(path)) <= 0) continue;
        if (!std::strstr(path, needle)) continue;
        hits++;
        std::fprintf(out, "  %-8d %s\n", (int)pid, path);
        if (apply) {
            if (kill(pid, SIGTERM) == 0)
                std::fprintf(out, "           -> SIGTERM 送信\n");
            else
                std::fprintf(out, "           -> SIGTERM 失敗\n");
        }
    }
    std::fprintf(out, "%s: %d 件%s\n", needle, hits,
                 apply ? " (SIGTERM 送信済)" : " (dry-run; --apply で送信)");
    return hits ? 0 : 1;
}

// ---------------------------------------------------------------- patch / resign
// 関数先頭を ret に潰す。vaddr は Mach-O の仮想アドレス。
extern "C" int nx_patch_ret(const char *path, uint64_t vaddr, FILE *out) {
    if (!out) out = stdout;
    if (!path) return 2;
    auto img = nx::MachOImage::open(path);
    if (!img) { std::fprintf(out, "[skip] not a 64-bit Mach-O: %s\n", path); return 1; }
    uint64_t off = 0;
    if (!img->vm_to_off(vaddr, &off)) {
        std::fprintf(out, "vaddr をファイル位置にできません: 0x%llx\n",
                     (unsigned long long)vaddr);
        return 1;
    }
    FILE *f = std::fopen(path, "r+b");
    if (!f) { std::fprintf(out, "開けません: %s\n", path); return 1; }
    std::fseek(f, (long)off, SEEK_SET);
    unsigned char ret[4] = { 0xD6, 0x5F, 0x03, 0xC0 };
    if (std::fwrite(ret, 1, 4, f) != 4) {
        std::fclose(f);
        std::fprintf(out, "書き込み失敗\n");
        return 1;
    }
    std::fclose(f);
    std::fprintf(out, "ret を書き込み: vaddr 0x%llx (file 0x%llx)\n",
                 (unsigned long long)vaddr, (unsigned long long)off);
    return 0;
}

static int nx_hexval(int c);
extern "C" int nx_patch_bytes(const char *path, uint64_t vaddr, const char *hex, FILE *out) {
    if (!out) out = stdout;
    if (!path || !hex) return 2;
    auto img = nx::MachOImage::open(path);
    if (!img) { std::fprintf(out, "[skip] not a 64-bit Mach-O: %s\n", path); return 1; }
    uint64_t off = 0;
    if (!img->vm_to_off(vaddr, &off)) { std::fprintf(out, "vaddr を変換できません\n"); return 1; }
    std::vector<unsigned char> b;
    int hi = -1;
    for (const char *p = hex; *p; p++) {
        int v = nx_hexval(*p);
        if (v < 0) { hi = -1; continue; }
        if (hi < 0) hi = v; else { b.push_back((unsigned char)((hi << 4) | v)); hi = -1; }
    }
    if (b.empty()) { std::fprintf(out, "hex が空\n"); return 1; }
    FILE *f = std::fopen(path, "r+b");
    if (!f) { std::fprintf(out, "開けません\n"); return 1; }
    std::fseek(f, (long)off, SEEK_SET);
    size_t w = std::fwrite(b.data(), 1, b.size(), f);
    std::fclose(f);
    std::fprintf(out, "patch: %zu バイト vaddr 0x%llx (file 0x%llx)\n",
                 w, (unsigned long long)vaddr, (unsigned long long)off);
    return w == b.size() ? 0 : 1;
}

// ad-hoc 再署名 (codesign --force --sign -) + 検証。
extern "C" int nx_resign(const char *path, FILE *out) {
    if (!out) out = stdout;
    if (!path || !*path) return 2;
    std::string q = "'";
    for (const char *p = path; *p; p++) { if (*p == '\'') q += "'\\''"; else q += *p; }
    q += "'";
    std::string sign = "codesign --force --sign - " + q;
    int rc = std::system(sign.c_str());
    std::fprintf(out, "codesign rc=%d\n", rc);
    std::string verify = "codesign --verify --strict " + q + " 2>&1";
    int vr = std::system(verify.c_str());
    std::fprintf(out, "verify rc=%d\n", vr);
    return (rc == 0 && vr == 0) ? 0 : 1;
}

// ---------------------------------------------------------------- move (auth)
// Authorization Services で root として mv する。Finder も AppleScript も使わない。
// 初回のみ管理者パスワードのダイアログが出る。
extern "C" int nx_move_auth(const char *src, const char *dst, FILE *out) {
    if (!out) out = stdout;
    if (!src || !dst) return 2;
    AuthorizationRef auth = NULL;
    OSStatus st = AuthorizationCreate(NULL, kAuthorizationEmptyEnvironment,
                                      kAuthorizationFlagDefaults, &auth);
    if (st != errAuthorizationSuccess) {
        std::fprintf(out, "AuthorizationCreate 失敗: %d\n", (int)st);
        return 1;
    }
    char *args[3];
    args[0] = (char *)src;
    args[1] = (char *)dst;
    args[2] = NULL;
    FILE *pipe = NULL;
    st = AuthorizationExecuteWithPrivileges(auth, "/bin/mv",
                                           kAuthorizationFlagDefaults, args, &pipe);
    if (st != errAuthorizationSuccess) {
        std::fprintf(out, "権限実行 失敗: %d (-60006 はキャンセル)\n", (int)st);
        AuthorizationFree(auth, kAuthorizationFlagDefaults);
        return 1;
    }
    if (pipe) {
        char buf[256];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, pipe)) > 0) fwrite(buf, 1, n, out);
        pclose(pipe);
    }
    AuthorizationFree(auth, kAuthorizationFlagDestroyRights);
    struct stat s2;
    if (stat(dst, &s2) == 0) {
        std::fprintf(out, "move 完了 (root): %s -> %s\n", src, dst);
        return 0;
    }
    std::fprintf(out, "move 失敗: dst がありません\n");
    return 1;
}

// ---------------------------------------------------------------- priv-run
// 管理者パスワードのダイアログを出し、任意のツールを root で一度だけ実行する。
// 「パスワード許可の開始手続き」を neodax 自身が持つ。move / chown / その他に使う。
extern "C" int nx_priv_exec(const char *tool, char *const args[], FILE *out) {
    if (!out) out = stdout;
    if (!tool || !*tool) {
        std::fprintf(out, "使い方: neodax --priv-run <tool> [args...]\n");
        return 2;
    }
    AuthorizationRef auth = NULL;
    OSStatus st = AuthorizationCreate(NULL, kAuthorizationEmptyEnvironment,
                                      kAuthorizationFlagDefaults, &auth);
    if (st != errAuthorizationSuccess) {
        std::fprintf(out, "AuthorizationCreate 失敗: %d\n", (int)st);
        return 1;
    }
    FILE *pipe = NULL;
    st = AuthorizationExecuteWithPrivileges(auth, tool, kAuthorizationFlagDefaults,
                                           args, &pipe);
    if (st != errAuthorizationSuccess) {
        std::fprintf(out, "権限実行 失敗: %d (-60006 はキャンセル)\n", (int)st);
        AuthorizationFree(auth, kAuthorizationFlagDefaults);
        return 1;
    }
    if (pipe) {
        char b[512];
        size_t n;
        while ((n = fread(b, 1, sizeof b, pipe)) > 0) fwrite(b, 1, n, out);
    }
    int rc = pipe ? pclose(pipe) : 0;
    AuthorizationFree(auth, kAuthorizationFlagDestroyRights);
    std::fprintf(out, "priv-run 完了: %s (status=%d)\n", tool, rc);
    return 0;
}

// ---------------------------------------------------------------- move
// 同一ボリュームの rename だけで移動する。コピーなし、Finder なし。
// /Applications への書込みには macOS の App Management 許可が必要。
extern "C" int nx_move(const char *src, const char *dst, FILE *out) {
    if (!out) out = stdout;
    if (!src || !dst) return 2;
    if (rename(src, dst) != 0) {
        int e = errno;
        std::fprintf(out, "move 失敗: %s (errno=%d %s)", src, e, std::strerror(e));
        if (e == EPERM || e == EACCES)
            std::fprintf(out, " — App Management の許可が必要");
        else if (e == EXDEV)
            std::fprintf(out, " — 別ボリュームなので rename 不可 (コピーが必要)");
        std::fprintf(out, "\n");
        return 1;
    }
    std::fprintf(out, "move 完了: %s -> %s\n", src, dst);
    return 0;
}

// ---------------------------------------------------------------- finder move
// Finder は App Management の対象外なので、Finder に move を依頼する。
// 初回だけ Automation 許可 (Finder を操作) が必要。以後は無言で動く。
extern "C" int nx_finder_move(const char *src, const char *dst_folder, FILE *out) {
    if (!out) out = stdout;
    if (!src || !dst_folder) return 2;
    struct stat stt;
    if (stat(dst_folder, &stt) != 0) {
        std::string p(dst_folder), cur;
        for (size_t i = 0; i < p.size(); i++) {
            cur += p[i];
            if (p[i] == '/' && cur.size() > 1) mkdir(cur.c_str(), 0755);
        }
        if (!p.empty()) mkdir(p.c_str(), 0755);
        std::fprintf(out, "dst を作成: %s\n", dst_folder);
    }
    FSRef s, d;
    if (FSPathMakeRef((const UInt8 *)src, &s, NULL) != noErr) {
        std::fprintf(out, "src が見つかりません: %s\n", src); return 1;
    }
    if (FSPathMakeRef((const UInt8 *)dst_folder, &d, NULL) != noErr) {
        std::fprintf(out, "dst が見つかりません: %s\n", dst_folder); return 1;
    }
    AEDesc sd, dd;
    AECreateDesc(typeFSRef, &s, sizeof(s), &sd);
    AECreateDesc(typeFSRef, &d, sizeof(d), &dd);
    AEAddressDesc addr;
    OSType finder = 'MACS';
    AECreateDesc(typeApplSignature, &finder, sizeof(finder), &addr);
    AppleEvent ev, rep;
    AECreateAppleEvent(kCoreEventClass, 'move', &addr,
                       kAutoGenerateReturnID, kAnyTransactionID, &ev);
    AEPutParamDesc(&ev, keyDirectObject, &sd);
    AEPutParamDesc(&ev, 'insl', &dd);
    OSStatus st = AESendMessage(&ev, &rep, kAEWaitReply | kAECanInteract, 300);
    SInt32 errn = 0;
    {
        Size sz = sizeof(errn);
        AEGetParamPtr(&rep, keyErrorNumber, typeSInt32, NULL, &errn, sizeof(errn), &sz);
    }
    std::fprintf(out, "Finder reply: st=%d errn=%d\n", (int)st, (int)errn);
    AEDisposeDesc(&ev);
    AEDisposeDesc(&rep);
    AEDisposeDesc(&sd);
    AEDisposeDesc(&dd);
    AEDisposeDesc(&addr);
    if (st != noErr || errn != 0) {
        std::fprintf(out, "Finder move 失敗: st=%d errn=%d"
                          " (-1743 は Automation 未許可)\n", (int)st, (int)errn);
        return 1;
    }
    std::fprintf(out, "Finder が move しました: %s -> %s\n", src, dst_folder);

    // Plug-ins / Presets / Scripts は「共有」のまま、移動先から symlink 参照する。
    std::string sp(src);
    size_t sl = sp.find_last_of('/');
    std::string sparent = (sl == std::string::npos) ? std::string(".") : sp.substr(0, sl);
    for (const char *n : { "Plug-ins", "Presets", "Scripts" }) {
        std::string a = sparent + "/" + n, b = std::string(dst_folder) + "/" + n;
        struct stat t1, t2;
        if (stat(a.c_str(), &t1) == 0 && stat(b.c_str(), &t2) != 0) {
            if (symlink(a.c_str(), b.c_str()) == 0)
                std::fprintf(out, "共有 symlink: %s -> %s\n", b.c_str(), a.c_str());
        }
    }
    return 0;
}

// ---------------------------------------------------------------- probe
// 一つのコマンドで「起動 → しばらく見る → 終了/停滞/稼働を報告」する。
extern "C" int nx_stasis_watch(const char *, int, FILE *);
// --probe は起動しない。既に動いている対象を測るだけ。実 AE を開かない。
extern "C" int nx_probe(const char *app, int total_ms, FILE *out) {
    return nx_stasis_watch(app, total_ms, out);
}

// ---------------------------------------------------------------- launch
// バンドルの実行ファイルを直接 detached で起動する (open / LaunchServices を使わない)。
extern "C" int nx_launch(const char *app, FILE *out) {
    if (!out) out = stdout;
    if (!app || !*app) { std::fprintf(out, "使い方: neodax --launch <app>\n"); return 2; }
    std::string exe = nx_resolve_target(app);
    if (!nx_path_exists(exe)) {
        std::fprintf(out, "実行ファイルがありません: %s\n", exe.c_str());
        return 1;
    }
    pid_t pid = fork();
    if (pid < 0) { std::fprintf(out, "fork 失敗\n"); return 1; }
    if (pid == 0) {
        setsid();
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); }
        execl(exe.c_str(), exe.c_str(), (char *)NULL);
        _exit(127);
    }
    std::fprintf(out, "launch: pid=%d  %s\n", (int)pid, exe.c_str());
    return 0;
}

extern "C" int nx_activate(const char *app, int total_ms, FILE *out) {
    if (!out) out = stdout;
    if (!app || !*app) { std::fprintf(out, "使い方: neodax --activate <app> [--count ms]\n"); return 2; }
    if (total_ms <= 0) total_ms = 10000;
    std::string exe = nx_resolve_target(app);
    if (!nx_path_exists(exe)) { std::fprintf(out, "実行ファイルがありません: %s\n", exe.c_str()); return 1; }
    std::fprintf(out, "起動: %s\n", exe.c_str());
    pid_t pid = fork();
    if (pid < 0) { std::fprintf(out, "fork 失敗\n"); return 1; }
    if (pid == 0) {
        setsid();
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, 1); dup2(dn, 2); }
        execl(exe.c_str(), exe.c_str(), (char *)NULL);
        _exit(127);
    }
    struct blk { double x, y, w, h, alpha; int layer, onscreen; char owner[128], title[128]; };
    int waited = 0;
    for (;;) {
        usleep(1000000);
        waited += 1000;
        if (kill(pid, 0) != 0) {
            std::fprintf(out, "exited (pid=%d) after %d ms\n", (int)pid, waited);
            return 0;
        }
        std::vector<blk> bs;
        CFArrayRef arr = CGWindowListCopyWindowInfo(
            kCGWindowListOptionAll | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
        if (arr) {
            for (CFIndex i = 0; i < CFArrayGetCount(arr); i++) {
                CFDictionaryRef d = (CFDictionaryRef)CFArrayGetValueAtIndex(arr, i);
                CFNumberRef pp = (CFNumberRef)CFDictionaryGetValue(d, kCGWindowOwnerPID);
                int wp = 0;
                if (pp) CFNumberGetValue(pp, kCFNumberIntType, &wp);
                if (wp != pid) continue;
                blk b;
                std::memset(&b, 0, sizeof b);
                CFDictionaryRef bg = (CFDictionaryRef)CFDictionaryGetValue(d, kCGWindowBounds);
                CGRect r = CGRectZero;
                if (bg) CGRectMakeWithDictionaryRepresentation(bg, &r);
                b.x = r.origin.x; b.y = r.origin.y; b.w = r.size.width; b.h = r.size.height;
                CFNumberRef lay = (CFNumberRef)CFDictionaryGetValue(d, kCGWindowLayer);
                if (lay) CFNumberGetValue(lay, kCFNumberIntType, &b.layer);
                CFNumberRef al = (CFNumberRef)CFDictionaryGetValue(d, kCGWindowAlpha);
                if (al) CFNumberGetValue(al, kCFNumberDoubleType, &b.alpha);
                CFBooleanRef on = (CFBooleanRef)CFDictionaryGetValue(d, kCGWindowIsOnscreen);
                b.onscreen = on ? (int)CFBooleanGetValue(on) : 0;
                CFStringRef ow = (CFStringRef)CFDictionaryGetValue(d, kCGWindowOwnerName);
                if (ow) CFStringGetCString(ow, b.owner, sizeof b.owner, kCFStringEncodingUTF8);
                CFStringRef nm = (CFStringRef)CFDictionaryGetValue(d, kCGWindowName);
                if (nm) CFStringGetCString(nm, b.title, sizeof b.title, kCFStringEncodingUTF8);
                bs.push_back(b);
            }
            CFRelease(arr);
        }
        double maxA = 0;
        for (const auto &b : bs)
            if (!(b.h < 50 && b.y < 10)) { double a = b.w * b.h; if (a > maxA) maxA = a; }
        for (const auto &b : bs) {
            const char *cls = (b.h < 50 && b.y < 10) ? "menubar"
                            : (maxA > 0 && b.w * b.h >= maxA - 0.5) ? "main"
                            : "panel/dialog";
            std::fprintf(out,
                "  t=%dms %-12s %.0fx%.0f @ %.0f,%.0f layer=%d alpha=%.2f on=%d owner=\"%s\" title=\"%s\"\n",
                waited, cls, b.w, b.h, b.x, b.y, b.layer, b.alpha, b.onscreen,
                b.owner, b.title);
        }
        std::fprintf(out, "  t=%dms windows=%zu\n", waited, bs.size());
        if (waited >= total_ms) {
            std::fprintf(out, "観測終了 (生きたまま windows=%zu)\n", bs.size());
            return 0;
        }
    }
}

// ---------------------------------------------------------------- ax
// Accessibility でウィンドウの identity (role/subrole/title) を取る。ピクセルなし。
static std::string nx_ax_str(AXUIElementRef el, CFStringRef attr) {
    CFTypeRef v = NULL;
    std::string r;
    if (AXUIElementCopyAttributeValue(el, attr, &v) != kAXErrorSuccess || !v) return r;
    if (CFGetTypeID(v) == CFStringGetTypeID()) {
        char b[512] = {0};
        if (CFStringGetCString((CFStringRef)v, b, sizeof b, kCFStringEncodingUTF8)) r = b;
    }
    CFRelease(v);
    return r;
}

static void nx_ax_dump(AXUIElementRef el, int depth, FILE *out) {
    if (depth > 4) return;
    std::string role = nx_ax_str(el, kAXRoleAttribute);
    std::string sub = nx_ax_str(el, kAXSubroleAttribute);
    std::string title = nx_ax_str(el, kAXTitleAttribute);
    std::string desc = nx_ax_str(el, kAXDescriptionAttribute);
    std::string val;
    CFTypeRef v = NULL;
    if (AXUIElementCopyAttributeValue(el, kAXValueAttribute, &v) == kAXErrorSuccess && v) {
        if (CFGetTypeID(v) == CFStringGetTypeID()) {
            char b[512] = {0};
            if (CFStringGetCString((CFStringRef)v, b, sizeof b, kCFStringEncodingUTF8)) val = b;
        }
        CFRelease(v);
    }
    std::fprintf(out, "%*s%s", depth * 2, "", role.c_str());
    if (!sub.empty()) std::fprintf(out, "[%s]", sub.c_str());
    if (!title.empty()) std::fprintf(out, " \"%s\"", title.c_str());
    if (!desc.empty()) std::fprintf(out, " desc=\"%s\"", desc.c_str());
    if (!val.empty()) std::fprintf(out, " value=\"%s\"", val.c_str());
    std::fprintf(out, "\n");
    CFTypeRef kids = NULL;
    if (AXUIElementCopyAttributeValue(el, kAXChildrenAttribute, &kids) == kAXErrorSuccess && kids) {
        CFArrayRef a = (CFArrayRef)kids;
        CFIndex kc = CFArrayGetCount(a);
        for (CFIndex i = 0; i < kc; i++)
            nx_ax_dump((AXUIElementRef)CFArrayGetValueAtIndex(a, i), depth + 1, out);
        CFRelease(kids);
    }
}

// pid 解決 (target が数字なら pid、そうでなければ実行パス部分一致)
static pid_t nx_resolve_pid(const char *target) {
    if (!target || !*target) return -1;
    if (std::isdigit((unsigned char)target[0])) return (pid_t)std::atoi(target);
    int np = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    if (np <= 0) return -1;
    std::vector<pid_t> pids((size_t)np / sizeof(pid_t) + 64);
    np = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), (int)(pids.size() * sizeof(pid_t)));
    int cnt = np / (int)sizeof(pid_t);
    pid_t first = -1, best = -1;
    size_t best_len = 0;
    for (int i = 0; i < cnt; i++) {
        pid_t p = pids[i];
        if (p <= 1) continue;
        char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
        if (proc_pidpath(p, path, sizeof path) <= 0) continue;
        if (!std::strstr(path, target)) continue;
        if (first < 0) first = p;
        // 補助プロセス (crashpad/Helper/GPU/Renderer) を除外し、本体を優先する
        bool helper = std::strstr(path, "crashpad") || std::strstr(path, "Helper") ||
                      std::strstr(path, "GPU") || std::strstr(path, "Renderer");
        if (!helper) {
            size_t l = std::strlen(path);
            if (l > best_len) { best_len = l; best = p; }
        }
    }
    if (best > 0) return best;
    return first;
}

static AXUIElementRef nx_ax_find_desc(AXUIElementRef el, const char *desc, int depth) {
    if (depth > 6) return NULL;
    std::string d = nx_ax_str(el, kAXDescriptionAttribute);
    std::string t = nx_ax_str(el, kAXTitleAttribute);
    std::string sb = nx_ax_str(el, kAXSubroleAttribute);
    if ((!d.empty() && d == desc) || (!t.empty() && t == desc) || (!sb.empty() && sb == desc))
        return (AXUIElementRef)CFRetain(el);
    CFTypeRef kids = NULL;
    if (AXUIElementCopyAttributeValue(el, kAXChildrenAttribute, &kids) == kAXErrorSuccess && kids) {
        CFArrayRef a = (CFArrayRef)kids;
        CFIndex kc = CFArrayGetCount(a);
        for (CFIndex i = 0; i < kc; i++) {
            AXUIElementRef r = nx_ax_find_desc((AXUIElementRef)CFArrayGetValueAtIndex(a, i), desc, depth + 1);
            if (r) { CFRelease(kids); return r; }
        }
        CFRelease(kids);
    }
    return NULL;
}

extern "C" int nx_ax_press(const char *target, const char *desc, FILE *out) {
    if (!out) out = stdout;
    if (!target || !desc) { std::fprintf(out, "使い方: neodax --ax-press <pid|S> <desc>\n"); return 2; }
    pid_t pid = nx_resolve_pid(target);
    if (pid <= 0) { std::fprintf(out, "対象が見つかりません: %s\n", target); return 1; }
    AXUIElementRef app = AXUIElementCreateApplication(pid);
    if (!app) return 1;
    AXUIElementSetAttributeValue(app, CFSTR("AXManualAccessibility"), kCFBooleanTrue);
    CFTypeRef wins = NULL;
    for (int tries = 0; tries < 12; tries++) {
        wins = NULL;
        if (AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, &wins) == kAXErrorSuccess && wins)
            break;
        if (wins) { CFRelease(wins); wins = NULL; }
        usleep(500000);
    }
    if (!wins) {
        std::fprintf(out, "windows を取得できません\n");
        CFRelease(app);
        return 1;
    }
    CFArrayRef a = (CFArrayRef)wins;
    AXUIElementRef hit = NULL;
    for (CFIndex i = 0; i < CFArrayGetCount(a) && !hit; i++)
        hit = nx_ax_find_desc((AXUIElementRef)CFArrayGetValueAtIndex(a, i), desc, 0);
    CFRelease(wins);
    CFRelease(app);
    if (!hit) { std::fprintf(out, "要素が見つかりません: \"%s\"\n", desc); return 1; }
    AXError e = AXUIElementPerformAction(hit, kAXPressAction);
    CFRelease(hit);
    std::fprintf(out, "press \"%s\": %s\n", desc, e == kAXErrorSuccess ? "ok" : "failed");
    return e == kAXErrorSuccess ? 0 : 1;
}

extern "C" int nx_ax_windows(const char *target, FILE *out) {
    if (!out) out = stdout;
    if (!target || !*target) { std::fprintf(out, "使い方: neodax --ax <pid|path-substr>\n"); return 2; }
    pid_t pid = 0;
    if (std::isdigit((unsigned char)target[0])) {
        pid = (pid_t)std::atoi(target);
    } else {
        int np = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
        if (np > 0) {
            std::vector<pid_t> pids((size_t)np / sizeof(pid_t) + 64);
            np = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), (int)(pids.size() * sizeof(pid_t)));
            int cnt = np / (int)sizeof(pid_t);
            for (int i = 0; i < cnt; i++) {
                pid_t p = pids[i];
                if (p <= 1) continue;
                char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
                if (proc_pidpath(p, path, sizeof path) <= 0) continue;
                if (std::strstr(path, target)) { pid = p; break; }
            }
        }
    }
    if (pid <= 0) { std::fprintf(out, "対象が見つかりません: %s\n", target); return 1; }

    bool trusted = AXIsProcessTrusted();
    if (!trusted) {
        const void *keys[] = { kAXTrustedCheckOptionPrompt };
        const void *vals[] = { kCFBooleanTrue };
        CFDictionaryRef opt = CFDictionaryCreate(NULL, keys, vals, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        trusted = AXIsProcessTrustedWithOptions(opt);
        CFRelease(opt);
    }
    std::fprintf(out, "pid=%d  AX_trusted=%d\n", (int)pid, (int)trusted);
    if (!trusted) {
        std::fprintf(out, "Accessibility の許可が必要 (一度だけ)。許可後に再実行。\n");
        return 1;
    }

    AXUIElementRef app = AXUIElementCreateApplication(pid);
    if (!app) { std::fprintf(out, "AXUIElementCreateApplication 失敗\n"); return 1; }
    // AE のような独自 UI は AXManualAccessibility を立てないと AX を出さない。
    AXUIElementSetAttributeValue(app, CFSTR("AXManualAccessibility"), kCFBooleanTrue);
    CFTypeRef wins = NULL;
    for (int tries = 0; tries < 10; tries++) {
        wins = NULL;
        if (AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, &wins) == kAXErrorSuccess && wins)
            break;
        if (wins) { CFRelease(wins); wins = NULL; }
        usleep(500000);
    }
    if (!wins) {
        std::fprintf(out, "windows を取得できません (AX を出さないアプリの可能性)\n");
        CFRelease(app);
        return 1;
    }
    CFArrayRef arr = (CFArrayRef)wins;
    CFIndex cnt = CFArrayGetCount(arr);
    for (CFIndex i = 0; i < cnt; i++) {
        AXUIElementRef w = (AXUIElementRef)CFArrayGetValueAtIndex(arr, i);
        std::fprintf(out, "  [%ld] ----\n", (long)i);
        nx_ax_dump(w, 2, out);
    }
    std::fprintf(out, "AX windows=%ld\n", (long)cnt);
    CFRelease(wins);
    CFRelease(app);
    return 0;
}

// ---------------------------------------------------------------- stasis
// プロセスが「生きているが進んでいない」状態 (stasis) を測る。
// CPU 時間の増分で判定する。終了したか、動いているか、止まっているか。
extern "C" int nx_stasis_watch(const char *needle, int total_ms, FILE *out) {
    if (!out) out = stdout;
    if (!needle || !*needle) {
        std::fprintf(out, "使い方: neodax --stasis <path-substr> [--count ms]\n");
        return 2;
    }
    if (total_ms <= 0) total_ms = 15000;
    auto find = [&]() -> pid_t {
        int np = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
        if (np <= 0) return -1;
        std::vector<pid_t> pids((size_t)np / sizeof(pid_t) + 64);
        np = proc_listpids(PROC_ALL_PIDS, 0, pids.data(),
                           (int)(pids.size() * sizeof(pid_t)));
        if (np <= 0) return -1;
        int cnt = np / (int)sizeof(pid_t);
        for (int i = 0; i < cnt; i++) {
            pid_t p = pids[i];
            if (p <= 1) continue;
            char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
            if (proc_pidpath(p, path, sizeof path) <= 0) continue;
            if (std::strstr(path, needle)) return p;
        }
        return -1;
    };
    auto cpu = [](pid_t p) -> uint64_t {
        struct proc_taskinfo ti;
        if (proc_pidinfo(p, PROC_PIDTASKINFO, 0, &ti, sizeof ti) == sizeof ti)
            return ti.pti_total_user + ti.pti_total_system;
        return 0;
    };
    pid_t pid = find();
    if (pid <= 0) { std::fprintf(out, "見つかりません: %s\n", needle); return 1; }
    uint64_t c0 = cpu(pid);
    int waited = 0, gone = 0;
    while (waited < total_ms) {
        usleep(1000000);
        waited += 1000;
        if (kill(pid, 0) != 0) { gone = 1; break; }
    }
    if (gone) { std::fprintf(out, "exited (pid=%d)\n", (int)pid); return 0; }
    uint64_t d_ms = (cpu(pid) - c0) / 1000000ULL;
    uint64_t thr = (uint64_t)total_ms / 50;   /* 2% */
    std::fprintf(out, "pid=%d  cpu=%llu ms (window %d ms)\n",
                 (int)pid, (unsigned long long)d_ms, total_ms);
    if (d_ms < thr) std::fprintf(out, "stasis: 生きているが進んでいない\n");
    else std::fprintf(out, "busy: 動いている\n");
    return 0;
}

// ---------------------------------------------------------------- watch
// 指定の実行パスを含むプロセスが現れたら、猶予後に SIGTERM で終える。
// 「サインイン窓が出たらアプリを自動で終わらせる」ための手続き。
// 窓はアプリのプロセス内にあるので、アプリを終える以外に外から消す方法はない。
extern "C" int nx_watch_proc(const char *needle, int grace_ms, int total_ms, FILE *out) {
    if (!out) out = stdout;
    if (!needle || !*needle) {
        std::fprintf(out, "使い方: neodax --watch <path-substr> [--count grace_ms]\n");
        return 2;
    }
    if (grace_ms <= 0) grace_ms = 3000;
    if (total_ms <= 0) total_ms = 120000;
    auto find = [&]() -> pid_t {
        int np = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
        if (np <= 0) return -1;
        std::vector<pid_t> pids((size_t)np / sizeof(pid_t) + 64);
        np = proc_listpids(PROC_ALL_PIDS, 0, pids.data(),
                           (int)(pids.size() * sizeof(pid_t)));
        if (np <= 0) return -1;
        int cnt = np / (int)sizeof(pid_t);
        for (int i = 0; i < cnt; i++) {
            pid_t p = pids[i];
            if (p <= 1) continue;
            char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
            if (proc_pidpath(p, path, sizeof path) <= 0) continue;
            if (std::strstr(path, needle)) return p;
        }
        return -1;
    };
    int waited = 0;
    pid_t target = -1;
    while (waited < total_ms) {
        target = find();
        if (target > 0) break;
        usleep(500000);
        waited += 500;
    }
    if (target <= 0) { std::fprintf(out, "見つかりません: %s\n", needle); return 1; }
    std::fprintf(out, "見つけました: pid=%d。%d ms 後に終了させます。\n",
                 (int)target, grace_ms);
    usleep((useconds_t)grace_ms * 1000);
    if (kill(target, SIGTERM) == 0)
        std::fprintf(out, "SIGTERM 送信: pid=%d\n", (int)target);
    else
        std::fprintf(out, "SIGTERM 失敗: pid=%d\n", (int)target);
    return 0;
}

// ---------------------------------------------------------------- load commands
static const char *nx_lc_name(uint32_t cmd) {
    switch (cmd) {
        case LC_SEGMENT_64: return "LC_SEGMENT_64";
        case LC_SYMTAB: return "LC_SYMTAB";
        case LC_DYLD_INFO: return "LC_DYLD_INFO";
        case LC_DYLD_INFO_ONLY: return "LC_DYLD_INFO_ONLY";
        case LC_DYLD_CHAINED_FIXUPS: return "LC_DYLD_CHAINED_FIXUPS";
        case LC_FUNCTION_STARTS: return "LC_FUNCTION_STARTS";
        case LC_MAIN: return "LC_MAIN";
        case LC_LOAD_DYLIB: return "LC_LOAD_DYLIB";
        case LC_LOAD_WEAK_DYLIB: return "LC_LOAD_WEAK_DYLIB";
        case LC_REEXPORT_DYLIB: return "LC_REEXPORT_DYLIB";
        case LC_LOAD_UPWARD_DYLIB: return "LC_LOAD_UPWARD_DYLIB";
        case LC_LAZY_LOAD_DYLIB: return "LC_LAZY_LOAD_DYLIB";
        case LC_ID_DYLIB: return "LC_ID_DYLIB";
        case LC_RPATH: return "LC_RPATH";
        case LC_CODE_SIGNATURE: return "LC_CODE_SIGNATURE";
        case LC_UUID: return "LC_UUID";
        case LC_BUILD_VERSION: return "LC_BUILD_VERSION";
        case LC_VERSION_MIN_MACOSX: return "LC_VERSION_MIN_MACOSX";
        case LC_DATA_IN_CODE: return "LC_DATA_IN_CODE";
        default: return "LC_?";
    }
}

extern "C" int nx_load_commands(const char *path, FILE *out) {
    if (!out) out = stdout;
    if (!path) return 2;
    FILE *f = std::fopen(path, "rb");
    if (!f) { std::fprintf(out, "開けません: %s\n", path); return 1; }
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); return 1; }
    std::vector<unsigned char> d((size_t)n);
    if (std::fread(d.data(), 1, (size_t)n, f) != (size_t)n) { std::fclose(f); return 1; }
    std::fclose(f);
    uint64_t base = 0;
    if (d.size() >= 8) {
        uint32_t be = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
        if (be == FAT_MAGIC || be == FAT_MAGIC_64) {
            bool is64 = (be == FAT_MAGIC_64);
            uint32_t nfat = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) | ((uint32_t)d[6] << 8) | d[7];
            for (uint32_t i = 0; i < nfat; i++) {
                size_t eo = 8 + (size_t)i * (is64 ? 32 : 20);
                uint32_t cpu = ((uint32_t)d[eo] << 24) | ((uint32_t)d[eo+1] << 16) | ((uint32_t)d[eo+2] << 8) | d[eo+3];
                uint64_t off;
                if (is64) { off = 0; for (int k = 0; k < 8; k++) off = (off << 8) | d[eo + 8 + k]; }
                else off = ((uint32_t)d[eo+8] << 24) | ((uint32_t)d[eo+9] << 16) | ((uint32_t)d[eo+10] << 8) | d[eo+11];
                if (cpu == CPU_TYPE_ARM64) { base = off; break; }
                if (i == nfat - 1) base = off;
            }
        }
    }
    if (base + sizeof(mach_header_64) > d.size()) { std::fprintf(out, "Mach-O ではありません\n"); return 1; }
    const mach_header_64 *mh = (const mach_header_64 *)(d.data() + base);
    if (mh->magic != MH_MAGIC_64) { std::fprintf(out, "MH_MAGIC_64 ではありません\n"); return 1; }
    std::fprintf(out, "ncmds=%u sizeofcmds=%u\n", mh->ncmds, mh->sizeofcmds);
    size_t off = (size_t)base + sizeof(mach_header_64);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (off + sizeof(load_command) > d.size()) break;
        const load_command *lc = (const load_command *)(d.data() + off);
        if (lc->cmdsize < sizeof(load_command) || off + lc->cmdsize > d.size()) break;
        std::fprintf(out, "  %-22s cmdsize=%u", nx_lc_name(lc->cmd), lc->cmdsize);
        if (lc->cmd == LC_LOAD_DYLIB || lc->cmd == LC_LOAD_WEAK_DYLIB ||
            lc->cmd == LC_REEXPORT_DYLIB || lc->cmd == LC_LOAD_UPWARD_DYLIB ||
            lc->cmd == LC_LAZY_LOAD_DYLIB || lc->cmd == LC_ID_DYLIB) {
            const dylib_command *dc = (const dylib_command *)lc;
            if (dc->dylib.name.offset < lc->cmdsize)
                std::fprintf(out, "  %s", (const char *)lc + dc->dylib.name.offset);
        } else if (lc->cmd == LC_RPATH) {
            const rpath_command *rc = (const rpath_command *)lc;
            if (rc->path.offset < lc->cmdsize)
                std::fprintf(out, "  %s", (const char *)lc + rc->path.offset);
        } else if (lc->cmd == LC_MAIN) {
            const entry_point_command *e = (const entry_point_command *)lc;
            std::fprintf(out, "  entryoff=0x%llx", (unsigned long long)e->entryoff);
        } else if (lc->cmd == LC_UUID) {
            const uuid_command *u = (const uuid_command *)lc;
            std::fprintf(out, "  ");
            for (int k = 0; k < 16; k++) std::fprintf(out, "%02x", u->uuid[k]);
        } else if (lc->cmd == LC_SEGMENT_64) {
            const segment_command_64 *sg = (const segment_command_64 *)lc;
            std::fprintf(out, "  %s vm=0x%llx size=0x%llx file=0x%llx/%llu",
                         sg->segname, (unsigned long long)sg->vmaddr,
                         (unsigned long long)sg->vmsize, (unsigned long long)sg->fileoff,
                         (unsigned long long)sg->filesize);
        } else if (lc->cmd == LC_CODE_SIGNATURE) {
            const linkedit_data_command *ld = (const linkedit_data_command *)lc;
            std::fprintf(out, "  dataoff=0x%x size=%u", ld->dataoff, ld->datasize);
        } else if (lc->cmd == LC_BUILD_VERSION) {
            const build_version_command *bv = (const build_version_command *)lc;
            std::fprintf(out, "  platform=%u minos=0x%x sdk=0x%x", bv->platform, bv->minos, bv->sdk);
        }
        std::fprintf(out, "\n");
        off += lc->cmdsize;
    }
    return 0;
}

// ---------------------------------------------------------------- class graph
// 実際のグラフ (関数 + 呼び出し辺) から、クラス視点のグラフを「導出」する。
// 視覚化は任意 (--save で SVG)。真実は関数グラフ側にあり、これは射影。
static std::string nx_demangle_sym(const std::string &sym);
static void nx_split_owner(const std::string &d, std::string &owner, std::string &member) {
    int depth = 0; size_t split = std::string::npos;
    for (size_t i = 0; i + 1 < d.size(); i++) {
        char c = d[i];
        if (c == '<') { depth++; continue; }
        if (c == '>') { if (depth > 0) depth--; continue; }
        if (c == '(') break;
        if (c == ':' && d[i + 1] == ':' && depth == 0) split = i;
    }
    if (split == std::string::npos) { owner.clear(); member = d; return; }
    owner = d.substr(0, split);
    member = d.substr(split + 2);
}

extern "C" int nx_class_graph(const char *path, const char *needle, const char *svg, FILE *out) {
    if (!out) out = stdout;
    if (!path) return 2;
    auto img = nx::MachOImage::open(path);
    if (!img) { std::fprintf(out, "[skip] not a 64-bit Mach-O: %s\n", path); return 1; }
    nx::ImportResolver imports; imports.parse(*img);
    nx::XRefIndex xr; xr.build(*img, imports);
    nx::BackTracker bt(*img, xr);

    std::map<uint64_t, std::string> addr_owner;
    std::map<std::string, std::set<std::string>> classes;
    for (const auto &kv : img->defined_symbols()) {
        const std::string &sym = kv.first;
        if (sym.rfind("__Z", 0) != 0) continue;
        std::string d = nx_demangle_sym(sym);
        if (d == sym) continue;
        if (d.rfind("vtable for ", 0) == 0 || d.rfind("typeinfo for ", 0) == 0 ||
            d.rfind("guard variable for ", 0) == 0 || d.rfind("VTT for ", 0) == 0) continue;
        std::string owner, member;
        nx_split_owner(d, owner, member);
        if (owner.empty()) continue;
        classes[owner].insert(member);
        addr_owner[kv.second] = owner;
    }

    struct ce { int count = 0; int back = 0; };
    std::map<std::pair<std::string, std::string>, ce> edges;
    for (const auto &s : xr.sites()) {
        if (!s.is_call) continue;
        uint64_t from = bt.function_containing(s.addr);
        uint64_t to = bt.function_containing(s.target_vm);
        auto fo = addr_owner.find(from);
        auto to_ = addr_owner.find(to);
        if (fo == addr_owner.end() || to_ == addr_owner.end()) continue;
        edges[{fo->second, to_->second}].count++;
    }
    for (auto &e : edges) {
        if (e.first.first == e.first.second) e.second.back = 1;
        auto rev = edges.find({e.first.second, e.first.first});
        if (rev != edges.end()) e.second.back = 1;
    }

    int nc = 0;
    for (const auto &c : classes) {
        if (needle && *needle && c.first.find(needle) == std::string::npos) continue;
        std::fprintf(out, "class %s {\n public:\n", c.first.c_str());
        int m = 0;
        for (const auto &mem : c.second) {
            if (m++ >= 40) { std::fprintf(out, "  ...\n"); break; }
            std::fprintf(out, "  %s;\n", mem.c_str());
        }
        std::fprintf(out, "};\n\n");
        nc++;
    }
    std::fprintf(out, "--- derived class graph ---\n");
    int ne = 0;
    for (const auto &e : edges) {
        if (needle && *needle &&
            e.first.first.find(needle) == std::string::npos &&
            e.first.second.find(needle) == std::string::npos) continue;
        std::fprintf(out, "  %s -> %s  x%d%s\n", e.first.first.c_str(), e.first.second.c_str(),
                     e.second.count, e.second.back ? "  [annulus]" : "");
        ne++;
    }
    std::fprintf(out, "classes=%d edges=%d\n", nc, ne);

    if (svg && *svg) {
        std::vector<std::string> names;
        for (const auto &c : classes)
            if (!needle || !*needle || c.first.find(needle) != std::string::npos)
                names.push_back(c.first);
        int cols = 4, bw = 260, bh = 70, gap = 40, margin = 20;
        int rows = (int)((names.size() + cols - 1) / cols);
        int W = margin * 2 + cols * (bw + gap);
        int H = margin * 2 + rows * (bh + gap);
        std::ofstream f(svg);
        if (f) {
            std::map<std::string, int> idx;
            f << "<svg xmlns='http://www.w3.org/2000/svg' width='" << W << "' height='" << H << "'>\n";
            f << "<style>text{font-family:monospace;font-size:11px;fill:#222}"
                 ".box{fill:#eef;stroke:#335;stroke-width:1.5}"
                 ".ann{stroke:#d22;stroke-width:2.5;fill:none}"
                 ".nec{stroke:#557;stroke-width:1.2;fill:none}</style>\n";
            for (size_t i = 0; i < names.size(); i++) {
                int r = (int)i / cols, cc = (int)i % cols;
                int x = margin + cc * (bw + gap), y = margin + r * (bh + gap);
                idx[names[i]] = (int)i;
                f << "<rect class='box' x='" << x << "' y='" << y << "' width='" << bw
                  << "' height='" << bh << "' rx='8'/>\n";
                f << "<text x='" << (x + 8) << "' y='" << (y + 18) << "'>" << names[i] << "</text>\n";
            }
            auto cx = [&](int i) { return margin + (i % cols) * (bw + gap) + bw / 2; };
            auto cy = [&](int i) { return margin + (i / cols) * (bh + gap) + bh / 2; };
            for (const auto &e : edges) {
                auto a = idx.find(e.first.first), b = idx.find(e.first.second);
                if (a == idx.end() || b == idx.end()) continue;
                f << "<line class='" << (e.second.back ? "ann" : "nec") << "' x1='" << cx(a->second)
                  << "' y1='" << cy(a->second) << "' x2='" << cx(b->second)
                  << "' y2='" << cy(b->second) << "'/>\n";
            }
            f << "</svg>\n";
            std::fprintf(out, "SVG: %s\n", svg);
        }
    }
    return 0;
}

// ---------------------------------------------------------------- display graph
// 実際の画面のノード木 (AX) を中立モデル DVNode に復号し、
// 同じモデルから C++ または SVG を多態的に出力する。
static std::string nx_ax_str(AXUIElementRef el, CFStringRef attr);

struct DVNode {
    std::string role, sub, title, desc;
    double x = 0, y = 0, w = 0, h = 0;
    std::vector<DVNode> kids;
};

static void nx_ax_frame(AXUIElementRef el, double *x, double *y, double *w, double *h) {
    *x = *y = *w = *h = 0;
    CFTypeRef p = NULL, s = NULL;
    if (AXUIElementCopyAttributeValue(el, kAXPositionAttribute, &p) == kAXErrorSuccess && p) {
        CGPoint pt; std::memset(&pt, 0, sizeof pt);
        AXValueType ty = AXValueGetType((AXValueRef)p);
        if (ty == kAXValueCGPointType)
            AXValueGetValue((AXValueRef)p, ty, (void *)&pt);
        *x = pt.x; *y = pt.y; CFRelease(p);
    }
    if (AXUIElementCopyAttributeValue(el, kAXSizeAttribute, &s) == kAXErrorSuccess && s) {
        CGSize sz; std::memset(&sz, 0, sizeof sz);
        AXValueType ty2 = AXValueGetType((AXValueRef)s);
        if (ty2 == kAXValueCGSizeType)
            AXValueGetValue((AXValueRef)s, ty2, (void *)&sz);
        *w = sz.width; *h = sz.height; CFRelease(s);
    }
}

static void dv_from_ax(AXUIElementRef el, DVNode &n, int depth, int &budget) {
    if (budget <= 0) return;
    budget--;
    n.role = nx_ax_str(el, kAXRoleAttribute);
    n.sub = nx_ax_str(el, kAXSubroleAttribute);
    n.title = nx_ax_str(el, kAXTitleAttribute);
    n.desc = nx_ax_str(el, kAXDescriptionAttribute);
    nx_ax_frame(el, &n.x, &n.y, &n.w, &n.h);
    if (depth >= 6) return;
    CFTypeRef kids = NULL;
    if (AXUIElementCopyAttributeValue(el, kAXChildrenAttribute, &kids) == kAXErrorSuccess && kids) {
        CFArrayRef a = (CFArrayRef)kids;
        CFIndex kc = CFArrayGetCount(a);
        for (CFIndex i = 0; i < kc && budget > 0; i++) {
            DVNode c;
            dv_from_ax((AXUIElementRef)CFArrayGetValueAtIndex(a, i), c, depth + 1, budget);
            n.kids.push_back(c);
        }
        CFRelease(kids);
    }
}

static void nx_cpp_esc(std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    s = o;
}

static void dv_cpp(const DVNode &n, int ind, FILE *out) {
    std::string r = n.role, t = n.title, d = n.desc, su = n.sub;
    nx_cpp_esc(r); nx_cpp_esc(t); nx_cpp_esc(d); nx_cpp_esc(su);
    std::fprintf(out, "%*sDVNode{\"%s\",\"%s\",\"%s\",\"%s\",%.1f,%.1f,%.1f,%.1f,{\n",
                 ind, "", r.c_str(), su.c_str(), t.c_str(), d.c_str(), n.x, n.y, n.w, n.h);
    for (const auto &k : n.kids) dv_cpp(k, ind + 2, out);
    std::fprintf(out, "%*s}},\n", ind, "");
}

static std::string dv_xml_esc(const std::string &s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&apos;"; break;
            default: o += c;
        }
    }
    return o;
}

static bool dv_dialog(const DVNode &n) {
    return n.sub.find("Dialog") != std::string::npos ||
           n.role.find("Dialog") != std::string::npos;
}

static bool dv_chrome(const DVNode &n) {
    return n.role.find("AXWindow") != std::string::npos ||
           n.sub == "AXFloatingWindow" ||
           (!n.title.empty() && n.role == "AXLayoutArea");
}

static std::string dv_clip(const std::string &s, double w) {
    int n = (int)((w - 6) / 6.2);
    if (n < 3) return "";
    if ((int)s.size() <= n) return s;
    return s.substr(0, (size_t)n) + "…";
}

static int dv_count(const DVNode &n) {
    int c = 1;
    for (const auto &k : n.kids) c += dv_count(k);
    return c;
}

static double dv_svg(const DVNode &n, double *maxy, FILE *out) {
    // スクロールバー等は描かない (子だけ通す)。
    if (n.role.find("ScrollBar") != std::string::npos ||
        n.role.find("ValueIndicator") != std::string::npos) {
        for (const auto &k : n.kids) dv_svg(k, maxy, out);
        return 0;
    }
    const bool dialog = dv_dialog(n);
    const bool chrome = dv_chrome(n);
    const bool web = n.role.find("AXWebArea") != std::string::npos;
    const bool structural = !dialog && !chrome && !web && n.title.empty() && n.desc.empty() &&
        (n.role == "AXGroup" || n.role == "AXScrollArea" || n.role == "AXPage" ||
         n.role == "AXSplitter");

    const double x = n.x, y = n.y;
    const double w = n.w > 4 ? n.w : 40, h = n.h > 4 ? n.h : 16;
    if (w >= 6 && h >= 6) {
        if (dialog) {
            std::fprintf(out, "<g><rect x='%.1f' y='%.1f' width='%.1f' height='%.1f' rx='10' "
                              "fill='#11151c' stroke='#f87171' stroke-width='2'/>\n", x, y, w, h);
            std::fprintf(out, "<rect x='%.1f' y='%.1f' width='%.1f' height='28' rx='10' fill='#3a1f24'/>\n",
                         x, y, w);
            std::fprintf(out, "<rect x='%.1f' y='%.1f' width='12' height='12' fill='none' stroke='#f87171'/>\n",
                         x + 10, y + 8);
            std::fprintf(out, "<text x='%.1f' y='%.1f' fill='#ffb4b4' font-size='12' font-weight='bold'>"
                              "AXDialog — web surface</text>\n", x + 32, y + 19);
        } else if (chrome) {
            std::fprintf(out, "<g><rect x='%.1f' y='%.1f' width='%.1f' height='%.1f' "
                              "fill='#1b2230' stroke='#3a4a63'/>\n", x, y, w, h);
            std::fprintf(out, "<rect x='%.1f' y='%.1f' width='%.1f' height='26' fill='#232c3d'/>\n", x, y, w);
            if (!n.title.empty())
                std::fprintf(out, "<text x='%.1f' y='%.1f' fill='#dbe5f1' font-size='12' font-weight='bold'>%s</text>\n",
                             x + 8, y + 18, dv_xml_esc(dv_clip(n.title, w - 16)).c_str());
        } else if (web) {
            std::fprintf(out, "<rect x='%.1f' y='%.1f' width='%.1f' height='%.1f' "
                              "fill='#0e1622' stroke='#2f3a4d'/>\n", x, y, w, h);
            std::fprintf(out, "<text x='%.1f' y='%.1f' fill='#7d8da3' font-size='10'>"
                              "AXWebArea — web content (AX text not exposed)</text>\n", x + 8, y + 16);
        } else if (structural) {
            std::fprintf(out, "<rect x='%.1f' y='%.1f' width='%.1f' height='%.1f' "
                              "fill='none' stroke='#2a3444' stroke-width='0.8' opacity='0.35'/>\n",
                         x, y, w, h);
        } else {
            std::fprintf(out, "<rect x='%.1f' y='%.1f' width='%.1f' height='%.1f' rx='3' "
                              "fill='#202836' stroke='#2f3a4d'/>\n", x, y, w, h);
            std::string lbl = !n.title.empty() ? n.title : !n.desc.empty() ? n.desc : "";
            if (lbl.empty() && (n.role.find("Button") != std::string::npos ||
                                n.role.find("StaticText") != std::string::npos ||
                                n.role.find("Radio") != std::string::npos ||
                                n.role.find("Check") != std::string::npos ||
                                n.role.find("TextField") != std::string::npos))
                lbl = n.role;
            std::string t = w >= 56 ? dv_clip(lbl, w - 8) : "";
            if (!t.empty())
                std::fprintf(out, "<text x='%.1f' y='%.1f' fill='#c9d4e3' font-size='10'>%s</text>\n",
                             x + 4, y + 12, dv_xml_esc(t).c_str());
        }
    }
    if (*maxy < y + h) *maxy = y + h;
    for (const auto &k : n.kids) dv_svg(k, maxy, out);
    if (dialog || chrome) std::fprintf(out, "</g>\n");
    return *maxy - y;
}

// 保存済みの DVNode モデル (--display-graph --save の C++ 出力) を読み直し、
// アプリを起動せずに SVG へ描く。AX を読まないので静的な表示イメージになる。
static void dv_skip_ws(const std::string &s, size_t &i) {
    while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
}

static bool dv_lit(const std::string &s, size_t &i, const char *lit) {
    size_t n = std::strlen(lit);
    if (s.compare(i, n, lit) != 0) return false;
    i += n;
    return true;
}

static bool dv_quoted(const std::string &s, size_t &i, std::string &v) {
    if (i >= s.size() || s[i] != '"') return false;
    i++;
    v.clear();
    while (i < s.size()) {
        char c = s[i++];
        if (c == '"') return true;
        if (c == '\\' && i < s.size()) {
            char e = s[i++];
            if (e == 'n') v += '\n';
            else if (e == 't') v += '\t';
            else v += e;
        } else {
            v += c;
        }
    }
    return false;
}

static bool dv_number(const std::string &s, size_t &i, double &v) {
    dv_skip_ws(s, i);
    const char *p = s.c_str() + i;
    char *end = NULL;
    v = std::strtod(p, &end);
    if (!end || end == p) return false;
    i = (size_t)(end - s.c_str());
    return true;
}

static bool dv_parse_node(const std::string &s, size_t &i, DVNode &n) {
    dv_skip_ws(s, i);
    if (!dv_lit(s, i, "DVNode{")) return false;
    if (!dv_quoted(s, i, n.role)) return false;
    if (!dv_lit(s, i, ",")) return false;
    if (!dv_quoted(s, i, n.sub)) return false;
    if (!dv_lit(s, i, ",")) return false;
    if (!dv_quoted(s, i, n.title)) return false;
    if (!dv_lit(s, i, ",")) return false;
    if (!dv_quoted(s, i, n.desc)) return false;
    if (!dv_lit(s, i, ",")) return false;
    if (!dv_number(s, i, n.x)) return false;
    if (!dv_lit(s, i, ",")) return false;
    if (!dv_number(s, i, n.y)) return false;
    if (!dv_lit(s, i, ",")) return false;
    if (!dv_number(s, i, n.w)) return false;
    if (!dv_lit(s, i, ",")) return false;
    if (!dv_number(s, i, n.h)) return false;
    dv_skip_ws(s, i);
    if (!dv_lit(s, i, ",{")) return false;
    for (;;) {
        dv_skip_ws(s, i);
        if (s.compare(i, 2, "}}") == 0) { i += 2; break; }
        DVNode c;
        if (!dv_parse_node(s, i, c)) return false;
        n.kids.push_back(c);
        dv_skip_ws(s, i);
        if (i < s.size() && s[i] == ',') i++;
    }
    dv_skip_ws(s, i);
    if (i < s.size() && s[i] == ',') i++;
    return true;
}

static bool dv_load_model(const char *path, std::vector<DVNode> &roots) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t i = s.find("std::vector<DVNode>");
    if (i == std::string::npos) i = 0;
    size_t br = s.find('{', i);
    if (br == std::string::npos) return false;
    i = br + 1;
    for (;;) {
        dv_skip_ws(s, i);
        if (i >= s.size() || s.compare(i, 2, "};") == 0) break;
        DVNode n;
        if (!dv_parse_node(s, i, n)) return false;
        roots.push_back(n);
    }
    return !roots.empty();
}

static bool dv_match_self(const DVNode &n, const std::string &f) {
    return n.role.find(f) != std::string::npos || n.sub.find(f) != std::string::npos ||
           n.title.find(f) != std::string::npos || n.desc.find(f) != std::string::npos;
}

static bool dv_match(const DVNode &n, const std::string &f) {
    if (dv_match_self(n, f)) return true;
    for (const auto &k : n.kids)
        if (dv_match(k, f)) return true;
    return false;
}

// focus がノード自身に一致したら部分木ごと残す。子だけ一致なら祖先を残す。
static void dv_prune(DVNode &n, const std::string &f) {
    if (dv_match_self(n, f)) return;
    std::vector<DVNode> keep;
    for (const auto &k : n.kids) {
        if (!dv_match(k, f)) continue;
        DVNode c = k;
        dv_prune(c, f);
        keep.push_back(c);
    }
    n.kids = keep;
}

static std::string g_dv_focus;

extern "C" void nx_display_set_focus(const char *f) {
    g_dv_focus = f ? f : "";
}

static int dv_write_model(const std::vector<DVNode> &roots, const char *save, FILE *out) {
    std::vector<DVNode> view;
    if (g_dv_focus.empty()) {
        view = roots;
    } else {
        for (const auto &r : roots) {
            if (!dv_match(r, g_dv_focus)) continue;
            DVNode c = r;
            dv_prune(c, g_dv_focus);
            view.push_back(c);
        }
    }
    bool svg = save && std::strlen(save) >= 4 &&
               (!std::strcmp(save + std::strlen(save) - 4, ".svg"));
    FILE *f = out;
    if (save && *save) {
        f = std::fopen(save, "w");
        if (!f) { std::fprintf(out, "書けません: %s\n", save); return 1; }
    }
    if (svg) {
        int nodes = 0;
        bool has_dialog = false;
        for (const auto &r : view) {
            nodes += dv_count(r);
            if (dv_dialog(r)) has_dialog = true;
        }
        std::fprintf(f, "<svg xmlns='http://www.w3.org/2000/svg' width='1710' height='1112' "
                        "viewBox='0 0 1710 1112'>\n");
        std::fprintf(f, "<style>text{font-family:Menlo,monospace;fill:#c9d4e3}</style>\n");
        std::fprintf(f, "<rect width='100%%' height='100%%' fill='#0b0e13'/>\n");

        double y = 0;
        // 通常ウィンドウを先、ダイアログを最後に描く (重なり順)。
        for (const auto &r : view)
            if (!dv_dialog(r)) dv_svg(r, &y, f);
        if (has_dialog) {
            std::fprintf(f, "<rect x='0' y='0' width='1710' height='1112' fill='#000' opacity='0.35'/>\n");
            for (const auto &r : view)
                if (dv_dialog(r)) dv_svg(r, &y, f);
        }
        std::fprintf(f, "<rect x='10' y='1076' width='760' height='26' rx='4' fill='#0b0e13' opacity='0.9'/>\n");
        std::fprintf(f, "<text x='18' y='1093' font-size='10' fill='#93a4bb'>"
                        "blue = window · red = dialog · gray = structural · filled = control · "
                        "windows=%zu · nodes=%d</text>\n", view.size(), nodes);
        std::fprintf(f, "</svg>\n");
    } else {
        std::fprintf(f, "// display graph decoded (C++), %zu windows\n", view.size());
        std::fprintf(f, "struct DVNode { const char*role; const char*sub; const char*title; "
                        "const char*desc; double x,y,w,h; std::vector<DVNode> kids; };\n");
        std::fprintf(f, "std::vector<DVNode> display = {\n");
        for (const auto &r : view) dv_cpp(r, 2, f);
        std::fprintf(f, "};\n");
    }
    if (f != out) std::fclose(f);
    std::fprintf(out, "display graph: windows=%zu -> %s\n", view.size(),
                 (save && *save) ? save : "(stdout)");
    return 0;
}

extern "C" int nx_display_graph(const char *target, const char *save, FILE *out) {
    if (!out) out = stdout;
    if (!target || !*target) { std::fprintf(out, "使い方: neodax --display-graph <pid|S|model.cpp> [--save out.cpp|out.svg]\n"); return 2; }
    // 保存済みモデルなら AX を読まずに描く (アプリを起動しない静的表示)。
    struct stat tst;
    if (stat(target, &tst) == 0 && S_ISREG(tst.st_mode)) {
        std::vector<DVNode> roots;
        if (!dv_load_model(target, roots)) {
            std::fprintf(out, "DVNode モデルを読めません: %s\n", target);
            return 2;
        }
        return dv_write_model(roots, save, out);
    }
    pid_t pid = nx_resolve_pid(target);
    if (pid <= 0) { std::fprintf(out, "対象が見つかりません: %s\n", target); return 1; }
    bool trusted = AXIsProcessTrusted();
    if (!trusted) { std::fprintf(out, "Accessibility の許可が必要\n"); return 1; }
    AXUIElementRef app = AXUIElementCreateApplication(pid);
    if (!app) return 1;
    AXUIElementSetAttributeValue(app, CFSTR("AXManualAccessibility"), kCFBooleanTrue);
    CFTypeRef wins = NULL;
    for (int i = 0; i < 12; i++) {
        wins = NULL;
        if (AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, &wins) == kAXErrorSuccess && wins) break;
        if (wins) { CFRelease(wins); wins = NULL; }
        usleep(500000);
    }
    if (!wins) { std::fprintf(out, "windows を取得できません\n"); CFRelease(app); return 1; }
    std::vector<DVNode> roots;
    int budget = 400;
    CFArrayRef a = (CFArrayRef)wins;
    for (CFIndex i = 0; i < CFArrayGetCount(a) && budget > 0; i++) {
        DVNode r; dv_from_ax((AXUIElementRef)CFArrayGetValueAtIndex(a, i), r, 0, budget);
        roots.push_back(r);
    }
    CFRelease(wins); CFRelease(app);
    return dv_write_model(roots, save, out);
}

// ---------------------------------------------------------------- imports
// 取り込んでいる外部シンボルと GOT スロットを一覧する。
// 「どのライセンスAPI / exit を呼んでいるか」を静的に見るための入口。
extern "C" int nx_list_imports(const char *path, const char *needle, FILE *out) {
    if (!out) out = stdout;
    if (!path) return 2;
    auto img = nx::MachOImage::open(path);
    if (!img) { std::fprintf(out, "[skip] not a 64-bit Mach-O: %s\n", path); return 1; }
    nx::ImportResolver imports;
    imports.parse(*img);
    nx::XRefIndex xr;
    xr.build(*img, imports);
    nx::BackTracker bt(*img, xr);
    int n = 0;
    for (const auto &b : imports.bindings()) {
        if (needle && *needle && b.symbol.find(needle) == std::string::npos) continue;
        std::fprintf(out, "  0x%llx  %s\n", (unsigned long long)b.slot, b.symbol.c_str());
        for (const auto &s : xr.sites()) {
            bool hit = (s.target_vm == b.slot) ||
                       (!s.target_symbol.empty() && s.target_symbol == b.symbol);
            if (!hit) continue;
            uint64_t f = bt.function_containing(s.addr);
            const std::string *fn = img->name_at(f);
            std::fprintf(out, "      site 0x%llx%s in 0x%llx %s\n",
                         (unsigned long long)s.addr, s.is_call ? " (call)" : "",
                         (unsigned long long)f, fn ? fn->c_str() : "(無名)");
        }
        n++;
    }
    std::fprintf(out, "%s の import は %d 個です。\n", (needle && *needle) ? needle : "(all)", n);
    return n ? 0 : 1;
}

// ---------------------------------------------------------------- prune svg
// 重要 (necessary) と 状態依存 (wait/spawn) だけを描き、irrelevant は捨てる。
extern "C" int nx_prune_svg(const char *path, const char *outpath, FILE *out) {
    if (!out) out = stdout;
    if (!path || !outpath) { std::fprintf(out, "使い方: neodax --prune-svg OUT.svg <binary>\n"); return 2; }
    auto img = nx::MachOImage::open(path);
    if (!img) { std::fprintf(out, "[skip] %s\n", path); return 1; }
    nx::ImportResolver imports; imports.parse(*img);
    nx::XRefIndex xr; xr.build(*img, imports);
    nx::AnnulusAnalyzer ra; ra.build(*img, xr); ra.find_annuli();
    nx::EdgeClassifier ec;
    uint64_t entry = img->entry();
    if (!entry && !ra.nodes().empty()) entry = ra.nodes()[0].addr;
    ec.run(ra, entry);

    std::vector<std::pair<std::string, std::string>> edges;
    std::set<std::string> wait_edges;
    std::set<std::string> nodes;
    for (const auto &e : ec.edges()) {
        bool keep = (e.cls == nx::EdgeClassifier::Cls::Necessary) || e.wait || e.spawn;
        if (!keep) continue;
        edges.push_back({ e.from, e.to });
        nodes.insert(e.from);
        nodes.insert(e.to);
        if (e.wait || e.spawn) wait_edges.insert(e.from + "\x1f" + e.to);
    }
    std::vector<std::string> names(nodes.begin(), nodes.end());
    std::sort(names.begin(), names.end());
    std::map<std::string, int> idx;
    for (size_t i = 0; i < names.size(); i++) idx[names[i]] = (int)i;

    const int W = 1200, BOXW = 420, BH = 26, GAP = 10;
    int H = std::max(200, (int)names.size() * (BH + GAP) + 60);
    std::vector<int> y(names.size());
    for (size_t i = 0; i < names.size(); i++) y[i] = 30 + (int)i * (BH + GAP);

    FILE *f = std::fopen(outpath, "w");
    if (!f) { std::fprintf(out, "書けません: %s\n", outpath); return 1; }
    std::fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    std::fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\">\n", W, H, W, H);
    std::fprintf(f, "<rect width=\"100%%\" height=\"100%%\" fill=\"#11151c\"/>\n");
    std::fprintf(f, "<style>text{font-family:Menlo,monospace;fill:#c8d3e0;font-size:12px}.need{stroke:#4c8bf5;stroke-width:1.6;fill:none}.wait{stroke:#ff5f56;stroke-width:2.4;fill:none;stroke-dasharray:6 4}</style>\n");
    for (size_t i = 0; i < names.size(); i++) {
        std::fprintf(f, "<rect x=\"30\" y=\"%d\" width=\"%d\" height=\"%d\" rx=\"6\" fill=\"#1b2230\" stroke=\"#2f3a4d\"/>\n", y[i], BOXW, BH);
        std::fprintf(f, "<text x=\"40\" y=\"%d\">%s</text>\n", y[i] + 17, names[i].c_str());
    }
    for (const auto &e : edges) {
        auto ia = idx.find(e.first), ib = idx.find(e.second);
        if (ia == idx.end() || ib == idx.end()) continue;
        int x1 = 30 + BOXW, y1 = y[ia->second] + BH / 2;
        int x2 = x1, y2 = y[ib->second] + BH / 2;
        int mx = x1 + 80;
        const char *cls = wait_edges.count(e.first + "\x1f" + e.second) ? "wait" : "need";
        std::fprintf(f, "<path class=\"%s\" d=\"M%d %d C%d %d %d %d %d %d\"/>\n",
                     cls, x1, y1, mx, y1, mx, y2, x2, y2);
    }
    std::fprintf(f, "<text x=\"30\" y=\"%d\" fill=\"#5d6b7d\">necessary+state-dependent only: nodes=%zu edges=%zu (wait=%zu). irrelevant pruned.</text>\n",
                 H - 12, names.size(), edges.size(), wait_edges.size());
    std::fprintf(f, "</svg>\n");
    std::fclose(f);
    std::fprintf(out, "SVG: %s  nodes=%zu edges=%zu wait=%zu\n", outpath, names.size(), edges.size(), wait_edges.size());
    return 0;
}

// ---------------------------------------------------------------- class svg
// mangled 名から class と method を切り出す (テンプレート引数 I..E を読み飛ばす)。
static bool nx_class_of(const std::string &sym, std::string &cls, std::string &meth) {
    size_t z = sym.find("_Z");
    if (z == std::string::npos) return false;
    size_t i = z + 2;
    if (i >= sym.size() || sym[i] != 'N') return false;
    i++;
    std::vector<std::string> parts;
    while (i < sym.size()) {
        char c = sym[i];
        if (c == 'E') break;
        if (c == 'I') {
            int d = 1; i++;
            while (i < sym.size() && d > 0) { if (sym[i] == 'I') d++; else if (sym[i] == 'E') d--; i++; }
            continue;
        }
        if (std::isdigit((unsigned char)c)) {
            size_t len = 0;
            while (i < sym.size() && std::isdigit((unsigned char)sym[i])) { len = len * 10 + (sym[i] - '0'); i++; }
            if (i + len > sym.size()) return false;
            parts.push_back(sym.substr(i, len)); i += len;
            continue;
        }
        if (c == 'S') { while (i < sym.size() && sym[i] != '_') i++; if (i < sym.size()) i++; parts.push_back("?"); continue; }
        if (c == 'K' || c == 'V' || c == 'r') { i++; continue; }
        break;
    }
    if (parts.size() < 2) return false;
    meth = parts.back();
    cls = parts[0];
    for (size_t k = 1; k + 1 < parts.size(); k++) { cls += "::"; cls += parts[k]; }
    return true;
}

extern "C" int nx_class_svg(const char *path, uint64_t start, const char *outpath, int maxsteps, FILE *out) {
    if (!out) out = stdout;
    if (!path || !outpath) { std::fprintf(out, "使い方: neodax --class-svg ADDR OUT.svg\n"); return 2; }
    auto img = nx::MachOImage::open(path);
    if (!img) { std::fprintf(out, "[skip] %s\n", path); return 1; }
    if (maxsteps <= 0) maxsteps = 400;
    using Op = nx::Arm64::Op;
    std::set<uint64_t> visited;
    std::vector<uint64_t> work{ start };
    std::map<std::string, std::set<std::string>> classes;   // class -> methods
    std::set<std::pair<std::string, std::string>> edges;     // caller -> callee class
    std::string scls, smeth;
    const std::string *sn = img->name_at(start);
    if (sn) nx_class_of(*sn, scls, smeth);
    int steps = 0;
    while (!work.empty() && steps < maxsteps) {
        uint64_t pc = work.back(); work.pop_back();
        while (steps < maxsteps) {
            if (!visited.insert(pc).second) break;
            const uint8_t *q = img->at(pc, 4);
            if (!q) break;
            nx::Arm64::Insn in;
            steps++;
            if (!nx::Arm64::decode(pc, *(const uint32_t *)q, &in) ||
                in.op == Op::Invalid || in.op == Op::Unknown) { pc += 4; continue; }
            if (in.op == Op::Bl) {
                const std::string *fn = img->name_at(in.target);
                if (fn) {
                    std::string cc, cm;
                    if (nx_class_of(*fn, cc, cm)) {
                        classes[cc].insert(cm);
                        if (!scls.empty()) edges.insert({ scls, cc });
                    }
                }
                pc += 4; continue;
            }
            if (in.op == Op::B) { pc = in.target; continue; }
            if (in.op == Op::BCond || in.op == Op::Cbz || in.op == Op::Cbnz ||
                in.op == Op::Tbz || in.op == Op::Tbnz) { work.push_back(in.target); pc += 4; continue; }
            if (in.op == Op::Ret) break;
            pc += 4;
        }
    }
    if (!scls.empty()) { classes[scls]; if (!smeth.empty()) classes[scls].insert(smeth); }

    std::vector<std::string> names;
    for (const auto &kv : classes) names.push_back(kv.first);
    std::sort(names.begin(), names.end());
    std::map<std::string, int> idx;
    for (size_t i = 0; i < names.size(); i++) idx[names[i]] = (int)i;

    const int W = 1100, BOXW = 340, LH = 20, PAD = 14;
    std::vector<int> h(names.size());
    int totalH = 0;
    for (size_t i = 0; i < names.size(); i++) {
        h[i] = PAD * 2 + LH + (int)classes[names[i]].size() * LH;
        totalH += h[i] + 24;
    }
    if (totalH < 200) totalH = 200;
    int H = totalH + 40;
    std::vector<int> y(names.size());
    int cy = 20;
    for (size_t i = 0; i < names.size(); i++) { y[i] = cy; cy += h[i] + 24; }

    FILE *f = std::fopen(outpath, "w");
    if (!f) { std::fprintf(out, "書けません: %s\n", outpath); return 1; }
    std::fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    std::fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\">\n", W, H, W, H);
    std::fprintf(f, "<rect width=\"100%%\" height=\"100%%\" fill=\"#11151c\"/>\n");
    std::fprintf(f, "<style>text{font-family:Menlo,monospace;fill:#e6edf3}.cls{fill:#c9a227;font-weight:bold}.m{fill:#9fb3c8}.e{stroke:#4c8bf5;stroke-width:1.6;fill:none}.ann{stroke:#ff5f56;stroke-width:2.6;fill:none;stroke-dasharray:6 4}</style>\n");
    for (size_t i = 0; i < names.size(); i++) {
        int x = 40, yy = y[i];
        std::fprintf(f, "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" rx=\"8\" fill=\"#1b2230\" stroke=\"#2f3a4d\"/>\n", x, yy, BOXW, h[i]);
        std::fprintf(f, "<text class=\"cls\" x=\"%d\" y=\"%d\">%s</text>\n", x + PAD, yy + PAD + 14, names[i].c_str());
        int ty = yy + PAD + LH + 14;
        for (const auto &m : classes[names[i]]) {
            std::fprintf(f, "<text class=\"m\" x=\"%d\" y=\"%d\">&#8226; %s</text>\n", x + PAD + 10, ty, m.c_str());
            ty += LH;
        }
    }
    // edges: same-class = annulus-ish loop; cross-class = line
    int ri = 0;
    for (const auto &e : edges) {
        int a = idx[e.first], b = idx[e.second];
        if (a == b) continue;
        int x1 = 40 + BOXW, y1 = y[a] + h[a] / 2;
        int x2 = x1, y2 = y[b] + h[b] / 2;
        int mx = x1 + 70;
        std::fprintf(f, "<path class=\"e\" d=\"M%d %d C%d %d %d %d %d %d\" marker-end=\"url(#ar)\"/>\n",
                     x1, y1, mx, y1, mx, y2, x2, y2);
        ri++;
    }
    std::fprintf(f, "<defs><marker id=\"ar\" markerWidth=\"8\" markerHeight=\"8\" refX=\"6\" refY=\"4\" orient=\"auto\"><path d=\"M0 0 L8 4 L0 8 z\" fill=\"#4c8bf5\"/></marker></defs>\n");
    std::fprintf(f, "<text x=\"40\" y=\"%d\" fill=\"#5d6b7d\">classes=%zu edges=%d  (vector, no pixels)</text>\n", H - 12, names.size(), ri);
    std::fprintf(f, "</svg>\n");
    std::fclose(f);
    std::fprintf(out, "SVG: %s  classes=%zu edges=%d  steps=%d\n", outpath, names.size(), ri, steps);
    return 0;
}

// ---------------------------------------------------------------- motion
// 指定関数から制御流れを仮想実行する (注入なし)。落ちる側・無条件分岐を辿り、
// BL 呼び出しと条件分岐を「動き」として出す。レジスタ値は扱わない (構造の運動)。
extern "C" int nx_motion(const char *path, uint64_t start, int maxsteps, FILE *out) {
    if (!out) out = stdout;
    if (!path) return 2;
    auto img = nx::MachOImage::open(path);
    if (!img) { std::fprintf(out, "[skip] not a 64-bit Mach-O: %s\n", path); return 1; }
    nx::ImportResolver mimp; mimp.parse(*img);
    if (maxsteps <= 0) maxsteps = 400;
    std::set<uint64_t> visited;
    std::vector<std::pair<uint64_t, int>> work;
    work.push_back({ start, 0 });
    int steps = 0, calls = 0, branches = 0;
    using Op = nx::Arm64::Op;
    while (!work.empty() && steps < maxsteps) {
        uint64_t pc = work.back().first;
        int depth = work.back().second;
        work.pop_back();
        while (steps < maxsteps) {
            if (!visited.insert(pc).second) break;
            const uint8_t *p = img->at(pc, 4);
            if (!p) break;
            uint32_t w = *(const uint32_t *)p;
            nx::Arm64::Insn in;
            steps++;
            if (!nx::Arm64::decode(pc, w, &in) || in.op == Op::Invalid ||
                in.op == Op::Unknown) {
                pc += 4;      // 未対応命令は飛ばして運動を続ける
                continue;
            }
            if (in.op == Op::Bl) {
                const std::string *fn = img->name_at(in.target);
                std::string nm = fn ? *fn : std::string();
                if (nm.empty()) {
                    const uint8_t *sp = img->at(in.target, 12);
                    if (sp) {
                        nx::Arm64::Insn a, b, c;
                        nx::Arm64::decode(in.target, *(const uint32_t *)sp, &a);
                        nx::Arm64::decode(in.target + 4, *(const uint32_t *)(sp + 4), &b);
                        nx::Arm64::decode(in.target + 8, *(const uint32_t *)(sp + 8), &c);
                        if (a.op == Op::Adrp && b.op == Op::Ldr && c.op == Op::Br) {
                            const std::string *sym = mimp.symbol_at_slot(a.target + b.imm);
                            if (sym) nm = *sym;
                        }
                    }
                }
                std::fprintf(out, "%*sBL   0x%llx  %s\n", depth * 2, "",
                             (unsigned long long)in.target, nm.empty() ? "(local)" : nm.c_str());
                calls++;
                pc += 4;
                continue;
            }
            if (in.op == Op::B) { pc = in.target; continue; }
            if (in.op == Op::BCond || in.op == Op::Cbz || in.op == Op::Cbnz ||
                in.op == Op::Tbz || in.op == Op::Tbnz) {
                std::fprintf(out, "%*s%s -> 0x%llx  (alt)\n", depth * 2, "",
                             nx::Arm64::op_name(in.op), (unsigned long long)in.target);
                branches++;
                work.push_back({ in.target, depth + 1 });
                pc += 4;
                continue;
            }
            if (in.op == Op::Ret) break;
            pc += 4;
        }
    }
    std::fprintf(out, "movement: steps=%d calls=%d branches=%d visited=%zu\n",
                 steps, calls, branches, visited.size());
    return 0;
}

// ---------------------------------------------------------------- C++ classes
// Itanium C++ ABI の vtable/typeinfo から、クラス・基底・仮想メソッドを復元する。
// __ZTV<type>=vtable, __ZTI<type>=typeinfo。Obj-C は使わない。
extern "C" int dax_demangle(const char *mangled, char *out, size_t outsz);

static std::string nx_demangle_sym(const std::string &sym) {
    const char *m = sym.c_str();
    if (m[0] == '_') m++;              // Mach-O の余分な '_' を落とす
    char out[512];
    out[0] = 0;
    (void)dax_demangle(m, out, sizeof(out));
    return out[0] ? std::string(out) : sym;
}

extern "C" int nx_cpp_classes(const char *path, const char *needle, FILE *out) {
    if (!out) out = stdout;
    if (!path) return 2;
    auto img = nx::MachOImage::open(path);
    if (!img) { std::fprintf(out, "[skip] not a 64-bit Mach-O: %s\n", path); return 1; }
    const auto &syms = img->defined_symbols();

    // 関数/データシンボルを demangle し、最後の :: より前を「所有者クラス」とする。
    // これで非ポリモーフィックなクラス (vtable を持たない) も復元できる。
    std::map<std::string, std::set<std::string>> groups;
    for (const auto &kv : syms) {
        const std::string &sym = kv.first;
        if (sym.rfind("__Z", 0) != 0) continue;         // Itanium C++ のみ
        std::string d = nx_demangle_sym(sym);
        if (d == sym) continue;                           // demangle できず
        if (d.find("vtable for ") == 0 || d.find("typeinfo for ") == 0 ||
            d.find("VTT for ") == 0 || d.find("guard variable for ") == 0)
            continue;
        size_t p = d.rfind("::");
        if (p == std::string::npos) continue;
        std::string owner = d.substr(0, p);
        std::string member = d.substr(p + 2);
        if (owner.empty() || member.empty()) continue;
        groups[owner].insert(member);
    }

    int n = 0;
    for (const auto &g : groups) {
        const std::string &cls = g.first;
        if (needle && *needle && cls.find(needle) == std::string::npos) continue;
        std::fprintf(out, "class %s {\n public:\n", cls.c_str());
        for (const auto &m : g.second)
            std::fprintf(out, "  %s;\n", m.c_str());
        std::fprintf(out, "};\n\n");
        n++;
    }
    std::fprintf(out, "%s のクラスは %d 個です。\n", (needle && *needle) ? needle : "(all)", n);
    return n ? 0 : 1;
}

// ---------------------------------------------------------------- wire
// 汎用のバイト列プローブ。ws 専用にしない。
// connect → 任意のバイト列を送信 → 受信 → hexdump → 種別推定 (HTTP / WebSocket / TLS / raw)。
// ws も http も「送るバイト列が違うだけ」になる。
static int nx_hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// ---------------------------------------------------------------- transport
// plain と TLS の差をここに閉じる。--wire は conn_* だけを使う。
// TLS は CFNetwork の CFStream に任せる (DNS/TLS/信頼/SNI を自前で書かない)。
struct nx_conn {
    int tls = 0;
    int fd = -1;
    CFReadStreamRef rs = nullptr;
    CFWriteStreamRef ws = nullptr;
};

static bool nx_conn_open(const std::string &host, const std::string &port,
                         int tls, int timeout_ms, nx_conn &c, FILE *out) {
    if (tls) {
        CFStringRef h = CFStringCreateWithCString(NULL, host.c_str(), kCFStringEncodingUTF8);
        if (!h) return false;
        CFStreamCreatePairWithSocketToHost(NULL, h, (UInt32)std::atoi(port.c_str()),
                                           &c.rs, &c.ws);
        CFRelease(h);
        if (!c.rs || !c.ws) return false;
        CFMutableDictionaryRef d = CFDictionaryCreateMutable(
            NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFStringRef peer = CFStringCreateWithCString(NULL, host.c_str(), kCFStringEncodingUTF8);
        CFDictionarySetValue(d, kCFStreamSSLPeerName, peer);
        CFDictionarySetValue(d, kCFStreamSSLLevel, kCFStreamSocketSecurityLevelNegotiatedSSL);
        CFReadStreamSetProperty(c.rs, kCFStreamPropertySSLSettings, d);
        CFWriteStreamSetProperty(c.ws, kCFStreamPropertySSLSettings, d);
        CFRelease(d);
        CFRelease(peer);
        if (!CFReadStreamOpen(c.rs) || !CFWriteStreamOpen(c.ws)) return false;
        CFTimeInterval dl = CFAbsoluteTimeGetCurrent() + timeout_ms / 1000.0;
        for (;;) {
            CFStreamStatus st = CFReadStreamGetStatus(c.rs);
            if (st == kCFStreamStatusOpen) break;
            if (st == kCFStreamStatusError || st == kCFStreamStatusClosed) return false;
            if (CFAbsoluteTimeGetCurrent() > dl) return false;
            usleep(10000);
        }
        c.tls = 1;
        return true;
    }
    struct addrinfo hints, *res = nullptr;
    std::memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        std::fprintf(out, "解決できません: %s:%s\n", host.c_str(), port.c_str());
        return false;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return false; }
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000; tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        std::fprintf(out, "接続失敗: %s:%s (%s)\n", host.c_str(), port.c_str(), std::strerror(errno));
        close(fd); freeaddrinfo(res); return false;
    }
    freeaddrinfo(res);
    c.fd = fd;
    return true;
}

static ssize_t nx_conn_write(nx_conn &c, const void *buf, size_t n) {
    if (c.tls) return (ssize_t)CFWriteStreamWrite(c.ws, (const UInt8 *)buf, (CFIndex)n);
    return send(c.fd, buf, n, 0);
}

static ssize_t nx_conn_read(nx_conn &c, void *buf, size_t n, int timeout_ms) {
    if (!c.tls) return recv(c.fd, buf, n, 0);
    CFTimeInterval dl = CFAbsoluteTimeGetCurrent() + timeout_ms / 1000.0;
    for (;;) {
        if (CFReadStreamHasBytesAvailable(c.rs)) {
            CFIndex r = CFReadStreamRead(c.rs, (UInt8 *)buf, (CFIndex)n);
            if (r < 0) return -1;
            return (ssize_t)r;
        }
        CFStreamStatus st = CFReadStreamGetStatus(c.rs);
        if (st == kCFStreamStatusAtEnd) return 0;
        if (st == kCFStreamStatusError || st == kCFStreamStatusClosed) return -1;
        if (CFAbsoluteTimeGetCurrent() > dl) return 0;
        usleep(5000);
    }
}

static void nx_conn_close(nx_conn &c) {
    if (c.tls) {
        if (c.rs) { CFReadStreamClose(c.rs); CFRelease(c.rs); c.rs = nullptr; }
        if (c.ws) { CFWriteStreamClose(c.ws); CFRelease(c.ws); c.ws = nullptr; }
    } else if (c.fd >= 0) {
        close(c.fd);
        c.fd = -1;
    }
}

// 保険用の保存先を作る (mkdir -p)。
static void nx_mkdir_p(const std::string &p) {
    std::string cur;
    for (size_t i = 0; i < p.size(); i++) {
        cur += p[i];
        if (p[i] == '/' && cur.size() > 1) mkdir(cur.c_str(), 0755);
    }
    if (!p.empty()) mkdir(p.c_str(), 0755);
}

extern "C" int nx_wire(const char *target, const char *send_txt,
                       const char *send_hex, int read_n, int timeout_ms,
                       const char *save_path, FILE *out) {
    if (!out) out = stdout;
    if (!target || !*target) {
        std::fprintf(out, "使い方: neodax --wire [https://]host:port[/path] [--send-text S] [--send-hex H] [--count N] [--save F]\n");
        return 2;
    }
    std::string t(target);
    int tls = 0, httpish = 0;
    if (t.rfind("https://", 0) == 0) { tls = 1; httpish = 1; t = t.substr(8); }
    else if (t.rfind("tls://", 0) == 0) { tls = 1; t = t.substr(6); }
    else if (t.rfind("http://", 0) == 0) { httpish = 1; t = t.substr(7); }
    std::string path = "/";
    size_t slash = t.find('/');
    if (slash != std::string::npos) { path = t.substr(slash); t = t.substr(0, slash); }
    std::string host, ports;
    size_t c = t.rfind(':');
    if (c == std::string::npos) { host = t; ports = tls ? "443" : "80"; }
    else { host = t.substr(0, c); ports = t.substr(c + 1); }
    if (host.empty()) host = "127.0.0.1";
    if (read_n <= 0) read_n = 512;
    if (timeout_ms <= 0) timeout_ms = 2000;

    nx_conn conn;
    if (!nx_conn_open(host, ports, tls, timeout_ms, conn, out)) return 1;

    std::vector<unsigned char> sbuf;
    if (send_hex && *send_hex) {
        int hi = -1;
        for (const char *p = send_hex; *p; p++) {
            int v = nx_hexval(*p);
            if (v < 0) { hi = -1; continue; }
            if (hi < 0) hi = v;
            else { sbuf.push_back((unsigned char)((hi << 4) | v)); hi = -1; }
        }
    } else if (send_txt && *send_txt) {
        sbuf.assign(send_txt, send_txt + std::strlen(send_txt));
    } else if (httpish) {
        // 低コードの FETCH: 送信未指定なら最小の GET を組む。
        std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host +
                          "\r\nConnection: close\r\n\r\n";
        sbuf.assign(req.begin(), req.end());
    }
    if (!sbuf.empty()) {
        ssize_t w = nx_conn_write(conn, sbuf.data(), sbuf.size());
        std::fprintf(out, "送信 %zd バイト\n", (ssize_t)w);
    }

    std::vector<unsigned char> rbuf((size_t)read_n);
    ssize_t total = 0;
    while (total < read_n) {
        ssize_t n = nx_conn_read(conn, rbuf.data() + total, (size_t)(read_n - total), timeout_ms);
        if (n <= 0) break;
        total += n;
    }
    nx_conn_close(conn);
    if (total <= 0) { std::fprintf(out, "受信 0 バイト\n"); return 1; }

    for (ssize_t i = 0; i < total; i += 16) {
        std::fprintf(out, "%08zx  ", (size_t)i);
        for (ssize_t j = 0; j < 16; j++) {
            if (i + j < total) std::fprintf(out, "%02x ", rbuf[(size_t)(i + j)]);
            else std::fprintf(out, "   ");
        }
        std::fprintf(out, " |");
        for (ssize_t j = 0; j < 16 && i + j < total; j++) {
            unsigned char ch = rbuf[(size_t)(i + j)];
            std::fputc((ch >= 0x20 && ch < 0x7f) ? ch : '.', out);
        }
        std::fprintf(out, "|\n");
    }

    std::string save_local;
    const char *savep = save_path;
    if (!save_path || !*save_path) {
        // 指定がなくても必ず残す (保険)。~/.neodax/wire/<host>-<time>.bin
        const char *h = std::getenv("HOME");
        if (h) {
            std::string dir = std::string(h) + "/.neodax/wire";
            nx_mkdir_p(dir);
            std::string safe;
            for (char ch : host)
                safe += (std::isalnum((unsigned char)ch) || ch == '.' || ch == '-') ? ch : '_';
            char nm[96];
            std::snprintf(nm, sizeof nm, "/%s-%ld.bin", safe.c_str(), (long)std::time(nullptr));
            save_local = dir + nm;
            savep = save_local.c_str();
        }
    }
    if (savep && *savep) {
        std::ofstream sf(savep, std::ios::binary | std::ios::trunc);
        if (sf) {
            sf.write((const char *)rbuf.data(), total);
            std::fprintf(out, "保存: %s (%zd バイト)\n", savep, total);
        } else {
            std::fprintf(out, "保存できません: %s\n", savep);
        }
    }

    std::string head((const char *)rbuf.data(), (size_t)std::min<ssize_t>(total, 64));
    const char *kind = "raw";
    if (head.rfind("HTTP/", 0) == 0)
        kind = (head.find(" 101 ") != std::string::npos ||
                head.find("Upgrade: websocket") != std::string::npos) ? "WebSocket" : "HTTP";
    else if (rbuf[0] == 0x16) kind = "TLS";
    else if (head.rfind("GET ", 0) == 0 || head.rfind("POST ", 0) == 0) kind = "HTTP request";
    std::fprintf(out, "受信 %zd バイト / 種別: %s\n", total, kind);
    return 0;
}
