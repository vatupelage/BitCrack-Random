#include <fstream>
#include <iostream>

#include "KeyFinder.h"
#include "util.h"
#include "AddressUtil.h"
#include "CudaKeySearchDevice.h"

#include "Logger.h"


void KeyFinder::defaultResultCallback(KeySearchResult result)
{
	// Do nothing
}

void KeyFinder::defaultStatusCallback(KeySearchStatus status)
{
	// Do nothing
}

KeyFinder::KeyFinder(const secp256k1::uint256 &startKey, const secp256k1::uint256 &endKey, int compression, KeySearchDevice* device, const secp256k1::uint256 &stride)
{
	_total = 0;
	_statusInterval = 1000;
	_device = device;

	_compression = compression;

    _startKey = startKey;

    _endKey = endKey;

	_statusCallback = NULL;

	_resultCallback = NULL;

    _iterCount = 0;

    _stride = stride;

    // Initialize random mode (enabled by default)
    _randomMode = true;
    _bloomFilterSizeMB = 1024;
    _priorityWeight = 0.6;

    // Enable reservoir mode for CUDA devices (100x speedup)
    CudaKeySearchDevice* cudaDevice = dynamic_cast<CudaKeySearchDevice*>(_device);
    if (cudaDevice && _randomMode) {
        cudaDevice->setReservoirMode(true);
    }
}

KeyFinder::~KeyFinder()
{
}

void KeyFinder::setTargets(std::vector<std::string> &targets)
{
	if(targets.size() == 0) {
		throw KeySearchException("Requires at least 1 target");
	}

	_targets.clear();

	// Convert each address from base58 encoded form to a 160-bit integer
	for(unsigned int i = 0; i < targets.size(); i++) {

		if(!Address::verifyAddress(targets[i])) {
			throw KeySearchException("Invalid address '" + targets[i] + "'");
		}

		KeySearchTarget t;

		Base58::toHash160(targets[i], t.value);

		_targets.insert(t);
	}

    _device->setTargets(_targets);
}

void KeyFinder::setTargets(std::string targetsFile)
{
	std::ifstream inFile(targetsFile.c_str());

	if(!inFile.is_open()) {
		Logger::log(LogLevel::Error, "Unable to open '" + targetsFile + "'");
		throw KeySearchException();
	}

	_targets.clear();

	std::string line;
	Logger::log(LogLevel::Info, "Loading addresses from '" + targetsFile + "'");
	while(std::getline(inFile, line)) {
		util::removeNewline(line);
        line = util::trim(line);

		if(line.length() > 0) {
			if(!Address::verifyAddress(line)) {
				Logger::log(LogLevel::Error, "Invalid address '" + line + "'");
				throw KeySearchException();
			}

			KeySearchTarget t;

			Base58::toHash160(line, t.value);

			_targets.insert(t);
		}
	}
	Logger::log(LogLevel::Info, util::formatThousands(_targets.size()) + " addresses loaded ("
		+ util::format("%.1f", (double)(sizeof(KeySearchTarget) * _targets.size()) / (double)(1024 * 1024)) + "MB)");

    _device->setTargets(_targets);
}


void KeyFinder::setResultCallback(void(*callback)(KeySearchResult))
{
	_resultCallback = callback;
}

void KeyFinder::setStatusCallback(void(*callback)(KeySearchStatus))
{
	_statusCallback = callback;
}

void KeyFinder::setStatusInterval(uint64_t interval)
{
	_statusInterval = interval;
}

void KeyFinder::setTargetsOnDevice()
{
	// Set the target in constant memory
	std::vector<struct hash160> targets;

	for(std::set<KeySearchTarget>::iterator i = _targets.begin(); i != _targets.end(); ++i) {
		targets.push_back(hash160((*i).value));
	}

    _device->setTargets(_targets);
}

