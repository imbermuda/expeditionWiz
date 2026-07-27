#include "recipes/RuneInventory.h"

#include <fstream>

#include "nlohmann/json.hpp"

#include "core/Logger.h"
#include "platform/PlatformPaths.h"

using json = nlohmann::json;

namespace
{
std::filesystem::path InventoryPath()
{
    return GetAppDataDir() / "rune_inventory.json";
}
}

bool RuneInventory::Load()
{
    std::ifstream file(InventoryPath());

    if (!file)
        return false;

    json j = json::parse(file, nullptr, false);

    if (j.is_discarded() || !j.is_object())
        return false;

    std::lock_guard lock(mutex_);
    runes_.clear();

    for (const auto& [key, value] : j.items())
    {
        if (value.is_number_integer() && value.get<int>() > 0)
            runes_[key] = value.get<int>();
    }

    ++revision_;
    return true;
}

bool RuneInventory::Save() const
{
    json j = json::object();

    {
        std::lock_guard lock(mutex_);
        for (const auto& [rune, count] : runes_)
        {
            if (count > 0)
                j[rune] = count;
        }
    }

    std::ofstream file(InventoryPath());

    if (!file)
    {
        LOG_ERROR("RuneInventory: failed to open " + InventoryPath().string() + " for writing");
        return false;
    }

    file << j.dump(4);
    return true;
}

void RuneInventory::Add(const std::string& rune, int amount)
{
    if (rune.empty() || amount <= 0)
        return;

    std::lock_guard lock(mutex_);
    runes_[rune] += amount;
    ++revision_;
}

void RuneInventory::Remove(const std::string& rune, int amount)
{
    if (rune.empty() || amount <= 0)
        return;

    std::lock_guard lock(mutex_);

    auto it = runes_.find(rune);

    if (it == runes_.end())
        return;

    it->second -= amount;

    if (it->second <= 0)
        runes_.erase(it);

    ++revision_;
}

void RuneInventory::Set(const std::string& rune, int amount)
{
    if (rune.empty())
        return;

    std::lock_guard lock(mutex_);

    if (amount <= 0)
        runes_.erase(rune);
    else
        runes_[rune] = amount;

    ++revision_;
}

void RuneInventory::Clear()
{
    std::lock_guard lock(mutex_);
    runes_.clear();
    ++revision_;
}

int RuneInventory::Count(const std::string& rune) const
{
    std::lock_guard lock(mutex_);
    auto it = runes_.find(rune);
    return it == runes_.end() ? 0 : it->second;
}

int RuneInventory::TotalRunes() const
{
    std::lock_guard lock(mutex_);
    int total = 0;
    for (const auto& [rune, count] : runes_)
        total += count;
    return total;
}

std::map<std::string, int> RuneInventory::Snapshot() const
{
    std::lock_guard lock(mutex_);
    return runes_;
}

uint64_t RuneInventory::Revision() const
{
    std::lock_guard lock(mutex_);
    return revision_;
}
