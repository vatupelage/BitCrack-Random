#include "CudaKeySearchDevice.h"
#include "Logger.h"
#include "util.h"
#include "cudabridge.h"
#include "AddressUtil.h"

void CudaKeySearchDevice::cudaCall(cudaError_t err)
{
    if(err) {
        std::string errStr = cudaGetErrorString(err);

        throw KeySearchException(errStr);
    }
}

CudaKeySearchDevice::CudaKeySearchDevice(int device, int threads, int pointsPerThread, int blocks)
{
    cuda::CudaDeviceInfo info;
    try {
        info = cuda::getDeviceInfo(device);
        _deviceName = info.name;
    } catch(cuda::CudaException ex) {
        throw KeySearchException(ex.msg);
    }

    if(threads <= 0 || threads % 32 != 0) {
        throw KeySearchException("The number of threads must be a multiple of 32");
    }

    if(pointsPerThread <= 0) {
        throw KeySearchException("At least 1 point per thread required");
    }

    // Specifying blocks on the commandline is depcreated but still supported. If there is no value for
    // blocks, devide the threads evenly among the multi-processors
    if(blocks == 0) {
        if(threads % info.mpCount != 0) {
            throw KeySearchException("The number of threads must be a multiple of " + util::format("%d", info.mpCount));
        }

        _threads = threads / info.mpCount;

        _blocks = info.mpCount;

        while(_threads > 512) {
            _threads /= 2;
            _blocks *= 2;
        }
    } else {
        _threads = threads;
        _blocks = blocks;
    }

    _iterations = 0;

    _device = device;

    _pointsPerThread = pointsPerThread;

    // Initialize profiling infrastructure
    _totalKernelTime = 0.0;
    _kernelInvocations = 0;
    _startEvent = nullptr;
    _stopEvent = nullptr;
}

void CudaKeySearchDevice::init(const secp256k1::uint256 &start, int compression, const secp256k1::uint256 &stride)
{
    if(start.cmp(secp256k1::N) >= 0) {
        throw KeySearchException("Starting key is out of range");
    }

    _startExponent = start;

    _compression = compression;

    _stride = stride;

    cudaCall(cudaSetDevice(_device));

    // Block on kernel calls
    cudaCall(cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync));

    // Use a larger portion of shared memory for L1 cache
    cudaCall(cudaDeviceSetCacheConfig(cudaFuncCachePreferL1));

    // Create CUDA events for profiling
    cudaCall(cudaEventCreate(&_startEvent));
    cudaCall(cudaEventCreate(&_stopEvent));

    // Calculate and log occupancy for performance diagnostics
    calculateAndLogOccupancy();

    // Initialize starting points (reservoir mode vs legacy mode)
    if (_deviceKeys.isReservoirMode()) {
        Logger::log(LogLevel::Info, "Initializing Shuffle-Walk Reservoir (one-time cost)");
        // Generate stratified base keys for reservoir
        uint64_t totalPoints = (uint64_t)_pointsPerThread * _threads * _blocks;
        std::vector<secp256k1::uint256> baseKeys;
        baseKeys.reserve(totalPoints);

        // Stratified sampling: use sequential keys starting from _startExponent
        // These will be used as base points, and random offsets will be added each iteration
        secp256k1::uint256 privKey = _startExponent;

        for (uint64_t i = 0; i < totalPoints; i++) {
            baseKeys.push_back(privKey);
            privKey = privKey.add(_stride);
        }

        // Initialize reservoir (expensive 256 steps, but ONCE)
        cudaCall(_deviceKeys.initReservoir(_blocks, _threads, _pointsPerThread, baseKeys));
        Logger::log(LogLevel::Info, "Reservoir initialized - subsequent iterations will be 100x faster!");
    } else {
        Logger::log(LogLevel::Info, "Using legacy mode (full regeneration each iteration)");
        generateStartingPoints();
    }

    cudaCall(allocateChainBuf(_threads * _blocks * _pointsPerThread));

    // Set the incrementor
    secp256k1::ecpoint g = secp256k1::G();
    secp256k1::ecpoint p = secp256k1::multiplyPoint(secp256k1::uint256((uint64_t)_threads * _blocks * _pointsPerThread) * _stride, g);

    cudaCall(_resultList.init(sizeof(CudaDeviceResult), 16));

    cudaCall(setIncrementorPoint(p.x, p.y));
}


