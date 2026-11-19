#pragma once
#include <cstdint>
#include <vector>
#include <string>

namespace nx_generated {

// generated from nx_spec "ws_frame" (family: websocket)
// WebSocket frame (RFC6455): FIN/opcode + MASK/payload-len
struct WsFrame {
    enum class Opcode : uint8_t {
        reserved0 = 0,
        text = 1,
        binary = 2,
        reserved3 = 3,
        reserved4 = 4,
        reserved5 = 5,
        reserved6 = 6,
        reserved7 = 7,
        close = 8,
        ping = 9,
        pong = 10,
        reserved11 = 11,
        reserved12 = 12,
        reserved13 = 13,
        reserved14 = 14,
        reserved15 = 15,
    };
    uint8_t fin = 0;
    uint8_t rsv1 = 0;
    uint8_t rsv2 = 0;
    uint8_t rsv3 = 0;
    Opcode opcode = Opcode::reserved0;
    uint8_t masked = 0;
    uint8_t payload_len = 0;
    std::vector<uint8_t> payload;   // trailing bytes

    bool parse(const uint8_t* data, size_t size, size_t* consumed = nullptr) {
        if (0 + 1 > size) return false;
        fin = (uint8_t)(((uint64_t)data[0] >> 7) & 0x1ULL);
        if (0 + 1 > size) return false;
        rsv1 = (uint8_t)(((uint64_t)data[0] >> 6) & 0x1ULL);
        if (0 + 1 > size) return false;
        rsv2 = (uint8_t)(((uint64_t)data[0] >> 5) & 0x1ULL);
        if (0 + 1 > size) return false;
        rsv3 = (uint8_t)(((uint64_t)data[0] >> 4) & 0x1ULL);
        if (0 + 1 > size) return false;
        opcode = (Opcode)(((uint64_t)data[0] >> 0) & 0xfULL);
        if (1 + 1 > size) return false;
        masked = (uint8_t)(((uint64_t)data[1] >> 7) & 0x1ULL);
        if (1 + 1 > size) return false;
        payload_len = (uint8_t)(((uint64_t)data[1] >> 0) & 0x7fULL);
        if (size > 2) payload.assign(data + 2, data + size);
        if (consumed) *consumed = size;
        return true;
    }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> out(2, 0);
        out[0] |= (uint8_t)(((uint64_t)fin & 0x1ULL) << 7);
        out[0] |= (uint8_t)(((uint64_t)rsv1 & 0x1ULL) << 6);
        out[0] |= (uint8_t)(((uint64_t)rsv2 & 0x1ULL) << 5);
        out[0] |= (uint8_t)(((uint64_t)rsv3 & 0x1ULL) << 4);
        out[0] |= (uint8_t)(((uint64_t)opcode & 0xfULL) << 0);
        out[1] |= (uint8_t)(((uint64_t)masked & 0x1ULL) << 7);
        out[1] |= (uint8_t)(((uint64_t)payload_len & 0x7fULL) << 0);
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    static const char* spec_name() { return "ws_frame"; }
};

// generated from nx_spec "lenprefixed_be" (family: length-prefixed)
// big-endian 32-bit length followed by payload
struct LenprefixedBe {
    uint32_t length = 0;
    std::vector<uint8_t> body;   // length-prefixed payload
    std::vector<uint8_t> payload;   // trailing bytes

    bool parse(const uint8_t* data, size_t size, size_t* consumed = nullptr) {
        if (0 + 4 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<4;++i)v=(v<<8)|data[0+i];for(int i=0;i<4;++i)u|=(uint64_t)data[0+i]<<(8*i); length=(uint32_t)(v); }
        if (size > 5) payload.assign(data + 5, data + size);
        if (consumed) *consumed = size;
        return true;
    }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> out(5, 0);
        { uint64_t v=(uint64_t)length; for(int i=0;i<4;++i) out[0+i]=(uint8_t)((v>>(8*(4-1-i)))&0xff); }
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    static const char* spec_name() { return "lenprefixed_be"; }
};

// generated from nx_spec "http_status_line" (family: http)
// HTTP status line (HTTP/x.y NNN ...)
struct HttpStatusLine {
    uint16_t status = 0;
    std::vector<uint8_t> payload;   // trailing bytes

