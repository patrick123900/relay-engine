#include "relay/render/asset_manifest.hpp"

#include "relay/core/json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace relay {
namespace {

constexpr std::uintmax_t max_manifest_bytes = 4U * 1024U * 1024U;
constexpr const char* manifest_filename = ".relay-imports.json";

std::vector<std::string> read_string_array(const JsonValue* value) {
    std::vector<std::string> output;
    if (value == nullptr || value->array() == nullptr) return output;
    for (const auto& element : *value->array()) {
        if (const auto* text = element.string()) output.push_back(*text);
    }
    return output;
}

void write_string_array(std::ostringstream& output, const std::vector<std::string>& values) {
    output << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0U) output << ',';
        output << '"' << json_escape(values[index]) << '"';
    }
    output << ']';
}

std::filesystem::path temporary_path_for(const std::filesystem::path& destination) {
    static std::atomic<std::uint64_t> serial{0U};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return destination.parent_path() /
           (destination.filename().string() + ".tmp." + std::to_string(ticks) + '.' +
            std::to_string(serial.fetch_add(1U, std::memory_order_relaxed)));
}

bool replace_atomically(const std::filesystem::path& temporary,
                        const std::filesystem::path& destination, std::string& error) {
#ifdef _WIN32
    if (MoveFileExW(temporary.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        error = "import manifest replacement failed with Windows error " +
                std::to_string(GetLastError());
        return false;
    }
#else
    if (std::rename(temporary.c_str(), destination.c_str()) != 0) {
        error = "import manifest could not be replaced";
        return false;
    }
#endif
    return true;
}

} // namespace

std::string ImportReloadReport::json() const {
    std::ostringstream output;
    output << "{\"restored\":" << restored << ",\"changed\":" << changed
           << ",\"failed\":" << failed << ",\"messages\":";
    write_string_array(output, messages);
    output << '}';
    return output.str();
}

std::filesystem::path ImportManifest::path_for(const std::filesystem::path& assets_root) {
    return assets_root / manifest_filename;
}

bool ImportManifest::load(const std::filesystem::path& assets_root, std::string& error) {
    entries_.clear();
    const auto path = path_for(assets_root);
    std::error_code code;
    if (!std::filesystem::exists(path, code)) return true;
    const auto size = std::filesystem::file_size(path, code);
    if (code || size > max_manifest_bytes) {
        error = "import manifest is unreadable or too large";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "import manifest could not be opened";
        return false;
    }
    const std::string text{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
    JsonParser parser(text);
    const auto document = parser.parse();
    if (!document || document->object() == nullptr) {
        error = "import manifest is not a JSON object: " + parser.error();
        return false;
    }
    const auto& root = *document->object();
    const auto* version = field(root, "version");
    if (version == nullptr || version->number() == nullptr ||
        static_cast<std::uint32_t>(*version->number()) != import_manifest_version) {
        error = "import manifest version is missing or unsupported";
        return false;
    }
    const auto* entries = field(root, "entries");
    if (entries == nullptr || entries->array() == nullptr) {
        error = "import manifest has no entries array";
        return false;
    }
    for (const auto& element : *entries->array()) {
        const auto* record = element.object();
        if (record == nullptr) continue;
        const auto* source = field(*record, "source");
        const auto* content_id = field(*record, "content_id");
        if (source == nullptr || source->string() == nullptr || content_id == nullptr ||
            content_id->string() == nullptr) {
            continue;
        }
        ImportManifestEntry entry;
        entry.source = *source->string();
        entry.content_id = *content_id->string();
        const auto* importer = field(*record, "importer_version");
        entry.importer_version = importer != nullptr && importer->number() != nullptr
                                     ? static_cast<std::uint32_t>(*importer->number())
                                     : 0U;
        entry.dependencies = read_string_array(field(*record, "dependencies"));
        entry.meshes = read_string_array(field(*record, "meshes"));
        entry.materials = read_string_array(field(*record, "materials"));
        entry.textures = read_string_array(field(*record, "textures"));
        entries_.push_back(std::move(entry));
    }
    return true;
}

bool ImportManifest::save(const std::filesystem::path& assets_root, std::string& error) const {
    const auto path = path_for(assets_root);
    std::ostringstream output;
    output << "{\"version\":" << import_manifest_version << ",\"entries\":[";
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        const auto& entry = entries_[index];
        if (index != 0U) output << ',';
        output << "{\"source\":\"" << json_escape(entry.source)
               << "\",\"importer_version\":" << entry.importer_version
               << ",\"content_id\":\"" << json_escape(entry.content_id) << "\",\"dependencies\":";
        write_string_array(output, entry.dependencies);
        output << ",\"meshes\":";
        write_string_array(output, entry.meshes);
        output << ",\"materials\":";
        write_string_array(output, entry.materials);
        output << ",\"textures\":";
        write_string_array(output, entry.textures);
        output << '}';
    }
    output << "]}\n";

    // Write beside the target and rename, so an interrupted save cannot truncate a good manifest.
    const auto temporary = temporary_path_for(path);
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "import manifest could not be written";
            return false;
        }
        const auto text = output.str();
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream) {
            error = "import manifest could not be written";
            return false;
        }
    }
    std::error_code code;
    if (!replace_atomically(temporary, path, error)) {
        std::filesystem::remove(temporary, code);
        return false;
    }
    return true;
}

void ImportManifest::record(ImportManifestEntry entry) {
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto& existing) {
        return existing.source == entry.source;
    });
    if (found == entries_.end()) {
        entries_.push_back(std::move(entry));
        return;
    }
    *found = std::move(entry);
}

const std::vector<ImportManifestEntry>& ImportManifest::entries() const { return entries_; }

ImportReloadReport reload_imported_assets(const std::filesystem::path& assets_root,
                                          AssetRegistry& registry) {
    ImportReloadReport report;
    ImportManifest manifest;
    std::string error;
    if (!manifest.load(assets_root, error)) {
        ++report.failed;
        report.messages.push_back(error);
        return report;
    }
    for (const auto& entry : manifest.entries()) {
        std::string import_error;
        const auto imported = import_model_asset(assets_root, entry.source, registry, nullptr,
                                                 import_error);
        if (!imported.imported) {
            ++report.failed;
            report.messages.push_back("could not restore '" + entry.source + "': " + import_error);
            continue;
        }
        if (entry.importer_version != model_importer_version) {
            ++report.changed;
            report.messages.push_back("'" + entry.source + "' was imported by importer version " +
                                      std::to_string(entry.importer_version) +
                                      "; assets were rebuilt with version " +
                                      std::to_string(model_importer_version));
            continue;
        }
        if (imported.content_id != entry.content_id) {
            ++report.changed;
            report.messages.push_back("'" + entry.source +
                                      "' changed on disk; scene references to its previous assets "
                                      "will not resolve");
            continue;
        }
        ++report.restored;
    }
    return report;
}

} // namespace relay
