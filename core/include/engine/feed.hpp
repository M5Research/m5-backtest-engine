#pragma once

#include <vector>
#include <string>
#include <fstream>
#include <cstring>
#include <stdexcept>

#include "engine/events.hpp"
#include "engine/symbol_metadata.hpp"

namespace engine {
// Binary archive I/O and depth sequence validation.

// Binary file header for normalized replay archives
struct BinaryArchiveHeader {
    char     magic[8]    = {'C','R','E','A','R','C','H','V'};
    uint32_t version     = 1;
    uint32_t num_events  = 0;
    uint16_t num_symbols = 0;
    uint16_t reserved    = 0;
};

static_assert(sizeof(BinaryArchiveHeader) <= 24, "Header must fit in 24 bytes");
// Sequence Validator
// Validates that depth diff sequence IDs are
// contiguous. The caller handles gaps by flushing and resynchronizing the book.
class SequenceValidator {
public:
    enum class Status {
        OK,
        GAP_DETECTED,
        STALE_DUPLICATE,
        AWAITING_SNAPSHOT
    };

    explicit SequenceValidator(uint16_t symbol_id)
        : symbol_id_(symbol_id) {}

    // Initialize from a depth snapshot's lastUpdateId
    void set_snapshot_id(uint64_t last_update_id) {
        last_update_id_ = last_update_id;
        is_synced_ = true;
    }

    // Validate a depth diff with (U, u) = (first_update_id, final_update_id)
    // Per Binance docs:
    //   Drop diffs where u <= lastUpdateId
    //   First valid diff: U <= lastUpdateId+1 && u >= lastUpdateId+1
    //   Subsequent: U == prev_u + 1
    Status validate(uint64_t first_update_id, uint64_t final_update_id) {
        if (!is_synced_) {
            return Status::AWAITING_SNAPSHOT;
        }

        // Stale diff
        if (final_update_id <= last_update_id_) {
            return Status::STALE_DUPLICATE;
        }

        // First diff after snapshot
        if (!has_first_diff_) {
            if (first_update_id <= last_update_id_ + 1 &&
                final_update_id >= last_update_id_ + 1) {
                has_first_diff_ = true;
                last_update_id_ = final_update_id;
                return Status::OK;
            }
            return Status::GAP_DETECTED;
        }

        // Subsequent diffs: must be contiguous
        if (first_update_id != last_update_id_ + 1) {
            // Gap detected; caller should flush book and resync
            is_synced_ = false;
            has_first_diff_ = false;
            return Status::GAP_DETECTED;
        }

        last_update_id_ = final_update_id;
        return Status::OK;
    }

    void reset() {
        is_synced_ = false;
        has_first_diff_ = false;
        last_update_id_ = 0;
    }

    [[nodiscard]] bool is_synced() const { return is_synced_; }
    [[nodiscard]] uint64_t last_update_id() const { return last_update_id_; }

private:
    uint16_t symbol_id_;
    uint64_t last_update_id_ = 0;
    bool     is_synced_       = false;
    bool     has_first_diff_  = false;
};
// Binary Archive Writer
// Writes normalized MarketEvent arrays to the binary replay format.
class BinaryArchiveWriter {
public:
    explicit BinaryArchiveWriter(const std::string& path)
        : path_(path) {}

    void write(const std::vector<MarketEvent>& events, uint16_t num_symbols) {
        std::ofstream out(path_, std::ios::binary);
        if (!out) {
            throw std::runtime_error("Cannot open archive for writing: " + path_);
        }

        BinaryArchiveHeader header{};
        header.num_events = static_cast<uint32_t>(events.size());
        header.num_symbols = num_symbols;

        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(events.data()),
                  static_cast<std::streamsize>(events.size() * sizeof(MarketEvent)));
    }

private:
    std::string path_;
};
// Binary Archive Reader
// Reads normalized archives. Returns a pointer + count suitable for
// passing directly to EventScheduler::add_stream().
class BinaryArchiveReader {
public:
    explicit BinaryArchiveReader(const std::string& path) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) {
            throw std::runtime_error("Cannot open archive for reading: " + path);
        }

        auto file_size = in.tellg();
        in.seekg(0, std::ios::beg);

        // Read header
        BinaryArchiveHeader header{};
        in.read(reinterpret_cast<char*>(&header), sizeof(header));

        if (std::memcmp(header.magic, "CREARCHV", 8) != 0) {
            throw std::runtime_error("Invalid archive magic number in: " + path);
        }

        num_events_ = header.num_events;
        num_symbols_ = header.num_symbols;

        events_.resize(num_events_);
        in.read(reinterpret_cast<char*>(events_.data()),
                static_cast<std::streamsize>(num_events_ * sizeof(MarketEvent)));
    }

    [[nodiscard]] const MarketEvent* data() const { return events_.data(); }
    [[nodiscard]] size_t size() const { return num_events_; }
    [[nodiscard]] uint16_t num_symbols() const { return num_symbols_; }

private:
    std::vector<MarketEvent> events_;
    uint32_t num_events_  = 0;
    uint16_t num_symbols_ = 0;
};

} // namespace engine
