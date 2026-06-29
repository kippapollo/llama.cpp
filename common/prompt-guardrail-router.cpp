#include "prompt-guardrail-router.h"

#include "common.h"

#include <cctype>
#include <sstream>

std::string common_prompt_guardrail_message_text(const common_chat_msg & msg) {
    std::ostringstream ss;
    bool wrote = false;

    if (!msg.content.empty()) {
        ss << msg.content;
        wrote = true;
    } else {
        for (const auto & part : msg.content_parts) {
            if (part.type == "text" && !part.text.empty()) {
                if (wrote) {
                    ss << "\n";
                }
                ss << part.text;
                wrote = true;
            }
        }
    }

    if (!msg.reasoning_content.empty()) {
        if (wrote) {
            ss << "\n";
        }
        ss << "[reasoning] " << msg.reasoning_content;
        wrote = true;
    }

    if (!msg.tool_calls.empty()) {
        if (wrote) {
            ss << "\n";
        }
        ss << "[tool_calls]";
        for (const auto & tool_call : msg.tool_calls) {
            ss << " " << tool_call.name;
            if (!tool_call.id.empty()) {
                ss << "#" << tool_call.id;
            }
            if (!tool_call.arguments.empty()) {
                ss << "(" << tool_call.arguments << ")";
            }
        }
    }

    return ss.str();
}

namespace {

constexpr const char * k_router_instruction =
    "You are a classifier for a Software Development Agent.\n"
    "Classify the user's input into exactly one of these three categories: TECH, NONTECH, UNSURE.\n"
    "Output only the single label. Do not add punctuation, explanation, or extra text.\n"
    "\n"
    "### Category Definitions:\n"
    "1. TECH:\n"
    "   - The verb \"write\" alone does not imply TECH.\n"
    "   - Writing, debugging, or explaining code.\n"
    "   - Software architecture, algorithms, data structures.\n"
    "   - DevOps, CI/CD, databases, APIs, Git, servers.\n"
    "   - Error logs, stack traces, technical documentation.\n"
    "   - Installing/configuring dev tools (IDEs, compilers, SDKs).\n"
    "\n"
    "2. NONTECH:\n"
    "   - General knowledge (history, science, geography).\n"
    "   - Creative writing (stories, poems, emails) unless about code.\n"
    "   - Plain-language writing, lists, or descriptions about cities, countries, places, or other general topics.\n"
    "   - Non-code requests about countries, cities, locations, people, politics, or economy remain NONTECH even if they mention IT or software as context.\n"
    "   - Math problems without coding context.\n"
    "   - Personal advice, health, legal, financial topics.\n"
    "   - Non-technical software usage (e.g., \"How to use Excel\").\n"
    "\n"
    "3. UNSURE:\n"
    "   - Ambiguous input with no clear context.\n"
    "   - Mixed topics where intent is unclear.\n"
    "   - Input too short to determine context (e.g., \"Hello\", \"Help\").\n"
    "\n"
    "Examples:\n"
    "   - Write a Python script that parses JSON logs and add pytest tests. -> TECH\n"
    "   - I see, but I want to explain of IT, software skill of africa -> NONTECH\n"
    "   - write country list in europ -> NONTECH\n"
    "   - write big city in world -> NONTECH";

constexpr const char * k_router_grammar = R"GRAMMAR(
root ::= label
label ::= "TECH" | "NONTECH" | "UNSURE"
)GRAMMAR";

std::string to_lower(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (unsigned char c : input) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

void append_chatml_turn(std::string & prompt, std::string_view role, std::string_view content) {
    prompt.append("<|im_start|>");
    prompt.append(role);
    prompt.push_back('\n');
    prompt.append(content);
    prompt.append("<|im_end|>\n");
}

bool utf8_pop_back(std::string & text) {
    if (text.empty()) {
        return false;
    }

    size_t pos = text.size();
    do {
        --pos;
    } while (pos > 0 && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80);

    text.resize(pos);
    return true;
}

std::string compact_role_tag(std::string_view role) {
    if (role.empty()) {
        return "usr";
    }

    const auto lowered = to_lower(role);
    if (lowered == "system") {
        return "sys";
    }
    if (lowered == "user") {
        return "usr";
    }
    if (lowered == "assistant") {
        return "asst";
    }
    if (lowered == "tool") {
        return "tool";
    }
    return lowered;
}

std::string build_router_prompt(std::string_view user_text) {
    std::string prompt;
    prompt.reserve(std::string_view(k_router_instruction).size() + user_text.size() + 64);
    append_chatml_turn(prompt, "system", k_router_instruction);
    append_chatml_turn(prompt, "user", user_text);
    prompt.append("<|im_start|>assistant\n");
    return prompt;
}

} // namespace

std::string common_prompt_guardrail_latest_user_text(const std::vector<common_chat_msg> & messages) {
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (compact_role_tag(it->role) != "usr") {
            continue;
        }

        return common_prompt_guardrail_message_text(*it);
    }

    return {};
}

std::string common_prompt_guardrail_router_prompt(
    const std::vector<common_chat_msg> & messages,
    int token_budget,
    const std::function<int(std::string_view)> & token_counter) {
    const auto user_text = common_prompt_guardrail_latest_user_text(messages);
    if (user_text.empty()) {
        return {};
    }

    std::string prompt = build_router_prompt(user_text);

    if (!token_counter) {
        return prompt;
    }

    if (token_counter(prompt) <= token_budget) {
        return prompt;
    }

    std::string trimmed_user_text = user_text;
    while (utf8_pop_back(trimmed_user_text)) {
        prompt = build_router_prompt(trimmed_user_text);
        if (token_counter(prompt) <= token_budget) {
            return prompt;
        }
    }

    return build_router_prompt({});
}

std::string common_prompt_guardrail_router_prompt(
    const std::vector<common_chat_msg> & messages,
    int token_budget,
    const llama_vocab * vocab) {
    if (vocab == nullptr) {
        return common_prompt_guardrail_router_prompt(
            messages,
            token_budget,
            [](std::string_view /* text */) { return 0; });
    }

    return common_prompt_guardrail_router_prompt(
        messages,
        token_budget,
        [vocab](std::string_view text) {
            return static_cast<int>(common_tokenize(vocab, std::string(text), false, true).size());
        });
}

const char * common_prompt_guardrail_router_grammar() {
    return k_router_grammar;
}

common_prompt_guardrail_route common_prompt_guardrail_parse_route(std::string_view text) {
    const auto start = text.find_first_not_of(" \t\n\r");
    if (start == std::string_view::npos) {
        return COMMON_PROMPT_GUARDRAIL_ROUTE_UNSURE;
    }
    const auto end = text.find_last_not_of(" \t\n\r");
    const auto trimmed = text.substr(start, end - start + 1);
    const auto lowered = to_lower(trimmed);

    if (lowered == "tech") {
        return COMMON_PROMPT_GUARDRAIL_ROUTE_TECH;
    }
    if (lowered == "nontech") {
        return COMMON_PROMPT_GUARDRAIL_ROUTE_NONTECH;
    }
    if (lowered == "unsure") {
        return COMMON_PROMPT_GUARDRAIL_ROUTE_UNSURE;
    }

    return COMMON_PROMPT_GUARDRAIL_ROUTE_UNSURE;
}