    bool parse(const uint8_t* data, size_t size, size_t* consumed = nullptr) {
        if (9 + 2 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<2;++i)v=(v<<8)|data[9+i];for(int i=0;i<2;++i)u|=(uint64_t)data[9+i]<<(8*i); status=(uint16_t)(v); }
        if (size > 11) payload.assign(data + 11, data + size);
        if (consumed) *consumed = size;
        return true;
    }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> out(11, 0);
        { uint64_t v=(uint64_t)status; for(int i=0;i<2;++i) out[9+i]=(uint8_t)((v>>(8*(2-1-i)))&0xff); }
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    static const char* spec_name() { return "http_status_line"; }
};

// generated from nx_spec "pe_dos_header" (family: pe)
// PE DOS header (MZ + e_lfanew)
struct PeDosHeader {
    uint16_t e_magic = 0;
    uint32_t e_lfanew = 0;
    std::vector<uint8_t> payload;   // trailing bytes

    bool parse(const uint8_t* data, size_t size, size_t* consumed = nullptr) {
        if (0 + 2 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<2;++i)v=(v<<8)|data[0+i];for(int i=0;i<2;++i)u|=(uint64_t)data[0+i]<<(8*i); e_magic=(uint16_t)(u); }
        if (60 + 4 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<4;++i)v=(v<<8)|data[60+i];for(int i=0;i<4;++i)u|=(uint64_t)data[60+i]<<(8*i); e_lfanew=(uint32_t)(u); }
        if (size > 64) payload.assign(data + 64, data + size);
        if (consumed) *consumed = size;
        return true;
    }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> out(64, 0);
        { uint64_t v=(uint64_t)e_magic; for(int i=0;i<2;++i) out[0+i]=(uint8_t)((v>>(8*i))&0xff); }
        { uint64_t v=(uint64_t)e_lfanew; for(int i=0;i<4;++i) out[60+i]=(uint8_t)((v>>(8*i))&0xff); }
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    static const char* spec_name() { return "pe_dos_header"; }
};

// generated from nx_spec "pe_nt_signature" (family: pe)
// PE NT signature (PE\0\0)
struct PeNtSignature {
    std::vector<uint8_t> payload;   // trailing bytes

    bool parse(const uint8_t* data, size_t size, size_t* consumed = nullptr) {
        if (size > 1) payload.assign(data + 1, data + size);
        if (consumed) *consumed = size;
        return true;
    }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> out(1, 0);
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    static const char* spec_name() { return "pe_nt_signature"; }
};

// generated from nx_spec "pe_file_header" (family: pe)
// PE COFF file header
struct PeFileHeader {
    uint16_t machine = 0;
    uint16_t number_of_sections = 0;
    uint32_t time_date_stamp = 0;
    std::vector<uint8_t> payload;   // trailing bytes

    bool parse(const uint8_t* data, size_t size, size_t* consumed = nullptr) {
        if (0 + 2 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<2;++i)v=(v<<8)|data[0+i];for(int i=0;i<2;++i)u|=(uint64_t)data[0+i]<<(8*i); machine=(uint16_t)(u); }
        if (2 + 2 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<2;++i)v=(v<<8)|data[2+i];for(int i=0;i<2;++i)u|=(uint64_t)data[2+i]<<(8*i); number_of_sections=(uint16_t)(u); }
        if (4 + 4 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<4;++i)v=(v<<8)|data[4+i];for(int i=0;i<4;++i)u|=(uint64_t)data[4+i]<<(8*i); time_date_stamp=(uint32_t)(u); }
        if (size > 8) payload.assign(data + 8, data + size);
        if (consumed) *consumed = size;
        return true;
    }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> out(8, 0);
        { uint64_t v=(uint64_t)machine; for(int i=0;i<2;++i) out[0+i]=(uint8_t)((v>>(8*i))&0xff); }
        { uint64_t v=(uint64_t)number_of_sections; for(int i=0;i<2;++i) out[2+i]=(uint8_t)((v>>(8*i))&0xff); }
        { uint64_t v=(uint64_t)time_date_stamp; for(int i=0;i<4;++i) out[4+i]=(uint8_t)((v>>(8*i))&0xff); }
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    static const char* spec_name() { return "pe_file_header"; }
};

