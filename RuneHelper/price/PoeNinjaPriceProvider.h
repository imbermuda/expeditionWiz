#pragma once

#include "price/PriceProvider.h"

#include <string>
#include <unordered_map>

#include "nlohmann/json.hpp"

class PoeNinjaPriceProvider final : public PriceProvider
{
public:
    std::unordered_map<std::string, PriceInfo> DownloadPrices(const std::string& league) override;

private:
    static std::string EncodeUrlComponent(const std::string& text);
    static std::string FormatExPrice(double value);

    std::unordered_map<std::string, PriceInfo> DownloadCategory(const std::string& league, const std::string& type);
    std::unordered_map<std::string, PriceInfo> ParseCategoryDump(const nlohmann::json& j);
};
