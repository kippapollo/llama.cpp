#include "output-guardrail.h"

#include "common.h"
#include "log.h"
#include "unicode.h"

#include <cstdarg>
#include <cstdio>
#define ORT_API_MANUAL_INIT
#include <nlohmann/json.hpp>
#include <onnxruntime_cxx_api.h>
#undef ORT_API_MANUAL_INIT

#include <algorithm>
#include <array>
#include <climits>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#   define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__) && defined(__MACH__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

using json = nlohmann::ordered_json;
namespace fs = std::filesystem;

namespace {

struct utf8_codepoint {
    uint32_t cpt = 0;
    size_t   start = 0;
    size_t   end = 0;
};

struct basic_token {
    size_t start = 0;
    size_t end = 0;
    std::string text;
};

struct wordpiece_token {
    int64_t id = -1;
    size_t start = 0;
    size_t end = 0;
    std::string text;
};

static fs::path get_executable_path() {
#if defined(_WIN32)
    wchar_t buf[32768] = { 0 };
    DWORD len = GetModuleFileNameW(nullptr, buf, _countof(buf));
    if (len == 0 || len >= _countof(buf)) {
        throw std::runtime_error("failed to resolve executable path");
    }
    return fs::path(buf);
#elif defined(__APPLE__) && defined(__MACH__)
    char small_path[PATH_MAX];
    uint32_t size = sizeof(small_path);

    if (_NSGetExecutablePath(small_path, &size) == 0) {
        try {
            return fs::canonical(fs::path(small_path));
        } catch (...) {
            return fs::path(small_path);
        }
    }

    std::vector<char> buf(size);
    if (_NSGetExecutablePath(buf.data(), &size) == 0) {
        try {
            return fs::canonical(fs::path(buf.data()));
        } catch (...) {
            return fs::path(buf.data());
        }
    }

    throw std::runtime_error("failed to resolve executable path");
#else
    char path[FILENAME_MAX];
    ssize_t count = readlink("/proc/self/exe", path, FILENAME_MAX);
    if (count <= 0) {
        throw std::runtime_error("failed to resolve /proc/self/exe");
    }
    return fs::path(std::string(path, count));
#endif
}

static bool path_exists(const fs::path & path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

static std::string read_text_file(const fs::path & path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("failed to open file: " + path.string());
    }

    std::ostringstream oss;
    oss << file.rdbuf();
    return oss.str();
}

static std::vector<utf8_codepoint> utf8_codepoints_with_offsets(const std::string & text) {
    std::vector<utf8_codepoint> out;
    out.reserve(text.size());

    size_t offset = 0;
    while (offset < text.size()) {
        const size_t start = offset;
        try {
            const uint32_t cpt = unicode_cpt_from_utf8(text, offset);
            out.push_back({cpt, start, offset});
        } catch (const std::invalid_argument &) {
            ++offset;
            out.push_back({0xFFFD, start, offset});
        }
    }

    return out;
}

static std::string utf8_from_cpt(uint32_t cpt) {
    return unicode_cpt_to_utf8(cpt);
}

static std::string entity_type_from_label(const std::string & label) {
    if (label == "O") {
        return label;
    }

    const auto pos = label.find('-');
    if (pos == std::string::npos) {
        return label;
    }

    return label.substr(pos + 1);
}

static bool is_redactable_entity_label(const std::string & label) {
    if (label.empty() || label == "O") {
        return false;
    }

    const std::string entity_type = entity_type_from_label(label);
    return entity_type == "PER" || entity_type == "ORG" || entity_type == "LOC";
}

static bool is_entity_label(const std::string & label) {
    return is_redactable_entity_label(label);
}

static std::vector<std::pair<size_t, size_t>> merge_spans(std::vector<std::pair<size_t, size_t>> spans) {
    spans.erase(std::remove_if(spans.begin(), spans.end(), [](const auto & span) {
        return span.second <= span.first;
    }), spans.end());

    if (spans.empty()) {
        return spans;
    }

    std::sort(spans.begin(), spans.end());

    std::vector<std::pair<size_t, size_t>> merged;
    merged.push_back(spans.front());

    for (size_t i = 1; i < spans.size(); ++i) {
        auto & last = merged.back();
        const auto & current = spans[i];
        if (current.first <= last.second) {
            last.second = std::max(last.second, current.second);
        } else {
            merged.push_back(current);
        }
    }

    return merged;
}

struct guardrail_backend_config {
    fs::path model_root;
    fs::path onnx_file;
    fs::path config_file;
    fs::path vocab_file;
    fs::path tokenizer_config_file;
};

struct tokenizer_config {
    bool do_lower_case = false;
    bool tokenize_chinese_chars = true;
    bool strip_accents = false;
    size_t model_max_length = 512;
    std::string unk_token = "[UNK]";
    std::string cls_token = "[CLS]";
    std::string sep_token = "[SEP]";
};

class guardrail_backend {
public:
    explicit guardrail_backend(const std::string & requested_model);

