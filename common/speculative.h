#pragma once

#include "llama.h"
#include "common.h"

struct common_speculative;

struct common_speculative_params {
    int n_draft = 0;
    int n_reuse = 0;
    float p_min = 0.0f;
};

// comma separated list of all types
std::string common_speculative_type_name_str();

// convert string to type
enum common_speculative_type common_speculative_type_from_name(const std::string & name);

// convert type to string
std::string common_speculative_type_to_str(enum common_speculative_type type);

// check if the llama_context is compatible for speculative decoding
// note: clears the memory of the context
bool common_speculative_is_compat(llama_context * ctx_tgt);

bool common_speculative_are_compatible(llama_context * ctx_tgt, llama_context * ctx_dft);

common_speculative * common_speculative_init(
        common_params_speculative & params,
        llama_context             * ctx_tgt);

common_speculative * common_speculative_init(
        llama_context * ctx_tgt,
        llama_context * ctx_dft);

void common_speculative_free(common_speculative * spec);

void common_speculative_add_replacement_tgt_dft(
        common_speculative * spec,
        const char *          tgt,
        const char *          dft);

// optionally call once at the beginning of a new generation
void common_speculative_begin(common_speculative * spec, const llama_tokens & prompt);

// sample up to n_draft tokens and add them to the batch using the draft model
llama_tokens common_speculative_draft(
                     common_speculative * spec,
        const common_params_speculative & params,
                     const llama_tokens & prompt,
                            llama_token   id_last);

llama_tokens common_speculative_gen_draft(
                     common_speculative * spec,
        const common_speculative_params & params,
                     const llama_tokens & prompt,
                            llama_token   id_last);

// informs the speculative decoder that n_accepted tokens were accepted by the target model
void common_speculative_accept(common_speculative * spec, uint16_t n_accepted);

// print statistics about the speculative decoding
void common_speculative_print_stats(const common_speculative * spec);
