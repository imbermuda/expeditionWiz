#pragma once

#include <map>
#include <mutex>
#include <string>

class RuneInventory
{
public:
    bool Load();
    bool Save() const;

    void Add(const std::string& rune, int amount = 1);
    void Remove(const std::string& rune, int amount = 1);
    void Set(const std::string& rune, int amount);
    void Clear();

    int Count(const std::string& rune) const;
    int TotalRunes() const;
    std::map<std::string, int> Snapshot() const;

    // increments whenever the inventory changes; cheap dirty-check for scoring
    uint64_t Revision() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, int> runes_;
    uint64_t revision_ = 0;
};
