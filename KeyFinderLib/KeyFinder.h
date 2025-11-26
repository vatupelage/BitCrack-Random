#ifndef _KEY_FINDER_H
#define _KEY_FINDER_H

#include <stdint.h>
#include <vector>
#include <set>
#include <memory>
#include "secp256k1.h"
#include "KeySearchTypes.h"
#include "KeySearchDevice.h"
#include "RandomKeyGenerator.h"
#include "BloomFilter.h"


class KeyFinder {

private:

    KeySearchDevice *_device;

	unsigned int _compression;

	std::set<KeySearchTarget> _targets;

	uint64_t _statusInterval;

    secp256k1::uint256 _stride = 1;
	uint64_t _iterCount;
	uint64_t _total;
	uint64_t _totalTime;

    secp256k1::uint256 _startKey;
    secp256k1::uint256 _endKey;

    // Random scanning components
    std::unique_ptr<RandomKeyGenerator> _randomGen;
    std::unique_ptr<BloomFilter> _bloomFilter;
    bool _randomMode;
    uint64_t _bloomFilterSizeMB;
    double _priorityWeight;

	// Each index of each thread gets a flag to indicate if it found a valid hash
	bool _running;

	void(*_resultCallback)(KeySearchResult);
	void(*_statusCallback)(KeySearchStatus);


	static void defaultResultCallback(KeySearchResult result);
	static void defaultStatusCallback(KeySearchStatus status);

	void removeTargetFromList(const unsigned int value[5]);
	bool isTargetInList(const unsigned int value[5]);
	void setTargetsOnDevice();

public:

    KeyFinder(const secp256k1::uint256 &startKey, const secp256k1::uint256 &endKey, int compression, KeySearchDevice* device, const secp256k1::uint256 &stride);

	~KeyFinder();

	void init();
	void run();
	void stop();

	void setResultCallback(void(*callback)(KeySearchResult));
	void setStatusCallback(void(*callback)(KeySearchStatus));
	void setStatusInterval(uint64_t interval);

	void setTargets(std::string targetFile);
	void setTargets(std::vector<std::string> &targets);

    secp256k1::uint256 getNextKey();

    // Random scanning methods
    void setRandomMode(bool enabled, uint64_t bloomFilterSizeMB = 1024, double priorityWeight = 0.6);
    bool isRandomMode() const { return _randomMode; }
    uint64_t getBloomFilterInsertedCount() const;
    double getBloomFilterFalsePositiveRate() const;
    bool saveRandomState(const std::string &filename) const;
    bool loadRandomState(const std::string &filename);
};

#endif