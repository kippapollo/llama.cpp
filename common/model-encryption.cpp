#include "model-encryption.h"

// xxhash is used as a fast keyed hash / stream generator for the model
// encryption container. This is obfuscation-grade, not strong cryptography.
#define XXH_INLINE_ALL
#include "xxhash/xxhash.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <chrono>
#include <random>
#include <sstream>
#include <vector>


namespace {

static inline void dbg(const char *) {}

static constexpr size_t SALT_SZ   = 16;
static constexpr size_t NONCE_SZ  = 16;
static constexpr size_t TAG_SZ    = 16; // XXH3_128bits

static constexpr uint32_t LLAMA_ENC_VERSION = 1;
static constexpr uint32_t DEFAULT_CHUNK_SIZE = 4u * 1024u * 1024u;

static const uint8_t MAGIC[8] = { 'L','G','G','U','F','E','N','C' };

static void write_u32_le(std::ostream & os, uint32_t v) {
    uint8_t b[4] = {
        (uint8_t)(v & 0xFFu),
        (uint8_t)((v >> 8) & 0xFFu),
        (uint8_t)((v >> 16) & 0xFFu),
        (uint8_t)((v >> 24) & 0xFFu),
    };
    os.write((const char *) b, sizeof(b));
}

static void write_u64_le(std::ostream & os, uint64_t v) {
    uint8_t b[8] = {
        (uint8_t)(v & 0xFFu),
        (uint8_t)((v >> 8) & 0xFFu),
        (uint8_t)((v >> 16) & 0xFFu),
        (uint8_t)((v >> 24) & 0xFFu),
        (uint8_t)((v >> 32) & 0xFFu),
        (uint8_t)((v >> 40) & 0xFFu),
        (uint8_t)((v >> 48) & 0xFFu),
        (uint8_t)((v >> 56) & 0xFFu),
    };
    os.write((const char *) b, sizeof(b));
}

static bool read_u32_le(std::istream & is, uint32_t & v) {
    uint8_t b[4];
    if (!is.read((char *) b, sizeof(b))) {
        return false;
    }
    v = (uint32_t)b[0] |
        ((uint32_t)b[1] << 8) |
        ((uint32_t)b[2] << 16) |
        ((uint32_t)b[3] << 24);
    return true;
}

static bool read_u64_le(std::istream & is, uint64_t & v) {
    uint8_t b[8];
    if (!is.read((char *) b, sizeof(b))) {
        return false;
    }
    v = (uint64_t)b[0] |
        ((uint64_t)b[1] << 8) |
        ((uint64_t)b[2] << 16) |
        ((uint64_t)b[3] << 24) |
        ((uint64_t)b[4] << 32) |
        ((uint64_t)b[5] << 40) |
        ((uint64_t)b[6] << 48) |
        ((uint64_t)b[7] << 56);
    return true;
}

static bool read_exact(std::istream & is, void * dst, size_t n) {
    return (bool) is.read((char *) dst, n);
}

static bool fwrite_exact(FILE * f, const void * src, size_t n, std::string & err) {
    if (n == 0) {
        return true;
    }
    if (std::fwrite(src, 1, n, f) != n) {
        err = std::string("write failed: ") + std::strerror(errno);
        return false;
    }
    return true;
}

static bool fread_exact(FILE * f, void * dst, size_t n, std::string & err) {
    if (n == 0) {
        return true;
    }
    if (std::fread(dst, 1, n, f) != n) {
        err = std::string("read failed: ") + std::strerror(errno);
        return false;
    }
    return true;
}

static bool fwrite_u32_le(FILE * f, uint32_t v, std::string & err) {
    uint8_t b[4] = {
        (uint8_t)(v & 0xFFu),
        (uint8_t)((v >> 8) & 0xFFu),
        (uint8_t)((v >> 16) & 0xFFu),
        (uint8_t)((v >> 24) & 0xFFu),
    };
    return fwrite_exact(f, b, sizeof(b), err);
}

static bool fwrite_u64_le(FILE * f, uint64_t v, std::string & err) {
    uint8_t b[8] = {
        (uint8_t)(v & 0xFFu),
        (uint8_t)((v >> 8) & 0xFFu),
        (uint8_t)((v >> 16) & 0xFFu),
        (uint8_t)((v >> 24) & 0xFFu),
        (uint8_t)((v >> 32) & 0xFFu),
        (uint8_t)((v >> 40) & 0xFFu),
        (uint8_t)((v >> 48) & 0xFFu),
        (uint8_t)((v >> 56) & 0xFFu),
    };
    return fwrite_exact(f, b, sizeof(b), err);
}

static bool fread_u32_le(FILE * f, uint32_t & v, std::string & err) {
    uint8_t b[4];
    if (!fread_exact(f, b, sizeof(b), err)) {
        return false;
    }
    v = (uint32_t)b[0] |
        ((uint32_t)b[1] << 8) |
        ((uint32_t)b[2] << 16) |
        ((uint32_t)b[3] << 24);
    return true;
}

static bool fread_u64_le(FILE * f, uint64_t & v, std::string & err) {
    uint8_t b[8];
    if (!fread_exact(f, b, sizeof(b), err)) {
        return false;
    }
    v = (uint64_t)b[0] |
        ((uint64_t)b[1] << 8) |
        ((uint64_t)b[2] << 16) |
        ((uint64_t)b[3] << 24) |
        ((uint64_t)b[4] << 32) |
        ((uint64_t)b[5] << 40) |
        ((uint64_t)b[6] << 48) |
        ((uint64_t)b[7] << 56);
    return true;
}

static uint64_t xxh64_of(const void * data, size_t n, uint64_t seed = 0) {
    return (uint64_t) XXH3_64bits_withSeed(data, n, (XXH64_hash_t) seed);
}

static uint64_t weak_seed_u64() {
    // Avoid std::random_device: on some MinGW builds it can be unreliable.
    const uint64_t t = (uint64_t) std::chrono::high_resolution_clock::now().time_since_epoch().count();
    uint64_t x = t;
    x ^= (uint64_t) (uintptr_t) &t;
    x ^= (uint64_t) (uintptr_t) &x;
    x ^= 0x9e3779b97f4a7c15ULL;
    return x;
}

static void fill_random(uint8_t * dst, size_t n) {
    std::mt19937_64 rng(weak_seed_u64());
    size_t i = 0;
    while (i < n) {
        const uint64_t r = rng();
        for (int b = 0; b < 8 && i < n; ++b, ++i) {
            dst[i] = (uint8_t) ((r >> (8*b)) & 0xFFu);
        }
    }
}

static uint64_t file_size_u64(const std::string & path, std::string & err) {
    err.clear();
    dbg("encrypt: stat open");

    FILE * f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = std::string("failed to open file: ") + std::strerror(errno);
        return 0;
    }

#if defined(_WIN32)
    if (_fseeki64(f, 0, SEEK_END) != 0) {
        err = std::string("failed to seek: ") + std::strerror(errno);
        std::fclose(f);
        return 0;
    }
    const __int64 pos = _ftelli64(f);
    if (pos < 0) {
        err = std::string("failed to tell: ") + std::strerror(errno);
        std::fclose(f);
        return 0;
    }
#else
    if (fseeko(f, 0, SEEK_END) != 0) {
        err = std::string("failed to seek: ") + std::strerror(errno);
        std::fclose(f);
        return 0;
    }
    const off_t pos = ftello(f);
    if (pos < 0) {
        err = std::string("failed to tell: ") + std::strerror(errno);
        std::fclose(f);
        return 0;
    }
#endif