    std::string redact(const std::string & text) const;
    std::string redact_prefix(const std::string & text, size_t prefix_bytes) const;

private:
    static guardrail_backend_config resolve_config(const std::string & requested_model);
    void load_tokenizer_config();
    void load_vocab();
    void load_labels();
    void build_session();

    std::vector<basic_token> basic_tokenize(const std::string & text) const;
    std::vector<wordpiece_token> wordpiece_tokenize(const basic_token & token) const;
    std::vector<wordpiece_token> tokenize(const std::string & text) const;
    std::vector<int64_t> run_model(
            const std::vector<int64_t> & input_ids,
            const std::vector<int64_t> & attention_mask,
            const std::vector<int64_t> & token_type_ids) const;
    std::vector<std::string> classify(const std::vector<wordpiece_token> & pieces) const;
    std::vector<std::pair<size_t, size_t>> redaction_spans(const std::string & text) const;

private:
    guardrail_backend_config config_;
    tokenizer_config tokenizer_config_;

    std::unique_ptr<Ort::Env> env_;
    std::unique_ptr<Ort::SessionOptions> session_options_;
    std::unique_ptr<Ort::Session> session_;

    std::unordered_map<std::string, int64_t> vocab_;
    std::vector<std::string> id2label_;
    std::vector<std::string> input_names_;
    std::vector<std::string> output_names_;

    int64_t unk_id_ = -1;
    int64_t cls_id_ = -1;
    int64_t sep_id_ = -1;
    mutable size_t num_labels_ = 0;
    mutable std::vector<float> last_logits_;
};

struct guardrail_state {
    std::mutex mutex;
    std::unique_ptr<guardrail_backend> backend;
    std::string model_root;
    std::string error;
};

static guardrail_state & state() {
    static guardrail_state s;
    return s;
}

static std::string path_string(const fs::path & path) {
    return path.string();
}

static void guardrail_startup_trace(const char * fmt, ...) {
    std::fputs("output guardrail: ", stderr);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fflush(stderr);
}

static std::vector<fs::path> dedupe_paths(const std::vector<fs::path> & input) {
    std::vector<fs::path> output;
    std::unordered_set<std::string> seen;
    for (const auto & path : input) {
        const std::string key = path.string();
        if (seen.insert(key).second) {
            output.push_back(path);
        }
    }
    return output;
}

static std::vector<fs::path> candidate_model_roots(const std::string & model) {
    std::vector<fs::path> candidates;

    fs::path exe_dir;
    try {
        exe_dir = get_executable_path().parent_path();
    } catch (...) {
        exe_dir = fs::path{};
    }

    auto append_variants = [&](const fs::path & base) {
        candidates.push_back(base);
        candidates.push_back(base / "models" / "guardrail");
        candidates.push_back(base / "guardrail");
    };

    if (!model.empty()) {
        const fs::path raw(model);
        append_variants(raw);
        if (!raw.is_absolute()) {
            if (path_exists(exe_dir)) {
                append_variants(exe_dir / raw);
                append_variants(exe_dir / "models" / raw);
            }
            append_variants(fs::current_path() / raw);
            append_variants(fs::current_path() / "models" / raw);
        }
    } else {
        if (path_exists(exe_dir)) {
            candidates.push_back(exe_dir / "models" / "guardrail");
            candidates.push_back(exe_dir / "guardrail");
            candidates.push_back(exe_dir.parent_path() / "models" / "guardrail");
        }
        candidates.push_back(fs::current_path() / "models" / "guardrail");
        candidates.push_back(fs::current_path() / "guardrail");
    }

    return dedupe_paths(candidates);
}

static fs::path resolve_candidate_model_root(const fs::path & candidate) {
    std::error_code ec;
    if (!fs::exists(candidate, ec)) {
        throw std::runtime_error("local guardrail model path not found: " + candidate.string());
    }

    if (fs::is_regular_file(candidate, ec)) {
        if (candidate.extension() != ".onnx") {
            throw std::runtime_error("local guardrail model file must be a .onnx file: " + candidate.string());
        }
        return candidate.parent_path();
    }

    if (!fs::is_directory(candidate, ec)) {
        throw std::runtime_error("local guardrail model path is neither a file nor a directory: " + candidate.string());
    }

    return candidate;
}

static fs::path find_onnx_model(const fs::path & root) {
    const fs::path preferred = root / "model.onnx";
    if (path_exists(preferred)) {
        return preferred;
    }

    std::vector<fs::path> matches;
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        throw std::runtime_error("local guardrail model path not found: " + root.string());
    }

    if (fs::is_regular_file(root, ec)) {
        if (root.extension() == ".onnx") {
            return root;
        }
        throw std::runtime_error("local guardrail model file must be a .onnx file: " + root.string());
    }

    if (!fs::is_directory(root, ec)) {
        throw std::runtime_error("local guardrail model path is neither a file nor a directory: " + root.string());
    }

    try {
        for (const auto & entry : fs::recursive_directory_iterator(root)) {
            if (entry.is_regular_file() && entry.path().extension() == ".onnx") {
                matches.push_back(entry.path());
            }
        }
    } catch (const fs::filesystem_error & e) {
        throw std::runtime_error(std::string("failed to scan guardrail model directory: ") + e.what());
    }

