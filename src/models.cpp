#include "models.hpp"

#include <gguf.h>
#include <ggml.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#include <openssl/evp.h>
#endif

namespace xvr {
namespace {

#ifdef _WIN32
struct Digest {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    Digest() {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0)
            BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    }
    ~Digest() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    bool add(char* data, size_t size) {
        return hash && BCryptHashData(hash, reinterpret_cast<PUCHAR>(data), ULONG(size), 0) == 0;
    }
    bool finish(unsigned char* out) { return hash && BCryptFinishHash(hash, out, 32, 0) == 0; }
};
#else
struct Digest {
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    bool ready = context && EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
    ~Digest() { EVP_MD_CTX_free(context); }
    bool add(char* data, size_t size) { return ready && EVP_DigestUpdate(context, data, size) == 1; }
    bool finish(unsigned char* out) { return ready && EVP_DigestFinal_ex(context, out, nullptr) == 1; }
};
#endif

std::string hex_digest(Digest& digest) {
    unsigned char raw[32];
    if (!digest.finish(raw)) return {};
    static constexpr char hex[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : raw) {
        text.push_back(hex[byte >> 4]);
        text.push_back(hex[byte & 15]);
    }
    return text;
}

std::string file_digest(const fs::path& path, const std::atomic<bool>& stop) {
    std::ifstream input(path, std::ios::binary);
    Digest digest;
    std::vector<char> block(256 * 1024);
    while (!stop && input) {
        input.read(block.data(), block.size());
        if (!digest.add(block.data(), size_t(input.gcount()))) return {};
    }
    return !stop && input.eof() && !input.bad() ? hex_digest(digest) : std::string{};
}

size_t read_metadata(void* userdata, void* data, uint64_t offset, size_t count) {
    constexpr uint64_t limit = 32 * 1024 * 1024;
    if (offset > limit || count > limit - offset) return 0;
    auto& input = *static_cast<std::ifstream*>(userdata);
    input.clear();
    if (input.tellg() != std::streamoff(offset)) input.seekg(std::streamoff(offset));
    input.read(static_cast<char*>(data), count);
    return size_t(input.gcount());
}

std::string meta(const gguf_context* context, const char* name) {
    const auto index = gguf_find_key(context, name);
    return index >= 0 && gguf_get_kv_type(context, index) == GGUF_TYPE_STRING ?
        gguf_get_val_str(context, index) : "";
}

Json inspect(const fs::path& path, const Json& catalog) {
    std::ifstream input(path, std::ios::binary);
    char signature[4];
    if (!input.read(signature, sizeof(signature)) || std::string_view(signature, 4) != "GGUF") return {};
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) return {};
    Json answer = {{"path", path.u8string()}, {"bytes", size}, {"state", "incompatible"}};
    std::unique_ptr<gguf_context, decltype(&gguf_free)> context(
        gguf_init_from_callback(read_metadata, &input, 0, size, {true, nullptr}), gguf_free);
    if (!context) {
        answer["state"] = "invalid";
        return answer;
    }
    const auto* gguf = context.get();
    const auto architecture = meta(gguf, "general.architecture");
    const auto variant = meta(gguf, "stt.variant");
    std::string id;
    if (architecture == "parakeet" && variant == "tdt-0.6b-v3") id = "parakeet-tdt-0.6b-v3";
    if (architecture == "parakeet" && variant == nemotron_id) id = nemotron_id;
    if (architecture == "moonshine_streaming" && variant.rfind("moonshine-streaming-", 0) == 0) id = variant;
    const auto type_key = gguf_find_key(gguf, "general.file_type");
    const uint32_t type = type_key >= 0 && gguf_get_kv_type(gguf, type_key) == GGUF_TYPE_UINT32 ?
        gguf_get_val_u32(gguf, type_key) : UINT32_MAX;
    bool q4 = false, q6 = false, q8 = false;
    const auto start = gguf_get_data_offset(gguf);
    const auto tensors = gguf_get_n_tensors(gguf);
    if (!tensors || start >= size) {
        answer["state"] = "invalid";
        return answer;
    }
    for (int64_t i = 0; i < tensors; ++i) {
        const auto offset = gguf_get_tensor_offset(gguf, i);
        const auto length = gguf_get_tensor_size(gguf, i);
        if (offset > size - start || length > size - start - offset) {
            answer["state"] = "invalid";
            return answer;
        }
        const auto tensor_type = gguf_get_tensor_type(gguf, i);
        q4 |= tensor_type == GGML_TYPE_Q4_K;
        q6 |= tensor_type == GGML_TYPE_Q6_K;
        q8 |= tensor_type == GGML_TYPE_Q8_0;
    }
    std::string quant;
    if (type == 7 && q8 && !q4 && !q6) quant = "Q8_0";
    if (type == 15 && q4) quant = "Q4_K_M";
    if (type == 18 && q6 && !q4) quant = "Q6_K";
    for (const auto& entry : catalog) {
        if (id == entry.at("id") && quant == entry.at("quant")) {
            answer["state"] = "candidate";
            answer["id"] = id;
            answer["quant"] = quant;
            break;
        }
    }
    return answer;
}

struct Transfer {
    std::ofstream output;
    Digest digest;
    const std::atomic<bool>& stop;
    const std::function<void(uint64_t)>& progress;
    uint64_t limit;
    uint64_t received = 0;
    Clock::time_point last_progress{};

    bool write(char* data, size_t size) {
        if (stop || size > limit - received || !digest.add(data, size)) return false;
        output.write(data, size);
        if (!output) return false;
        received += size;
        const auto now = Clock::now();
        if (now - last_progress >= 200ms || received == limit) {
            last_progress = now;
            progress(received);
        }
        return true;
    }
};

