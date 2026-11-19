// generated from nx_spec "leveldb_ls_record" (family: persisted-state)
// Chromium localStorage record (_origin\0\1key)
struct LocalStorageRecord {
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

