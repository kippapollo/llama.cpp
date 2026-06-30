#include "prompt-guardrail.h"

#include "common.h"
#include "forced-system-prompt.h"
#include "log.h"
#include "prompt-guardrail-embedding.h"
#include "prompt-guardrail-router.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
std::atomic<bool> g_tool_force_mode{false};
}

void common_prompt_guardrail_set_tool_force_mode(bool enabled) {
    g_tool_force_mode.store(enabled, std::memory_order_relaxed);
}

bool common_prompt_guardrail_tool_force_mode() {
    return g_tool_force_mode.load(std::memory_order_relaxed);
}

namespace {

constexpr const char * k_prompt_guardrail_refusal_prompt =
    "Refuse any request that is not software, IT, or coding related. "
    "Reply with exactly: I can only assist with software, IT, and coding requests.";

constexpr int    k_prompt_guardrail_n_ctx         = 4096;
constexpr int    k_prompt_guardrail_n_batch       = 512;
constexpr int    k_prompt_guardrail_n_ubatch      = 128;
constexpr size_t k_prompt_guardrail_decode_chunk  = 256;

using fs_path = std::filesystem::path;

struct prompt_guardrail_state {
    std::mutex mutex;

    bool enabled = false;
    bool load_attempted = false;
    bool load_ok = false;

    std::string model_path;
    std::string samples_path;
    fs_path resolved_model_path;
    fs_path resolved_samples_path;
    fs_path resolved_system_prompt_path;
    std::string forced_system_prompt = LLAMA_CPP_FORCED_SYSTEM_PROMPT;
    std::string error;