// generated from nx_spec "pe_section_header" (family: pe)
// PE section table entry (40 bytes)
struct PeSectionHeader {
    std::string name;
    uint32_t virtual_size = 0;
    uint32_t virtual_address = 0;
    uint32_t size_of_raw_data = 0;
    uint32_t pointer_to_raw = 0;
    std::vector<uint8_t> payload;   // trailing bytes

    bool parse(const uint8_t* data, size_t size, size_t* consumed = nullptr) {
        if (0 + 8 > size) return false;
        name.assign(reinterpret_cast<const char*>(data + 0), 8);
        if (8 + 4 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<4;++i)v=(v<<8)|data[8+i];for(int i=0;i<4;++i)u|=(uint64_t)data[8+i]<<(8*i); virtual_size=(uint32_t)(u); }
        if (12 + 4 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<4;++i)v=(v<<8)|data[12+i];for(int i=0;i<4;++i)u|=(uint64_t)data[12+i]<<(8*i); virtual_address=(uint32_t)(u); }
        if (16 + 4 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<4;++i)v=(v<<8)|data[16+i];for(int i=0;i<4;++i)u|=(uint64_t)data[16+i]<<(8*i); size_of_raw_data=(uint32_t)(u); }
        if (20 + 4 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<4;++i)v=(v<<8)|data[20+i];for(int i=0;i<4;++i)u|=(uint64_t)data[20+i]<<(8*i); pointer_to_raw=(uint32_t)(u); }
        if (size > 24) payload.assign(data + 24, data + size);
        if (consumed) *consumed = size;
        return true;
    }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> out(24, 0);
        for (size_t i=0;i<name.size() && 0+i<out.size();++i) out[0+i]=(uint8_t)name[i];
        { uint64_t v=(uint64_t)virtual_size; for(int i=0;i<4;++i) out[8+i]=(uint8_t)((v>>(8*i))&0xff); }
        { uint64_t v=(uint64_t)virtual_address; for(int i=0;i<4;++i) out[12+i]=(uint8_t)((v>>(8*i))&0xff); }
        { uint64_t v=(uint64_t)size_of_raw_data; for(int i=0;i<4;++i) out[16+i]=(uint8_t)((v>>(8*i))&0xff); }
        { uint64_t v=(uint64_t)pointer_to_raw; for(int i=0;i<4;++i) out[20+i]=(uint8_t)((v>>(8*i))&0xff); }
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    static const char* spec_name() { return "pe_section_header"; }
};

// generated from nx_spec "macho64" (family: macho)
// Mach-O 64-bit (magic feedfacf; .o=filetype object)
struct Macho64 {
    enum class Filetype : uint8_t {
        ?0 = 0,
        object = 1,
        execute = 2,
        fvmlib = 3,
        core = 4,
        preload = 5,
        dylib = 6,
        dylinker = 7,
        bundle = 8,
        dylib_stub = 9,
        dsym = 10,
        kext_bundle = 11,
    };
    uint32_t magic = 0;
    uint32_t cputype = 0;
    uint32_t cpusubtype = 0;
    Filetype filetype = Filetype::?0;
    uint32_t ncmds = 0;
    uint32_t sizeofcmds = 0;
    std::vector<uint8_t> payload;   // trailing bytes

