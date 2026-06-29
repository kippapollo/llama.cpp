// Tests the streaming guardrail helper.

#include "common.h"
#include "output-guardrail.h"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

template <class T>
static void assert_equals(const T & expected, const T & actual) {
    if (expected != actual) {
        std::cerr << "Expected: " << expected << std::endl;
        std::cerr << "  Actual: " << actual << std::endl;
        throw std::runtime_error("Test failed");
    }
}

int main() {
    try {
        {
            const std::string text = std::string("caf") + static_cast<char>(0xC3) + static_cast<char>(0xA9) + " ";
            assert_equals<size_t>(5, output_guardrail_safe_prefix_len(text, 1));
        }

        {
            output_guardrail redactor(
                [](const std::string & text) {
                    return text;
                },
                1024
            );

            assert_equals(std::string(), redactor.feed(std::string(1023, 'a')));
            assert_equals(std::string(896, 'a'), redactor.feed("b"));
            assert_equals(std::string(127, 'a') + "b", redactor.flush());
        }

        {
            const std::string url1 = "https://www.google.com";
            const std::string url2 = "https://www.youtube.com";

            std::string input;
            input.reserve(1024);
            input.append(780, 'a');
            input += url1;
            input.append(93, 'b');
            input += url2;
            input.append(1024 - input.size(), 'c');

            assert_equals<size_t>(1024, input.size());

            output_guardrail redactor(
                [](const std::string & text) {
                    std::string out = text;
                    string_replace_all(out, "https://www.google.com", "[REDACTED]");
                    string_replace_all(out, "https://www.youtube.com", "[REDACTED]");
                    return out;
                },
                1024
            );

            std::string expected = input.substr(0, 896);
            string_replace_all(expected, url1, "[REDACTED]");

            assert_equals(expected, redactor.feed(input));
            assert_equals(input.substr(896), redactor.flush());
        }

        {
            assert_equals(true, output_guardrail_label_is_redactable("B-PER"));
            assert_equals(true, output_guardrail_label_is_redactable("I-ORG"));
            assert_equals(true, output_guardrail_label_is_redactable("B-LOC"));
            assert_equals(false, output_guardrail_label_is_redactable("B-MISC"));
            assert_equals(false, output_guardrail_label_is_redactable("I-MISC"));
            assert_equals(false, output_guardrail_label_is_redactable("O"));
        }

        {
            std::string content = "Alice visited Paris and https://example.com";
            common_chat_msg msg;
            msg.content = "Alice visited Paris";
            msg.reasoning_content = "Alice thought about Paris";
            msg.tool_calls.push_back({
                "lookup",
                "{\"name\":\"Alice\",\"url\":\"https://example.com\"}",
                "call_1",
            });

            output_guardrail_redact_assistant_content(
                content,
                msg,
                [](const std::string & text) {
                    std::string out = text;
                    string_replace_all(out, "Alice", "[REDACTED]");
                    string_replace_all(out, "Paris", "[REDACTED]");
                    string_replace_all(out, "https://example.com", "[REDACTED]");
                    return out;
                }
            );

            assert_equals(std::string("[REDACTED] visited [REDACTED] and [REDACTED]"), content);
            assert_equals(std::string("[REDACTED] visited [REDACTED]"), msg.content);
            assert_equals(std::string("[REDACTED] thought about [REDACTED]"), msg.reasoning_content);
            assert_equals(std::string("{\"name\":\"Alice\",\"url\":\"https://example.com\"}"), msg.tool_calls[0].arguments);
        }

        {
            struct async_result {
                std::mutex mutex;
                std::condition_variable cv;
                bool done = false;
                bool ok = false;
                std::string error;
            };

            auto result = std::make_shared<async_result>();
            output_guardrail_set_model_root("__definitely_missing_guardrail_root__");

            std::thread([result]() {
                std::string error;
                const bool ok = output_guardrail_require_backend(&error);

                {
                    std::lock_guard<std::mutex> lock(result->mutex);
                    result->done = true;
                    result->ok = ok;
                    result->error = std::move(error);
                }
                result->cv.notify_one();
            }).detach();

            std::unique_lock<std::mutex> lock(result->mutex);
            if (!result->cv.wait_for(lock, std::chrono::seconds(3), [result]() { return result->done; })) {
                throw std::runtime_error("output_guardrail_require_backend did not return");
            }
            lock.unlock();

            output_guardrail_set_model_root("");
            assert_equals(false, result->ok);
            if (result->error.empty()) {
                throw std::runtime_error("Expected output_guardrail_require_backend to report an error");
            }
        }

        return 0;
    } catch (const std::exception & e) {
        std::cerr << "Test failed: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Test failed: unknown exception" << std::endl;
        return 1;
    }
}