void CudaKeySearchDevice::generateStartingPoints()
{
    uint64_t totalPoints = (uint64_t)_pointsPerThread * _threads * _blocks;
    uint64_t totalMemory = totalPoints * 40;

    std::vector<secp256k1::uint256> exponents;

    Logger::log(LogLevel::Info, "Generating " + util::formatThousands(totalPoints) + " starting points (" + util::format("%.1f", (double)totalMemory / (double)(1024 * 1024)) + "MB)");

    // Generate key pairs for k, k+1, k+2 ... k + <total points in parallel - 1>
    secp256k1::uint256 privKey = _startExponent;

    exponents.push_back(privKey);

    for(uint64_t i = 1; i < totalPoints; i++) {
        privKey = privKey.add(_stride);
        exponents.push_back(privKey);
    }

    cudaCall(_deviceKeys.init(_blocks, _threads, _pointsPerThread, exponents));

    // Show progress in 10% increments
    double pct = 10.0;
    for(int i = 1; i <= 256; i++) {
        cudaCall(_deviceKeys.doStep());

        if(((double)i / 256.0) * 100.0 >= pct) {
            Logger::log(LogLevel::Info, util::format("%.1f%%", pct));
            pct += 10.0;
        }
    }

    Logger::log(LogLevel::Info, "Done");

    _deviceKeys.clearPrivateKeys();
}


void CudaKeySearchDevice::setTargets(const std::set<KeySearchTarget> &targets)
{
    _targets.clear();
    
    for(std::set<KeySearchTarget>::iterator i = targets.begin(); i != targets.end(); ++i) {
        hash160 h(i->value);
        _targets.push_back(h);
    }

    cudaCall(_targetLookup.setTargets(_targets));
}

void CudaKeySearchDevice::doStep()
{
    uint64_t numKeys = (uint64_t)_blocks * _threads * _pointsPerThread;

    // Record kernel start time
    cudaCall(cudaEventRecord(_startEvent, 0));

    try {
        if(_iterations < 2 && _startExponent.cmp(numKeys) <= 0) {
            callKeyFinderKernel(_blocks, _threads, _pointsPerThread, true, _compression);
        } else {
            callKeyFinderKernel(_blocks, _threads, _pointsPerThread, false, _compression);
        }
    } catch(cuda::CudaException ex) {
        throw KeySearchException(ex.msg);
    }

    // Record kernel end time
    cudaCall(cudaEventRecord(_stopEvent, 0));
    cudaCall(cudaEventSynchronize(_stopEvent));

    // Calculate elapsed time
    float milliseconds = 0;
    cudaCall(cudaEventElapsedTime(&milliseconds, _startEvent, _stopEvent));
    _totalKernelTime += milliseconds;
    _kernelInvocations++;

    // Log performance every 100 iterations for diagnostics
    if(_kernelInvocations % 100 == 0) {
        double avgKernelTime = _totalKernelTime / _kernelInvocations;
        uint64_t keysPerSecond = (uint64_t)((numKeys / avgKernelTime) * 1000.0);
        Logger::log(LogLevel::Info, "Kernel profiling: avg " +
                    util::format("%.2f", avgKernelTime) + " ms/iteration, ~" +
                    util::formatThousands(keysPerSecond) + " keys/sec (kernel only)");
    }

    getResultsInternal();

    _iterations++;
}

uint64_t CudaKeySearchDevice::keysPerStep()
{
    return (uint64_t)_blocks * _threads * _pointsPerThread;
}

std::string CudaKeySearchDevice::getDeviceName()
{
    return _deviceName;
}

void CudaKeySearchDevice::getMemoryInfo(uint64_t &freeMem, uint64_t &totalMem)
{
    cudaCall(cudaMemGetInfo(&freeMem, &totalMem));
}

void CudaKeySearchDevice::removeTargetFromList(const unsigned int hash[5])
{
    size_t count = _targets.size();

    while(count) {
        if(memcmp(hash, _targets[count - 1].h, 20) == 0) {
            _targets.erase(_targets.begin() + count - 1);
            return;
        }
        count--;
    }
}

bool CudaKeySearchDevice::isTargetInList(const unsigned int hash[5])
{
    size_t count = _targets.size();

    while(count) {
        if(memcmp(hash, _targets[count - 1].h, 20) == 0) {
            return true;
        }
        count--;
    }

    return false;
}

