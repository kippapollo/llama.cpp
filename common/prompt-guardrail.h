#ifndef LLAMA_CPP_PROMPT_GUARDRAIL_H
#define LLAMA_CPP_PROMPT_GUARDRAIL_H

#include "chat.h"

#include <string>
#include <vector>

enum common_prompt_guardrail_route {
    COMMON_PROMPT_GUARDRAIL_ROUTE_TECH = 0,
    COMMON_PROMPT_GUARDRAIL_ROUTE_NONTECH = 1,
    COMMON_PROMPT_GUARDRAIL_ROUTE_UNSURE = 2,
};

enum common_prompt_guardrail_backend {
    COMMON_PROMPT_GUARDRAIL_BACKEND_NONE = 0,
    COMMON_PROMPT_GUARDRAIL_BACKEND_EMBEDDING = 1,
    COMMON_PROMPT_GUARDRAIL_BACKEND_SAMPLE_SIMILARITY = 2,
};

struct common_prompt_guardrail_result {
    common_prompt_guardrail_route route = COMMON_PROMPT_GUARDRAIL_ROUTE_TECH;
    float tech_probability = 1.0f;
    float nontech_probability = 0.0f;
    float unsure_probability = 0.0f;
    float signed_score = 0.0f;
    float dead_zone = 0.0f;
    bool refuse = false;
    bool enabled = false;
    bool loaded = false;
};

bool common_prompt_guardrail_enabled();
void common_prompt_guardrail_set_model_root(const std::string & model_root);
bool common_prompt_guardrail_has_assets(const std::string & model_root);
bool common_prompt_guardrail_require_ready(std::string * error = nullptr);
const char * common_prompt_guardrail_route_name(common_prompt_guardrail_route route);
const char * common_prompt_guardrail_system_prompt(common_prompt_guardrail_route route);
common_prompt_guardrail_result common_prompt_guardrail_classify(const std::vector<common_chat_msg> & messages);

#endif // LLAMA_CPP_PROMPT_GUARDRAIL_H
