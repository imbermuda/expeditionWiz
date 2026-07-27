#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "recipes/RecipeDatabase.h"
#include "recipes/RecipeTypes.h"
#include "recipes/RuneInventory.h"

class PriceCache;

// Owns the recipe database + rune inventory and computes marginal-EV pick
// recommendations. Thread-safe: UI thread mutates the inventory, the OCR
// worker calls Recompute() and reads scores.
class ExpeditionAdvisor
{
public:
    bool Init();

    bool Ready() const;
    std::string DataStatus() const;

    RuneInventory& Inventory() { return inventory_; }
    const RecipeDatabase& Database() const { return database_; }

    // Maps an OCR'd/matched item name to a normalized rune name ("" if not a rune).
    std::string MatchRuneName(const std::string& itemName) const;

    // Recomputes all rune scores + closest recipes if inventory or prices changed.
    // Called from the OCR worker loop; cheap no-op when nothing changed.
    void Recompute(PriceCache& priceCache);

    std::optional<RuneScore> ScoreFor(const std::string& runeName) const;
    std::vector<RuneScore> AllScores() const;
    std::vector<RecipeProgress> ClosestRecipes() const;

    // Convenience for the UI: add a rune by (possibly un-normalized) name.
    bool TakeRune(const std::string& itemName);

private:
    void RecomputeLocked(PriceCache& priceCache);
    static std::optional<double> RecipeValue(const Recipe& recipe, PriceCache& priceCache);

private:
    RecipeDatabase database_;
    RuneInventory inventory_;

    mutable std::mutex scoreMutex_;
    std::map<std::string, RuneScore> scores_;
    std::vector<RecipeProgress> closestRecipes_;

    uint64_t scoredRevision_ = 0;
    size_t scoredPriceCount_ = 0;
};
