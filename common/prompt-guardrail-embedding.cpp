#include "prompt-guardrail-embedding.h"

#include "common.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::ordered_json;

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static std::string trim_copy(std::string_view text) {
    const auto start = text.find_first_not_of(" \t\n\r");
    if (start == std::string_view::npos) {
        return {};
    }

    const auto end = text.find_last_not_of(" \t\n\r");
    return std::string(text.substr(start, end - start + 1));
}

static std::string format_embedding_text(
        const common_prompt_guardrail_embedding_samples & samples,
        std::string_view tag,
        std::string_view text) {
    std::string formatted;
    formatted.reserve(samples.instruction.size() + text.size() + tag.size() + 48);
    formatted.append("<Instruct>: ");
    formatted.append(samples.instruction);
    formatted.append("\n\n");
    formatted.append(tag.data(), tag.size());
    formatted.append(": ");
    formatted.append(text.data(), text.size());
    return formatted;
}

static std::string read_text_file(const std::string & path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("failed to open prompt guardrail samples file: " + path);
    }

    std::ostringstream oss;
    oss << file.rdbuf();
    return oss.str();
}

static std::optional<std::string> find_string_field(
        const json & root,
        std::initializer_list<const char *> keys) {
    for (const char * key : keys) {
        if (!key || !root.contains(key)) {
            continue;
        }
        const auto & value = root.at(key);
        if (value.is_string()) {
            return value.get<std::string>();
        }
    }
    return std::nullopt;
}

static bool load_string_array_field(
        const json & root,
        std::initializer_list<const char *> keys,
        std::vector<std::string> * out,
        std::string * error) {
    for (const char * key : keys) {
        if (!key || !root.contains(key)) {
            continue;
        }

        const auto & value = root.at(key);
        if (!value.is_array()) {
            if (error) {
                *error = std::string("prompt guardrail samples field must be an array: ") + key;
            }
            return false;
        }

        for (const auto & elem : value) {
            if (!elem.is_string()) {
                if (error) {
                    *error = std::string("prompt guardrail samples field must contain strings: ") + key;
                }
                return false;
            }

            const auto item = trim_copy(elem.get<std::string>());
            if (!item.empty()) {
                out->push_back(item);
            }
        }

        return true;
    }

    if (error) {
        *error = "prompt guardrail samples file is missing a required array field";
    }
    return false;
}

static bool load_float_field(
        const json & root,
        std::initializer_list<const char *> keys,
        float * out,
        std::string * error) {
    for (const char * key : keys) {
        if (!key || !root.contains(key)) {
            continue;
        }

        const auto & value = root.at(key);
        if (!value.is_number()) {
            if (error) {
                *error = std::string("prompt guardrail samples field must be a number: ") + key;
            }
            return false;
        }

        const float parsed = value.get<float>();
        if (!std::isfinite(parsed) || parsed < 0.0f || parsed > 1.0f) {
            if (error) {
                *error = std::string("prompt guardrail samples field must be a finite value between 0 and 1: ") + key;
            }
            return false;
        }

        *out = parsed;
        return true;
    }

    return true;
}

} // namespace

bool common_prompt_guardrail_load_embedding_samples(
        const std::string & path,
        common_prompt_guardrail_embedding_samples * samples,
        std::string * error) {
    if (!samples) {
        if (error) {
            *error = "prompt guardrail samples output is null";
        }
        return false;
    }

    try {
        samples->negative_similarity_threshold = 0.90f;
        const std::string text = read_text_file(path);
        json root = json::parse(text);
        if (!root.is_object()) {
            if (error) {
                *error = "prompt guardrail samples file must contain a JSON object";
            }
            return false;
        }

        const auto instruction = find_string_field(root, {"instruction", "query_instruction"});
        if (!instruction || trim_copy(*instruction).empty()) {
            if (error) {
                *error = "prompt guardrail samples file is missing a non-empty instruction field";
            }
            return false;
        }

        std::vector<std::string> positive_examples;
        std::vector<std::string> negative_examples;
        if (!load_string_array_field(root, {"positive_examples", "positive"}, &positive_examples, error)) {
            return false;
        }
        if (!load_string_array_field(root, {"negative_examples", "negative"}, &negative_examples, error)) {
            return false;
        }
        if (!load_float_field(root, {"negative_similarity_threshold", "negative_threshold"}, &samples->negative_similarity_threshold, error)) {
            return false;
        }

        if (positive_examples.empty()) {
            if (error) {
                *error = "prompt guardrail samples file must contain at least one positive example";
            }
            return false;
        }
        if (negative_examples.empty()) {
            if (error) {
                *error = "prompt guardrail samples file must contain at least one negative example";
            }
            return false;
        }

        samples->instruction = trim_copy(*instruction);
        samples->positive_examples = std::move(positive_examples);
        samples->negative_examples = std::move(negative_examples);

        if (error) {
            error->clear();
        }
        return true;
    } catch (const std::exception & e) {
        if (error) {
            *error = std::string("failed to parse prompt guardrail samples file: ") + e.what();
        }
        return false;
    }
}

std::string common_prompt_guardrail_embedding_query_text(
        const common_prompt_guardrail_embedding_samples & samples,
        std::string_view user_text) {
    return format_embedding_text(samples, "<Query>", user_text);
}

std::string common_prompt_guardrail_embedding_document_text(
        const common_prompt_guardrail_embedding_samples & samples,
        std::string_view document_text) {
    return format_embedding_text(samples, "<Document>", document_text);
}

float common_prompt_guardrail_embedding_max_negative_similarity(
        const std::vector<std::vector<float>> & negative_embeddings,
        const std::vector<float> & query_embedding) {
    float max_similarity = -1.0f;

    for (const auto & negative_embedding : negative_embeddings) {
        const int n = static_cast<int>(std::min(negative_embedding.size(), query_embedding.size()));
        if (n <= 0) {
            continue;
        }

        const float similarity = common_embd_similarity_cos(
            query_embedding.data(),
            negative_embedding.data(),
            n);
        max_similarity = std::max(max_similarity, similarity);
    }

    return max_similarity;
}

common_prompt_guardrail_route common_prompt_guardrail_embedding_route_from_negative_similarity(
        float max_similarity,
        float threshold) {
    return max_similarity >= threshold
        ? COMMON_PROMPT_GUARDRAIL_ROUTE_NONTECH
        : COMMON_PROMPT_GUARDRAIL_ROUTE_TECH;
}

common_prompt_guardrail_route common_prompt_guardrail_embedding_route_from_scores(
        float tech_similarity,
        float nontech_similarity,
        float threshold,
        float margin) {
    if (tech_similarity >= threshold && tech_similarity >= nontech_similarity + margin) {
        return COMMON_PROMPT_GUARDRAIL_ROUTE_TECH;
    }

    if (nontech_similarity >= threshold && nontech_similarity >= tech_similarity + margin) {
        return COMMON_PROMPT_GUARDRAIL_ROUTE_NONTECH;
    }

    return COMMON_PROMPT_GUARDRAIL_ROUTE_UNSURE;
}
