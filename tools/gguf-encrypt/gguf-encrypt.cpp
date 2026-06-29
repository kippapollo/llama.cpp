#include "model-bundle.h"
#include "model-encryption.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {

static void print_usage(const char * argv0) {
    printf("usage:\n");
    printf("  %s --in <path> --out <path> --passcode <dddd-dddd-dddd-dddd-dddd> [--decrypt]\n", argv0);
    printf("\n");
    printf("Encrypt a GGUF file or a whole model directory tree into one encrypted bundle.\n");
    printf("When decrypting a bundled model, the output path is treated as a directory.\n");
    printf("A model directory may also include system-prompt.txt and prompt-guardrail-samples.json; if present, they are preserved in the bundle.\n");
    printf("\n");
    printf("options:\n");
    printf("  -h, --help            show this help\n");
    printf("  --in <path>           input gguf file or model directory\n");
    printf("  --out <path>          output file path or output directory for bundles\n");
    printf("  --passcode <code>     5x4 digits, e.g. 3456-2342-2342-3423-2112\n");
    printf("  --decrypt             decrypt the input file\n");
}

static bool path_exists(const fs::path & path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

static bool path_is_directory(const fs::path & path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
}

static bool ensure_parent_directories(const fs::path & path, std::string & err) {
    if (!path.has_parent_path()) {
        return true;
    }

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        err = std::string("failed to create output directories: ") + ec.message();
        return false;
    }

    return true;
}

static bool ensure_output_directory(const fs::path & path, std::string & err) {
    std::error_code ec;
    if (fs::exists(path, ec)) {
        if (!fs::is_directory(path, ec)) {
            err = "output path exists and is not a directory: " + path.string();
            return false;
        }
        return true;
    }

    fs::create_directories(path, ec);
    if (ec) {
        err = std::string("failed to create output directory: ") + ec.message();
        return false;
    }

    return true;
}

static fs::path make_temp_path(const char * prefix, const char * suffix) {
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec);
    if (ec) {
        return {};
    }

    const uint64_t seed =
        static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) ^
        reinterpret_cast<uintptr_t>(&base);
    std::mt19937_64 rng(seed);

    for (int attempt = 0; attempt < 32; ++attempt) {
        std::ostringstream oss;
        oss << prefix << std::hex << rng() << "_" << attempt << suffix;
        const fs::path candidate = base / oss.str();
        if (!path_exists(candidate)) {
            return candidate;
        }
    }

    return {};
}

static bool copy_file(const fs::path & src, const fs::path & dst, std::string & err) {
    std::error_code ec;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        err = std::string("failed to copy decrypted file: ") + ec.message();
        return false;
    }
    return true;
}

struct scoped_path_remover {
    fs::path path;
    bool is_dir = false;

    ~scoped_path_remover() {
        if (path.empty()) {
            return;
        }

        std::error_code ec;
        if (is_dir) {
            fs::remove_all(path, ec);
        } else {
            fs::remove(path, ec);
        }
    }
};

int run_encrypt(const fs::path & input_path, const fs::path & output_path, const std::string & passcode, std::string & err) {
    if (path_is_directory(input_path)) {
        const fs::path temp_archive = make_temp_path("llama_bundle_", ".archive");
        if (temp_archive.empty()) {
            err = "failed to allocate a temporary bundle archive path";
            return 1;
        }

        scoped_path_remover cleanup{temp_archive, false};
        if (!common_model_bundle_pack_directory(input_path.string(), temp_archive.string(), err)) {
            return 1;
        }

        if (!ensure_parent_directories(output_path, err)) {
            return 1;
        }

        if (path_is_directory(output_path)) {
            err = "output path is a directory: " + output_path.string();
            return 1;
        }

        if (!common_model_encrypt_file(temp_archive.string(), output_path.string(), passcode, err)) {
            return 1;
        }

        return 0;
    }

    if (!ensure_parent_directories(output_path, err)) {
        return 1;
    }

    if (path_is_directory(output_path)) {
        err = "output path is a directory: " + output_path.string();
        return 1;
    }

    if (!common_model_encrypt_file(input_path.string(), output_path.string(), passcode, err)) {
        return 1;
    }

    return 0;
}

int run_decrypt(const fs::path & input_path, const fs::path & output_path, const std::string & passcode, std::string & err) {
    if (!common_model_is_encrypted_file(input_path.string())) {
        err = "input file is not an encrypted model";
        return 1;
    }

    const std::string temp_plain = common_model_decrypt_to_temp_file(input_path.string(), passcode, err);
    if (temp_plain.empty()) {
        return 1;
    }

    scoped_path_remover cleanup{fs::path(temp_plain), false};

    if (common_model_bundle_is_archive_file(temp_plain)) {
        if (!ensure_output_directory(output_path, err)) {
            return 1;
        }
        if (!common_model_bundle_unpack_archive(temp_plain, output_path.string(), err)) {
            return 1;
        }
        return 0;
    }

    if (path_is_directory(output_path)) {
        err = "output path is a directory: " + output_path.string();
        return 1;
    }

    if (!ensure_parent_directories(output_path, err)) {
        return 1;
    }

    if (!copy_file(fs::path(temp_plain), output_path, err)) {
        return 1;
    }

    return 0;
}

} // namespace

int main(int argc, char ** argv) {
    std::string in_path;
    std::string out_path;
    std::string passcode;
    bool do_decrypt = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--dec" || arg == "--decrypt") {
            do_decrypt = true;
            continue;
        }
        if (arg == "--in" && i + 1 < argc) {
            in_path = argv[++i];
            continue;
        }
        if (arg == "--out" && i + 1 < argc) {
            out_path = argv[++i];
            continue;
        }
        if (arg == "--passcode" && i + 1 < argc) {
            passcode = argv[++i];
            continue;
        }

        fprintf(stderr, "unknown/invalid argument: %s\n", arg.c_str());
        print_usage(argv[0]);
        return 2;
    }

    if (in_path.empty() || out_path.empty() || passcode.empty()) {
        print_usage(argv[0]);
        return 2;
    }

    if (!common_model_validate_passcode_5x4(passcode)) {
        fprintf(stderr, "error: passcode must be in format dddd-dddd-dddd-dddd-dddd\n");
        return 1;
    }

    const fs::path input_path(in_path);
    const fs::path output_path(out_path);

    if (!path_exists(input_path)) {
        fprintf(stderr, "error: input path not found: %s\n", in_path.c_str());
        return 1;
    }

    if (do_decrypt && path_is_directory(input_path)) {
        fprintf(stderr, "error: decrypt mode requires an encrypted input file, not a directory\n");
        return 1;
    }

    std::string err;
    const int rc = do_decrypt
        ? run_decrypt(input_path, output_path, passcode, err)
        : run_encrypt(input_path, output_path, passcode, err);

    if (rc != 0) {
        if (!err.empty()) {
            fprintf(stderr, "error: %s\n", err.c_str());
        }
        return rc;
    }

    fprintf(stderr, "OK: wrote %s\n", out_path.c_str());
    return 0;
}
