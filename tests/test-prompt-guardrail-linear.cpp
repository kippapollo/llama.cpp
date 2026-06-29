#include "prompt-guardrail-linear.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

static common_prompt_guardrail_linear_options make_options() {
    common_prompt_guardrail_linear_options options;
    options.epochs = 128;
    options.learning_rate = 0.05f;
    options.l2_regularization = 0.001f;
    options.dead_zone = 0.10f;
    return options;
}

static std::vector<common_prompt_guardrail_linear_sample> make_samples() {
    return {
        { { 2.0f,  1.0f }, true },
        { { 3.0f,  2.0f }, true },
        { { 1.0f,  3.0f }, true },
        { { -2.0f, -1.0f }, false },
        { { -3.0f, -2.0f }, false },
        { { -1.0f, -3.0f }, false },
    };
}

static void require(bool condition, const std::string & message) {
    if (!condition) {
        std::cerr << "Test failed: " << message << "\n";
        std::exit(1);
    }
}

static void require_close(float a, float b, const std::string & message) {
    require(std::fabs(a - b) < 1e-6f, message);
}

static void test_train_is_deterministic() {
    const auto samples = make_samples();
    const auto options = make_options();

    common_prompt_guardrail_linear_model model_a;
    common_prompt_guardrail_linear_model model_b;
    std::string error;

    require(common_prompt_guardrail_linear_train(samples, options, &model_a, &error),
            "first training run should succeed");
    require(error.empty(), "first training run should not report an error");
    require(common_prompt_guardrail_linear_train(samples, options, &model_b, &error),
            "second training run should succeed");
    require(error.empty(), "second training run should not report an error");

    require(model_a.weights.size() == model_b.weights.size(), "trained models should use the same feature size");
    for (size_t i = 0; i < model_a.weights.size(); ++i) {
        require_close(model_a.weights[i], model_b.weights[i], "trained model weights should be deterministic");
    }
    require_close(model_a.bias, model_b.bias, "trained model bias should be deterministic");
    require_close(model_a.dead_zone, options.dead_zone, "dead zone should be copied into the model");

    const std::vector<float> tech_features { 4.0f, 3.0f };
    const std::vector<float> nontech_features { -4.0f, -3.0f };
    require(common_prompt_guardrail_linear_classify(model_a, tech_features) == COMMON_PROMPT_GUARDRAIL_ROUTE_TECH,
            "positive features should classify as tech");
    require(common_prompt_guardrail_linear_classify(model_a, nontech_features) == COMMON_PROMPT_GUARDRAIL_ROUTE_NONTECH,
            "negative features should classify as nontech");
}

static void test_route_from_score() {
    require(common_prompt_guardrail_linear_route_from_score(0.15f, 0.10f)
        == COMMON_PROMPT_GUARDRAIL_ROUTE_TECH, "positive score should map to tech");
    require(common_prompt_guardrail_linear_route_from_score(-0.15f, 0.10f)
        == COMMON_PROMPT_GUARDRAIL_ROUTE_NONTECH, "negative score should map to nontech");
    require(common_prompt_guardrail_linear_route_from_score(0.03f, 0.10f)
        == COMMON_PROMPT_GUARDRAIL_ROUTE_UNSURE, "small positive score should map to unsure");
    require(common_prompt_guardrail_linear_route_from_score(-0.03f, 0.10f)
        == COMMON_PROMPT_GUARDRAIL_ROUTE_UNSURE, "small negative score should map to unsure");
}

static void test_training_requires_data() {
    common_prompt_guardrail_linear_model model;
    std::string error;
    const auto options = make_options();

    require(!common_prompt_guardrail_linear_train({}, options, &model, &error),
            "empty training data should fail");
    require(!error.empty(), "empty training data should report an error");
}

static void test_training_requires_both_labels() {
    common_prompt_guardrail_linear_model model;
    std::string error;
    const auto options = make_options();

    const std::vector<common_prompt_guardrail_linear_sample> samples = {
        { { 1.0f, 1.0f }, true },
        { { 2.0f, 1.0f }, true },
    };

    require(!common_prompt_guardrail_linear_train(samples, options, &model, &error),
            "single-class training data should fail");
    require(!error.empty(), "single-class training data should report an error");
}

} // namespace

int main() {
    test_train_is_deterministic();
    test_route_from_score();
    test_training_requires_data();
    test_training_requires_both_labels();
    std::cout << "OK\n";
    return 0;
}