    if (matches.empty()) {
        throw std::runtime_error("no .onnx file found under local guardrail model directory: " + root.string());
    }

    std::sort(matches.begin(), matches.end(), [](const fs::path & a, const fs::path & b) {
        std::error_code ec_a;
        std::error_code ec_b;
        const auto size_a = fs::file_size(a, ec_a);
        const auto size_b = fs::file_size(b, ec_b);
        const auto key_a = std::make_tuple(ec_a ? std::numeric_limits<uintmax_t>::max() : size_a, a.string());
        const auto key_b = std::make_tuple(ec_b ? std::numeric_limits<uintmax_t>::max() : size_b, b.string());
        return key_a < key_b;
    });

    return matches.front();
}

static fs::path find_local_file(const fs::path & root, const std::string & name, bool required = true) {
    const fs::path direct = root / name;
    if (path_exists(direct)) {
        return direct;
    }

    std::error_code ec;
    if (!fs::exists(root, ec)) {
        if (required) {
            throw std::runtime_error("local guardrail model path not found: " + root.string());
        }
        return {};
    }

    if (!fs::is_directory(root, ec)) {
        if (required) {
            throw std::runtime_error("local guardrail model root is not a directory: " + root.string());
        }
        return {};
    }

    try {
        for (const auto & entry : fs::recursive_directory_iterator(root)) {
            if (entry.is_regular_file() && entry.path().filename() == name) {
                return entry.path();
            }
        }
    } catch (const fs::filesystem_error & e) {
        if (required) {
            throw std::runtime_error(std::string("failed to scan guardrail model directory: ") + e.what());
        }
    }

    if (required) {
        throw std::runtime_error("missing guardrail asset: " + (root / name).string());
    }

    return {};
}

static guardrail_backend & ensure_backend_locked() {
    auto & s = state();
    if (!s.backend) {
        try {
            const std::string requested_model = s.model_root;
            guardrail_startup_trace("initializing native backend (model source: %s)\n",
                                    requested_model.empty() ? "<local defaults>" : requested_model.c_str());
            s.backend = std::make_unique<guardrail_backend>(requested_model);
            s.error.clear();
        } catch (const std::exception & e) {
            guardrail_startup_trace("initialization failed: %s\n", e.what());
            s.error = e.what();
        }
    }

    if (!s.backend) {
        throw std::runtime_error(s.error.empty() ? "output guardrail backend is unavailable" : s.error);
    }

    return *s.backend;
}

guardrail_backend::guardrail_backend(const std::string & requested_model) {
    guardrail_startup_trace("startup begins\n");
    guardrail_startup_trace("initializing ONNX Runtime environment\n");
    const OrtApiBase * api_base = OrtGetApiBase();
    if (api_base == nullptr) {
        throw std::runtime_error("failed to obtain ONNX Runtime API base");
    }

    const char * ort_version = api_base->GetVersionString();
    guardrail_startup_trace("detected ONNX Runtime runtime version %s\n", ort_version ? ort_version : "<unknown>");

    const OrtApi * api = nullptr;
    int ort_api_version = 0;
    for (int v = ORT_API_VERSION; v >= 1; --v) {
        api = api_base->GetApi(static_cast<uint32_t>(v));
        if (api != nullptr) {
            ort_api_version = v;
            break;
        }
    }

    if (api == nullptr) {
        throw std::runtime_error("no compatible ONNX Runtime API version is available");
    }

    guardrail_startup_trace("binding ONNX Runtime API version %d\n", ort_api_version);
    Ort::InitApi(api);
    env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "output_guardrail");
    guardrail_startup_trace("ONNX Runtime environment ready\n");
    guardrail_startup_trace("initializing ONNX Runtime session options\n");
    session_options_ = std::make_unique<Ort::SessionOptions>();
    guardrail_startup_trace("ONNX Runtime session options ready\n");
    config_ = resolve_config(requested_model);
    load_tokenizer_config();
    load_vocab();
    load_labels();
    build_session();
    guardrail_startup_trace("backend startup complete\n");
}

guardrail_backend_config guardrail_backend::resolve_config(const std::string & requested_model) {
    guardrail_backend_config config;
    const std::vector<fs::path> candidates = candidate_model_roots(requested_model);

    guardrail_startup_trace("searching %zu local model candidates\n", candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        const std::string candidate = path_string(candidates[i]);
        guardrail_startup_trace("candidate[%zu] = %s\n", i, candidate.c_str());
    }

    for (const auto & candidate : candidates) {
        if (!path_exists(candidate)) {
            continue;
        }

        config.model_root = resolve_candidate_model_root(candidate);
        guardrail_startup_trace("selected model root = %s\n", path_string(config.model_root).c_str());
        break;
    }

    if (config.model_root.empty()) {
        if (!requested_model.empty()) {
            throw std::runtime_error("local guardrail model path not found: " + requested_model);
        }

        throw std::runtime_error(
            "no local ONNX guardrail model found. Place the NER assets under the model root directory."
        );
    }

    config.onnx_file = find_onnx_model(config.model_root);
    config.config_file = find_local_file(config.model_root, "config.json");
    config.vocab_file = find_local_file(config.model_root, "vocab.txt");
    config.tokenizer_config_file = find_local_file(config.model_root, "tokenizer_config.json", false);

    guardrail_startup_trace("resolved assets\n");
    guardrail_startup_trace("  model_root = %s\n", path_string(config.model_root).c_str());
    guardrail_startup_trace("  onnx_file  = %s\n", path_string(config.onnx_file).c_str());
    guardrail_startup_trace("  config     = %s\n", path_string(config.config_file).c_str());
    guardrail_startup_trace("  vocab      = %s\n", path_string(config.vocab_file).c_str());
    guardrail_startup_trace("  tokenizer  = %s\n", config.tokenizer_config_file.empty() ? "<default>" : path_string(config.tokenizer_config_file).c_str());

    return config;
}

