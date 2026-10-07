#pragma once

#include <vector>
#include <queue>
#include <functional>
#include <cstdint>
#include <cassert>

#include "engine/events.hpp"

namespace engine {
// Event Comparator
// Orders events by exchange timestamp. Ties broken by sequence_id.
// For std::priority_queue (max-heap), we reverse the comparison to get
// a min-heap (earliest event first).
struct EventComparator {
    bool operator()(const MarketEvent& a, const MarketEvent& b) const {
        if (a.exchange_ts != b.exchange_ts) {
            return a.exchange_ts > b.exchange_ts; // Min-heap: smaller ts = higher priority
        }
        return a.sequence_id > b.sequence_id;
    }
};
// Monotonic Cursor
// Represents a single pre-sorted data stream for the K-way merge.
struct MonotonicCursor {
    const MarketEvent* data = nullptr;
    size_t              size = 0;
    size_t              pos  = 0;

    [[nodiscard]] bool exhausted() const { return pos >= size; }
    [[nodiscard]] const MarketEvent& peek() const { return data[pos]; }
    void advance() { ++pos; }
};
// Event Scheduler
// Implements the hybrid scheduling strategy for historical and internal events:
//   - K-way merge of N monotonic cursors for historical data streams
//   - Min-heap for internally-generated events (latency timers, fills)
//
// This avoids O(log N) priority queue overhead for the dominant historical
// data path, which is already sorted.
class EventScheduler {
public:
    // Register a pre-sorted data stream for k-way merge
    void add_stream(const MarketEvent* data, size_t count) {
        cursors_.push_back({data, count, 0});
    }

    // Push an internally-generated event (e.g., latency timer, fill report)
    void push_internal(const MarketEvent& event) {
        internal_events_.push(event);
    }

    // Pop the next chronological event across all sources.
    // Returns true if an event was available, false if all sources are exhausted.
    bool pop(MarketEvent& out) {
        // Find the cursor with the smallest next timestamp
        int best_cursor = -1;
        int64_t best_ts = INT64_MAX;
        uint64_t best_seq = UINT64_MAX;

        for (size_t i = 0; i < cursors_.size(); ++i) {
            if (cursors_[i].exhausted()) continue;
            const auto& ev = cursors_[i].peek();
            if (ev.exchange_ts < best_ts ||
                (ev.exchange_ts == best_ts && ev.sequence_id < best_seq)) {
                best_ts = ev.exchange_ts;
                best_seq = ev.sequence_id;
                best_cursor = static_cast<int>(i);
            }
        }

        // Check internal event heap
        bool has_internal = !internal_events_.empty();
        bool has_stream = (best_cursor >= 0);

        if (!has_internal && !has_stream) {
            return false; // All exhausted
        }

        if (has_internal && has_stream) {
            const auto& internal_top = internal_events_.top();
            if (internal_top.exchange_ts <= best_ts) {
                out = internal_top;
                internal_events_.pop();
            } else {
                out = cursors_[static_cast<size_t>(best_cursor)].peek();
                cursors_[static_cast<size_t>(best_cursor)].advance();
            }
        } else if (has_internal) {
            out = internal_events_.top();
            internal_events_.pop();
        } else {
            out = cursors_[static_cast<size_t>(best_cursor)].peek();
            cursors_[static_cast<size_t>(best_cursor)].advance();
        }

        return true;
    }

    [[nodiscard]] bool empty() const {
        if (!internal_events_.empty()) return false;
        for (const auto& c : cursors_) {
            if (!c.exhausted()) return false;
        }
        return true;
    }

    void clear() {
        cursors_.clear();
        while (!internal_events_.empty()) internal_events_.pop();
    }

    [[nodiscard]] size_t num_streams() const { return cursors_.size(); }

private:
    std::vector<MonotonicCursor> cursors_;
    std::priority_queue<MarketEvent, std::vector<MarketEvent>, EventComparator> internal_events_;
};

} // namespace engine
