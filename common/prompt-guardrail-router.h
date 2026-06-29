#ifndef LLAMA_CPP_PROMPT_GUARDRAIL_ROUTER_H
#define LLAMA_CPP_PROMPT_GUARDRAIL_ROUTER_H

#include "chat.h"
#include "prompt-guardrail.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct llama_vocab;

std::string common_prompt_guardrail_message_text(const common_chat_msg & msg);

std::string common_prompt_guardrail_latest_user_text(const std::vector<common_chat_msg> & messages);

std::string common_prompt_guardrail_router_prompt(
    const std::vector<common_chat_msg> & messages,
    int token_budget,
    const std::function<int(std::string_view)> & token_counter);

std::string common_prompt_guardrail_router_prompt(
    const std::vector<common_chat_msg> & messages,
    int token_budget,
    const llama_vocab * vocab);

const char * common_prompt_guardrail_router_grammar();

common_prompt_guardrail_route common_prompt_guardrail_parse_route(std::string_view text);

#endif // LLAMA_CPP_PROMPT_GUARDRAIL_ROUTER_H
