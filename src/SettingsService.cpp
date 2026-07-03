#include "SettingsService.h"
#include "ShellHelpers.h"

#include <fstream>
#include <sstream>
#include <cctype>

namespace {

bool FindBool(const std::string& text, const char* key, bool defaultValue) {
    std::string needle = std::string("\"") + key + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) return defaultValue;
    pos = text.find(':', pos);
    if (pos == std::string::npos) return defaultValue;
    size_t truePos = text.find("true", pos);
    size_t falsePos = text.find("false", pos);
    size_t commaPos = text.find_first_of(",}\n", pos);
    if (falsePos != std::string::npos && (commaPos == std::string::npos || falsePos < commaPos)) return false;
    if (truePos != std::string::npos && (commaPos == std::string::npos || truePos < commaPos)) return true;
    return defaultValue;
}

uint32_t FindUInt(const std::string& text, const char* key, uint32_t defaultValue) {
    std::string needle = std::string("\"") + key + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) return defaultValue;
    pos = text.find(':', pos);
    if (pos == std::string::npos) return defaultValue;
    ++pos;
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
    size_t digitsStart = pos;
    while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) ++pos;
    if (pos == digitsStart) return defaultValue;
    try {
        return static_cast<uint32_t>(std::stoul(text.substr(digitsStart, pos - digitsStart)));
    } catch (...) {
        return defaultValue;
    }
}

void WriteBool(std::ostringstream& out, const char* key, bool value, bool trailingComma) {
    out << "  \"" << key << "\": " << (value ? "true" : "false") << (trailingComma ? ",\n" : "\n");
}

void WriteUInt(std::ostringstream& out, const char* key, uint32_t value, bool trailingComma) {
    out << "  \"" << key << "\": " << value << (trailingComma ? ",\n" : "\n");
}

} // namespace

std::wstring SettingsService::SettingsFilePath() {
    std::wstring dir = ShellHelpers::GetAppDataDirectory(/*localAppData=*/false);
    if (dir.empty()) return L"";
    return dir + L"\\settings.json";
}

AppSettings SettingsService::Load() {
    AppSettings settings; // defaults

    std::wstring path = SettingsFilePath();
    if (path.empty()) return settings;

    std::ifstream in(path, std::ios::binary);
    if (!in) return settings;

    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();
    if (text.empty()) return settings;

    settings.enableIndexing = FindBool(text, "EnableIndexing", settings.enableIndexing);
    settings.autoIndexFixedNtfsDrives = FindBool(text, "AutoIndexFixedNtfsDrives", settings.autoIndexFixedNtfsDrives);
    settings.useRawMftScan = FindBool(text, "UseRawMftScan", settings.useRawMftScan);
    settings.includeRemovableDrives = FindBool(text, "IncludeRemovableDrives", settings.includeRemovableDrives);
    settings.includeNetworkDrives = FindBool(text, "IncludeNetworkDrives", settings.includeNetworkDrives);
    settings.useFastNtfsUsnScan = FindBool(text, "UseFastNtfsUsnScan", settings.useFastNtfsUsnScan);
    settings.persistIndexCache = FindBool(text, "PersistIndexCache", settings.persistIndexCache);
    settings.useIncrementalUsnUpdates = FindBool(text, "UseIncrementalUsnUpdates", settings.useIncrementalUsnUpdates);
    settings.includeHiddenFiles = FindBool(text, "IncludeHiddenFiles", settings.includeHiddenFiles);
    settings.includeSystemFiles = FindBool(text, "IncludeSystemFiles", settings.includeSystemFiles);
    settings.avoidReparsePoints = FindBool(text, "AvoidReparsePoints", settings.avoidReparsePoints);
    settings.maxDisplayedResults = FindUInt(text, "MaxDisplayedResults", settings.maxDisplayedResults);
    settings.searchDebounceMs = FindUInt(text, "SearchDebounceMs", settings.searchDebounceMs);
    settings.alwaysOnTop = FindBool(text, "AlwaysOnTop", settings.alwaysOnTop);
    settings.indexStalenessWarningDays = FindUInt(text, "IndexStalenessWarningDays", settings.indexStalenessWarningDays);
    settings.startupElevationChoice = static_cast<StartupElevationChoice>(
        FindUInt(text, "StartupElevationChoice", static_cast<uint32_t>(settings.startupElevationChoice)));

    return settings;
}

bool SettingsService::Save(const AppSettings& settings) {
    std::wstring path = SettingsFilePath();
    if (path.empty()) return false;

    std::ostringstream out;
    out << "{\n";
    WriteBool(out, "EnableIndexing", settings.enableIndexing, true);
    WriteBool(out, "AutoIndexFixedNtfsDrives", settings.autoIndexFixedNtfsDrives, true);
    WriteBool(out, "UseRawMftScan", settings.useRawMftScan, true);
    WriteBool(out, "IncludeRemovableDrives", settings.includeRemovableDrives, true);
    WriteBool(out, "IncludeNetworkDrives", settings.includeNetworkDrives, true);
    WriteBool(out, "UseFastNtfsUsnScan", settings.useFastNtfsUsnScan, true);
    WriteBool(out, "PersistIndexCache", settings.persistIndexCache, true);
    WriteBool(out, "UseIncrementalUsnUpdates", settings.useIncrementalUsnUpdates, true);
    WriteBool(out, "IncludeHiddenFiles", settings.includeHiddenFiles, true);
    WriteBool(out, "IncludeSystemFiles", settings.includeSystemFiles, true);
    WriteBool(out, "AvoidReparsePoints", settings.avoidReparsePoints, true);
    WriteUInt(out, "MaxDisplayedResults", settings.maxDisplayedResults, true);
    WriteUInt(out, "SearchDebounceMs", settings.searchDebounceMs, true);
    WriteBool(out, "AlwaysOnTop", settings.alwaysOnTop, true);
    WriteUInt(out, "IndexStalenessWarningDays", settings.indexStalenessWarningDays, true);
    WriteUInt(out, "StartupElevationChoice", static_cast<uint32_t>(settings.startupElevationChoice), false);
    out << "}\n";

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    std::string content = out.str();
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    return file.good();
}
