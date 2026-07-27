#include "recipes/RecipeDatabase.h"

#include <algorithm>
#include <cctype>
#include <fstream>

#include "nlohmann/json.hpp"

#include "core/Logger.h"
#include "platform/PlatformPaths.h"

using json = nlohmann::json;

namespace
{
std::string ToLower(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string Trim(std::string_view s)
{
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])))
        --end;
    return std::string(s.substr(begin, end - begin));
}
}

std::string RecipeDatabase::NormalizeRune(std::string_view name)
{
    std::string trimmed = Trim(name);

    constexpr std::string_view kSuffix = " Rune";
    if (trimmed.size() > kSuffix.size())
    {
        std::string_view tail(trimmed.data() + trimmed.size() - kSuffix.size(), kSuffix.size());
        if (ToLower(tail) == ToLower(kSuffix))
            trimmed = Trim(trimmed.substr(0, trimmed.size() - kSuffix.size()));
    }

    return trimmed;
}

std::string RecipeDatabase::MatchRuneName(std::string_view itemName) const
{
    const std::string normalized = NormalizeRune(itemName);
    const std::string lowered = ToLower(normalized);

    for (const auto& rune : runeNames_)
    {
        if (ToLower(rune) == lowered)
            return rune;
    }

    return {};
}

bool RecipeDatabase::Load()
{
    const std::filesystem::path candidates[] = {
        GetAppDataDir() / "combinations.json",
        std::filesystem::path("combinations.json"),
        std::filesystem::path("resources") / "combinations.json",
        std::filesystem::path("RuneHelper") / "resources" / "combinations.json",
    };

    for (const auto& path : candidates)
    {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            continue;

        if (LoadFromFile(path))
            return true;
    }

    LOG_ERROR("RecipeDatabase: no loadable combinations.json found");
    return false;
}

bool RecipeDatabase::LoadFromFile(const std::filesystem::path& path)
{
    std::ifstream file(path);

    if (!file)
        return false;

    json j = json::parse(file, nullptr, false);

    if (j.is_discarded() || !j.contains("combinations") || !j["combinations"].is_array())
    {
        LOG_ERROR("RecipeDatabase: invalid combinations.json: " + path.string());
        return false;
    }

    std::vector<Recipe> recipes;
    std::set<std::string> runeNames;

    for (const auto& entry : j["combinations"])
    {
        Recipe recipe;
        recipe.output = entry.value("output", std::string());
        recipe.count = entry.value("count", 1);
        recipe.level = entry.value("level", 0);
        recipe.category = entry.value("category", std::string("unknown"));

        if (recipe.output.empty() || !entry.contains("runes") || !entry["runes"].is_array())
            continue;

        for (const auto& runeJson : entry["runes"])
        {
            if (!runeJson.is_string())
                continue;

            const std::string rune = NormalizeRune(runeJson.get<std::string>());

            if (rune.empty())
                continue;

            recipe.runes.push_back(rune);
            ++recipe.runeCounts[rune];
            runeNames.insert(rune);
        }

        if (!recipe.runes.empty())
            recipes.push_back(std::move(recipe));
    }

    if (recipes.empty())
    {
        LOG_ERROR("RecipeDatabase: no valid combinations in " + path.string());
        return false;
    }

    recipes_ = std::move(recipes);
    runeNames_ = std::move(runeNames);
    complete_ = j.value("complete", false);
    loadedFrom_ = path.string();
    loaded_ = true;

    LOG_INFO(
        "RecipeDatabase: loaded " + std::to_string(recipes_.size()) +
        " combinations (" + std::to_string(runeNames_.size()) + " rune types) from " + loadedFrom_ +
        (complete_ ? "" : " [PARTIAL dataset - run tools/scrape_poe2db.py]"));

    return true;
}
