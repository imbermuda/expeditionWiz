#pragma once

#include <string>
#include <unordered_map>
#include <vector>

struct Recipe
{
    std::string output;
    int count = 1;
    int level = 0;
    std::string category;
    std::vector<std::string> runes;                    // as listed (may contain duplicates)
    std::unordered_map<std::string, int> runeCounts;   // rune -> required amount
};

struct RuneScore
{
    std::string rune;             // normalized name, e.g. "Fire"
    double ev = 0.0;              // marginal expected value (in exalts)
    int rank = 0;                 // 1 = best pick among scored runes
    std::string bestRecipe;       // highest-contribution recipe this rune advances
    int bestRecipeMissing = 0;    // runes still missing for that recipe (incl. this one)
    double bestRecipeValue = 0.0; // full value of that recipe (in exalts)
};

struct RecipeProgress
{
    std::string output;
    std::string category;
    int count = 1;
    int have = 0;        // required runes already owned
    int total = 0;       // total required runes
    int missing = 0;     // total - have
    double value = 0.0;  // full reward value (in exalts), 0 if unpriced
    std::string priceText;
    std::vector<std::string> missingRunes;
};
