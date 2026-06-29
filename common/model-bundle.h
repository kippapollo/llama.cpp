#ifndef LLAMA_CPP_MODEL_BUNDLE_H
#define LLAMA_CPP_MODEL_BUNDLE_H

#include <string>

// Plain-text archive for a model directory tree.
// This is used as the intermediate format before applying the existing
// encrypted model container.

bool common_model_bundle_is_archive_file(const std::string & path);

// Pack a directory tree into a plaintext archive file.
bool common_model_bundle_pack_directory(
    const std::string & input_dir,
    const std::string & output_path,
    std::string & err);

// Unpack a plaintext archive file into a directory tree.
bool common_model_bundle_unpack_archive(
    const std::string & archive_path,
    const std::string & output_dir,
    std::string & err);

// Unpack a plaintext archive file into a temporary directory and return that path.
std::string common_model_bundle_unpack_archive_to_temp_dir(
    const std::string & archive_path,
    std::string & err);

#endif // LLAMA_CPP_MODEL_BUNDLE_H