void guardrail_backend::load_tokenizer_config() {
    tokenizer_config_ = tokenizer_config{};

    if (!config_.tokenizer_config_file.empty()) {
        guardrail_startup_trace("loading tokenizer config from %s\n", path_string(config_.tokenizer_config_file).c_str());
        const json data = json::parse(read_text_file(config_.tokenizer_config_file));
        tokenizer_config_.do_lower_case = data.value("do_lower_case", tokenizer_config_.do_lower_case);
        tokenizer_config_.tokenize_chinese_chars = data.value("tokenize_chinese_chars", tokenizer_config_.tokenize_chinese_chars);
        if (data.contains("strip_accents") && !data["strip_accents"].is_null()) {
            tokenizer_config_.strip_accents = data.value("strip_accents", tokenizer_config_.strip_accents);
        }
        tokenizer_config_.model_max_length = data.value("model_max_length", tokenizer_config_.model_max_length);
        tokenizer_config_.unk_token = data.value("unk_token", tokenizer_config_.unk_token);
        tokenizer_config_.cls_token = data.value("cls_token", tokenizer_config_.cls_token);
        tokenizer_config_.sep_token = data.value("sep_token", tokenizer_config_.sep_token);
    } else {
        guardrail_startup_trace("tokenizer config not found, using built-in defaults\n");
    }

    if (tokenizer_config_.strip_accents) {
        throw std::runtime_error("native guardrail tokenizer does not support strip_accents=true");
    }

    guardrail_startup_trace("tokenizer settings lower_case=%s tokenize_chinese_chars=%s strip_accents=%s model_max_length=%zu\n",
                            tokenizer_config_.do_lower_case ? "true" : "false",
                            tokenizer_config_.tokenize_chinese_chars ? "true" : "false",
                            tokenizer_config_.strip_accents ? "true" : "false",
                            tokenizer_config_.model_max_length);
}

void guardrail_backend::load_vocab() {
    guardrail_startup_trace("loading vocab from %s\n", path_string(config_.vocab_file).c_str());
    std::ifstream file(config_.vocab_file, std::ios::binary);
    if (!file) {
        throw std::runtime_error("failed to open guardrail vocab file: " + config_.vocab_file.string());
    }

    std::string line;
    int64_t id = 0;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        vocab_.emplace(line, id++);
    }

    if (vocab_.empty()) {
        throw std::runtime_error("guardrail vocab is empty: " + config_.vocab_file.string());
    }

    auto require_token = [&](const std::string & token, const char * name) -> int64_t {
        auto it = vocab_.find(token);
        if (it == vocab_.end()) {
            throw std::runtime_error(std::string("missing required tokenizer token ") + name + ": " + token);
        }
        return it->second;
    };

    unk_id_ = require_token(tokenizer_config_.unk_token, "unk");
    cls_id_ = require_token(tokenizer_config_.cls_token, "cls");
    sep_id_ = require_token(tokenizer_config_.sep_token, "sep");

    guardrail_startup_trace("loaded %zu vocab entries (unk=%lld cls=%lld sep=%lld)\n",
                            vocab_.size(),
                            static_cast<long long>(unk_id_),
                            static_cast<long long>(cls_id_),
                            static_cast<long long>(sep_id_));
}