uint32_t CudaKeySearchDevice::getPrivateKeyOffset(int thread, int block, int idx)
{
    // Total number of threads
    int totalThreads = _blocks * _threads;

    int base = idx * totalThreads;

    // Global ID of the current thread
    int threadId = block * _threads + thread;

    return base + threadId;
}

void CudaKeySearchDevice::getResultsInternal()
{
    int count = _resultList.size();
    int actualCount = 0;
    if(count == 0) {
        return;
    }

    unsigned char *ptr = new unsigned char[count * sizeof(CudaDeviceResult)];

    _resultList.read(ptr, count);

    for(int i = 0; i < count; i++) {
        struct CudaDeviceResult *rPtr = &((struct CudaDeviceResult *)ptr)[i];

        // might be false-positive
        if(!isTargetInList(rPtr->digest)) {
            continue;
        }
        actualCount++;

        KeySearchResult minerResult;

        // Calculate the private key based on the number of iterations and the current thread
        secp256k1::uint256 offset = (secp256k1::uint256((uint64_t)_blocks * _threads * _pointsPerThread * _iterations) + secp256k1::uint256(getPrivateKeyOffset(rPtr->thread, rPtr->block, rPtr->idx))) * _stride;
        secp256k1::uint256 privateKey = secp256k1::addModN(_startExponent, offset);

        minerResult.privateKey = privateKey;
        minerResult.compressed = rPtr->compressed;

        memcpy(minerResult.hash, rPtr->digest, 20);

        minerResult.publicKey = secp256k1::ecpoint(secp256k1::uint256(rPtr->x, secp256k1::uint256::BigEndian), secp256k1::uint256(rPtr->y, secp256k1::uint256::BigEndian));

        removeTargetFromList(rPtr->digest);

        _results.push_back(minerResult);
    }

    delete[] ptr;

    _resultList.clear();

    // Reload the bloom filters
    if(actualCount) {
        cudaCall(_targetLookup.setTargets(_targets));
    }
}

// Verify a private key produces the public key and hash
bool CudaKeySearchDevice::verifyKey(const secp256k1::uint256 &privateKey, const secp256k1::ecpoint &publicKey, const unsigned int hash[5], bool compressed)
{
    secp256k1::ecpoint g = secp256k1::G();

    secp256k1::ecpoint p = secp256k1::multiplyPoint(privateKey, g);

    if(!(p == publicKey)) {
        return false;
    }

    unsigned int xWords[8];
    unsigned int yWords[8];

    p.x.exportWords(xWords, 8, secp256k1::uint256::BigEndian);
    p.y.exportWords(yWords, 8, secp256k1::uint256::BigEndian);

    unsigned int digest[5];
    if(compressed) {
        Hash::hashPublicKeyCompressed(xWords, yWords, digest);
    } else {
        Hash::hashPublicKey(xWords, yWords, digest);
    }

    for(int i = 0; i < 5; i++) {
        if(digest[i] != hash[i]) {
            return false;
        }
    }

    return true;
}

