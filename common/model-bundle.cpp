#include "model-bundle.h"

#include "common.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>

namespace {

namespace fs = std::filesystem;

static constexpr uint32_t k_bundle_version = 1;
static constexpr size_t    k_copy_chunk_sz  = 4u * 1024u * 1024u;
static const uint8_t       k_bundle_magic[8] = { 'L', 'G', 'G', 'U', 'F', 'B', 'D', 'L' };

static bool write_exact(std::ostream & os, const void * data, size_t size, std::string & err) {
    if (size == 0) {
        return true;
    }

    os.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    if (!os) {
        err = "failed to write archive data";
        return false;
    }

    return true;
}

static bool read_exact(std::istream & is, void * data, size_t size, std::string & err) {
    if (size == 0) {
        return true;
    }

    is.read(reinterpret_cast<char *>(data), static_cast<std::streamsize>(size));
    if (!is) {
        err = "failed to read archive data";
        return false;
    }

    return true;
}

static bool write_u32_le(std::ostream & os, uint32_t v, std::string & err) {
    const uint8_t b[4] = {
        static_cast<uint8_t>(v & 0xFFu),
        static_cast<uint8_t>((v >> 8) & 0xFFu),
        static_cast<uint8_t>((v >> 16) & 0xFFu),
        static_cast<uint8_t>((v >> 24) & 0xFFu),
    };
    return write_exact(os, b, sizeof(b), err);
}

static bool write_u64_le(std::ostream & os, uint64_t v, std::string & err) {
    const uint8_t b[8] = {
        static_cast<uint8_t>(v & 0xFFu),
        static_cast<uint8_t>((v >> 8) & 0xFFu),
        static_cast<uint8_t>((v >> 16) & 0xFFu),
        static_cast<uint8_t>((v >> 24) & 0xFFu),
        static_cast<uint8_t>((v >> 32) & 0xFFu),
        static_cast<uint8_t>((v >> 40) & 0xFFu),
        static_cast<uint8_t>((v >> 48) & 0xFFu),
        static_cast<uint8_t>((v >> 56) & 0xFFu),
    };
    return write_exact(os, b, sizeof(b), err);
}

static bool read_u32_le(std::istream & is, uint32_t & v, std::string & err) {
    uint8_t b[4];
    if (!read_exact(is, b, sizeof(b), err)) {
        return false;
    }
    v = static_cast<uint32_t>(b[0]) |
        (static_cast<uint32_t>(b[1]) << 8) |
        (static_cast<uint32_t>(b[2]) << 16) |
        (static_cast<uint32_t>(b[3]) << 24);
    return true;
}

static bool read_u64_le(std::istream & is, uint64_t & v, std::string & err) {
    uint8_t b[8];
    if (!read_exact(is, b, sizeof(b), err)) {
        return false;
    }
    v = static_cast<uint64_t>(b[0]) |
        (static_cast<uint64_t>(b[1]) << 8) |
        (static_cast<uint64_t>(b[2]) << 16) |
        (static_cast<uint64_t>(b[3]) << 24) |
        (static_cast<uint64_t>(b[4]) << 32) |
        (static_cast<uint64_t>(b[5]) << 40) |
        (static_cast<uint64_t>(b[6]) << 48) |
        (static_cast<uint64_t>(b[7]) << 56);
    return true;
}

static bool write_header(std::ostream & os, uint32_t file_count, std::string & err) {
    if (!write_exact(os, k_bundle_magic, sizeof(k_bundle_magic), err)) {
        return false;
    }
    if (!write_u32_le(os, k_bundle_version, err)) {
        return false;
    }
    if (!write_u32_le(os, file_count, err)) {
        return false;
    }
    return true;
}

static bool read_header(std::istream & is, uint32_t & file_count, std::string & err) {
    uint8_t magic[8];
    if (!read_exact(is, magic, sizeof(magic), err)) {
        return false;
    }
    if (std::memcmp(magic, k_bundle_magic, sizeof(k_bundle_magic)) != 0) {
        err = "not a model bundle archive";
        return false;
    }

    uint32_t version = 0;
    if (!read_u32_le(is, version, err)) {
        return false;
    }
    if (version != k_bundle_version) {
        err = "unsupported model bundle archive version";
        return false;
    }

    if (!read_u32_le(is, file_count, err)) {
        return false;
    }

    return true;
}

static bool copy_stream(std::istream & in, std::ostream & out, uint64_t size, std::string & err) {
    std::vector<char> buf(k_copy_chunk_sz);
    uint64_t remaining = size;
    while (remaining > 0) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, buf.size()));
        if (!read_exact(in, buf.data(), want, err)) {
            return false;
        }
        if (!write_exact(out, buf.data(), want, err)) {
            return false;
        }
        remaining -= want;
    }
    return true;
}

