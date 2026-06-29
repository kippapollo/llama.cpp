#ifndef LLAMA_CPP_PROMPT_GUARDRAIL_LINEAR_H
#define LLAMA_CPP_PROMPT_GUARDRAIL_LINEAR_H

#include "prompt-guardrail.h"

#include <string>
#include <vector>

struct common_prompt_guardrail_linear_sample {
    std::vector<float> features;
    bool positive = false;
};

struct common_prompt_guardrail_linear_options {
    int   epochs = 64;
    float learning_rate = 0.05f;
    float l2_regularization = 0.001f;
    float dead_zone = 0.05f;
};

struct common_prompt_guardrail_linear_model {
    std::vector<float> weights;
    float bias = 0.0f;
    float dead_zone = 0.05f;
};

bool common_prompt_guardrail_linear_train(
        const std::vector<common_prompt_guardrail_linear_sample> & samples,
        const common_prompt_guardrail_linear_options & options,
        common_prompt_guardrail_linear_model * model,
        std::string * error = nullptr);

float common_prompt_guardrail_linear_score(
        const common_prompt_guardrail_linear_model & model,
        const std::vector<float> & features);

common_prompt_guardrail_route common_prompt_guardrail_linear_route_from_score(
        float score,
        float dead_zone);

common_prompt_guardrail_route common_prompt_guardrail_linear_classify(
        const common_prompt_guardrail_linear_model & model,
        const std::vector<float> & features);

#endif // LLAMA_CPP_PROMPT_GUARDRAIL_LINEAR_H