    bool parse(const uint8_t* data, size_t size, size_t* consumed = nullptr) {
        if (0 + 4 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<4;++i)v=(v<<8)|data[0+i];for(int i=0;i<4;++i)u|=(uint64_t)data[0+i]<<(8*i); magic=(uint32_t)(u); }
        if (4 + 4 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<4;++i)v=(v<<8)|data[4+i];for(int i=0;i<4;++i)u|=(uint64_t)data[4+i]<<(8*i); cputype=(uint32_t)(u); }
        if (8 + 4 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<4;++i)v=(v<<8)|data[8+i];for(int i=0;i<4;++i)u|=(uint64_t)data[8+i]<<(8*i); cpusubtype=(uint32_t)(u); }
        if (12 + 4 > size) return false;
        filetype = (Filetype)data[12];
        if (16 + 4 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<4;++i)v=(v<<8)|data[16+i];for(int i=0;i<4;++i)u|=(uint64_t)data[16+i]<<(8*i); ncmds=(uint32_t)(u); }
        if (20 + 4 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<4;++i)v=(v<<8)|data[20+i];for(int i=0;i<4;++i)u|=(uint64_t)data[20+i]<<(8*i); sizeofcmds=(uint32_t)(u); }
        if (size > 24) payload.assign(data + 24, data + size);
        if (consumed) *consumed = size;
        return true;
    }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> out(24, 0);
        { uint64_t v=(uint64_t)magic; for(int i=0;i<4;++i) out[0+i]=(uint8_t)((v>>(8*i))&0xff); }
        { uint64_t v=(uint64_t)cputype; for(int i=0;i<4;++i) out[4+i]=(uint8_t)((v>>(8*i))&0xff); }
        { uint64_t v=(uint64_t)cpusubtype; for(int i=0;i<4;++i) out[8+i]=(uint8_t)((v>>(8*i))&0xff); }
        out[12] = (uint8_t)filetype;
        { uint64_t v=(uint64_t)ncmds; for(int i=0;i<4;++i) out[16+i]=(uint8_t)((v>>(8*i))&0xff); }
        { uint64_t v=(uint64_t)sizeofcmds; for(int i=0;i<4;++i) out[20+i]=(uint8_t)((v>>(8*i))&0xff); }
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    static const char* spec_name() { return "macho64"; }
};

// generated from nx_spec "macho32" (family: macho)
// Mach-O 32-bit (magic feedface)
struct Macho32 {
    enum class Filetype : uint8_t {
        ?0 = 0,
        object = 1,
        execute = 2,
        fvmlib = 3,
        core = 4,
        preload = 5,
        dylib = 6,
        dylinker = 7,
        bundle = 8,
        dylib_stub = 9,
        dsym = 10,
        kext_bundle = 11,
    };
    uint32_t magic = 0;
    uint32_t cputype = 0;
    uint32_t cpusubtype = 0;
    Filetype filetype = Filetype::?0;
    uint32_t ncmds = 0;
    uint32_t sizeofcmds = 0;
    std::vector<uint8_t> payload;   // trailing bytes

    bool parse(const uint8_t* data, size_t size, size_t* consumed = nullptr) {
        if (0 + 4 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<4;++i)v=(v<<8)|data[0+i];for(int i=0;i<4;++i)u|=(uint64_t)data[0+i]<<(8*i); magic=(uint32_t)(u); }
        if (4 + 4 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<4;++i)v=(v<<8)|data[4+i];for(int i=0;i<4;++i)u|=(uint64_t)data[4+i]<<(8*i); cputype=(uint32_t)(u); }
        if (8 + 4 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<4;++i)v=(v<<8)|data[8+i];for(int i=0;i<4;++i)u|=(uint64_t)data[8+i]<<(8*i); cpusubtype=(uint32_t)(u); }
        if (12 + 4 > size) return false;
        filetype = (Filetype)data[12];
        if (16 + 4 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<4;++i)v=(v<<8)|data[16+i];for(int i=0;i<4;++i)u|=(uint64_t)data[16+i]<<(8*i); ncmds=(uint32_t)(u); }
        if (20 + 4 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<4;++i)v=(v<<8)|data[20+i];for(int i=0;i<4;++i)u|=(uint64_t)data[20+i]<<(8*i); sizeofcmds=(uint32_t)(u); }
        if (size > 24) payload.assign(data + 24, data + size);
        if (consumed) *consumed = size;
        return true;
    }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> out(24, 0);
        { uint64_t v=(uint64_t)magic; for(int i=0;i<4;++i) out[0+i]=(uint8_t)((v>>(8*i))&0xff); }
        { uint64_t v=(uint64_t)cputype; for(int i=0;i<4;++i) out[4+i]=(uint8_t)((v>>(8*i))&0xff); }
        { uint64_t v=(uint64_t)cpusubtype; for(int i=0;i<4;++i) out[8+i]=(uint8_t)((v>>(8*i))&0xff); }
        out[12] = (uint8_t)filetype;
        { uint64_t v=(uint64_t)ncmds; for(int i=0;i<4;++i) out[16+i]=(uint8_t)((v>>(8*i))&0xff); }
        { uint64_t v=(uint64_t)sizeofcmds; for(int i=0;i<4;++i) out[20+i]=(uint8_t)((v>>(8*i))&0xff); }
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    static const char* spec_name() { return "macho32"; }
};