    std::fclose(f);
    dbg("encrypt: stat ok");
    return (uint64_t) pos;
}

static std::string get_tmp_dir() {
    const char * vars[] = {
        "TMPDIR",
        "TMP",
        "TEMP",
    };
    for (const char * v : vars) {
        const char * val = std::getenv(v);
        if (val && *val) {
            return val;
        }
    }
#if defined(_WIN32)
    return ".";
#else
    return "/tmp";
#endif
}

static std::string make_temp_path_for(const std::string & input_path) {
    (void) input_path;
    std::string base = get_tmp_dir();
    if (!base.empty()) {
        const char last = base.back();
        if (last != '/' && last != '\\') {
#if defined(_WIN32)
            base.push_back('\\');
#else
            base.push_back('/');
#endif
        }
    }
    std::string fname = "llama_decrypted_";
    {
        std::mt19937_64 rng(weak_seed_u64());
        std::ostringstream ss;
        ss << std::hex;
        for (int i = 0; i < 4; ++i) {
            ss << (uint32_t) rng();
        }
        fname += ss.str();
    }
    fname += ".gguf";
    return base + fname;
}

struct enc_header {
    uint8_t  magic[8];
    uint32_t version;
    uint8_t  salt[SALT_SZ];
    uint8_t  nonce[NONCE_SZ];
    uint32_t chunk_size;
    uint64_t orig_size;
};