void guardrail_backend::load_labels() {
    guardrail_startup_trace("loading labels from %s\n", path_string(config_.config_file).c_str());
    const json data = json::parse(read_text_file(config_.config_file));
    if (!data.contains("id2label") && !data.contains("label2id")) {
        throw std::runtime_error("guardrail config is missing id2label/label2id: " + config_.config_file.string());
    }

    std::unordered_map<int64_t, std::string> labels;
    if (data.contains("id2label")) {
        const auto & id2label = data["id2label"];
        if (id2label.is_array()) {
            for (size_t i = 0; i < id2label.size(); ++i) {
                labels.emplace(static_cast<int64_t>(i), id2label.at(i).get<std::string>());
            }
        } else if (id2label.is_object()) {
            for (const auto & item : id2label.items()) {
                labels.emplace(std::stoll(item.key()), item.value().get<std::string>());
            }
        } else {
            throw std::runtime_error("guardrail config id2label must be an array or object");
        }
    } else {
        const auto & label2id = data["label2id"];
        if (!label2id.is_object()) {
            throw std::runtime_error("guardrail config label2id must be an object");
        }
        for (const auto & item : label2id.items()) {
            labels.emplace(item.value().get<int64_t>(), item.key());
        }
    }

    if (labels.empty()) {
        throw std::runtime_error("guardrail config did not provide any labels: " + config_.config_file.string());
    }

    const auto max_id = std::max_element(labels.begin(), labels.end(),
        [](const auto & lhs, const auto & rhs) { return lhs.first < rhs.first; })->first;
    id2label_.assign(static_cast<size_t>(max_id) + 1, "");
    for (const auto & it : labels) {
        if (it.first < 0) {
            throw std::runtime_error("guardrail config contains negative label ids");
        }
        id2label_[static_cast<size_t>(it.first)] = it.second;
    }

    for (size_t i = 0; i < id2label_.size(); ++i) {
        if (id2label_[i].empty()) {
            throw std::runtime_error("guardrail config is missing a label entry for id " + std::to_string(i));
        }
    }

    if (std::find(id2label_.begin(), id2label_.end(), "O") == id2label_.end()) {
        throw std::runtime_error("guardrail config does not define an O label");
    }

    guardrail_startup_trace("loaded %zu labels\n", id2label_.size());
}

void guardrail_backend::build_session() {
    if (!path_exists(config_.onnx_file)) {
        throw std::runtime_error("guardrail ONNX file not found: " + config_.onnx_file.string());
    }
    if (!env_) {
        throw std::runtime_error("guardrail ONNX Runtime environment is not initialized");
    }
    if (!session_options_) {
        throw std::runtime_error("guardrail ONNX Runtime session options are not initialized");
    }

    guardrail_startup_trace("creating ONNX Runtime session from %s\n", path_string(config_.onnx_file).c_str());
    session_options_->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    session_options_->SetIntraOpNumThreads(1);
    session_options_->SetInterOpNumThreads(1);
    session_options_->SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);

    session_ = std::make_unique<Ort::Session>(*env_, config_.onnx_file.c_str(), *session_options_);

    Ort::AllocatorWithDefaultOptions allocator;
    const size_t n_inputs = session_->GetInputCount();
    const size_t n_outputs = session_->GetOutputCount();

    input_names_.reserve(n_inputs);
    for (size_t i = 0; i < n_inputs; ++i) {
        auto name = session_->GetInputNameAllocated(i, allocator);
        input_names_.emplace_back(name.get());
    }

    output_names_.reserve(n_outputs);
    for (size_t i = 0; i < n_outputs; ++i) {
        auto name = session_->GetOutputNameAllocated(i, allocator);
        output_names_.emplace_back(name.get());
    }

    if (output_names_.empty()) {
        throw std::runtime_error("guardrail model has no outputs: " + config_.onnx_file.string());
    }

    guardrail_startup_trace("session ready (%zu inputs, %zu outputs)\n", input_names_.size(), output_names_.size());
    for (size_t i = 0; i < input_names_.size(); ++i) {
        guardrail_startup_trace("input[%zu] = %s\n", i, input_names_[i].c_str());
    }
    for (size_t i = 0; i < output_names_.size(); ++i) {
        guardrail_startup_trace("output[%zu] = %s\n", i, output_names_[i].c_str());
    }
}

std::vector<basic_token> guardrail_backend::basic_tokenize(const std::string & text) const {
    std::vector<basic_token> tokens;
    auto cps = utf8_codepoints_with_offsets(text);

    size_t current_start = std::string::npos;
    size_t current_end = 0;
    std::string current_text;

    auto flush = [&]() {
        if (current_start != std::string::npos) {
            tokens.push_back({current_start, current_end, current_text});
            current_start = std::string::npos;
            current_end = 0;
            current_text.clear();
        }
    };

    for (const auto & cp : cps) {
        const auto flags = unicode_cpt_flags_from_cpt(cp.cpt);

        if (flags.is_whitespace) {
            flush();
            continue;
        }

        if (cp.cpt == 0 || cp.cpt == 0xFFFD || flags.is_control) {
            continue;
        }

        const bool is_chinese = tokenizer_config_.tokenize_chinese_chars && unicode_cpt_is_han(cp.cpt);
        const bool is_punctuation = flags.is_punctuation || flags.is_symbol;
        const std::string cpt_text = utf8_from_cpt(tokenizer_config_.do_lower_case ? unicode_tolower(cp.cpt) : cp.cpt);

        if (is_chinese || is_punctuation) {
            flush();
            tokens.push_back({cp.start, cp.end, cpt_text});
            continue;
        }

        if (current_start == std::string::npos) {
            current_start = cp.start;
        }
        current_end = cp.end;
        current_text += cpt_text;
    }

    flush();
    return tokens;
}

