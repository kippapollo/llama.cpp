#pragma once

#include "chat.h"

#include <functional>
#include <string>
#include <string_view>

using output_guardrail_redactor = std::function<std::string(const std::string &)>;

// Ensure the native guardrail backend is available and initialized.
// Returns false and populates `error` when the local ONNX Runtime backend or
// required model assets are missing.
bool output_guardrail_require_backend(std::string * error = nullptr);

// Set the local model root directory used to resolve ONNX guardrail assets.
// Passing an empty string disables the guardrail.
void output_guardrail_set_model_root(const std::string & model_root);

// Return true when the output guardrail has been explicitly configured.
bool output_guardrail_is_configured();

// Return true when the optional local output-guardrail assets can be resolved.
bool output_guardrail_has_assets(const std::string & model_root);

// Return a UTF-8 safe prefix length that keeps a trailing overlap.
size_t output_guardrail_safe_prefix_len(std::string_view text, size_t holdback_bytes);

// Redact a full string using the configured backend. If the guardrail is not
// available, this throws.
std::string output_guardrail_redact_text(const std::string & text);

// Return true for NER labels that should be redacted by policy.
bool output_guardrail_label_is_redactable(const std::string & label);

// Stateful streaming guardrail. The default constructor uses the configured
// backend when one is explicitly available; otherwise it behaves as a no-op.
class output_guardrail {
public:
    output_guardrail();
    explicit output_guardrail(output_guardrail_redactor redactor, size_t buffer_bytes = 1024);

    // Feed a new chunk and return the redacted prefix that can be emitted now.
    std::string feed(const std::string & text);

    // Flush the remaining buffered suffix.
    std::string flush();

    void reset();

    bool enabled() const { return enabled_; }

private:
    output_guardrail_redactor redactor_;
    std::function<std::string(const std::string &, size_t)> window_redactor_;
    std::string pending_;
    size_t buffer_bytes_ = 1024;
    size_t overlap_bytes_ = 128;
    bool enabled_ = false;
};

inline void output_guardrail_redact_assistant_content(
        std::string & content,
        common_chat_msg & msg,
        const output_guardrail_redactor & redactor) {
    if (!redactor) {
        return;
    }

    content = redactor(content);
    msg.content = redactor(msg.content);
    msg.reasoning_content = redactor(msg.reasoning_content);
}