static std::vector<uint8_t> header_bytes(const enc_header & h) {
    std::vector<uint8_t> out;
    out.reserve(8 + 4 + SALT_SZ + NONCE_SZ + 4 + 8);
    out.insert(out.end(), h.magic, h.magic + sizeof(h.magic));
    out.push_back((uint8_t)(h.version & 0xFFu));
    out.push_back((uint8_t)((h.version >> 8) & 0xFFu));
    out.push_back((uint8_t)((h.version >> 16) & 0xFFu));
    out.push_back((uint8_t)((h.version >> 24) & 0xFFu));
    out.insert(out.end(), h.salt, h.salt + SALT_SZ);
    out.insert(out.end(), h.nonce, h.nonce + NONCE_SZ);
    out.push_back((uint8_t)(h.chunk_size & 0xFFu));
    out.push_back((uint8_t)((h.chunk_size >> 8) & 0xFFu));
    out.push_back((uint8_t)((h.chunk_size >> 16) & 0xFFu));
    out.push_back((uint8_t)((h.chunk_size >> 24) & 0xFFu));
    for (int i = 0; i < 8; ++i) {
        out.push_back((uint8_t)((h.orig_size >> (8*i)) & 0xFFu));
    }
    return out;
}

static bool write_header(std::ostream & os, const enc_header & h) {
    os.write((const char *) h.magic, sizeof(h.magic));
    write_u32_le(os, h.version);
    os.write((const char *) h.salt, SALT_SZ);
    os.write((const char *) h.nonce, NONCE_SZ);
    write_u32_le(os, h.chunk_size);
    write_u64_le(os, h.orig_size);
    return (bool) os;
}

static bool fwrite_header(FILE * f, const enc_header & h, std::string & err) {
    if (!fwrite_exact(f, h.magic, sizeof(h.magic), err)) {
        return false;
    }
    if (!fwrite_u32_le(f, h.version, err)) {
        return false;
    }
    if (!fwrite_exact(f, h.salt, SALT_SZ, err)) {
        return false;
    }
    if (!fwrite_exact(f, h.nonce, NONCE_SZ, err)) {
        return false;
    }
    if (!fwrite_u32_le(f, h.chunk_size, err)) {
        return false;
    }
    if (!fwrite_u64_le(f, h.orig_size, err)) {
        return false;
    }
    return true;
}

static bool read_header(std::istream & is, enc_header & h) {
    if (!read_exact(is, h.magic, sizeof(h.magic))) {
        return false;
    }
    if (!read_u32_le(is, h.version)) {
        return false;
    }
    if (!read_exact(is, h.salt, SALT_SZ)) {
        return false;
    }
    if (!read_exact(is, h.nonce, NONCE_SZ)) {
        return false;
    }
    if (!read_u32_le(is, h.chunk_size)) {
        return false;
    }
    if (!read_u64_le(is, h.orig_size)) {
        return false;
    }
    return true;
}

static bool fread_header(FILE * f, enc_header & h, std::string & err) {
    if (!fread_exact(f, h.magic, sizeof(h.magic), err)) {
        return false;
    }
    if (!fread_u32_le(f, h.version, err)) {
        return false;
    }
    if (!fread_exact(f, h.salt, SALT_SZ, err)) {
        return false;
    }
    if (!fread_exact(f, h.nonce, NONCE_SZ, err)) {
        return false;
    }
    if (!fread_u32_le(f, h.chunk_size, err)) {
        return false;
    }
    if (!fread_u64_le(f, h.orig_size, err)) {
        return false;
    }
    return true;
}

