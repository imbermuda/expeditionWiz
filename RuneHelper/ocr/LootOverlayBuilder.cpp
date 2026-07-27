#include "ocr/LootOverlayBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ocr/LootParser.h"
#include "recipes/ExpeditionAdvisor.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
std::wstring ToWide(const std::string& s)
{
    if (s.empty())
        return L"";

#ifdef _WIN32
    int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);

    if (size <= 0)
        return L"";

    std::wstring result(size - 1, L'\0');

    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, result.data(), size);

    return result;
#else
    return std::wstring(s.begin(), s.end());
#endif
}

OverlayColor OverlayColorForPrice(double priceEx, const AppConfig& config)
{
    if (priceEx > config.priceColorVeryHigh)
        return OverlayRgb(255, 60, 60);

    if (priceEx > config.priceColorHigh)
        return OverlayRgb(255, 220, 80);

    if (priceEx > config.priceColorMedium)
        return OverlayRgb(80, 255, 80);

    return OverlayRgb(160, 160, 160);
}

std::string FormatEv(double ev)
{
    char buffer[32];

    if (ev >= 100.0)
        std::snprintf(buffer, sizeof(buffer), "%.0fev", ev);
    else
        std::snprintf(buffer, sizeof(buffer), "%.1fev", ev);

    return buffer;
}

struct RowInfo
{
    DebugLine debug;
    std::string name;
    int quantity = 1;
    std::optional<std::string> price;
    double totalValue = 0.0;
    int overlayY = 0;
    std::string rune;
    std::optional<RuneScore> score;
    int screenRank = 0;
};
}

LootOverlayBuildResult LootOverlayBuilder::Build(
    const std::vector<LootLine>& loot,
    const cv::Rect& region,
    const AppConfig& config,
    PriceCache& priceCache,
    const std::vector<CachedItemName>& cachedNames,
    const ExpeditionAdvisor* advisor)
{
    LootOverlayBuildResult result;

    if (advisor && (!config.advisorEnabled || !advisor->Ready()))
        advisor = nullptr;

    // Phase 1: parse, match, price and score every OCR row.
    std::vector<RowInfo> rows;
    rows.reserve(loot.size());

    for (const auto& item : loot)
    {
        RowInfo row;
        row.debug.ocrText = item.text;
        row.debug.matchedText = "-";
        row.debug.price = "-";
        row.debug.confidence = 0;

        auto parsed = LootParser::ParseLootLine(item.text);
        row.name = parsed.itemName;
        row.quantity = parsed.quantity;
        row.overlayY = region.y + (item.y1 + item.y2) / 2 + config.overlayOffsetY;

        if (config.priceSearchEnabled)
        {
            auto price = priceCache.GetPrice(row.name);

            if (price)
            {
                row.debug.matchedText = row.name;
                row.debug.confidence = 100;
            }
            else
            {
                auto guess = FindBestItemMatch(row.name, cachedNames);

                if (guess)
                {
                    row.debug.matchedText = guess->name;
                    row.debug.confidence = guess->confidence;
                    price = priceCache.GetPrice(guess->name);
                }
            }

            if (price)
            {
                row.debug.price = *price;

                std::optional<double> value = LootParser::ParsePriceValue(*price);
                row.totalValue = value ? (*value * row.quantity) : 0.0;
            }

            row.price = std::move(price);
        }

        if (advisor)
        {
            const std::string& matched =
                row.debug.matchedText != "-" ? row.debug.matchedText : row.name;

            row.rune = advisor->MatchRuneName(matched);

            if (row.rune.empty() && matched != row.name)
                row.rune = advisor->MatchRuneName(row.name);

            if (!row.rune.empty())
            {
                auto score = advisor->ScoreFor(row.rune);

                if (score && score->ev > 0.0)
                    row.score = std::move(score);
            }
        }

        rows.push_back(std::move(row));
    }

    // Phase 2: rank the runes visible on screen by marginal EV.
    {
        std::vector<size_t> runeRows;

        for (size_t i = 0; i < rows.size(); ++i)
        {
            if (rows[i].score)
                runeRows.push_back(i);
        }

        std::sort(runeRows.begin(), runeRows.end(),
            [&rows](size_t a, size_t b) { return rows[a].score->ev > rows[b].score->ev; });

        for (size_t rank = 0; rank < runeRows.size(); ++rank)
            rows[runeRows[rank]].screenRank = static_cast<int>(rank) + 1;
    }

    // Phase 3: emit overlay texts.
    for (auto& row : rows)
    {
        if (!row.price && !row.score)
        {
            result.debug.lines.push_back(std::move(row.debug));
            continue;
        }

        if (HasCloseOverlayText(result.texts, row.overlayY, 25))
        {
            result.debug.lines.push_back(std::move(row.debug));
            continue;
        }

        std::string text;

        if (row.price)
            text = LootParser::FormatStackPrice(*row.price, row.quantity);

        if (row.score)
        {
            if (!text.empty())
                text += "  ";

            if (row.screenRank == 1)
                text += "PICK ";

            text += "#" + std::to_string(row.screenRank) + " " + FormatEv(row.score->ev);
        }

        OverlayText t;
        t.color = row.score && row.screenRank == 1
            ? OverlayRgb(80, 220, 255)
            : OverlayColorForPrice(row.totalValue, config);
        t.text = ToWide(text);
        t.x = region.x + region.width + config.overlayOffsetX;
        t.y = row.overlayY;

        result.texts.push_back(std::move(t));
        result.debug.lines.push_back(std::move(row.debug));
    }

    return result;
}

bool LootOverlayBuilder::HasCloseOverlayText(const std::vector<OverlayText>& texts, int y, int minDistance)
{
    for (const auto& t : texts)
    {
        if (std::abs(t.y - y) < minDistance)
            return true;
    }

    return false;
}
