#include "ocr/LootOverlayBuilder.h"

#include <cmath>
#include <optional>
#include <utility>

#include "core/Helpers.h"
#include "ocr/LootParser.h"

LootOverlayBuildResult LootOverlayBuilder::Build(
    const std::vector<LootLine>& loot,
    const cv::Rect& region,
    const AppConfig& config,
    PriceCache& priceCache,
    const std::vector<CachedItemName>& cachedNames)
{
    LootOverlayBuildResult result;

    for (const auto& item : loot)
    {
        DebugLine debugLine;
        debugLine.ocrText = item.text;
        debugLine.matchedText = "-";
        debugLine.price = "-";
        debugLine.confidence = 0;

        auto parsed = LootParser::ParseLootLine(item.text);

        std::string rawName = parsed.itemName;
        int quantity = parsed.quantity;

        auto price = priceCache.GetPrice(rawName);

        if (price)
        {
            debugLine.matchedText = rawName;
            debugLine.confidence = 100;
        }
        else
        {
            auto guess = FindBestItemMatch(rawName, cachedNames);

            if (guess)
            {
                debugLine.matchedText = guess->name;
                debugLine.confidence = guess->confidence;
                price = priceCache.GetPrice(guess->name);
            }
        }

        if (!price)
        {
            result.debug.lines.push_back(std::move(debugLine));
            continue;
        }

        debugLine.price = *price;

        std::optional<double> value = LootParser::ParsePriceValue(*price);
        double totalValue = value ? (*value * quantity) : 0.0;

        int overlayY = region.y + (item.y1 + item.y2) / 2 + config.overlayOffsetY;

        if (HasCloseOverlayText(result.texts, overlayY, 25))
        {
            result.debug.lines.push_back(std::move(debugLine));
            continue;
        }

        OverlayText t;
        t.color = GetPriceColor(totalValue, config);
        t.text = ToWide(LootParser::FormatStackPrice(*price, quantity));
        t.x = region.x + region.width + config.overlayOffsetX;
        t.y = region.y + (item.y1 + item.y2) / 2 + config.overlayOffsetY;

        result.texts.push_back(std::move(t));
        result.debug.lines.push_back(std::move(debugLine));
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
