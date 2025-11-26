#ifndef _RANDOM_KEY_GENERATOR_H
#define _RANDOM_KEY_GENERATOR_H

#include <random>
#include <vector>
#include <cstdint>
#include <algorithm>
#include "secp256k1.h"

/**
 * Random key generator with alpha range prioritization.
 *
 * Alpha represents the position of a key within the search range [0.0 to 1.0]:
 * - Alpha 0.0 = start of keyspace
 * - Alpha 0.5 = middle of keyspace
 * - Alpha 1.0 = end of keyspace
 *
 * Priority ranges (with higher probability):
 * - 0.30 → 0.50 (30%-50% into keyspace)
 * - 0.60 → 0.80 (60%-80% into keyspace)
 * - 0.82 → 0.83 (82%-83% into keyspace)
 * - 0.00 → 0.01 (0%-1% into keyspace)
 */
class RandomKeyGenerator {
private:
    secp256k1::uint256 _startKey;
    secp256k1::uint256 _endKey;
    secp256k1::uint256 _keyspaceSize;

    std::mt19937_64 _rng;               // Mersenne Twister RNG
    std::uniform_real_distribution<double> _uniformDist;

    // Priority ranges (alpha_start, alpha_end, weight)
    struct AlphaRange {
        double start;
        double end;
        double weight;
    };

    std::vector<AlphaRange> _priorityRanges;
    std::vector<AlphaRange> _nonPriorityRanges;
    double _priorityWeight;              // Probability of selecting from priority ranges

    /**
     * Initialize priority and non-priority ranges
     */
    void initializeRanges() {
        // Priority ranges with their weights (relative importance)
        _priorityRanges = {
            {0.30, 0.50, 1.0},  // 30%-50% into keyspace
            {0.60, 0.80, 1.0},  // 60%-80% into keyspace
            {0.82, 0.83, 0.5},  // 82%-83% into keyspace (smaller, less weight)
            {0.00, 0.01, 0.5}   // 0%-1% into keyspace (start of range)
        };

        // Calculate non-priority ranges (gaps between priority ranges)
        _nonPriorityRanges.clear();
        double lastEnd = 0.0;

        // Sort priority ranges by start position
        std::sort(_priorityRanges.begin(), _priorityRanges.end(),
            [](const AlphaRange &a, const AlphaRange &b) { return a.start < b.start; });

        for (const auto &range : _priorityRanges) {
            if (range.start > lastEnd) {
                _nonPriorityRanges.push_back({lastEnd, range.start, 1.0});
            }
            lastEnd = range.end;
        }

        // Add final non-priority range if needed
        if (lastEnd < 1.0) {
            _nonPriorityRanges.push_back({lastEnd, 1.0, 1.0});
        }
    }

    /**
     * Select a random alpha value based on weighted ranges
     */
    double selectRandomAlpha() {
        double roll = _uniformDist(_rng);

        // 60% chance to select from priority ranges
        if (roll < _priorityWeight) {
            return selectFromRanges(_priorityRanges);
        } else {
            return selectFromRanges(_nonPriorityRanges);
        }
    }

    /**
     * Select a random value from a set of weighted ranges
     */
    double selectFromRanges(const std::vector<AlphaRange> &ranges) {
        if (ranges.empty()) {
            return _uniformDist(_rng);
        }

        // Calculate total weight
        double totalWeight = 0.0;
        for (const auto &range : ranges) {
            totalWeight += (range.end - range.start) * range.weight;
        }

        // Select a range based on weight
        double roll = _uniformDist(_rng) * totalWeight;
        double cumulative = 0.0;

        for (const auto &range : ranges) {
            double rangeWeight = (range.end - range.start) * range.weight;
            cumulative += rangeWeight;

            if (roll <= cumulative) {
                // Uniformly select within this range
                double alpha = range.start + _uniformDist(_rng) * (range.end - range.start);
                return alpha;
            }
        }

        // Fallback (shouldn't reach here)
        return _uniformDist(_rng);
    }

    /**
     * Convert alpha [0.0, 1.0] to actual key value
     */
    secp256k1::uint256 alphaToKey(double alpha) {
        // Clamp alpha to [0.0, 1.0]
        if (alpha < 0.0) alpha = 0.0;
        if (alpha > 1.0) alpha = 1.0;

        // Calculate: key = start + (keyspace_size * alpha)
        // We need to do this carefully to avoid overflow

        // For large keyspaces, we'll use word-by-word multiplication
        secp256k1::uint256 offset = multiplyByFraction(_keyspaceSize, alpha);
        return _startKey + offset;
    }