static bool is_magic(const uint8_t magic[8]) {
    for (size_t i = 0; i < 8; ++i) {
        if (magic[i] != MAGIC[i]) {
            return false;
        }
    }
    return true;
}

static void derive_seeds(
    const std::string & passcode,
    const uint8_t salt[SALT_SZ],
    uint64_t & out_enc_seed,
    uint64_t & out_mac_seed)
{
    const uint64_t pass_seed = xxh64_of(passcode.data(), passcode.size(), 0);
    out_enc_seed = xxh64_of(salt, SALT_SZ, pass_seed ^ 0x9e3779b97f4a7c15ULL);
    out_mac_seed = xxh64_of(salt, SALT_SZ, pass_seed ^ 0x243f6a8885a308d3ULL);
}

static void stream_xor_inplace(
    uint64_t enc_seed,
    const uint8_t nonce[NONCE_SZ],
    uint64_t chunk_index,
    std::vector<uint8_t> & buf)
{
    size_t off = 0;
    uint64_t block = 0;
    while (off < buf.size()) {
        uint8_t ctr[32];
        memcpy(ctr, nonce, NONCE_SZ);
        for (int i = 0; i < 8; ++i) {
            ctr[NONCE_SZ + i] = (uint8_t)((chunk_index >> (8*i)) & 0xFFu);
            ctr[NONCE_SZ + 8 + i] = (uint8_t)((block >> (8*i)) & 0xFFu);
        }
        memset(ctr + NONCE_SZ + 16, 0, 32 - (NONCE_SZ + 16));

        const XXH128_hash_t ks = XXH3_128bits_withSeed(ctr, sizeof(ctr), (XXH64_hash_t) enc_seed);
        uint8_t ks_bytes[16];
        for (int i = 0; i < 8; ++i) {
            ks_bytes[i]     = (uint8_t)((ks.low64  >> (8*i)) & 0xFFu);
            ks_bytes[i + 8] = (uint8_t)((ks.high64 >> (8*i)) & 0xFFu);
        }

        const size_t n = std::min(buf.size() - off, (size_t) 16);
        for (size_t i = 0; i < n; ++i) {
            buf[off + i] ^= ks_bytes[i];
        }
        off += n;
        block++;
    }
}

static void compute_tag(
    uint64_t mac_seed,
    const std::vector<uint8_t> & hdr_bytes,
    uint64_t chunk_index,
    uint32_t chunk_len,
    const std::vector<uint8_t> & ciphertext,
    uint8_t out_tag[TAG_SZ])
{
    std::vector<uint8_t> buf;
    buf.reserve(hdr_bytes.size() + 12 + ciphertext.size());
    buf.insert(buf.end(), hdr_bytes.begin(), hdr_bytes.end());

    uint8_t meta[12];
    for (int i = 0; i < 8; ++i) {
        meta[i] = (uint8_t)((chunk_index >> (8*i)) & 0xFFu);
    }
    meta[8]  = (uint8_t)(chunk_len & 0xFFu);
    meta[9]  = (uint8_t)((chunk_len >> 8) & 0xFFu);
    meta[10] = (uint8_t)((chunk_len >> 16) & 0xFFu);
    meta[11] = (uint8_t)((chunk_len >> 24) & 0xFFu);
    buf.insert(buf.end(), meta, meta + sizeof(meta));

    buf.insert(buf.end(), ciphertext.begin(), ciphertext.end());

    const XXH128_hash_t h = XXH3_128bits_withSeed(buf.data(), buf.size(), (XXH64_hash_t) mac_seed);
    for (int i = 0; i < 8; ++i) {
        out_tag[i]     = (uint8_t)((h.low64  >> (8*i)) & 0xFFu);
        out_tag[i + 8] = (uint8_t)((h.high64 >> (8*i)) & 0xFFu);
    }
}

}