static bool pack_directory_impl(
    const fs::path & input_dir,
    const fs::path & output_path,
    std::string & err)
{
    err.clear();

    std::error_code ec;
    if (!fs::exists(input_dir, ec) || !fs::is_directory(input_dir, ec)) {
        err = "bundle input path is not a directory: " + input_dir.string();
        return false;
    }

    std::vector<fs::path> files;
    try {
        for (const auto & entry : fs::recursive_directory_iterator(input_dir)) {
            if (entry.is_regular_file()) {
                files.push_back(entry.path());
            }
        }
    } catch (const fs::filesystem_error & e) {
        err = std::string("failed to scan bundle input directory: ") + e.what();
        return false;
    }

    std::sort(files.begin(), files.end(), [&](const fs::path & a, const fs::path & b) {
        std::error_code ec_a;
        std::error_code ec_b;
        const auto rel_a = fs::relative(a, input_dir, ec_a).generic_string();
        const auto rel_b = fs::relative(b, input_dir, ec_b).generic_string();
        if (ec_a || ec_b) {
            return a.string() < b.string();
        }
        return rel_a < rel_b;
    });

    if (output_path.has_parent_path()) {
        try {
            fs::create_directories(output_path.parent_path());
        } catch (const fs::filesystem_error & e) {
            err = std::string("failed to create bundle output parent directories: ") + e.what();
            return false;
        }
    }

    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        err = std::string("failed to open bundle output: ") + output_path.string();
        return false;
    }

    if (!write_header(out, static_cast<uint32_t>(files.size()), err)) {
        return false;
    }

    for (const auto & file : files) {
        std::error_code rel_ec;
        const fs::path rel_path = fs::relative(file, input_dir, rel_ec);
        if (rel_ec) {
            err = std::string("failed to resolve bundle entry path: ") + rel_ec.message();
            return false;
        }

        const std::string rel = rel_path.generic_string();
        if (rel.empty() || !fs_validate_filename(rel, true)) {
            err = "invalid bundle entry path: " + rel;
            return false;
        }

        std::ifstream in(file, std::ios::binary | std::ios::ate);
        if (!in) {
            err = "failed to open bundle source file: " + file.string();
            return false;
        }

        const auto pos = in.tellg();
        if (pos < 0) {
            err = "failed to determine bundle source file size: " + file.string();
            return false;
        }
        const uint64_t file_size = static_cast<uint64_t>(pos);
        in.seekg(0, std::ios::beg);

        if (!write_u32_le(out, static_cast<uint32_t>(rel.size()), err) ||
            !write_exact(out, rel.data(), rel.size(), err) ||
            !write_u64_le(out, file_size, err) ||
            !copy_stream(in, out, file_size, err)) {
            return false;
        }
    }

    out.flush();
    if (!out) {
        err = "failed to flush bundle output";
        return false;
    }

    return true;
}

static bool unpack_archive_impl(
    const fs::path & archive_path,
    const fs::path & output_dir,
    std::string & err)
{
    err.clear();

    std::ifstream in(archive_path, std::ios::binary);
    if (!in) {
        err = "failed to open bundle archive: " + archive_path.string();
        return false;
    }

    uint32_t file_count = 0;
    if (!read_header(in, file_count, err)) {
        return false;
    }

    std::error_code ec;
    if (fs::exists(output_dir, ec) && !fs::is_directory(output_dir, ec)) {
        err = "bundle output path is not a directory: " + output_dir.string();
        return false;
    }

    if (!fs::exists(output_dir, ec)) {
        try {
            fs::create_directories(output_dir);
        } catch (const fs::filesystem_error & e) {
            err = std::string("failed to create bundle output directory: ") + e.what();
            return false;
        }
    }

    for (uint32_t i = 0; i < file_count; ++i) {
        uint32_t path_len = 0;
        if (!read_u32_le(in, path_len, err)) {
            return false;
        }

        std::string rel(path_len, '\0');
        if (!read_exact(in, rel.data(), rel.size(), err)) {
            return false;
        }

        if (rel.empty() || !fs_validate_filename(rel, true)) {
            err = "invalid bundle entry path: " + rel;
            return false;
        }

        uint64_t file_size = 0;
        if (!read_u64_le(in, file_size, err)) {
            return false;
        }

        const fs::path out_path = output_dir / fs::path(rel);
        if (out_path.has_parent_path()) {
            try {
                fs::create_directories(out_path.parent_path());
            } catch (const fs::filesystem_error & e) {
                err = std::string("failed to create bundle output parents: ") + e.what();
                return false;
            }
        }

        std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "failed to create bundle output file: " + out_path.string();
            return false;
        }

        if (!copy_stream(in, out, file_size, err)) {
            return false;
        }

        out.flush();
        if (!out) {
            err = "failed to flush bundle output file: " + out_path.string();
            return false;
        }
    }

    return true;
}

static std::string make_temp_dir_path() {
    fs::path base;
    std::error_code ec;
    base = fs::temp_directory_path(ec);
    if (ec) {
        return {};
    }

    const uint64_t seed = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()) ^
        reinterpret_cast<uintptr_t>(&base);

    std::mt19937_64 rng(seed);
    for (int attempt = 0; attempt < 32; ++attempt) {
        const uint64_t value = rng();
        std::ostringstream oss;
        oss << "llama_bundle_" << std::hex << value << "_" << attempt;
        const fs::path candidate = base / oss.str();
        if (fs::exists(candidate, ec)) {
            continue;
        }
        try {
            fs::create_directories(candidate);
            return candidate.string();
        } catch (...) {
            // try another name
        }
    }

    return {};
}

} // namespace

bool common_model_bundle_is_archive_file(const std::string & path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }

    uint32_t file_count = 0;
    std::string err;
    return read_header(in, file_count, err);
}

bool common_model_bundle_pack_directory(
    const std::string & input_dir,
    const std::string & output_path,
    std::string & err)
{
    return pack_directory_impl(fs::path(input_dir), fs::path(output_path), err);
}

bool common_model_bundle_unpack_archive(
    const std::string & archive_path,
    const std::string & output_dir,
    std::string & err)
{
    return unpack_archive_impl(fs::path(archive_path), fs::path(output_dir), err);
}

std::string common_model_bundle_unpack_archive_to_temp_dir(
    const std::string & archive_path,
    std::string & err)
{
    const std::string temp_dir = make_temp_dir_path();
    if (temp_dir.empty()) {
        err = "failed to create temporary directory for bundle unpack";
        return {};
    }

    if (!common_model_bundle_unpack_archive(archive_path, temp_dir, err)) {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
        return {};
    }

    return temp_dir;
}
