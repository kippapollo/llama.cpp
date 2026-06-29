#include "prompt-guardrail-embedding.h"
#include "prompt-guardrail.h"
#include "forced-system-prompt.h"

#include <cmath>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

std::filesystem::path make_temp_json_file(const std::string & text) {
    const auto dir = std::filesystem::temp_directory_path() / "llama_prompt_guardrail_tests";
    std::filesystem::create_directories(dir);

    const auto path = dir / "embedding-samples.json";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        std::cerr << "failed to create temp test file: " << path << "\n";
        std::exit(1);
    }
    file << text;
    return path;
}

void test_load_embedding_samples() {
    const auto path = make_temp_json_file(R"JSON(
{
  "instruction": "Determine whether the user's request is primarily about coding, programming, commands, algorithms, debugging, scripting, system implementation, or software development.",
  "negative_similarity_threshold": 0.87,
  "positive_examples": [
    "Write a Python script that parses JSON logs and adds pytest tests.",
    "Explain this stack trace and fix the bug in my C++ code."
  ],
  "negative_examples": [
    "Plan a two-day beach vacation with food recommendations.",
    "Write a polite birthday email to my manager."
  ]
}
)JSON");

    common_prompt_guardrail_embedding_samples samples;
    std::string error;
    const bool loaded = common_prompt_guardrail_load_embedding_samples(path.string(), &samples, &error);

    assert(loaded);
    (void) loaded;
    assert(error.empty());
    assert(samples.instruction.find("Determine whether") != std::string::npos);
    assert(samples.positive_examples.size() == 2);
    assert(samples.negative_examples.size() == 2);
    if (std::fabs(samples.negative_similarity_threshold - 0.87f) > 1e-6f) {
        std::cerr << "unexpected negative_similarity_threshold: " << samples.negative_similarity_threshold << "\n";
        std::exit(1);
    }
}

void test_embedding_query_text() {
    common_prompt_guardrail_embedding_samples samples;
    samples.instruction = "Classify whether the input is about coding.";

    const auto text = common_prompt_guardrail_embedding_query_text(samples, "Write a Rust unit test.");
    const std::string expected =
        "<Instruct>: Classify whether the input is about coding.\n\n"
        "<Query>: Write a Rust unit test.";

    assert(text == expected);
}

void test_embedding_document_text() {
    common_prompt_guardrail_embedding_samples samples;
    samples.instruction = "Classify whether the input is about coding.";

    const auto text = common_prompt_guardrail_embedding_document_text(samples, "Write a Rust unit test.");
    const std::string expected =
        "<Instruct>: Classify whether the input is about coding.\n\n"
        "<Document>: Write a Rust unit test.";

    assert(text == expected);
}

void test_route_from_negative_similarity() {
    const std::vector<std::vector<float>> negative_embeddings = {
        { 1.0f, 0.0f },
        { 0.0f, 1.0f },
    };
    const std::vector<float> query_embedding = { 0.10f, 0.99f };

    const float max_similarity = common_prompt_guardrail_embedding_max_negative_similarity(
        negative_embeddings,
        query_embedding);

    if (!(max_similarity > 0.99f)) {
        std::cerr << "expected nearest negative similarity above 0.99, got " << max_similarity << "\n";
        std::exit(1);
    }

    if (common_prompt_guardrail_embedding_route_from_negative_similarity(max_similarity, 0.90f)
            != COMMON_PROMPT_GUARDRAIL_ROUTE_NONTECH) {
        std::cerr << "expected refusal when the prompt is near a negative sample\n";
        std::exit(1);
    }

    if (common_prompt_guardrail_embedding_route_from_negative_similarity(0.89f, 0.90f)
            != COMMON_PROMPT_GUARDRAIL_ROUTE_TECH) {
        std::cerr << "expected allow when the prompt is below the threshold\n";
        std::exit(1);
    }
}

void test_route_prompts() {
    assert(std::string(common_prompt_guardrail_route_name(COMMON_PROMPT_GUARDRAIL_ROUTE_TECH)) == "tech");
    assert(std::string(common_prompt_guardrail_route_name(COMMON_PROMPT_GUARDRAIL_ROUTE_NONTECH)) == "nontech");
    assert(std::string(common_prompt_guardrail_route_name(COMMON_PROMPT_GUARDRAIL_ROUTE_UNSURE)) == "unsure");

    assert(std::string(common_prompt_guardrail_system_prompt(COMMON_PROMPT_GUARDRAIL_ROUTE_TECH))
        == std::string(LLAMA_CPP_FORCED_SYSTEM_PROMPT));
    assert(std::string(common_prompt_guardrail_system_prompt(COMMON_PROMPT_GUARDRAIL_ROUTE_NONTECH))
        == std::string(common_prompt_guardrail_system_prompt(COMMON_PROMPT_GUARDRAIL_ROUTE_UNSURE)));
}

void test_system_prompt_file_matches_fallback() {
    const auto path = std::filesystem::path("models/guardrail/system-prompt.txt");
    std::ifstream file(path, std::ios::binary);
    assert(file);

    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }

    assert(text == LLAMA_CPP_FORCED_SYSTEM_PROMPT);
}

} // namespace

int main() {
    test_load_embedding_samples();
    test_embedding_query_text();
    test_embedding_document_text();
    test_route_from_negative_similarity();
    test_route_prompts();
    test_system_prompt_file_matches_fallback();
    std::cout << "OK\n";
    return 0;
}
