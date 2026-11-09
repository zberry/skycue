#pragma once

// The per-aircraft state map of the 1090 and 978 decoders: keyed state with an
// LRU list and two ways out, a key limit and idle age. State derives from
// KeyBase. A reference or pointer to a State stays valid until that key is
// evicted (nodes of the unordered_map never move on insert or rehash).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>

#include "decode/decoder_types.hpp"
#include "decode/key_state.hpp"

namespace skycued::decode {

template <class State>
class KeyTable {
public:
    // evicted_counter is the owner's DecodeCounters::evicted; every eviction adds one.
    KeyTable(std::size_t max_keys, std::chrono::seconds idle_evict, uint64_t& evicted_counter)
        : max_keys_(max_keys), idle_evict_(idle_evict), evicted_(evicted_counter) {}

    // nullptr when absent. Does not touch the LRU.
    State* find(const StateKey& k) {
        const auto it = map_.find(k);
        return it == map_.end() ? nullptr : &it->second;
    }

    // Find or create. A new key at max_keys > 0 with the table full first evicts the oldest
    // one. An existing key moves to the young end. Sets last_seen = now and source = src.
    State& obtain(const StateKey& k, skycue::Source src, TimePoint now) {
        State* ks = find(k);
        if (!ks) {
            if (max_keys_ > 0 && map_.size() >= max_keys_ && !lru_.empty()) {
                map_.erase(lru_.front());
                lru_.pop_front();
                ++evicted_;
            }
            lru_.push_back(k);
            ks = &map_[k];
            ks->key = k;
            ks->lru = std::prev(lru_.end());
        } else {
            lru_.splice(lru_.end(), lru_, ks->lru);
        }
        ks->last_seen = now;
        ks->source = src;
        return *ks;
    }

    // Evicts from the old end while now - last_seen > idle_evict.
    void evictIdle(TimePoint now) {
        while (!lru_.empty()) {
            const auto it = map_.find(lru_.front());
            if (it != map_.end() && now - it->second.last_seen <= idle_evict_) break;
            if (it != map_.end()) map_.erase(it);
            lru_.pop_front();
            ++evicted_;
        }
    }

    std::size_t size() const { return map_.size(); }

private:
    std::size_t max_keys_;
    std::chrono::seconds idle_evict_;
    uint64_t& evicted_;
    std::unordered_map<StateKey, State, StateKeyHash> map_;
    std::list<StateKey> lru_;  // oldest first
};

}  // namespace skycued::decode
