#ifndef _BLOOM_FILTER_H
#define _BLOOM_FILTER_H

#include <vector>
#include <cstdint>
#include <cstring>
#include "secp256k1.h"

/**
 * Bloom filter for tracking visited keys in random keyspace scanning.
 *
 * Uses multiple hash functions to achieve low false positive rate.
 * Designed for billions of keys with ~1-2GB memory footprint.
 *
 * False positive rate formula: (1 - e^(-k*n/m))^k
 * where k = number of hash functions, n = number of elements, m = bit array size
 */
class BloomFilter {
private:
    std::vector<uint8_t> _bits;        // Bit array for bloom filter
    uint64_t _bitCount;                 // Total number of bits
    uint32_t _hashFunctions;            // Number of hash functions (k)
    uint64_t _insertedCount;            // Number of keys inserted

    /**
     * Hash function using MurmurHash3-style mixing
     */
    uint64_t hash(const secp256k1::uint256 &key, uint32_t seed) const {
        uint32_t words[8];
        key.exportWords(words, 8, secp256k1::uint256::LittleEndian);

        uint64_t h = seed;
        for (int i = 0; i < 8; i++) {
            h ^= words[i];
            h *= 0xc6a4a7935bd1e995ULL;
            h ^= (h >> 47);
        }
        return h;
    }

    /**
     * Set a bit in the filter
     */
    void setBit(uint64_t index) {
        _bits[index / 8] |= (1 << (index % 8));
    }

    /**
     * Check if a bit is set
     */
    bool getBit(uint64_t index) const {
        return (_bits[index / 8] & (1 << (index % 8))) != 0;
    }

public:
    /**
     * Constructor
     * @param sizeInMB Size of bloom filter in megabytes (default 1024 MB = 1 GB)
     * @param hashFunctions Number of hash functions (default 7 for ~1% FPR)
     */
    BloomFilter(uint64_t sizeInMB = 1024, uint32_t hashFunctions = 7)
        : _hashFunctions(hashFunctions), _insertedCount(0) {

        _bitCount = sizeInMB * 1024 * 1024 * 8; // Convert MB to bits
        _bits.resize(sizeInMB * 1024 * 1024, 0);
    }

    /**
     * Insert a key into the bloom filter
     */
    void insert(const secp256k1::uint256 &key) {
        for (uint32_t i = 0; i < _hashFunctions; i++) {
            uint64_t h = hash(key, i);
            uint64_t index = h % _bitCount;
            setBit(index);
        }
        _insertedCount++;
    }

    /**
     * Check if a key was likely visited before
     * @return true if definitely NOT visited, false if probably visited
     */
    bool probablyContains(const secp256k1::uint256 &key) const {
        for (uint32_t i = 0; i < _hashFunctions; i++) {
            uint64_t h = hash(key, i);
            uint64_t index = h % _bitCount;
            if (!getBit(index)) {
                return false; // Definitely not in set
            }
        }
        return true; // Probably in set
    }

    /**
     * Clear all entries
     */
    void clear() {
        std::memset(_bits.data(), 0, _bits.size());
        _insertedCount = 0;
    }

    /**
     * Get estimated false positive rate
     */
    double getFalsePositiveRate() const {
        if (_insertedCount == 0) return 0.0;

        // FPR = (1 - e^(-k*n/m))^k
        double exponent = -(double)_hashFunctions * (double)_insertedCount / (double)_bitCount;
        double base = 1.0 - exp(exponent);
        double fpr = pow(base, (double)_hashFunctions);
        return fpr;
    }

    /**
     * Get number of keys inserted
     */
    uint64_t getInsertedCount() const {
        return _insertedCount;
    }

    /**
     * Get memory usage in bytes
     */
    uint64_t getMemoryUsage() const {
        return _bits.size();
    }

    /**
     * Save bloom filter to file
     */
    bool saveToFile(const std::string &filename) const {
        FILE *fp = fopen(filename.c_str(), "wb");
        if (!fp) return false;

        // Write header
        fwrite(&_bitCount, sizeof(_bitCount), 1, fp);
        fwrite(&_hashFunctions, sizeof(_hashFunctions), 1, fp);
        fwrite(&_insertedCount, sizeof(_insertedCount), 1, fp);

        // Write bit array
        fwrite(_bits.data(), 1, _bits.size(), fp);

        fclose(fp);
        return true;
    }

    /**
     * Load bloom filter from file
     */
    bool loadFromFile(const std::string &filename) {
        FILE *fp = fopen(filename.c_str(), "rb");
        if (!fp) return false;

        // Read header
        uint64_t bitCount;
        uint32_t hashFunctions;
        uint64_t insertedCount;

        if (fread(&bitCount, sizeof(bitCount), 1, fp) != 1 ||
            fread(&hashFunctions, sizeof(hashFunctions), 1, fp) != 1 ||
            fread(&insertedCount, sizeof(insertedCount), 1, fp) != 1) {
            fclose(fp);
            return false;
        }

        // Validate and resize
        _bitCount = bitCount;
        _hashFunctions = hashFunctions;
        _insertedCount = insertedCount;
        _bits.resize(_bitCount / 8);

        // Read bit array
        size_t bytesRead = fread(_bits.data(), 1, _bits.size(), fp);
        fclose(fp);

        return bytesRead == _bits.size();
    }
};

#endif // _BLOOM_FILTER_H
