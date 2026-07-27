#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "recipes/RecipeTypes.h"

class RecipeDatabase
{
public:
    // Tries, in order: appdata/combinations.json, ./combinations.json,
    // ./resources/combinations.json, RuneHelper/resources/combinations.json.
    bool Load();
    bool LoadFromFile(const std::filesystem::path& path);

    bool Loaded() const { return loaded_; }
    bool Complete() const { return complete_; }
    const std::string& LoadedFrom() const { return loadedFrom_; }

    const std::vector<Recipe>& Recipes() const { return recipes_; }
    const std::set<std::string>& RuneNames() const { return runeNames_; }

    // "Fire Rune" / "fire rune" / "Fire" -> "Fire" (empty if not a known rune)
    std::string MatchRuneName(std::string_view itemName) const;

    // "Fire Rune" -> "Fire"
    static std::string NormalizeRune(std::string_view name);

private:
    bool loaded_ = false;
    bool complete_ = false;
    std::string loadedFrom_;
    std::vector<Recipe> recipes_;
    std::set<std::string> runeNames_;
};