bool common_model_validate_passcode_5x4(const std::string & passcode) {
    // dddd-dddd-dddd-dddd-dddd
    if (passcode.size() != 24) {
        return false;
    }
    for (size_t i = 0; i < passcode.size(); ++i) {
        if ((i + 1) % 5 == 0) {
            if (passcode[i] != '-') {
                return false;
            }
        } else {
            if (passcode[i] < '0' || passcode[i] > '9') {
                return false;
            }
        }
    }
    return true;
}

bool common_model_is_encrypted_file(const std::string & path) {
    FILE * f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    uint8_t magic[8];
    std::string err;
    const bool ok = fread_exact(f, magic, sizeof(magic), err);
    std::fclose(f);
    return ok && is_magic(magic);
}

bool common_model_encrypt_file(
    const std::string & input_path,
    const std::string & output_path,
    const std::string & passcode,
    std::string & err)
{
    err.clear();

    dbg("encrypt: enter");

    if (!common_model_validate_passcode_5x4(passcode)) {
        err = "passcode must be in format dddd-dddd-dddd-dddd-dddd";
        return false;
    }

    std::string stat_err;
    const uint64_t orig_size = file_size_u64(input_path, stat_err);
    if (!stat_err.empty()) {
        err = stat_err;
        return false;
    }

    FILE * fin = std::fopen(input_path.c_str(), "rb");
    if (!fin) {
        err = std::string("failed to open input file: ") + std::strerror(errno);
        return false;
    }

    FILE * fout = std::fopen(output_path.c_str(), "wb");
    if (!fout) {
        err = std::string("failed to open output file: ") + std::strerror(errno);
        std::fclose(fin);
        return false;
    }

    enc_header h;
    memcpy(h.magic, MAGIC, sizeof(h.magic));
    h.version = LLAMA_ENC_VERSION;
    fill_random(h.salt, sizeof(h.salt));
    fill_random(h.nonce, sizeof(h.nonce));
    h.chunk_size = DEFAULT_CHUNK_SIZE;
    h.orig_size = orig_size;

    if (!fwrite_header(fout, h, err)) {
        if (err.empty()) {
            err = "failed to write header";
        }
        std::fclose(fout);
        std::fclose(fin);
        return false;
    }

    // Ensure the header hits the filesystem early so the output file is visible with non-zero size.
    std::fflush(fout);

    const auto hdr = header_bytes(h);

    uint64_t enc_seed = 0;
    uint64_t mac_seed = 0;
    derive_seeds(passcode, h.salt, enc_seed, mac_seed);

    std::vector<uint8_t> buf;
    buf.resize(h.chunk_size);

    uint64_t remaining = orig_size;
    uint64_t chunk_index = 0;
    while (remaining > 0) {
        const uint32_t chunk_len = (uint32_t) std::min<uint64_t>(remaining, h.chunk_size);
        if (!fread_exact(fin, buf.data(), chunk_len, err)) {
            if (err.empty()) {
                err = "failed to read input";
            }
            std::fclose(fout);
            std::fclose(fin);
            return false;
        }
        buf.resize(chunk_len);

        // Encrypt in-place.
        stream_xor_inplace(enc_seed, h.nonce, chunk_index, buf);

        uint8_t tag[TAG_SZ];
        compute_tag(mac_seed, hdr, chunk_index, chunk_len, buf, tag);

        if (!fwrite_u32_le(fout, chunk_len, err) ||
            !fwrite_exact(fout, buf.data(), buf.size(), err) ||
            !fwrite_exact(fout, tag, sizeof(tag), err)) {
            if (err.empty()) {
                err = "failed to write output";
            }
            std::fclose(fout);
            std::fclose(fin);
            return false;
        }

        remaining -= chunk_len;
        chunk_index++;
        buf.resize(h.chunk_size);
    }

    std::fflush(fout);

    std::fclose(fout);
    std::fclose(fin);

    dbg("encrypt: success");

    return true;
}

