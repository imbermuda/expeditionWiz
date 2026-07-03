#include "price/PoeNinjaPriceProvider.h"

#include <iomanip>
#include <sstream>
#include <vector>

#include <cpr/cpr.h>

#include "core/Logger.h"

using json = nlohmann::json;

namespace
{
const std::vector<std::string> kPoeNinjaCategories =
{
    "Runes",
    "Currency",
    "UncutGems",
    "Expedition",
    "Ritual",
    "Breach",
    "Verisium",
    "Idols",
    "SoulCores",
    "Essences",
    "LineageSupportGems",
    "Abyss",
    "Fragments"
};
}

std::unordered_map<std::string, PriceInfo> PoeNinjaPriceProvider::DownloadPrices(const std::string& league)
{
    LOG_INFO("PoeNinjaPriceProvider::DownloadPrices() -> poe.ninja");

    std::unordered_map<std::string, PriceInfo> result;
    result.reserve(512);

    for (const auto& category : kPoeNinjaCategories)
    {
        auto dump = DownloadCategory(league, category);

        LOG_INFO("Downloaded " + category + ": " + std::to_string(dump.size()));

        for (auto& [name, info] : dump)
            result[name] = std::move(info);
    }

    LOG_INFO("PoeNinjaPriceProvider::DownloadPrices() -> total prices: " + std::to_string(result.size()));

    return result;
}

std::string PoeNinjaPriceProvider::EncodeUrlComponent(const std::string& text)
{
    std::ostringstream out;
    out << std::uppercase << std::hex;

    for (unsigned char ch : text)
    {
        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~')
        {
            out << static_cast<char>(ch);
        }
        else if (ch == ' ')
        {
            out << '+';
        }
        else
        {
            out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }

    return out.str();
}

std::string PoeNinjaPriceProvider::FormatExPrice(double value)
{
    std::ostringstream ss;

    if (value >= 100.0)
        ss << std::fixed << std::setprecision(0);
    else if (value >= 10.0)
        ss << std::fixed << std::setprecision(1);
    else
        ss << std::fixed << std::setprecision(2);

    ss << value << " ex";

    return ss.str();
}

std::unordered_map<std::string, PriceInfo>
PoeNinjaPriceProvider::DownloadCategory(const std::string& league, const std::string& type)
{
    const std::string encodedLeague = EncodeUrlComponent(league);
    const std::string url = "https://poe.ninja/poe2/api/economy/exchange/current/overview?league=" + encodedLeague + "&type=" + type;

    LOG_INFO("PoeNinjaPriceProvider::DownloadCategory() -> " + url);

    auto r = cpr::Get(
        cpr::Url{ url },
        cpr::Header{
            { "User-Agent", "RuneHelper/1.0" },
            { "Accept", "application/json" },
            { "Referer", "https://poe.ninja/poe2/economy/" }
        },
        cpr::Timeout{ 15000 }
    );

    LOG_INFO("PoeNinjaPriceProvider::DownloadCategory() HTTP: " + std::to_string(r.status_code) + " bytes=" + std::to_string(r.text.size()));

    if (r.error.code != cpr::ErrorCode::OK)
    {
        LOG_ERROR("PoeNinjaPriceProvider CPR error: code=" + std::to_string(static_cast<int>(r.error.code)) + " message=" + r.error.message);
        return {};
    }

    if (r.status_code != 200)
    {
        LOG_ERROR("PoeNinjaPriceProvider HTTP error: " + std::to_string(r.status_code));
        return {};
    }

    json j = json::parse(r.text, nullptr, false);

    if (j.is_discarded())
    {
        LOG_ERROR("PoeNinjaPriceProvider JSON parse failed");
        return {};
    }

    return ParseCategoryDump(j);
}

std::unordered_map<std::string, PriceInfo> PoeNinjaPriceProvider::ParseCategoryDump(const json& j)
{
    std::unordered_map<std::string, PriceInfo> result;

    if (!j.contains("core") ||
        !j["core"].contains("rates") ||
        !j.contains("items") ||
        !j["items"].is_array() ||
        !j.contains("lines") ||
        !j["lines"].is_array())
    {
        LOG_ERROR("PoeNinjaPriceProvider::ParseCategoryDump() invalid JSON structure");
        return result;
    }

    double divineToEx = j["core"]["rates"].value("exalted", 0.0);

    if (divineToEx <= 0.0)
    {
        LOG_ERROR("PoeNinjaPriceProvider::ParseCategoryDump() invalid exalted rate");
        return result;
    }

    std::unordered_map<std::string, std::string> idToName;
    idToName.reserve(j["items"].size());

    for (const auto& item : j["items"])
    {
        std::string id = item.value("id", "");
        std::string name = item.value("name", "");

        if (!id.empty() && !name.empty())
            idToName.emplace(std::move(id), std::move(name));
    }

    result.reserve(j["lines"].size());
    for (const auto& line : j["lines"])
    {
        std::string id = line.value("id", "");

        if (id.empty())
            continue;

        auto it = idToName.find(id);

        if (it == idToName.end())
            continue;

        double primaryValue = line.value("primaryValue", 0.0);

        if (primaryValue <= 0.0)
            continue;

        double exValue = primaryValue * divineToEx;

        result[it->second] = PriceInfo{
            FormatExPrice(exValue)
        };
    }

    LOG_INFO("PoeNinjaPriceProvider::ParseCategoryDump() parsed prices: " + std::to_string(result.size()));

    return result;
}