// generated from nx_spec "macho_fat" (family: macho)
// Mach-O universal binary (cafebabe + nfat_arch)
struct MachoFat {
    uint32_t magic = 0;
    uint32_t nfat_arch = 0;
    std::vector<uint8_t> payload;   // trailing bytes

    bool parse(const uint8_t* data, size_t size, size_t* consumed = nullptr) {
        if (0 + 4 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<4;++i)v=(v<<8)|data[0+i];for(int i=0;i<4;++i)u|=(uint64_t)data[0+i]<<(8*i); magic=(uint32_t)(v); }
        if (4 + 4 > size) return false;
        { uint64_t v=0,u=0; for(int i=0;i<4;++i)v=(v<<8)|data[4+i];for(int i=0;i<4;++i)u|=(uint64_t)data[4+i]<<(8*i); nfat_arch=(uint32_t)(v); }
        if (size > 8) payload.assign(data + 8, data + size);
        if (consumed) *consumed = size;
        return true;
    }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> out(8, 0);
        { uint64_t v=(uint64_t)magic; for(int i=0;i<4;++i) out[0+i]=(uint8_t)((v>>(8*(4-1-i)))&0xff); }
        { uint64_t v=(uint64_t)nfat_arch; for(int i=0;i<4;++i) out[4+i]=(uint8_t)((v>>(8*(4-1-i)))&0xff); }
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    static const char* spec_name() { return "macho_fat"; }
};

// generated from nx_spec "json_state" (family: persisted-state)
// persisted workspace state ({"state":{"tabs":...}})
struct JsonState {
    std::string prefix;
    std::vector<uint8_t> payload;   // trailing bytes

    bool parse(const uint8_t* data, size_t size, size_t* consumed = nullptr) {
        if (0 + 8 > size) return false;
        prefix.assign(reinterpret_cast<const char*>(data + 0), 8);
        if (size > 8) payload.assign(data + 8, data + size);
        if (consumed) *consumed = size;
        return true;
    }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> out(8, 0);
        for (size_t i=0;i<prefix.size() && 0+i<out.size();++i) out[0+i]=(uint8_t)prefix[i];
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    static const char* spec_name() { return "json_state"; }
};

// generated from nx_spec "json_persist" (family: persisted-state)
// zustand persist envelope ({"state":...,"version":N})
struct JsonPersist {
    std::string prefix;
    std::vector<uint8_t> payload;   // trailing bytes

    bool parse(const uint8_t* data, size_t size, size_t* consumed = nullptr) {
        if (0 + 8 > size) return false;
        prefix.assign(reinterpret_cast<const char*>(data + 0), 8);
        if (size > 8) payload.assign(data + 8, data + size);
        if (consumed) *consumed = size;
        return true;
    }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> out(8, 0);
        for (size_t i=0;i<prefix.size() && 0+i<out.size();++i) out[0+i]=(uint8_t)prefix[i];
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    static const char* spec_name() { return "json_persist"; }
};

// generated from nx_spec "leveldb_ls_record" (family: persisted-state)
// Chromium localStorage record (_origin\0\1key)
struct LeveldbLsRecord {
    std::string origin;
    std::vector<uint8_t> payload;   // trailing bytes

    bool parse(const uint8_t* data, size_t size, size_t* consumed = nullptr) {
        if (1 + 24 > size) return false;
        origin.assign(reinterpret_cast<const char*>(data + 1), 24);
        if (size > 25) payload.assign(data + 25, data + size);
        if (consumed) *consumed = size;
        return true;
    }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> out(25, 0);
        for (size_t i=0;i<origin.size() && 1+i<out.size();++i) out[1+i]=(uint8_t)origin[i];
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    static const char* spec_name() { return "leveldb_ls_record"; }
};

} // namespace nx_generated