static bool decrypt_file_impl(
    const std::string & encrypted_path,
    const std::string & output_path,
    const std::string & passcode,
    std::string & err)
{
    err.clear();

    dbg("decrypt: enter");

    if (!common_model_validate_passcode_5x4(passcode)) {
        err = "passcode must be in format dddd-dddd-dddd-dddd-dddd";
        return false;
    }

    FILE * fin = std::fopen(encrypted_path.c_str(), "rb");
    if (!fin) {
        err = std::string("failed to open encrypted model: ") + std::strerror(errno);
        return false;
    }

    enc_header h;
    if (!fread_header(fin, h, err)) {
        if (err.empty()) {
            err = "failed to read encryption header";
        }
        std::fclose(fin);
        return false;
    }
    if (!is_magic(h.magic)) {
        err = "model is not encrypted";
        std::fclose(fin);
        return false;
    }
    if (h.version != LLAMA_ENC_VERSION) {
        err = "unsupported encrypted model version";
        std::fclose(fin);
        return false;
    }
    if (h.chunk_size == 0 || h.chunk_size > (64u * 1024u * 1024u)) {
        err = "invalid chunk size";
        std::fclose(fin);
        return false;
    }

    const auto hdr = header_bytes(h);

    uint64_t enc_seed = 0;
    uint64_t mac_seed = 0;
    derive_seeds(passcode, h.salt, enc_seed, mac_seed);

    FILE * fout = std::fopen(output_path.c_str(), "wb");
    if (!fout) {
        err = std::string("failed to create decrypted model: ") + std::strerror(errno);
        std::fclose(fin);
        return false;
    }

    uint64_t remaining = h.orig_size;
    uint64_t chunk_index = 0;
    std::vector<uint8_t> buf;
    while (remaining > 0) {
        uint32_t chunk_len = 0;
        if (!fread_u32_le(fin, chunk_len, err)) {
            if (err.empty()) {
                err = "truncated encrypted model";
            }
            std::fclose(fout);
            std::fclose(fin);
            return false;
        }
        if (chunk_len == 0 || chunk_len > h.chunk_size) {
            err = "invalid chunk length";
            std::fclose(fout);
            std::fclose(fin);
            return false;
        }
        if (chunk_len > remaining) {
            err = "chunk length exceeds original size";
            std::fclose(fout);
            std::fclose(fin);
            return false;
        }

        buf.resize(chunk_len);
        if (!fread_exact(fin, buf.data(), buf.size(), err)) {
            if (err.empty()) {
                err = "truncated encrypted model (ciphertext)";
            }
            std::fclose(fout);
            std::fclose(fin);
            return false;
        }

        uint8_t tag[TAG_SZ];
        if (!fread_exact(fin, tag, sizeof(tag), err)) {
            if (err.empty()) {
                err = "truncated encrypted model (tag)";
            }
            std::fclose(fout);
            std::fclose(fin);
            return false;
        }

        uint8_t expected[TAG_SZ];
        compute_tag(mac_seed, hdr, chunk_index, chunk_len, buf, expected);
        if (memcmp(tag, expected, TAG_SZ) != 0) {
            err = "invalid passcode or corrupted encrypted model";
            std::fclose(fout);
            std::fclose(fin);
            return false;
        }

        // Decrypt in-place.
        stream_xor_inplace(enc_seed, h.nonce, chunk_index, buf);

        if (!fwrite_exact(fout, buf.data(), buf.size(), err)) {
            if (err.empty()) {
                err = "failed to write decrypted model";
            }
            std::fclose(fout);
            std::fclose(fin);
            return false;
        }

        remaining -= chunk_len;
        chunk_index++;
    }

    std::fflush(fout);
    std::fclose(fout);
    std::fclose(fin);

    return true;
}

std::string common_model_decrypt_to_temp_file(
    const std::string & encrypted_path,
    const std::string & passcode,
    std::string & err)
{
    const std::string tmp_path = make_temp_path_for(encrypted_path);
    if (!decrypt_file_impl(encrypted_path, tmp_path, passcode, err)) {
        return "";
    }

    return tmp_path;
}

bool common_model_decrypt_file(
    const std::string & encrypted_path,
    const std::string & output_path,
    const std::string & passcode,
    std::string & err)
{
    return decrypt_file_impl(encrypted_path, output_path, passcode, err);
}