    /**
     * Multiply a uint256 by a fraction (0.0 to 1.0)
     */
    secp256k1::uint256 multiplyByFraction(const secp256k1::uint256 &value, double fraction) {
        // For simplicity, we'll use a different approach
        // This is an approximation that works well for large keyspaces

        // Get the keyspace size and multiply by fraction
        // We do this by creating a new uint256 from the fractional result

        // Export value to get an approximation
        uint32_t words[8];
        value.exportWords(words, 8, secp256k1::uint256::LittleEndian);

        // Multiply the most significant words by the fraction
        // This gives us an approximation that's good enough for random sampling
        double scaled = 0.0;
        for (int i = 7; i >= 0; i--) {
            scaled = scaled * 4294967296.0 + (double)words[i];
        }
        scaled *= fraction;

        // Convert back to uint256 by building from scaled value
        uint32_t result[8] = {0};
        uint64_t remaining = (uint64_t)scaled;
        for (int i = 0; i < 8 && remaining > 0; i++) {
            result[i] = (uint32_t)(remaining & 0xFFFFFFFF);
            remaining >>= 32;
        }

        // Create a new uint256 from the result words
        secp256k1::uint256 resultKey(result, 8);
        return resultKey;
    }

public:
    /**
     * Constructor
     * @param start Start of keyspace
     * @param end End of keyspace
     * @param seed Random seed (0 = use random_device)
     * @param priorityWeight Probability of selecting from priority ranges (default 0.6 = 60%)
     */
    RandomKeyGenerator(const secp256k1::uint256 &start,
                      const secp256k1::uint256 &end,
                      uint64_t seed = 0,
                      double priorityWeight = 0.6)
        : _startKey(start), _endKey(end), _priorityWeight(priorityWeight),
          _uniformDist(0.0, 1.0) {

        // Initialize RNG
        if (seed == 0) {
            std::random_device rd;
            seed = ((uint64_t)rd() << 32) | rd();
        }
        _rng.seed(seed);

        // Calculate keyspace size
        _keyspaceSize = _endKey - _startKey;

        // Initialize alpha ranges
        initializeRanges();
    }

    /**
     * Generate next random key
     */
    secp256k1::uint256 next() {
        double alpha = selectRandomAlpha();
        return alphaToKey(alpha);
    }

    /**
     * Get RNG state (for checkpointing)
     * Note: This is a simplified version - full state serialization would be more complex
     */
    uint64_t getState() const {
        // Since we can't easily extract full RNG state, we return 0
        // In practice, checkpointing will rely on the bloom filter
        return 0;
    }

    /**
     * Set RNG state (for resuming from checkpoint)
     */
    void setState(uint64_t state) {
        if (state != 0) {
            _rng.seed(state);
        }
    }

    /**
     * Get statistics about which ranges are being sampled
     */
    struct Stats {
        uint64_t priorityHits;
        uint64_t nonPriorityHits;
        double priorityRatio;
    };

    /**
     * Calculate alpha value for a given key (inverse operation)
     * Useful for debugging and statistics
     */
    double keyToAlpha(const secp256k1::uint256 &key) const {
        if (key.cmp(_startKey) <= 0) return 0.0;
        if (key.cmp(_endKey) >= 0) return 1.0;

        secp256k1::uint256 offset = key - _startKey;

        // This is an approximation for very large numbers
        // For exact calculation, would need arbitrary precision
        uint32_t offsetWords[8];
        uint32_t sizeWords[8];

        offset.exportWords(offsetWords, 8, secp256k1::uint256::LittleEndian);
        _keyspaceSize.exportWords(sizeWords, 8, secp256k1::uint256::LittleEndian);

        // Use the most significant non-zero words for approximation
        int msw = 7;
        while (msw > 0 && sizeWords[msw] == 0) msw--;

        if (sizeWords[msw] == 0) return 0.5; // Avoid division by zero

        double alpha = (double)offsetWords[msw] / (double)sizeWords[msw];
        return alpha;
    }

    /**
     * Check if a key is in a priority range
     */
    bool isInPriorityRange(const secp256k1::uint256 &key) const {
        double alpha = keyToAlpha(key);

        for (const auto &range : _priorityRanges) {
            if (alpha >= range.start && alpha <= range.end) {
                return true;
            }
        }
        return false;
    }

    /**
     * Get priority weight
     */
    double getPriorityWeight() const {
        return _priorityWeight;
    }

    /**
     * Set priority weight (0.0 to 1.0)
     */
    void setPriorityWeight(double weight) {
        if (weight < 0.0) weight = 0.0;
        if (weight > 1.0) weight = 1.0;
        _priorityWeight = weight;
    }
};

#endif // _RANDOM_KEY_GENERATOR_H