    common_init_result_ptr init;
    common_prompt_guardrail_embedding_samples samples;
    std::vector<std::vector<float>> negative_embeddings;
    common_prompt_guardrail_backend backend = COMMON_PROMPT_GUARDRAIL_BACKEND_NONE;
};

prompt_guardrail_state & state() {
    static prompt_guardrail_state s;
    return s;
}

static std::string to_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static bool path_exists(const fs_path & path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

static bool path_is_file(const fs_path & path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

static bool path_is_dir(const fs_path & path) {
    return fs_is_directory(path.string());
}

static std::vector<fs_path> candidate_roots() {
    std::vector<fs_path> roots;

    try {
        fs_path current = std::filesystem::current_path();
        for (int i = 0; i < 4 && !current.empty(); ++i) {
            roots.push_back(current);
            if (!current.has_parent_path() || current.parent_path() == current) {
                break;
            }
            current = current.parent_path();
        }
    } catch (...) {
        // Ignore path resolution failures and fall back to explicit env paths.
    }

    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    return roots;
}

static fs_path find_gguf_in_directory(const fs_path & directory) {
    const fs_path preferred = directory / "embedding.gguf";
    return path_exists(preferred) ? preferred : fs_path{};
}

static fs_path resolve_model_path(const std::string & requested_model) {
    if (requested_model.empty()) {
        return {};
    }

    std::vector<fs_path> candidates;
    const fs_path raw(requested_model);
    candidates.push_back(raw);

    if (!raw.is_absolute()) {
        for (const auto & root : candidate_roots()) {
            candidates.push_back(root / raw);
            candidates.push_back(root / "models" / raw);
            candidates.push_back(root / "guardrail" / raw);
            candidates.push_back(root / "models" / "guardrail" / raw);
        }
    }

    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    for (const auto & candidate : candidates) {
        if (!path_exists(candidate)) {
            continue;
        }

        if (path_is_file(candidate)) {
            if (to_lower_copy(candidate.extension().string()) == ".gguf") {
                return candidate;
            }
            continue;
        }

        if (path_is_dir(candidate)) {
            const auto resolved = find_gguf_in_directory(candidate);
            if (!resolved.empty()) {
                return resolved;
            }
        }
    }

    return {};
}

static fs_path resolve_samples_path(
        const std::string & requested_samples,
        const fs_path & resolved_model_path) {
    constexpr const char * k_samples_filename = "prompt-guardrail-samples.json";

    std::vector<fs_path> candidates;

    if (!requested_samples.empty()) {
        const fs_path raw(requested_samples);
        candidates.push_back(raw);

        if (!raw.is_absolute()) {
            for (const auto & root : candidate_roots()) {
                candidates.push_back(root / raw);
                candidates.push_back(root / "models" / raw);
                candidates.push_back(root / "guardrail" / raw);
                candidates.push_back(root / "models" / "guardrail" / raw);
            }
        }
    } else {
        if (!resolved_model_path.empty()) {
            const fs_path model_dir = path_is_dir(resolved_model_path) ? resolved_model_path : resolved_model_path.parent_path();
            if (!model_dir.empty()) {
                candidates.push_back(model_dir);
                if (model_dir.has_parent_path()) {
                    candidates.push_back(model_dir.parent_path());
                }
            }
        }

        for (const auto & root : candidate_roots()) {
            candidates.push_back(root / "models" / "guardrail");
            candidates.push_back(root / "guardrail");
        }
    }

    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    for (const auto & candidate : candidates) {
        if (!path_exists(candidate)) {
            continue;
        }

        if (path_is_file(candidate)) {
            return candidate;
        }

        if (path_is_dir(candidate)) {
            const fs_path nested = candidate / k_samples_filename;
            if (path_is_file(nested)) {
                return nested;
            }
        }
    }

    return {};
}

static fs_path resolve_system_prompt_path(const fs_path & resolved_model_path) {
    if (resolved_model_path.empty()) {
        return {};
    }

    fs_path model_root = resolved_model_path;
    if (path_is_file(resolved_model_path)) {
        model_root = resolved_model_path.parent_path();
    }

    if (model_root.empty()) {
        return {};
    }

    const fs_path prompt_path = model_root / "system-prompt.txt";
    return path_is_file(prompt_path) ? prompt_path : fs_path{};
}

static bool read_text_file(
        const fs_path & path,
        std::string * out) {
    if (out == nullptr) {
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    *out = std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
    return true;
}

static void reset_loaded_state(prompt_guardrail_state & s) {
    s.load_attempted = false;
    s.load_ok = false;
    s.backend = COMMON_PROMPT_GUARDRAIL_BACKEND_NONE;
    s.init.reset();
    s.samples = {};
    s.negative_embeddings.clear();
    s.resolved_model_path.clear();
    s.resolved_samples_path.clear();
    s.resolved_system_prompt_path.clear();
    s.forced_system_prompt = LLAMA_CPP_FORCED_SYSTEM_PROMPT;
    s.error.clear();
}

static void refresh_locked(prompt_guardrail_state & s) {
    const bool enabled = !s.model_path.empty();
    if (enabled != s.enabled) {
        reset_loaded_state(s);
    }
    s.enabled = enabled;
}

static void clear_context_memory(llama_context * ctx) {
    if (ctx == nullptr) {
        return;
    }

    if (auto * memory = llama_get_memory(ctx); memory != nullptr) {
        llama_memory_clear(memory, true);
    }
    llama_synchronize(ctx);
}

static bool embedding_is_zero(const std::vector<float> & embedding) {
    return std::all_of(embedding.begin(), embedding.end(), [](float value) {
        return value == 0.0f;
    });
}

static bool embed_text_locked(
        prompt_guardrail_state & s,
        std::string_view formatted_text,
        std::vector<float> * out,
        std::string * error) {
    if (!s.init || !s.init->model() || !s.init->context()) {
        if (error) {
            *error = "prompt guardrail model is not initialized";
        }
        return false;
    }

    const auto * model = s.init->model();
    const auto * vocab = llama_model_get_vocab(model);
    auto * ctx = s.init->context();

    if (!vocab || !ctx) {
        if (error) {
            *error = "prompt guardrail model vocabulary is unavailable";
        }
        return false;
    }

    if (formatted_text.empty()) {
        if (error) {
            *error = "prompt guardrail embedding text is empty";
        }
        return false;
    }

    std::string text(formatted_text);
    auto tokens = common_tokenize(vocab, text, /* add_special= */ true, /* parse_special= */ true);
    if (tokens.empty()) {
        if (error) {
            *error = "prompt guardrail embedding tokenization produced no tokens";
        }
        return false;
    }

    const int n_ctx = llama_n_ctx(ctx);
    if (static_cast<int>(tokens.size()) > n_ctx) {
        if (error) {
            *error = "prompt guardrail embedding text exceeds the model context length";
        }
        return false;
    }

    const int n_embd_out = llama_model_n_embd_out(model);
    if (n_embd_out <= 0) {
        if (error) {
            *error = "prompt guardrail model does not expose an embedding output";
        }
        return false;
    }

    const bool use_encoder = llama_model_has_encoder(model);
    const bool use_decoder = llama_model_has_decoder(model);
    if (!use_encoder && !use_decoder) {
        if (error) {
            *error = "prompt guardrail model does not expose an encoder or decoder";
        }
        return false;
    }

    clear_context_memory(ctx);
    llama_set_embeddings(ctx, true);

    const size_t batch_limit = std::max<size_t>(
        1,
        std::min<size_t>(k_prompt_guardrail_decode_chunk, static_cast<size_t>(llama_n_batch(ctx))));

    for (size_t offset = 0; offset < tokens.size(); offset += batch_limit) {
        const size_t n_chunk = std::min(batch_limit, tokens.size() - offset);
        auto batch = llama_batch_init(static_cast<int32_t>(n_chunk), 0, 1);
        if (batch.token == nullptr || batch.pos == nullptr || batch.seq_id == nullptr || batch.n_seq_id == nullptr || batch.logits == nullptr) {
            llama_batch_free(batch);
            if (error) {
                *error = "prompt guardrail failed to allocate an embedding batch";
            }
            clear_context_memory(ctx);
            return false;
        }

        for (size_t i = 0; i < n_chunk; ++i) {
            common_batch_add(
                batch,
                tokens[offset + i],
                static_cast<llama_pos>(offset + i),
                { 0 },
                true);
        }

        const int rc = use_encoder ? llama_encode(ctx, batch) : llama_decode(ctx, batch);
        llama_batch_free(batch);
        if (rc != 0) {
            if (error) {
                *error = "prompt guardrail embedding pass failed";
            }
            clear_context_memory(ctx);
            return false;
        }
    }

    const float * embd = llama_get_embeddings_seq(ctx, 0);
    if (embd == nullptr) {
        if (error) {
            *error = "prompt guardrail embedding output was unavailable";
        }
        clear_context_memory(ctx);
        return false;
    }

    out->assign(static_cast<size_t>(n_embd_out), 0.0f);
    common_embd_normalize(embd, out->data(), n_embd_out, 2);

    if (embedding_is_zero(*out)) {
        if (error) {
            *error = "prompt guardrail embedding output normalized to a zero vector";
        }
        clear_context_memory(ctx);
        return false;
    }

    clear_context_memory(ctx);
    return true;
}

static bool load_backend_locked(prompt_guardrail_state & s, std::string * error) {
    if (!s.enabled) {
        if (error) {
            error->clear();
        }
        return true;
    }

    if (s.load_attempted) {
        if (!s.load_ok && error) {
            *error = s.error;
        }
        return s.load_ok;
    }

    s.load_attempted = true;
    s.error.clear();

    try {
        s.resolved_model_path = resolve_model_path(s.model_path);
        if (s.resolved_model_path.empty()) {
            throw std::runtime_error(std::string("prompt guardrail model path not found: ") + s.model_path);
        }

        s.resolved_samples_path = resolve_samples_path(s.samples_path, s.resolved_model_path);
        if (s.resolved_samples_path.empty()) {
            if (!s.samples_path.empty()) {
                throw std::runtime_error(std::string("prompt guardrail samples path not found: ") + s.samples_path);
            }

            throw std::runtime_error(
                "prompt guardrail samples file not found; set LLAMA_PROMPT_GUARDRAIL_SAMPLES "
                "or place prompt-guardrail-samples.json under models/guardrail");
        }

        s.resolved_system_prompt_path = resolve_system_prompt_path(s.resolved_model_path);
        s.forced_system_prompt = LLAMA_CPP_FORCED_SYSTEM_PROMPT;
        if (!s.resolved_system_prompt_path.empty()) {
            std::string system_prompt_text;
            if (read_text_file(s.resolved_system_prompt_path, &system_prompt_text)) {
                system_prompt_text = string_strip(system_prompt_text);
                if (!system_prompt_text.empty()) {
                    s.forced_system_prompt = std::move(system_prompt_text);
                    LOG_INF("%s: loaded prompt guardrail system prompt from '%s'\n",
                            __func__,
                            s.resolved_system_prompt_path.string().c_str());
                } else {
                    LOG_INF("%s: prompt guardrail system prompt file '%s' is empty, using built-in fallback\n",
                            __func__,
                            s.resolved_system_prompt_path.string().c_str());
                }
            } else {
                LOG_WRN("%s: failed to open prompt guardrail system prompt file '%s', using built-in fallback\n",
                        __func__,
                        s.resolved_system_prompt_path.string().c_str());
            }
        } else {
            LOG_INF("%s: prompt guardrail system prompt file not found, using built-in fallback\n",
                    __func__);
        }

        if (!common_prompt_guardrail_load_embedding_samples(
                s.resolved_samples_path.string(),
                &s.samples,
                &s.error)) {
            throw std::runtime_error(s.error);
        }

        common_params params;
        params.model.path = s.resolved_model_path.string();
        params.n_ctx = k_prompt_guardrail_n_ctx;
        params.n_batch = k_prompt_guardrail_n_batch;
        params.n_ubatch = k_prompt_guardrail_n_ubatch;
        params.n_parallel = 1;
        params.n_sequences = 1;
        params.n_predict = 1;
        params.embedding = true;
        params.pooling_type = LLAMA_POOLING_TYPE_LAST;
        params.fit_params = false;
        params.warmup = false;
        params.use_mmap = true;
        params.use_mlock = false;
        params.use_direct_io = true;
        params.model.allow_plain_model = true;
        params.verbosity = LOG_LEVEL_ERROR;
        params.no_perf = true;

        LOG_INF("%s: loading prompt guardrail embedding model from '%s'\n",
                __func__,
                s.resolved_model_path.string().c_str());
        LOG_INF("%s: loading prompt guardrail samples from '%s'\n",
                __func__,
                s.resolved_samples_path.string().c_str());

        s.init = common_init_from_params(params);
        if (!s.init || !s.init->model() || !s.init->context()) {
            throw std::runtime_error(
                std::string("failed to load prompt guardrail embedding model: ") + s.resolved_model_path.string());
        }

        LOG_INF("%s: prompt guardrail mode=sample-similarity model='%s' samples='%s' threshold=%.2f\n",
                __func__,
                s.resolved_model_path.string().c_str(),
                s.resolved_samples_path.string().c_str(),
                s.samples.negative_similarity_threshold);
        LOG_INF("%s: prompt guardrail system prompt source=%s\n",
                __func__,
                s.resolved_system_prompt_path.empty()
                    ? "<builtin>"
                    : s.resolved_system_prompt_path.string().c_str());

        llama_set_embeddings(s.init->context(), true);

        s.negative_embeddings.clear();
        s.negative_embeddings.reserve(s.samples.negative_examples.size());
        std::vector<float> sample_embedding;
        std::string embed_error;

        for (const auto & sample : s.samples.negative_examples) {
            const auto formatted = common_prompt_guardrail_embedding_document_text(s.samples, sample);
            if (!embed_text_locked(s, formatted, &sample_embedding, &embed_error)) {
                throw std::runtime_error(std::string("failed to embed negative prompt sample: ") + embed_error);
            }
            s.negative_embeddings.push_back(sample_embedding);
        }

        s.backend = COMMON_PROMPT_GUARDRAIL_BACKEND_SAMPLE_SIMILARITY;
        s.load_ok = true;
        s.error.clear();

        LOG_INF("%s: selected backend=sample-similarity\n", __func__);
        LOG_INF("%s: loaded %zu positive and %zu negative samples\n",
                __func__,
                s.samples.positive_examples.size(),
                s.samples.negative_examples.size());

        if (error) {
            error->clear();
        }

        return true;
    } catch (const std::exception & e) {
        s.error = e.what();
        s.load_ok = false;
        s.backend = COMMON_PROMPT_GUARDRAIL_BACKEND_NONE;
        s.init.reset();
        s.negative_embeddings.clear();

        if (error) {
            *error = s.error;
        }

        LOG_ERR("%s\n", s.error.c_str());
        return false;
    }
}

static void set_route_probabilities(
        common_prompt_guardrail_result & result,
        common_prompt_guardrail_route route) {
    result.tech_probability = route == COMMON_PROMPT_GUARDRAIL_ROUTE_TECH ? 1.0f : 0.0f;
    result.nontech_probability = route == COMMON_PROMPT_GUARDRAIL_ROUTE_NONTECH ? 1.0f : 0.0f;
    result.unsure_probability = route == COMMON_PROMPT_GUARDRAIL_ROUTE_UNSURE ? 1.0f : 0.0f;
}

static common_prompt_guardrail_result classify_locked(
        prompt_guardrail_state & s,
        const std::vector<common_chat_msg> & messages) {
    refresh_locked(s);

    common_prompt_guardrail_result result;
    result.enabled = s.enabled;

    if (!s.enabled) {
        set_route_probabilities(result, result.route);
        return result;
    }

    std::string load_error;
    if (!load_backend_locked(s, &load_error)) {
        if (!load_error.empty()) {
            LOG_WRN("%s: prompt guardrail initialization failed: %s\n", __func__, load_error.c_str());
        }
        result.loaded = false;
        result.route = COMMON_PROMPT_GUARDRAIL_ROUTE_UNSURE;
        result.refuse = true;
        set_route_probabilities(result, result.route);
        return result;
    }

    result.loaded = true;

    if (s.backend != COMMON_PROMPT_GUARDRAIL_BACKEND_SAMPLE_SIMILARITY) {
        result.route = COMMON_PROMPT_GUARDRAIL_ROUTE_UNSURE;
        result.refuse = true;
        set_route_probabilities(result, result.route);
        return result;
    }

    const auto latest_user_text = common_prompt_guardrail_latest_user_text(messages);
    if (latest_user_text.empty()) {
        LOG_WRN("%s: prompt guardrail received an empty latest user prompt, refusing\n", __func__);
        result.route = COMMON_PROMPT_GUARDRAIL_ROUTE_UNSURE;
        result.refuse = true;
        set_route_probabilities(result, result.route);
        return result;
    }

    const auto user_query = common_prompt_guardrail_embedding_query_text(s.samples, latest_user_text);
    std::vector<float> query_embedding;
    std::string embed_error;
    if (!embed_text_locked(s, user_query, &query_embedding, &embed_error)) {
        LOG_WRN("%s: prompt guardrail query embedding failed: %s\n", __func__, embed_error.c_str());
        result.route = COMMON_PROMPT_GUARDRAIL_ROUTE_UNSURE;
        result.refuse = true;
        set_route_probabilities(result, result.route);
        return result;
    }

    const float max_similarity = common_prompt_guardrail_embedding_max_negative_similarity(
        s.negative_embeddings,
        query_embedding);
    const auto route = common_prompt_guardrail_embedding_route_from_negative_similarity(
        max_similarity,
        s.samples.negative_similarity_threshold);

    result.route = route;
    result.refuse = route != COMMON_PROMPT_GUARDRAIL_ROUTE_TECH;
    result.signed_score = s.samples.negative_similarity_threshold - max_similarity;
    result.dead_zone = 0.0f;
    set_route_probabilities(result, route);

    LOG_INF("%s: route=%s max_similarity=%.3f threshold=%.2f\n",
            __func__,
            common_prompt_guardrail_route_name(route),
            max_similarity,
            s.samples.negative_similarity_threshold);

    return result;
}

} // namespace

void common_prompt_guardrail_set_model_root(const std::string & model_root) {
    auto & s = state();
    std::lock_guard<std::mutex> lock(s.mutex);

    if (s.model_path != model_root || !s.samples_path.empty()) {
        reset_loaded_state(s);
    }

    s.model_path = model_root;
    s.samples_path.clear();
    s.enabled = !s.model_path.empty();
}

bool common_prompt_guardrail_has_assets(const std::string & model_root) {
    if (model_root.empty()) {
        return false;
    }

    const fs_path resolved_model_path = resolve_model_path(model_root);
    if (resolved_model_path.empty()) {
        return false;
    }

    const fs_path resolved_samples_path = resolve_samples_path("", resolved_model_path);
    return !resolved_samples_path.empty();
}

bool common_prompt_guardrail_enabled() {
    auto & s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    refresh_locked(s);
    return s.enabled;
}

bool common_prompt_guardrail_require_ready(std::string * error) {
    auto & s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    refresh_locked(s);

    if (!s.enabled) {
        LOG_INF("%s: prompt guardrail mode=disabled\n", __func__);
        if (error) {
            error->clear();
        }
        return true;
    }

    return load_backend_locked(s, error);
}

const char * common_prompt_guardrail_route_name(common_prompt_guardrail_route route) {
    switch (route) {
        case COMMON_PROMPT_GUARDRAIL_ROUTE_TECH:
            return "tech";
        case COMMON_PROMPT_GUARDRAIL_ROUTE_NONTECH:
            return "nontech";
        case COMMON_PROMPT_GUARDRAIL_ROUTE_UNSURE:
            return "unsure";
    }

    return "unknown";
}

const char * common_prompt_guardrail_system_prompt(common_prompt_guardrail_route route) {
    switch (route) {
        case COMMON_PROMPT_GUARDRAIL_ROUTE_TECH:
            break;
        case COMMON_PROMPT_GUARDRAIL_ROUTE_NONTECH:
        case COMMON_PROMPT_GUARDRAIL_ROUTE_UNSURE:
            return k_prompt_guardrail_refusal_prompt;
    }

    auto & s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    refresh_locked(s);

    static thread_local std::string tech_prompt;
    tech_prompt = s.forced_system_prompt.empty()
        ? LLAMA_CPP_FORCED_SYSTEM_PROMPT
        : s.forced_system_prompt;
    return tech_prompt.c_str();
}

common_prompt_guardrail_result common_prompt_guardrail_classify(
        const std::vector<common_chat_msg> & messages) {
    auto & s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return classify_locked(s, messages);
}