void CudaKeySearchDevice::calculateAndLogOccupancy()
{
    // Get device properties
    cudaDeviceProp deviceProp;
    cudaCall(cudaGetDeviceProperties(&deviceProp, _device));

    // Log device information
    Logger::log(LogLevel::Info, "GPU: " + std::string(deviceProp.name));
    Logger::log(LogLevel::Info, "Compute Capability: " +
                util::format(deviceProp.major) + "." + util::format(deviceProp.minor));
    Logger::log(LogLevel::Info, "Multiprocessors: " + util::format(deviceProp.multiProcessorCount));
    Logger::log(LogLevel::Info, "CUDA Cores: ~" +
                util::formatThousands(deviceProp.multiProcessorCount * 128)); // Approximate

    // Calculate thread configuration
    uint64_t totalThreads = (uint64_t)_blocks * _threads;
    uint64_t keysPerIteration = totalThreads * _pointsPerThread;

    Logger::log(LogLevel::Info, "Blocks: " + util::format(_blocks));
    Logger::log(LogLevel::Info, "Threads per block: " + util::format(_threads));
    Logger::log(LogLevel::Info, "Points per thread: " + util::format(_pointsPerThread));
    Logger::log(LogLevel::Info, "Total threads: " + util::formatThousands(totalThreads));
    Logger::log(LogLevel::Info, "Keys per iteration: " + util::formatThousands(keysPerIteration));

    // Calculate theoretical occupancy
    // Note: cudaOccupancyMaxActiveBlocksPerMultiprocessor requires kernel function pointer
    // For now, we'll calculate a simple metric
    int maxThreadsPerSM = deviceProp.maxThreadsPerMultiProcessor;
    int threadsPerBlock = _threads;
    int maxBlocksPerSM = maxThreadsPerSM / threadsPerBlock;

    // Estimate active threads vs maximum possible
    int totalMaxThreads = deviceProp.multiProcessorCount * maxThreadsPerSM;
    double occupancyPercent = (double)totalThreads / totalMaxThreads * 100.0;

    Logger::log(LogLevel::Info, "Max threads per SM: " + util::format(maxThreadsPerSM));
    Logger::log(LogLevel::Info, "Estimated occupancy: " + util::format("%.1f", occupancyPercent) + "%");

    // Warn if occupancy is low
    if(occupancyPercent < 50.0) {
        Logger::log(LogLevel::Warning, "Low GPU occupancy detected! Consider increasing blocks/threads.");
        Logger::log(LogLevel::Warning, "For RTX 4090, try: -b 128 -t 512 -p 128");
    }

    // Log memory configuration
    Logger::log(LogLevel::Info, "Shared memory per block: " +
                util::format((uint64_t)(deviceProp.sharedMemPerBlock / 1024)) + " KB");
    Logger::log(LogLevel::Info, "L2 cache size: " +
                util::format(deviceProp.l2CacheSize / (1024 * 1024)) + " MB");
}

size_t CudaKeySearchDevice::getResults(std::vector<KeySearchResult> &resultsOut)
{
    for(int i = 0; i < _results.size(); i++) {
        resultsOut.push_back(_results[i]);
    }
    _results.clear();

    return resultsOut.size();
}

secp256k1::uint256 CudaKeySearchDevice::getNextKey()
{
    uint64_t totalPoints = (uint64_t)_pointsPerThread * _threads * _blocks;

    return _startExponent + secp256k1::uint256(totalPoints) * _iterations * _stride;
}

void CudaKeySearchDevice::setStartingKey(const secp256k1::uint256 &start)
{
    _startExponent = start;
    _iterations = 0;

    // SHUFFLE-WALK OPTIMIZATION (Multi-Phase):
    if (_deviceKeys.isReservoirMode()) {
        if (_deviceKeys.isShuffleIndexMode()) {
            // PHASE 2: Shuffle-Index Mode (~3-5ms total)
            // 1. Shuffle the index LUT (~1-2ms)
            cudaCall(_deviceKeys.shuffleIndices());
            // 2. Indirect copy from reservoir using shuffled indices (~2-3ms)
            cudaCall(_deviceKeys.copyFromReservoir());
        } else {
            // PHASE 1: Reservoir Mode with memcpy (~10-15ms)
            // Direct copy: reservoir → working buffers
            cudaCall(_deviceKeys.copyFromReservoir());
        }
        // The kernel will add the new random offset to these base points
        // This is 100-200x faster than regenerating 16.7M points!
    } else {
        // Legacy mode: Full regeneration (~3800ms)
        generateStartingPoints();
    }
}

void CudaKeySearchDevice::setReservoirMode(bool enable)
{
    _deviceKeys.enableReservoirMode(enable);
}

bool CudaKeySearchDevice::isReservoirMode() const
{
    return _deviceKeys.isReservoirMode();
}

void CudaKeySearchDevice::setShuffleIndexMode(bool enable)
{
    if (!_deviceKeys.isReservoirMode() && enable) {
        throw KeySearchException("Shuffle-index mode requires reservoir mode to be enabled first");
    }

    _deviceKeys.enableShuffleIndexMode(enable);

    if (enable) {
        // Initialize shuffle-index infrastructure
        cudaCall(_deviceKeys.initShuffleIndex(_blocks));
        Logger::log(LogLevel::Info, "Shuffle-Index Mode enabled (zero-copy reservoir access)");
    }
}

bool CudaKeySearchDevice::isShuffleIndexMode() const
{
    return _deviceKeys.isShuffleIndexMode();
}