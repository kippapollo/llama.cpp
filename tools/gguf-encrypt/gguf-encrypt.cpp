#include "common.h"

#include "model-encryption.h"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <string>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#define getcwd _getcwd
#endif

static std::string get_cwd_str() {
#if defined(_WIN32)
    char buf[4096];
    if (getcwd(buf, sizeof(buf)) == nullptr) {
        return std::string();
    }
    return std::string(buf);
#else
    return std::string();
#endif
}

static std::string abs_path(const std::string & path) {
#if defined(_WIN32)
    char buf[_MAX_PATH];
    if (_fullpath(buf, path.c_str(), sizeof(buf)) == nullptr) {
        return std::string();
    }
    return std::string(buf);
#else
    return std::string();
#endif
}

static bool file_exists(const std::string & path) {
#if defined(_WIN32)
    const DWORD attrs = GetFileAttributesA(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    // treat directories as existing too
    return true;
#else
    std::ifstream in(path, std::ios::binary);
    return (bool) in;
#endif
}

static void log_out_exists_probe(const std::string & out_path) {
#if defined(_WIN32)
    const DWORD attrs = GetFileAttributesA(out_path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD e = GetLastError();
        fprintf(stderr, "[gguf-encrypt] out_exists: false (GetFileAttributesA err=%lu)\n", (unsigned long) e);
    } else {
        fprintf(stderr, "[gguf-encrypt] out_exists: true (attrs=0x%08lx)\n", (unsigned long) attrs);
    }
#else
    fprintf(stderr, "[gguf-encrypt] out_exists: %s\n", file_exists(out_path) ? "true" : "false");
#endif
}

static uint64_t file_size(const std::string & path, std::string & err) {
    err.clear();
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        err = std::string("open failed: ") + std::strerror(errno);
        return 0;
    }
    auto pos = in.tellg();
    if (pos < 0) {
        err = "tellg failed";
        return 0;
    }
    return (uint64_t) pos;
}

static void log_kv(const char * k, const std::string & v) {
    fprintf(stderr, "[gguf-encrypt] %s: %s\n", k, v.c_str());
}

static void log_kv_u64(const char * k, uint64_t v) {
    fprintf(stderr, "[gguf-encrypt] %s: %llu\n", k, (unsigned long long) v);
}

static void log_line(const char * s) {
    fprintf(stderr, "[gguf-encrypt] %s\n", s);
}

static void print_usage(const char * argv0) {
    printf("usage: %s --in <model.gguf> --out <model.gguf.enc> --passcode <dddd-dddd-dddd-dddd-dddd>\n", argv0);
    printf("\n");
    printf("Encrypt a GGUF model file into an encrypted container.\n");
    printf("\n");
    printf("options:\n");
    printf("  -h, --help            show this help\n");
    printf("  --in <path>           input gguf model path\n");
    printf("  --out <path>          output encrypted model path\n");
    printf("  --passcode <code>     5x4 digits, e.g. 3456-2342-2342-3423-2112\n");
}

int main(int argc, char ** argv) {
    std::string in_path;
    std::string out_path;
    std::string passcode;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
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

    log_kv("cwd", get_cwd_str());
    log_kv("in", in_path);
    log_kv("out", out_path);
    {
        const auto in_abs = abs_path(in_path);
        const auto out_abs = abs_path(out_path);
        if (!in_abs.empty()) {
            log_kv("in_abs", in_abs);
        }
        if (!out_abs.empty()) {
            log_kv("out_abs", out_abs);
        }
    }
    log_kv("passcode", passcode);
    log_kv("passcode_valid", common_model_validate_passcode_5x4(passcode) ? "true" : "false");

    if (!file_exists(in_path)) {
        fprintf(stderr, "[gguf-encrypt] error: input file not found or not readable\n");
        return 1;
    }

    {
        std::string sz_err;
        const uint64_t sz = file_size(in_path, sz_err);
        if (!sz_err.empty()) {
            fprintf(stderr, "[gguf-encrypt] warning: cannot determine input size: %s\n", sz_err.c_str());
        } else {
            log_kv_u64("in_size_bytes", sz);
        }
    }

    if (file_exists(out_path)) {
        fprintf(stderr, "[gguf-encrypt] warning: output file already exists; it will be overwritten\n");
    }
    log_out_exists_probe(out_path);

    // Preflight: ensure we can create/truncate the output file in this directory.
    {
        std::ofstream out_probe(out_path, std::ios::binary | std::ios::trunc);
        if (!out_probe) {
            fprintf(stderr, "[gguf-encrypt] error: cannot create output file: %s\n", std::strerror(errno));
            return 1;
        }
    }

    log_line("starting encryption");

    std::string err;
    const bool ok = common_model_encrypt_file(in_path, out_path, passcode, err);
    log_kv("encrypt_return", ok ? "true" : "false");
    if (!err.empty()) {
        log_kv("encrypt_err", err);
    }

    if (!ok) {
        fprintf(stderr, "[gguf-encrypt] encryption failed\n");
        return 1;
    }

    fprintf(stderr, "[gguf-encrypt] OK: wrote %s\n", out_path.c_str());
    if (!file_exists(out_path)) {
        fprintf(stderr, "[gguf-encrypt] warning: output path still not readable after write\n");
    } else {
        std::string out_sz_err;
        const uint64_t out_sz = file_size(out_path, out_sz_err);
        if (out_sz_err.empty()) {
            log_kv_u64("out_size_bytes", out_sz);
        }
    }
    return 0;
}
