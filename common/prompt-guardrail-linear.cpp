#include "prompt-guardrail-linear.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {

static float dot_product(const std::vector<float> & a, const std::vector<float> & b) {
    const size_t n = std::min(a.size(), b.size());
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sum += static_cast<double>(a[i]) * static_cast<double>(b[i]);
    }
    return static_cast<float>(sum);
}

static bool validate_samples(
        const std::vector<common_prompt_guardrail_linear_sample> & samples,
        size_t * feature_count,
        std::string * error) {
    if (samples.empty()) {
        if (error) {
            *error = "prompt guardrail linear classifier requires at least one sample";
        }
        return false;
    }

    if (!feature_count) {
        if (error) {
            *error = "prompt guardrail linear classifier output feature count is null";
        }
        return false;
    }

    const size_t dim = samples.front().features.size();
    if (dim == 0) {
        if (error) {
            *error = "prompt guardrail linear classifier requires non-empty feature vectors";
        }
        return false;
    }

    bool saw_positive = false;
    bool saw_negative = false;
    for (size_t index = 0; index < samples.size(); ++index) {
        const auto & sample = samples[index];
        if (sample.features.size() != dim) {
            if (error) {
                *error = "prompt guardrail linear classifier samples must all have the same feature dimension";
            }
            return false;
        }

        if (sample.positive) {
            saw_positive = true;
        } else {
            saw_negative = true;
        }
    }

    if (!saw_positive || !saw_negative) {
        if (error) {
            *error = "prompt guardrail linear classifier requires both positive and negative samples";
        }
        return false;
    }

    *feature_count = dim;
    return true;
}

static float sanitize_dead_zone(float dead_zone) {
    return dead_zone > 0.0f ? dead_zone : 0.0f;
}

static float sanitize_learning_rate(float learning_rate) {
    if (learning_rate <= 0.0f || !std::isfinite(learning_rate)) {
        return 0.05f;
    }
    return learning_rate;
}

static float sanitize_regularization(float l2_regularization) {
    if (l2_regularization < 0.0f || !std::isfinite(l2_regularization)) {
        return 0.0f;
    }
    return l2_regularization;
}

} // namespace

bool common_prompt_guardrail_linear_train(
        const std::vector<common_prompt_guardrail_linear_sample> & samples,
        const common_prompt_guardrail_linear_options & options,
        common_prompt_guardrail_linear_model * model,
        std::string * error) {
    if (model == nullptr) {
        if (error) {
            *error = "prompt guardrail linear classifier output model is null";
        }
        return false;
    }

    size_t feature_count = 0;
    if (!validate_samples(samples, &feature_count, error)) {
        return false;
    }

    const float learning_rate = sanitize_learning_rate(options.learning_rate);
    const float l2_regularization = sanitize_regularization(options.l2_regularization);
    const int epochs = options.epochs > 0 ? options.epochs : 1;
    const float shrink = std::max(0.0f, 1.0f - learning_rate * l2_regularization);

    model->weights.assign(feature_count, 0.0f);
    model->bias = 0.0f;
    model->dead_zone = sanitize_dead_zone(options.dead_zone);

    for (int epoch = 0; epoch < epochs; ++epoch) {
        for (const auto & sample : samples) {
            const float y = sample.positive ? 1.0f : -1.0f;
            const float score = dot_product(model->weights, sample.features) + model->bias;
            const bool violates_margin = y * score < 1.0f;

            for (float & weight : model->weights) {
                weight *= shrink;
            }

            if (violates_margin) {
                for (size_t i = 0; i < feature_count; ++i) {
                    model->weights[i] += learning_rate * y * sample.features[i];
                }
                model->bias += learning_rate * y;
            }
        }
    }

    if (error) {
        error->clear();
    }
    return true;
}

float common_prompt_guardrail_linear_score(
        const common_prompt_guardrail_linear_model & model,
        const std::vector<float> & features) {
    return dot_product(model.weights, features) + model.bias;
}

common_prompt_guardrail_route common_prompt_guardrail_linear_route_from_score(
        float score,
        float dead_zone) {
    const float band = sanitize_dead_zone(dead_zone);
    if (score >= band) {
        return COMMON_PROMPT_GUARDRAIL_ROUTE_TECH;
    }

    if (score <= -band) {
        return COMMON_PROMPT_GUARDRAIL_ROUTE_NONTECH;
    }

    return COMMON_PROMPT_GUARDRAIL_ROUTE_UNSURE;
}

common_prompt_guardrail_route common_prompt_guardrail_linear_classify(
        const common_prompt_guardrail_linear_model & model,
        const std::vector<float> & features) {
    return common_prompt_guardrail_linear_route_from_score(
        common_prompt_guardrail_linear_score(model, features),
        model.dead_zone);
}