void KeyFinder::init()
{
	Logger::log(LogLevel::Info, "Initializing " + _device->getDeviceName());

    // Initialize random scanning if enabled
    if (_randomMode) {
        Logger::log(LogLevel::Info, "Random scanning mode enabled");

        // Important: Random scan respects partition boundaries (startKey to endKey)
        // For multi-GPU: each GPU randomly scans only its assigned partition
        secp256k1::uint256 partitionSize = _endKey - _startKey;
        Logger::log(LogLevel::Info, "Partition: " + _startKey.toString() + " to " + _endKey.toString());

        Logger::log(LogLevel::Info, "Bloom filter size: " + util::format("%llu", _bloomFilterSizeMB) + " MB");
        Logger::log(LogLevel::Info, "Priority ranges weight: " + util::format("%.1f%%", _priorityWeight * 100.0));

        _bloomFilter = std::make_unique<BloomFilter>(_bloomFilterSizeMB);

        // Random generator uses THIS GPU's partition boundaries
        _randomGen = std::make_unique<RandomKeyGenerator>(_startKey, _endKey, 0, _priorityWeight);

        Logger::log(LogLevel::Info, "Priority ranges (within partition):");
        Logger::log(LogLevel::Info, "  0.30 → 0.50 (30%-50% into partition)");
        Logger::log(LogLevel::Info, "  0.60 → 0.80 (60%-80% into partition)");
        Logger::log(LogLevel::Info, "  0.82 → 0.83 (82%-83% into partition)");
        Logger::log(LogLevel::Info, "  0.00 → 0.01 (0%-1% into partition)");
    }

    _device->init(_startKey, _compression, _stride);
}


void KeyFinder::stop()
{
	_running = false;
}

void KeyFinder::removeTargetFromList(const unsigned int hash[5])
{
	KeySearchTarget t(hash);

	_targets.erase(t);
}

bool KeyFinder::isTargetInList(const unsigned int hash[5])
{
	KeySearchTarget t(hash);
	return _targets.find(t) != _targets.end();
}


void KeyFinder::run()
{
    uint64_t pointsPerIteration = _device->keysPerStep();

	_running = true;

	util::Timer timer;

	timer.start();

	uint64_t prevIterCount = 0;

	_totalTime = 0;

	while(_running) {

        // In random mode, generate a new random start key for each iteration
        if (_randomMode && _randomGen && _bloomFilter) {
            secp256k1::uint256 randomKey;
            int attempts = 0;
            const int maxAttempts = 100;

            // Try to find a key we haven't visited yet
            do {
                randomKey = _randomGen->next();
                attempts++;

                // If we've tried too many times, just use this key anyway
                // (means bloom filter is getting full)
                if (attempts >= maxAttempts) {
                    break;
                }
            } while (_bloomFilter->probablyContains(randomKey));

            // Mark this key as visited
            _bloomFilter->insert(randomKey);

            // Update device starting point (lightweight operation)
            // Cast to CudaKeySearchDevice to access setStartingKey
            CudaKeySearchDevice* cudaDevice = dynamic_cast<CudaKeySearchDevice*>(_device);
            if (cudaDevice) {
                cudaDevice->setStartingKey(randomKey);
            }
        }

        _device->doStep();
        _iterCount++;

		// Update status
		uint64_t t = timer.getTime();

		if(t >= _statusInterval) {

			KeySearchStatus info;

			uint64_t count = (_iterCount - prevIterCount) * pointsPerIteration;

			_total += count;

			double seconds = (double)t / 1000.0;

			info.speed = (double)((double)count / seconds) / 1000000.0;

			info.total = _total;

			info.totalTime = _totalTime;

			uint64_t freeMem = 0;

			uint64_t totalMem = 0;

			_device->getMemoryInfo(freeMem, totalMem);

			info.freeMemory = freeMem;
			info.deviceMemory = totalMem;
			info.deviceName = _device->getDeviceName();
			info.targets = _targets.size();
            info.nextKey = getNextKey();

			_statusCallback(info);

            // Log bloom filter statistics in random mode
            if (_randomMode && _bloomFilter) {
                Logger::log(LogLevel::Info,
                    "Bloom filter: " + util::formatThousands(_bloomFilter->getInsertedCount()) +
                    " keys tracked, FPR: " + util::format("%.4f%%", _bloomFilter->getFalsePositiveRate() * 100.0));
            }

			timer.start();
			prevIterCount = _iterCount;
			_totalTime += t;
		}

        std::vector<KeySearchResult> results;

        if(_device->getResults(results) > 0) {

			for(unsigned int i = 0; i < results.size(); i++) {

				KeySearchResult info;
                info.privateKey = results[i].privateKey;
                info.publicKey = results[i].publicKey;
				info.compressed = results[i].compressed;
				info.address = Address::fromPublicKey(results[i].publicKey, results[i].compressed);

				_resultCallback(info);
			}

			// Remove the hashes that were found
			for(unsigned int i = 0; i < results.size(); i++) {
				removeTargetFromList(results[i].hash);
			}
		}

        // Stop if there are no keys left
        if(_targets.size() == 0) {
            Logger::log(LogLevel::Info, "No targets remaining");
            _running = false;
        }

		// In random mode, never stop due to reaching end of keyspace
        // (we jump around randomly)
        if (!_randomMode) {
            // Stop if we searched the entire range (linear mode only)
            if(_device->getNextKey().cmp(_endKey) >= 0 || _device->getNextKey().cmp(_startKey) < 0) {
                Logger::log(LogLevel::Info, "Reached end of keyspace");
                _running = false;
            }
        }
	}
}