#ifdef _WIN32
struct HttpHandle {
    HINTERNET handle;
    ~HttpHandle() { if (handle) WinHttpCloseHandle(handle); }
    explicit operator bool() const { return handle != nullptr; }
};
bool get(const std::string& url, Transfer& transfer) {
    const auto wide = fs::u8path(url).wstring();
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = DWORD(-1);
    components.dwUrlPathLength = DWORD(-1);
    components.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(wide.c_str(), DWORD(wide.size()), 0, &components) ||
        components.nScheme != INTERNET_SCHEME_HTTPS) return false;
    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength) path.append(components.lpszExtraInfo, components.dwExtraInfoLength);

    HttpHandle session{WinHttpOpen(L"X-Voice-Rekon/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session || !WinHttpSetTimeouts(session.handle, 5000, 5000, 5000, 3000)) return false;
    HttpHandle connection{WinHttpConnect(session.handle, host.c_str(), components.nPort, 0)};
    if (!connection) return false;
    HttpHandle request{WinHttpOpenRequest(connection.handle, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request) return false;
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    DWORD features = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(request.handle, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)) ||
        !WinHttpSetOption(request.handle, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof(features)) ||
        !WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.handle, nullptr)) return false;
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             nullptr, &status, &size, nullptr) || status != 200) return false;
    std::array<char, 64 * 1024> bytes{};
    while (!transfer.stop) {
        DWORD received = 0;
        if (!WinHttpReadData(request.handle, bytes.data(), DWORD(bytes.size()), &received)) return false;
        if (!received) return true;
        if (!transfer.write(bytes.data(), received)) return false;
    }
    return false;
}
#else
bool get(const std::string& url, Transfer& transfer) {
    static const bool initialized = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!initialized) return false;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) return false;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "X-Voice-Rekon/0.1");
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION,
        +[](char* buffer, size_t size, size_t count, void* userdata) -> size_t {
            const auto bytes = size * count;
            return static_cast<Transfer*>(userdata)->write(buffer, bytes) ? bytes : 0;
        });
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION,
        +[](void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            return *static_cast<const std::atomic<bool>*>(userdata) ? 1 : 0;
        });
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &transfer.stop);
    const auto result = curl_easy_perform(curl.get());
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    return result == CURLE_OK && status == 200;
}
#endif

} // namespace

Json scan_models(const fs::path& folder, const Json& catalog, const std::atomic<bool>& stop) {
    Json found = Json::array();
    std::error_code ec;
    size_t visited = 0;
    for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end && !stop && visited < 2048 && found.size() < 64; it.increment(ec), ++visited) {
        std::error_code item_error;
        if (!it->is_regular_file(item_error) || it->path().extension() == ".part") continue;
        auto model = inspect(it->path(), catalog);
        if (model.is_object()) found.push_back(std::move(model));
    }
    std::sort(found.begin(), found.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.at("path") < rhs.at("path");
    });
    return found;
}

Json identify_model(const fs::path& path, const Json& catalog, const std::atomic<bool>& stop) {
    auto model = inspect(path, catalog);
    if (!model.is_object() || model.value("state", "") != "candidate") return {};
    const auto hash = file_digest(path, stop);
    if (hash.empty()) return {};
    model["sha256"] = hash;
    model["revision"] = "sha256:" + hash;
    for (const auto& entry : catalog) {
        if (entry.at("sha256") == hash && entry.at("bytes") == model.at("bytes") &&
            entry.at("id") == model.at("id") && entry.at("quant") == model.at("quant")) {
            model["revision"] = entry.at("revision");
            break;
        }
    }
    return model;
}

bool matches_digest(const fs::path& path, const std::string& expected, const std::atomic<bool>& stop) {
    return expected.size() == 64 && file_digest(path, stop) == expected;
}

std::string download_model(const fs::path& folder, const Json& entry,
                           const std::atomic<bool>& stop, const std::function<void(uint64_t)>& progress) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) return "destination";
    const auto name = fs::u8path(entry.at("file").get<std::string>());
    if (name.empty() || name.has_parent_path() || name.extension() != ".gguf") return "destination";
    const auto final = folder / name;
    if (fs::exists(final, ec) && file_digest(final, stop) == entry.at("sha256")) return "ok";
    if (stop) return "cancelled";
    if (fs::exists(final, ec)) return "exists";
    const auto temp = folder / fs::u8path(name.u8string() + "." +
        std::to_string(Clock::now().time_since_epoch().count()) + ".part");
    std::string status = "network";
    for (const auto& item : entry.at("urls")) {
        if (stop) break;
        const auto url = item.get<std::string>();
        Transfer transfer{std::ofstream(temp, std::ios::binary | std::ios::trunc), {},
                          stop, progress, entry.at("bytes").get<uint64_t>()};
        if (!transfer.output) return "destination";
        const bool received = get(url, transfer);
        transfer.output.close();
        if (stop) break;
        if (!received || transfer.received != transfer.limit) continue;
        if (transfer.output.fail() || hex_digest(transfer.digest) != entry.at("sha256")) {
            status = "integrity";
            continue;
        }
#ifdef _WIN32
        if (MoveFileExW(temp.c_str(), final.c_str(), MOVEFILE_WRITE_THROUGH)) return "ok";
#else
        fs::create_hard_link(temp, final, ec);
        if (!ec) {
            fs::remove(temp, ec);
            return "ok";
        }
#endif
        status = "destination";
    }
    fs::remove(temp, ec);
    return stop ? "cancelled" : status;
}

} // namespace xvr
