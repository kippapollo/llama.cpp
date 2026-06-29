#ifndef LLAMA_CPP_PROMPT_GUARDRAIL_EMBEDDING_H
#define LLAMA_CPP_PROMPT_GUARDRAIL_EMBEDDING_H

#include "prompt-guardrail.h"

#include <string>
#include <string_view>
#include <vector>

struct common_prompt_guardrail_embedding_samples {
    std::string instruction;
    float negative_similarity_threshold = 0.90f;
    std::vector<std::string> positive_examples;
    std::vector<std::string> negative_examples;
};

bool common_prompt_guardrail_load_embedding_samples(
    const std::string & path,
    common_prompt_guardrail_embedding_samples * samples,
    std::string * error = nullptr);

std::string common_prompt_guardrail_embedding_query_text(
    const common_prompt_guardrail_embedding_samples & samples,
    std::string_view user_text);

std::string common_prompt_guardrail_embedding_document_text(
    const common_prompt_guardrail_embedding_samples & samples,
    std::string_view document_text);

float common_prompt_guardrail_embedding_max_negative_similarity(
    const std::vector<std::vector<float>> & negative_embeddings,
    const std::vector<float> & query_embedding);

common_prompt_guardrail_route common_prompt_guardrail_embedding_route_from_negative_similarity(
    float max_similarity,
    float threshold);

common_prompt_guardrail_route common_prompt_guardrail_embedding_route_from_scores(
    float tech_similarity,
    float nontech_similarity,
    float threshold,
    float margin);

#endif // LLAMA_CPP_PROMPT_GUARDRAIL_EMBEDDING_H
