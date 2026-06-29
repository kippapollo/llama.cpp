#ifndef LLAMA_CPP_MODEL_ENCRYPTION_H
#define LLAMA_CPP_MODEL_ENCRYPTION_H

#include <string>

// Simple streaming encryption container for model files.
// NOTE: This is intended for local enforcement/obfuscation, not high-grade DRM.

bool common_model_is_encrypted_file(const std::string & path);

// Decrypts an encrypted model file to a temporary plaintext file and returns the temp path.
// On error, returns empty string and sets err.
std::string common_model_decrypt_to_temp_file(
    const std::string & encrypted_path,
    const std::string & passcode,
    std::string & err);

// Encrypts input file to output path.
// On error, returns false and sets err.
bool common_model_encrypt_file(
    const std::string & input_path,
    const std::string & output_path,
    const std::string & passcode,
    std::string & err);

// Decrypts encrypted model to output path.
// On error, returns false and sets err.
bool common_model_decrypt_file(
    const std::string & encrypted_path,
    const std::string & output_path,
    const std::string & passcode,
    std::string & err);

// Validates the 5x4-digit passcode format: dddd-dddd-dddd-dddd-dddd
bool common_model_validate_passcode_5x4(const std::string & passcode);

#endif // LLAMA_CPP_MODEL_ENCRYPTION_H
