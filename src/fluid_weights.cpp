#include "fluid_weights.hpp"
#include "fluid_recipes.hpp"
#include "models.hpp"

#include <algorithm>
#include <fstream>
#include <limits>

namespace xsl {
namespace {

struct RecipeReader {
    std::string_view bytes;

    uint64_t number(size_t width) {
        if (width > bytes.size()) throw std::runtime_error("Incomplete Fluid recipe");
        uint64_t value = 0;
        for (size_t i = 0; i < width; ++i)
            value |= uint64_t(static_cast<unsigned char>(bytes[i])) << (i * 8);
        bytes.remove_prefix(width);
        return value;
    }
};

template <size_t size>
std::string_view recipe_bytes(const unsigned char (&recipe)[size]) {
    return {reinterpret_cast<const char*>(recipe), size};
}

bool write_weights(std::string_view recipe, const fs::path& source, const fs::path& target,
                   const std::atomic<bool>& stop) {
    RecipeReader reader{recipe};
    const auto total = reader.number(8);
    std::ifstream input(source, std::ios::binary | std::ios::ate);
    const auto input_size = input.tellg();
    if (input_size < 0 || total > uint64_t(input_size)) return false;
    std::ofstream output(target, std::ios::binary | std::ios::trunc);
    std::vector<char> buffer(1024 * 1024);
    uint64_t written = 0;
    while (!reader.bytes.empty() && !stop && output) {
        const auto offset = reader.number(8);
        auto length = reader.number(4);
        if (!length || length > total - written) return false;
        written += length;
        if (offset == std::numeric_limits<uint64_t>::max()) {
            if (length > reader.bytes.size()) return false;
            output.write(reader.bytes.data(), static_cast<std::streamsize>(length));
            reader.bytes.remove_prefix(static_cast<size_t>(length));
            continue;
        }
        if (offset > uint64_t(input_size) || length > uint64_t(input_size) - offset) return false;
        input.seekg(static_cast<std::streamoff>(offset));
        while (length && !stop && output) {
            const auto count = static_cast<std::streamsize>(std::min(length, uint64_t(buffer.size())));
            if (!input.read(buffer.data(), count)) return false;
            output.write(buffer.data(), count);
            length -= uint64_t(count);
        }
    }
    output.close();
    return !stop && output && written == total;
}

} // namespace

bool FluidWeights::prepare(const std::string& id, const fs::path& original, std::string_view digest,
                           const std::atomic<bool>& stop) {
    if (!path_.empty()) return true;
    constexpr size_t digest_bytes = 64;
    constexpr size_t hash_offset = 4;
    constexpr size_t payload_offset = hash_offset + 2 * digest_bytes;
    auto recipe = id == fluid_mini_id ? recipe_bytes(fluid_mini_recipe) : recipe_bytes(fluid_pro_recipe);
    const auto crispasr = recipe_bytes(fluid_pro_crispasr_recipe);
    if (id == fluid_pro_id && crispasr.substr(hash_offset, digest_bytes) == digest) recipe = crispasr;
    if (recipe.size() <= payload_offset || recipe.substr(0, hash_offset) != "XLF1" ||
        recipe.substr(hash_offset, digest_bytes) != digest) return false;

    std::error_code error;
    const auto temporary = fs::temp_directory_path(error);
    if (error) return false;
    const auto folder = temporary / ("xsl-model-" + std::to_string(Clock::now().time_since_epoch().count()));
    if (!fs::create_directory(folder, error)) return false;
    folder_ = folder;
    const auto file = folder / "fluid.gguf";
    const auto expected = std::string(recipe.substr(hash_offset + digest_bytes, digest_bytes));
    if (!write_weights(recipe.substr(payload_offset), original, file, stop) || !matches_digest(file, expected, stop)) {
        reset();
        return false;
    }
    path_ = file;
    return true;
}

void FluidWeights::reset() {
    if (folder_.empty()) return;
    std::error_code error;
    fs::remove_all(folder_, error);
    folder_.clear();
    path_.clear();
}

} // namespace xsl