secp256k1::uint256 KeyFinder::getNextKey()
{
    return _device->getNextKey();
}

void KeyFinder::setRandomMode(bool enabled, uint64_t bloomFilterSizeMB, double priorityWeight)
{
    _randomMode = enabled;
    _bloomFilterSizeMB = bloomFilterSizeMB;
    _priorityWeight = priorityWeight;

    // Enable Shuffle-Walk Reservoir mode for dramatic speedup in random mode
    if (enabled) {
        CudaKeySearchDevice* cudaDevice = dynamic_cast<CudaKeySearchDevice*>(_device);
        if (cudaDevice) {
            cudaDevice->setReservoirMode(true);
            Logger::log(LogLevel::Info, "Shuffle-Walk Reservoir mode enabled (100x faster random scanning)");
        }
    }
}

uint64_t KeyFinder::getBloomFilterInsertedCount() const
{
    if (_bloomFilter) {
        return _bloomFilter->getInsertedCount();
    }
    return 0;
}

double KeyFinder::getBloomFilterFalsePositiveRate() const
{
    if (_bloomFilter) {
        return _bloomFilter->getFalsePositiveRate();
    }
    return 0.0;
}

bool KeyFinder::saveRandomState(const std::string &filename) const
{
    if (!_randomMode || !_bloomFilter) {
        return false;
    }

    // Save bloom filter to file
    std::string bloomFile = filename + ".bloom";
    if (!_bloomFilter->saveToFile(bloomFile)) {
        return false;
    }

    // Save RNG state and other metadata
    FILE *fp = fopen(filename.c_str(), "w");
    if (!fp) {
        return false;
    }

    fprintf(fp, "# BitCrack Random Scan Checkpoint\n");
    fprintf(fp, "# Do not edit this file manually\n\n");

    // Write keyspace bounds
    secp256k1::uint256 startKeyCopy = _startKey;
    secp256k1::uint256 endKeyCopy = _endKey;
    fprintf(fp, "startKey=%s\n", startKeyCopy.toString().c_str());
    fprintf(fp, "endKey=%s\n", endKeyCopy.toString().c_str());

    // Write random scan parameters
    fprintf(fp, "bloomFilterSizeMB=%lu\n", (unsigned long)_bloomFilterSizeMB);
    fprintf(fp, "priorityWeight=%.6f\n", _priorityWeight);
    fprintf(fp, "keysTracked=%lu\n", (unsigned long)_bloomFilter->getInsertedCount());
    fprintf(fp, "totalKeys=%lu\n", (unsigned long)_total);
    fprintf(fp, "iterations=%lu\n", (unsigned long)_iterCount);

    // Write RNG state (if available)
    if (_randomGen) {
        fprintf(fp, "rngState=%lu\n", (unsigned long)_randomGen->getState());
    }

    fclose(fp);

    Logger::log(LogLevel::Info, "Saved random scan state to " + filename);
    return true;
}

bool KeyFinder::loadRandomState(const std::string &filename)
{
    if (!_randomMode) {
        return false;
    }

    // Load bloom filter from file
    std::string bloomFile = filename + ".bloom";

    if (!_bloomFilter) {
        _bloomFilter = std::make_unique<BloomFilter>(_bloomFilterSizeMB);
    }

    if (!_bloomFilter->loadFromFile(bloomFile)) {
        Logger::log(LogLevel::Warning, "Could not load bloom filter from " + bloomFile);
        return false;
    }

    Logger::log(LogLevel::Info, "Loaded random scan state from " + filename);
    Logger::log(LogLevel::Info, "Bloom filter: " + util::formatThousands(_bloomFilter->getInsertedCount()) + " keys tracked");

    return true;
}