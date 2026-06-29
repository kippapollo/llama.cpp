#include "model-bundle.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <iterator>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

static fs::path unique_temp_root() {
    const auto stamp = static_cast<unsigned long long>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    return fs::temp_directory_path() / ("llama_model_bundle_test_" + std::to_string(stamp));
}

static void write_file(const fs::path & path, const std::string & contents) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw std::runtime_error("failed to create file: " + path.string());
    }

    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!file) {
        throw std::runtime_error("failed to write file: " + path.string());
    }
}

static std::string read_file(const fs::path & path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("failed to open file: " + path.string());
    }

    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

static void require(bool condition, const std::string & message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main() {
    try {
        const fs::path root = unique_temp_root();
        const fs::path input = root / "input";
        const fs::path output = root / "output";
        const fs::path archive = root / "coder.bundle";

        std::filesystem::create_directories(input / "ner");
        write_file(input / "coder.gguf", std::string("main-model\0payload", 17));
        write_file(input / "embedding.gguf", std::string("embedding-model"));
        write_file(input / "prompt-guardrail-samples.json", R"JSON({
  "instruction": "Classify coding-related prompts.",
  "positive_examples": ["Write a Python unit test."],
  "negative_examples": ["Plan a beach trip."]
})JSON");
        write_file(input / "ner" / "model.onnx", std::string("\0\1\2\3", 4));
        write_file(input / "ner" / "config.json", R"JSON({"id2label":{"0":"O"}})JSON");
        write_file(input / "ner" / "vocab.txt", "O\n");
        write_file(input / "ner" / "tokenizer.json", "{}");
        write_file(input / "ner" / "tokenizer_config.json", R"JSON({"do_lower_case":true})JSON");
        write_file(input / "ner" / "special_tokens_map.json", R"JSON({"unk_token":"[UNK]"})JSON");

        std::string err;
        require(common_model_bundle_pack_directory(input.string(), archive.string(), err), err);
        require(err.empty(), "pack should not report an error");
        require(common_model_bundle_is_archive_file(archive.string()), "archive should be recognized");
        require(common_model_bundle_unpack_archive(archive.string(), output.string(), err), err);
        require(err.empty(), "unpack should not report an error");

        require(read_file(output / "coder.gguf") == read_file(input / "coder.gguf"), "coder.gguf mismatch");
        require(read_file(output / "embedding.gguf") == read_file(input / "embedding.gguf"), "embedding.gguf mismatch");
        require(read_file(output / "prompt-guardrail-samples.json") == read_file(input / "prompt-guardrail-samples.json"),
                "prompt-guardrail-samples.json mismatch");
        require(read_file(output / "ner" / "model.onnx") == read_file(input / "ner" / "model.onnx"), "ner/model.onnx mismatch");
        require(read_file(output / "ner" / "config.json") == read_file(input / "ner" / "config.json"), "ner/config.json mismatch");
        require(read_file(output / "ner" / "vocab.txt") == read_file(input / "ner" / "vocab.txt"), "ner/vocab.txt mismatch");
        require(read_file(output / "ner" / "tokenizer.json") == read_file(input / "ner" / "tokenizer.json"), "ner/tokenizer.json mismatch");
        require(read_file(output / "ner" / "tokenizer_config.json") == read_file(input / "ner" / "tokenizer_config.json"),
                "ner/tokenizer_config.json mismatch");
        require(read_file(output / "ner" / "special_tokens_map.json") == read_file(input / "ner" / "special_tokens_map.json"),
                "ner/special_tokens_map.json mismatch");

        std::error_code ec;
        fs::remove_all(root, ec);

        std::cout << "OK\n";
        return 0;
    } catch (const std::exception & e) {
        std::cerr << "Test failed: " << e.what() << '\n';
        return 1;
    }
}
