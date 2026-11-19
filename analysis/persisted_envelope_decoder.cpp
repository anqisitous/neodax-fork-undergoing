// generated from nx_spec "json_persist" (family: persisted-state)
// zustand persist envelope ({"state":...,"version":N})
struct PersistedEnvelope {
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