std::vector<wordpiece_token> guardrail_backend::wordpiece_tokenize(const basic_token & token) const {
    std::vector<wordpiece_token> pieces;
    if (token.text.empty()) {
        return pieces;
    }

    const auto token_boundaries = utf8_codepoints_with_offsets(token.text);
    std::vector<size_t> boundary_bytes;
    boundary_bytes.reserve(token_boundaries.size() + 1);
    boundary_bytes.push_back(0);
    for (const auto & cp : token_boundaries) {
        boundary_bytes.push_back(cp.end);
    }

    const std::string & raw = token.text;
    size_t start = 0;
    bool is_first = true;

    while (start < raw.size()) {
        size_t best_end = 0;
        int64_t best_id = -1;
        std::string best_text;

        for (size_t idx = boundary_bytes.size(); idx-- > 0; ) {
            const size_t end = boundary_bytes[idx];
            if (end <= start) {
                break;
            }

            std::string candidate = raw.substr(start, end - start);
            if (!is_first) {
                candidate = "##" + candidate;
            }

            auto it = vocab_.find(candidate);
            if (it != vocab_.end()) {
                best_end = end;
                best_id = it->second;
                best_text = candidate;
                break;
            }
        }

        if (best_id < 0) {
            pieces.push_back({unk_id_, token.start, token.end, tokenizer_config_.unk_token});
            return pieces;
        }

        pieces.push_back({best_id, token.start + start, token.start + best_end, best_text});
        start = best_end;
        is_first = false;
    }

    return pieces;
}

std::vector<wordpiece_token> guardrail_backend::tokenize(const std::string & text) const {
    std::vector<wordpiece_token> pieces;
    const auto basic_tokens = basic_tokenize(text);
    for (const auto & token : basic_tokens) {
        auto wordpieces = wordpiece_tokenize(token);
        pieces.insert(pieces.end(), wordpieces.begin(), wordpieces.end());
    }
    return pieces;
}

std::vector<int64_t> guardrail_backend::run_model(
        const std::vector<int64_t> & input_ids,
        const std::vector<int64_t> & attention_mask,
        const std::vector<int64_t> & token_type_ids) const {
    if (!session_) {
        throw std::runtime_error("guardrail ONNX session is not initialized");
    }

    Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    std::vector<Ort::Value> inputs;
    std::vector<const char*> input_names;
    std::vector<const char*> output_names;
    inputs.reserve(input_names_.size());
    input_names.reserve(input_names_.size());
    output_names.reserve(output_names_.size());

    auto create_tensor = [&](const std::vector<int64_t> & data) {
        const std::array<int64_t, 2> shape = { 1, static_cast<int64_t>(data.size()) };
        return Ort::Value::CreateTensor<int64_t>(
            memory_info,
            const_cast<int64_t *>(data.data()),
            data.size(),
            shape.data(),
            shape.size()
        );
    };

    for (const auto & name : input_names_) {
        if (name == "input_ids") {
            inputs.push_back(create_tensor(input_ids));
            input_names.push_back(name.c_str());
        } else if (name == "attention_mask") {
            inputs.push_back(create_tensor(attention_mask));
            input_names.push_back(name.c_str());
        } else if (name == "token_type_ids") {
            inputs.push_back(create_tensor(token_type_ids));
            input_names.push_back(name.c_str());
        } else {
            throw std::runtime_error("unsupported guardrail model input: " + name);
        }
    }
    for (const auto & name : output_names_) {
        output_names.push_back(name.c_str());
    }

    Ort::RunOptions run_options;
    auto outputs = session_->Run(
        run_options,
        input_names.data(),
        inputs.data(),
        inputs.size(),
        output_names.data(),
        output_names.size()
    );

    if (outputs.empty()) {
        throw std::runtime_error("guardrail model produced no outputs");
    }

    const Ort::Value & logits_value = outputs.front();
    auto shape_info = logits_value.GetTensorTypeAndShapeInfo();
    const auto shape = shape_info.GetShape();
    if (shape.size() < 2) {
        throw std::runtime_error("guardrail model output has unexpected rank");
    }

    size_t seq_len = 0;
    if (shape.size() == 2) {
        seq_len = static_cast<size_t>(shape[0]);
        num_labels_ = static_cast<size_t>(shape[1]);
    } else if (shape.size() == 3) {
        if (shape[0] != 1) {
            throw std::runtime_error("guardrail model output batch dimension must be 1");
        }
        seq_len = static_cast<size_t>(shape[1]);
        num_labels_ = static_cast<size_t>(shape[2]);
    } else {
        throw std::runtime_error("guardrail model output has unsupported rank");
    }

    if (seq_len != input_ids.size()) {
        throw std::runtime_error("guardrail model output length does not match inputs");
    }
    if (num_labels_ == 0) {
        throw std::runtime_error("guardrail model output has no labels");
    }
    if (num_labels_ > id2label_.size()) {
        throw std::runtime_error("guardrail config label count does not match model output");
    }

    const float * logits = logits_value.GetTensorData<float>();
    if (logits == nullptr) {
        throw std::runtime_error("guardrail model returned a null logits tensor");
    }

    last_logits_.assign(logits, logits + (seq_len * num_labels_));

    std::vector<int64_t> pred_ids;
    pred_ids.reserve(seq_len);
    for (size_t i = 0; i < seq_len; ++i) {
        const float * row = logits + (i * num_labels_);
        size_t best_idx = 0;
        float best_score = row[0];
        for (size_t j = 1; j < num_labels_; ++j) {
            if (row[j] > best_score) {
                best_score = row[j];
                best_idx = j;
            }
        }
        pred_ids.push_back(static_cast<int64_t>(best_idx));
    }

    return pred_ids;
}

