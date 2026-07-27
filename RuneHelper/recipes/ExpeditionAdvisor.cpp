#include "recipes/ExpeditionAdvisor.h"

#include <algorithm>
#include <cmath>

#include "core/Logger.h"
#include "ocr/LootParser.h"
#include "price/PriceCache.h"

namespace
{
// How strongly near-complete recipes dominate the score. With exponent 2,
// a recipe missing 1 rune contributes value/1, missing 2 -> value/4,
// missing 3 -> value/9, etc.
constexpr double kMissingExponent = 2.0;

constexpr size_t kClosestRecipesLimit = 12;
}

bool ExpeditionAdvisor::Init()
{
    const bool dbLoaded = database_.Load();
    inventory_.Load();
    return dbLoaded;
}

bool ExpeditionAdvisor::Ready() const
{
    return database_.Loaded();
}

std::string ExpeditionAdvisor::DataStatus() const
{
    if (!database_.Loaded())
        return "no combinations.json loaded";

    std::string status = std::to_string(database_.Recipes().size()) + " combos";

    if (!database_.Complete())
        status += " (partial data - run tools/scrape_poe2db.py)";

    return status;
}

std::string ExpeditionAdvisor::MatchRuneName(const std::string& itemName) const
{
    return database_.MatchRuneName(itemName);
}

bool ExpeditionAdvisor::TakeRune(const std::string& itemName)
{
    const std::string rune = MatchRuneName(itemName);

    if (rune.empty())
        return false;

    inventory_.Add(rune);
    inventory_.Save();
    return true;
}

std::optional<double> ExpeditionAdvisor::RecipeValue(const Recipe& recipe, PriceCache& priceCache)
{
    auto price = priceCache.GetPrice(recipe.output);

    if (!price)
        return std::nullopt;

    auto value = LootParser::ParsePriceValue(*price);

    if (!value)
        return std::nullopt;

    return *value * recipe.count;
}

void ExpeditionAdvisor::Recompute(PriceCache& priceCache)
{
    if (!database_.Loaded())
        return;

    const uint64_t revision = inventory_.Revision();
    const size_t priceCount = priceCache.GetPriceCount();

    {
        std::lock_guard lock(scoreMutex_);
        if (revision == scoredRevision_ && priceCount == scoredPriceCount_ && !scores_.empty())
            return;
    }

    RecomputeLocked(priceCache);

    std::lock_guard lock(scoreMutex_);
    scoredRevision_ = revision;
    scoredPriceCount_ = priceCount;
}

void ExpeditionAdvisor::RecomputeLocked(PriceCache& priceCache)
{
    const std::map<std::string, int> owned = inventory_.Snapshot();

    std::map<std::string, RuneScore> scores;
    std::map<std::string, double> bestContribution;
    std::vector<RecipeProgress> progress;

    for (const auto& rune : database_.RuneNames())
    {
        RuneScore score;
        score.rune = rune;
        scores[rune] = std::move(score);
    }

    for (const auto& recipe : database_.Recipes())
    {
        int totalRequired = 0;
        int totalMissing = 0;
        std::vector<std::string> missingRunes;

        for (const auto& [rune, required] : recipe.runeCounts)
        {
            totalRequired += required;

            const auto it = owned.find(rune);
            const int have = it == owned.end() ? 0 : it->second;
            const int missing = std::max(0, required - have);

            totalMissing += missing;

            for (int i = 0; i < missing; ++i)
                missingRunes.push_back(rune);
        }

        const auto value = RecipeValue(recipe, priceCache);
        const double recipeValue = value.value_or(0.0);

        RecipeProgress rp;
        rp.output = recipe.output;
        rp.category = recipe.category;
        rp.count = recipe.count;
        rp.total = totalRequired;
        rp.have = totalRequired - totalMissing;
        rp.missing = totalMissing;
        rp.value = recipeValue;
        rp.missingRunes = missingRunes;

        if (auto price = priceCache.GetPrice(recipe.output))
            rp.priceText = LootParser::FormatStackPrice(*price, recipe.count);

        progress.push_back(std::move(rp));

        if (totalMissing == 0 || recipeValue <= 0.0)
            continue;

        // Each still-missing rune of this recipe gets a share of the recipe's
        // value, discounted by how far the recipe is from completion.
        const double contribution =
            recipeValue / std::pow(static_cast<double>(totalMissing), kMissingExponent);

        for (const auto& [rune, required] : recipe.runeCounts)
        {
            const auto it = owned.find(rune);
            const int have = it == owned.end() ? 0 : it->second;

            if (required - have <= 0)
                continue; // recipe doesn't need more of this rune

            auto& score = scores[rune];
            score.ev += contribution;

            if (contribution > bestContribution[rune])
            {
                bestContribution[rune] = contribution;
                score.bestRecipe = recipe.output;
                score.bestRecipeMissing = totalMissing;
                score.bestRecipeValue = recipeValue;
            }
        }
    }

    // Rank runes by EV (1 = best).
    std::vector<RuneScore*> ranked;
    ranked.reserve(scores.size());

    for (auto& [rune, score] : scores)
        ranked.push_back(&score);

    std::sort(ranked.begin(), ranked.end(),
        [](const RuneScore* a, const RuneScore* b) { return a->ev > b->ev; });

    for (size_t i = 0; i < ranked.size(); ++i)
        ranked[i]->rank = static_cast<int>(i) + 1;

    // Closest completable recipes: in progress, sorted by (missing asc, value desc).
    std::sort(progress.begin(), progress.end(),
        [](const RecipeProgress& a, const RecipeProgress& b)
        {
            if (a.missing != b.missing)
                return a.missing < b.missing;
            return a.value > b.value;
        });

    std::vector<RecipeProgress> closest;

    for (auto& rp : progress)
    {
        if (rp.have <= 0 && rp.missing > 0)
            continue; // not started

        closest.push_back(rp);

        if (closest.size() >= kClosestRecipesLimit)
            break;
    }

    std::lock_guard lock(scoreMutex_);
    scores_ = std::move(scores);
    closestRecipes_ = std::move(closest);
}

std::optional<RuneScore> ExpeditionAdvisor::ScoreFor(const std::string& runeName) const
{
    std::lock_guard lock(scoreMutex_);

    auto it = scores_.find(runeName);

    if (it == scores_.end())
        return std::nullopt;

    return it->second;
}

std::vector<RuneScore> ExpeditionAdvisor::AllScores() const
{
    std::lock_guard lock(scoreMutex_);

    std::vector<RuneScore> out;
    out.reserve(scores_.size());

    for (const auto& [rune, score] : scores_)
        out.push_back(score);

    std::sort(out.begin(), out.end(),
        [](const RuneScore& a, const RuneScore& b) { return a.ev > b.ev; });

    return out;
}

std::vector<RecipeProgress> ExpeditionAdvisor::ClosestRecipes() const
{
    std::lock_guard lock(scoreMutex_);
    return closestRecipes_;
}
