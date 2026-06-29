#include "prompt-guardrail-router.h"

#include <cassert>
#include <string>
#include <string_view>
#include <vector>

static common_chat_msg make_msg(const char * role, const char * content) {
    common_chat_msg msg;
    msg.role = role;
    msg.content = content;
    return msg;
}

static int count_chars(std::string_view text) {
    return static_cast<int>(text.size());
}

static void test_message_text_serializes_parts_reasoning_and_tools() {
    common_chat_msg msg;
    msg.content_parts = {
        {"text", "alpha"},
        {"image", "ignored"},
        {"text", "beta"},
    };
    msg.reasoning_content = "because";
    msg.tool_calls = {
        {"calc", "{\"x\":1}", "tool-1"},
    };

    const auto text = common_prompt_guardrail_message_text(msg);

    assert(text.find("alpha") != std::string::npos);
    assert(text.find("beta") != std::string::npos);
    assert(text.find("[reasoning] because") != std::string::npos);
    assert(text.find("[tool_calls] calc#tool-1({\"x\":1})") != std::string::npos);
}

static void test_empty_history_returns_empty_prompt() {
    std::vector<common_chat_msg> messages;

    const auto prompt = common_prompt_guardrail_router_prompt(messages, 1000, count_chars);

    assert(prompt.empty());
}

static void test_prompt_includes_instruction_and_labels() {
    std::vector<common_chat_msg> messages = {
        make_msg("system", "You are a router."),
        make_msg("user", "How do I write a unit test?"),
        make_msg("assistant", "Use assertions."),
        make_msg("tool", "lint"),
    };

    const auto prompt = common_prompt_guardrail_router_prompt(messages, 1000, count_chars);

    assert(prompt.find("Classify the conversation as TECH, NONTECH, or UNSURE. Output only one label.") != std::string::npos);
    assert(prompt.find("Use compact role tags: sys=system, usr=user, asst=assistant, tool=tool.") != std::string::npos);
    assert(prompt.find("sys: You are a router.") != std::string::npos);
    assert(prompt.find("usr: How do I write a unit test?") != std::string::npos);
    assert(prompt.find("asst: Use assertions.") != std::string::npos);
    assert(prompt.find("tool: lint") != std::string::npos);
    assert(prompt.find("system:") == std::string::npos);
    assert(prompt.find("assistant:") == std::string::npos);
}

static void test_prompt_truncates_oldest_turns() {
    std::vector<common_chat_msg> messages = {
        make_msg("system", "one"),
        make_msg("user", "two"),
        make_msg("assistant", "three"),
    };

    const std::string instruction =
        "Classify the conversation as TECH, NONTECH, or UNSURE. Output only one label.\n"
        "Use compact role tags: sys=system, usr=user, asst=assistant, tool=tool.";
    const std::string expected = instruction + "\n\nasst: three";
    const int budget = static_cast<int>(expected.size());

    const auto prompt = common_prompt_guardrail_router_prompt(messages, budget, count_chars);

    assert(prompt.find("asst: three") != std::string::npos);
    assert(prompt.find("sys: one") == std::string::npos);
    assert(prompt.find("usr: two") == std::string::npos);
}

static void test_parse_route_labels_case_insensitive() {
    assert(common_prompt_guardrail_parse_route("TECH") == COMMON_PROMPT_GUARDRAIL_ROUTE_TECH);
    assert(common_prompt_guardrail_parse_route("nontech") == COMMON_PROMPT_GUARDRAIL_ROUTE_NONTECH);
    assert(common_prompt_guardrail_parse_route("Unsure") == COMMON_PROMPT_GUARDRAIL_ROUTE_UNSURE);
}

static void test_parse_route_rejects_junk_and_partials() {
    assert(common_prompt_guardrail_parse_route("maybe") == COMMON_PROMPT_GUARDRAIL_ROUTE_UNSURE);
    assert(common_prompt_guardrail_parse_route("TECHNOLOGY") == COMMON_PROMPT_GUARDRAIL_ROUTE_UNSURE);
    assert(common_prompt_guardrail_parse_route("TECH please") == COMMON_PROMPT_GUARDRAIL_ROUTE_UNSURE);
}

int main() {
    test_message_text_serializes_parts_reasoning_and_tools();
    test_empty_history_returns_empty_prompt();
    test_prompt_includes_instruction_and_labels();
    test_prompt_truncates_oldest_turns();
    test_parse_route_labels_case_insensitive();
    test_parse_route_rejects_junk_and_partials();
    return 0;
}