std::vector<std::string> guardrail_backend::classify(const std::vector<wordpiece_token> & pieces) const {
    if (pieces.empty()) {
        return {};
    }

    const size_t window_capacity = tokenizer_config_.model_max_length > 2 ? tokenizer_config_.model_max_length - 2 : 0;
    if (window_capacity == 0) {
        throw std::runtime_error("guardrail tokenizer max length is too small");
    }

    std::vector<std::string> labels(pieces.size(), "O");
    std::vector<float> scores(pieces.size(), -std::numeric_limits<float>::infinity());
    const size_t stride = std::max<size_t>(1, window_capacity / 2);

    for (size_t window_start = 0; window_start < pieces.size(); window_start += stride) {
        const size_t window_end = std::min(window_start + window_capacity, pieces.size());
        const size_t window_len = window_end - window_start;
        if (window_len == 0) {
            break;
        }

        std::vector<int64_t> input_ids;
        std::vector<int64_t> attention_mask;
        std::vector<int64_t> token_type_ids;
        std::vector<size_t> token_indices;

        input_ids.reserve(window_len + 2);
        attention_mask.reserve(window_len + 2);
        token_type_ids.reserve(window_len + 2);
        token_indices.reserve(window_len);

        input_ids.push_back(cls_id_);
        attention_mask.push_back(1);
        token_type_ids.push_back(0);

        for (size_t i = window_start; i < window_end; ++i) {
            input_ids.push_back(pieces[i].id);
            attention_mask.push_back(1);
            token_type_ids.push_back(0);
            token_indices.push_back(i);
        }

        input_ids.push_back(sep_id_);
        attention_mask.push_back(1);
        token_type_ids.push_back(0);

        const auto pred_ids = run_model(input_ids, attention_mask, token_type_ids);
        if (pred_ids.size() != input_ids.size()) {
            throw std::runtime_error("guardrail model returned unexpected token count");
        }

        for (size_t j = 1; j + 1 < pred_ids.size(); ++j) {
            const size_t global_idx = token_indices[j - 1];
            const int64_t label_id = pred_ids[j];
            if (label_id < 0 || static_cast<size_t>(label_id) >= id2label_.size()) {
                continue;
            }

            const std::string & label = id2label_[static_cast<size_t>(label_id)];
            if (!is_entity_label(label)) {
                continue;
            }

            const float score = last_logits_[j * num_labels_ + static_cast<size_t>(label_id)];
            if (labels[global_idx] == "O" || score > scores[global_idx]) {
                labels[global_idx] = label;
                scores[global_idx] = score;
            }
        }
    }

    return labels;
}

std::vector<std::pair<size_t, size_t>> guardrail_backend::redaction_spans(const std::string & text) const {
    std::vector<std::pair<size_t, size_t>> spans;
    if (text.empty()) {
        return spans;
    }

    auto pieces = tokenize(text);
    if (pieces.empty()) {
        return spans;
    }

    const std::vector<std::string> labels = classify(pieces);
    spans.reserve(pieces.size());

    std::string current_type;
    size_t current_start = 0;
    size_t current_end = 0;

    for (size_t i = 0; i < pieces.size(); ++i) {
        const auto & piece = pieces[i];
        if (piece.end <= piece.start) {
            continue;
        }

        const std::string & label = labels[i];
        if (!is_entity_label(label)) {
            if (!current_type.empty()) {
                spans.emplace_back(current_start, current_end);
                current_type.clear();
            }
            continue;
        }

        const std::string entity_type = entity_type_from_label(label);
        if (!current_type.empty() && current_type == entity_type && piece.start <= current_end) {
            current_end = std::max(current_end, piece.end);
        } else {
            if (!current_type.empty()) {
                spans.emplace_back(current_start, current_end);
            }
            current_type = entity_type;
            current_start = piece.start;
            current_end = piece.end;
        }
    }

    if (!current_type.empty()) {
        spans.emplace_back(current_start, current_end);
    }

    static const std::regex url_re(R"((https?://[^\s<>\"]+|www\.[^\s<>\"]+))", std::regex::icase);
    for (auto it = std::sregex_iterator(text.begin(), text.end(), url_re); it != std::sregex_iterator(); ++it) {
        spans.emplace_back(static_cast<size_t>(it->position()), static_cast<size_t>(it->position() + it->length()));
    }

    return merge_spans(std::move(spans));
}

std::string guardrail_backend::redact(const std::string & text) const {
    if (text.empty()) {
        return {};
    }

    std::vector<std::pair<size_t, size_t>> spans = redaction_spans(text);
    if (spans.empty()) {
        return text;
    }

    std::string out = text;
    for (auto it = spans.rbegin(); it != spans.rend(); ++it) {
        if (it->first >= out.size()) {
            continue;
        }
        const size_t end = std::min(it->second, out.size());
        if (end <= it->first) {
            continue;
        }
        out.replace(it->first, end - it->first, "[REDACTED]");
    }

    return out;
}

std::string guardrail_backend::redact_prefix(const std::string & text, size_t prefix_bytes) const {
    if (text.empty() || prefix_bytes == 0) {
        return {};
    }

    const size_t limit = std::min(prefix_bytes, text.size());
    std::vector<std::pair<size_t, size_t>> spans = redaction_spans(text);
    if (spans.empty()) {
        return text.substr(0, limit);
    }

    std::string out = text.substr(0, limit);
    for (auto it = spans.rbegin(); it != spans.rend(); ++it) {
        if (it->second > limit) {
            continue;
        }
        if (it->first >= out.size()) {
            continue;
        }
        const size_t end = std::min(it->second, out.size());
        if (end <= it->first) {
            continue;
        }
        out.replace(it->first, end - it->first, "[REDACTED]");
    }

    return out;
}

} // namespace

bool output_guardrail_is_configured() {
    auto & s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return !s.model_root.empty();
}

bool output_guardrail_has_assets(const std::string & model_root) {
    if (model_root.empty()) {
        return false;
    }

    const std::vector<fs::path> candidates = candidate_model_roots(model_root);
    for (const auto & candidate : candidates) {
        if (!path_exists(candidate)) {
            continue;
        }

        try {
            const fs::path root = resolve_candidate_model_root(candidate);
            (void) find_onnx_model(root);
            (void) find_local_file(root, "config.json");
            (void) find_local_file(root, "vocab.txt");
            (void) find_local_file(root, "tokenizer_config.json", false);
            return true;
        } catch (...) {
            continue;
        }
    }

    return false;
}

void output_guardrail_set_model_root(const std::string & model_root) {
    auto & s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.model_root != model_root) {
        s.backend.reset();
        s.error.clear();
    }
    s.model_root = model_root;
}

bool output_guardrail_label_is_redactable(const std::string & label) {
    return is_redactable_entity_label(label);
}

bool output_guardrail_require_backend(std::string * error) {
    auto & s = state();
    std::lock_guard<std::mutex> lock(s.mutex);

    if (s.model_root.empty()) {
        if (error) {
            *error = "output guardrail is not configured";
        }
        return false;
    }

    try {
        (void) ensure_backend_locked();
        if (error) {
            error->clear();
        }
        return true;
    } catch (const std::exception & e) {
        if (error) {
            *error = e.what();
        }
        return false;
    }
}

size_t output_guardrail_safe_prefix_len(std::string_view text, size_t holdback_bytes) {
    if (text.size() <= holdback_bytes) {
        return 0;
    }

    size_t safe = text.size() - holdback_bytes;
    while (safe > 0 && safe < text.size() && (static_cast<unsigned char>(text[safe]) & 0xC0) == 0x80) {
        --safe;
    }
    return safe;
}

std::string output_guardrail_redact_text(const std::string & text) {
    if (!output_guardrail_is_configured()) {
        return text;
    }

    auto & s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return ensure_backend_locked().redact(text);
}

output_guardrail::output_guardrail() {
    if (!output_guardrail_is_configured()) {
        enabled_ = false;
        return;
    }

    std::string error;
    if (!output_guardrail_require_backend(&error)) {
        throw std::runtime_error("output guardrail backend unavailable: " + error);
    }

    redactor_ = [](const std::string & text) {
        return output_guardrail_redact_text(text);
    };
    window_redactor_ = [](const std::string & text, size_t prefix_bytes) {
        auto & s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        return ensure_backend_locked().redact_prefix(text, prefix_bytes);
    };
    enabled_ = true;
}

output_guardrail::output_guardrail(output_guardrail_redactor redactor, size_t buffer_bytes)
    : redactor_(std::move(redactor))
    , buffer_bytes_(buffer_bytes)
    , enabled_(static_cast<bool>(redactor_)) {
    if (enabled_ && buffer_bytes_ <= overlap_bytes_) {
        throw std::invalid_argument("output guardrail buffer size must be larger than overlap size");
    }
}

std::string output_guardrail::feed(const std::string & text) {
    if (!enabled_) {
        return text;
    }

    pending_ += text;
    std::string out;
    while (pending_.size() >= buffer_bytes_) {
        const std::string window = pending_.substr(0, buffer_bytes_);
        size_t emit_bytes = output_guardrail_safe_prefix_len(window, overlap_bytes_);
        if (emit_bytes == 0) {
            emit_bytes = buffer_bytes_ - overlap_bytes_;
        }

        if (window_redactor_) {
            out += window_redactor_(window, emit_bytes);
        } else {
            out += redactor_(window.substr(0, emit_bytes));
        }

        pending_.erase(0, emit_bytes);
    }

    return out;
}

std::string output_guardrail::flush() {
    if (!enabled_) {
        pending_.clear();
        return {};
    }

    if (pending_.empty()) {
        return {};
    }

    std::string out = redactor_(pending_);
    pending_.clear();
    return out;
}

void output_guardrail::reset() {
    pending_.clear();
}
