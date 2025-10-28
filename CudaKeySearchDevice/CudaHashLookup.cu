#include <cuda.h>
#include <cuda_runtime.h>
#include <math.h>
#include <vector>
#include <algorithm>

#include "KeySearchDevice.h"

#include "CudaHashLookup.h"

#include "CudaHashLookup.cuh"

#include "Logger.h"

#include "util.h"

#define MAX_TARGETS_CONSTANT_MEM 16
#define MAX_TARGETS_SORTED_MEM 1024  // Use sorted array + binary search for 17-1024 targets

__constant__ unsigned int _TARGET_HASH[MAX_TARGETS_CONSTANT_MEM][5];
__constant__ unsigned int _NUM_TARGET_HASHES[1];
__constant__ unsigned int *_BLOOM_FILTER[1];
__constant__ unsigned int _BLOOM_FILTER_MASK[1];
__constant__ unsigned long long _BLOOM_FILTER_MASK64[1];

// For sorted target mode (17-1024 targets)
__constant__ unsigned int *_SORTED_TARGETS[1];
__constant__ unsigned int _NUM_SORTED_TARGETS[1];

// Lookup mode: 0=constant mem (≤16), 1=sorted+binary search (17-1024), 2=bloom32, 3=bloom64
__constant__ unsigned int _LOOKUP_MODE[1];


static unsigned int swp(unsigned int x)
{
	return (x << 24) | ((x << 8) & 0x00ff0000) | ((x >> 8) & 0x0000ff00) | (x >> 24);
}

static void undoRMD160FinalRound(const unsigned int hIn[5], unsigned int hOut[5])
{
	unsigned int iv[5] = {
		0x67452301,
		0xefcdab89,
		0x98badcfe,
		0x10325476,
		0xc3d2e1f0
	};

	for(int i = 0; i < 5; i++) {
		hOut[i] = swp(hIn[i]) - iv[(i + 1) % 5];
	}
}

/**
Copies the target hashes to constant memory
*/
cudaError_t CudaHashLookup::setTargetConstantMemory(const std::vector<struct hash160> &targets)
{
	size_t count = targets.size();

	for(size_t i = 0; i < count; i++) {
		unsigned int h[5];

		undoRMD160FinalRound(targets[i].h, h);

		cudaError_t err = cudaMemcpyToSymbol(_TARGET_HASH, h, sizeof(unsigned int) * 5, i * sizeof(unsigned int) * 5);

		if(err) {
			return err;
		}
	}

	cudaError_t err = cudaMemcpyToSymbol(_NUM_TARGET_HASHES, &count, sizeof(unsigned int));
	if(err) {
		return err;
	}

	// Mode 0: constant memory (≤16 targets)
	unsigned int lookupMode = 0;
	err = cudaMemcpyToSymbol(_LOOKUP_MODE, &lookupMode, sizeof(unsigned int));
	if(err) {
		return err;
	}

	Logger::log(LogLevel::Info, "Using constant memory for " + util::format((int)count) + " targets");

	return cudaSuccess;
}

/**
 * Sets L2 cache persistence for a memory region (CUDA 11.0+, Ampere/Ada GPUs)
 * This pins frequently accessed data in the L2 cache for better performance
 */
cudaError_t CudaHashLookup::setL2CachePersistence(void *ptr, size_t bytes)
{
	// Check if GPU supports L2 cache persistence
	int device;
	cudaError_t err = cudaGetDevice(&device);
	if(err != cudaSuccess) {
		return err;
	}

	cudaDeviceProp deviceProp;
	err = cudaGetDeviceProperties(&deviceProp, device);
	if(err != cudaSuccess) {
		return err;
	}

	// L2 persistence is supported on Ampere (8.x) and newer
	if(deviceProp.major < 8) {
		Logger::log(LogLevel::Info, "L2 cache persistence not available on compute capability < 8.0");
		return cudaSuccess;  // Not an error, just not supported
	}

	// Create stream if not already created
	if(_stream == NULL) {
		err = cudaStreamCreate(&_stream);
		if(err != cudaSuccess) {
			return err;
		}
	}

	// Set L2 cache persistence hint
	cudaStreamAttrValue stream_attribute;
	stream_attribute.accessPolicyWindow.base_ptr = ptr;
	stream_attribute.accessPolicyWindow.num_bytes = bytes;
	stream_attribute.accessPolicyWindow.hitRatio = 1.0f;  // Expect 100% hit ratio
	stream_attribute.accessPolicyWindow.hitProp = cudaAccessPropertyPersisting;
	stream_attribute.accessPolicyWindow.missProp = cudaAccessPropertyStreaming;

	err = cudaStreamSetAttribute(_stream, cudaStreamAttributeAccessPolicyWindow, &stream_attribute);
	if(err == cudaSuccess) {
		Logger::log(LogLevel::Info, "L2 cache persistence enabled for " +
		           util::format((uint64_t)(bytes / 1024)) + " KB");
	}

	return err;
}

/**
Returns the optimal bloom filter size in bits given the probability of false-positives and the
number of hash functions
*/
unsigned int CudaHashLookup::getOptimalBloomFilterBits(double p, size_t n)
{
	double m = 3.6 * ceil((n * log(p)) / log(1 / pow(2, log(2))));

	return (unsigned int)ceil(log(m) / log(2));
}

/**
 * Comparison function for sorting hashes (for binary search)
 * Compares hash160 structures lexicographically
 */
static bool compareHash160(const hash160 &a, const hash160 &b)
{
	for(int i = 0; i < 5; i++) {
		if(a.h[i] < b.h[i]) return true;
		if(a.h[i] > b.h[i]) return false;
	}
	return false;  // Equal
}

/**
 * Sets up sorted target array for medium-sized target sets (17-1024 targets)
 * Uses binary search on GPU for efficient lookup
 */
cudaError_t CudaHashLookup::setTargetSortedMemory(const std::vector<struct hash160> &targets)
{
	size_t count = targets.size();

	Logger::log(LogLevel::Info, "Using sorted memory + binary search for " +
	           util::format((int)count) + " targets (" +
	           util::format((uint64_t)((count * 20) / 1024)) + " KB)");

	// Sort targets for binary search
	std::vector<hash160> sortedTargets = targets;
	std::sort(sortedTargets.begin(), sortedTargets.end(), compareHash160);

	// Undo RMD160 final round for GPU comparison
	std::vector<unsigned int> deviceHashes(count * 5);
	for(size_t i = 0; i < count; i++) {
		unsigned int h[5];
		undoRMD160FinalRound(sortedTargets[i].h, h);
		for(int j = 0; j < 5; j++) {
			deviceHashes[i * 5 + j] = h[j];
		}
	}

	// Allocate GPU memory
	size_t bytes = count * 5 * sizeof(unsigned int);
	cudaError_t err = cudaMalloc(&_sortedTargetsPtr, bytes);
	if(err) {
		Logger::log(LogLevel::Error, "Device error: " + std::string(cudaGetErrorString(err)));
		return err;
	}

	// Copy to device
	err = cudaMemcpy(_sortedTargetsPtr, deviceHashes.data(), bytes, cudaMemcpyHostToDevice);
	if(err) {
		cudaFree(_sortedTargetsPtr);
		_sortedTargetsPtr = NULL;
		return err;
	}

	// Set L2 cache persistence for sorted targets (RTX 4090 has 72MB L2)
	err = setL2CachePersistence(_sortedTargetsPtr, bytes);
	if(err != cudaSuccess) {
		Logger::log(LogLevel::Warning, "Could not set L2 persistence: " +
		           std::string(cudaGetErrorString(err)));
		// Continue anyway, this is just an optimization
	}

	// Copy pointer to constant memory
	err = cudaMemcpyToSymbol(_SORTED_TARGETS, &_sortedTargetsPtr, sizeof(unsigned int *));
	if(err) {
		cudaFree(_sortedTargetsPtr);
		_sortedTargetsPtr = NULL;
		return err;
	}

	// Set count
	err = cudaMemcpyToSymbol(_NUM_SORTED_TARGETS, &count, sizeof(unsigned int));
	if(err) {
		cudaFree(_sortedTargetsPtr);
		_sortedTargetsPtr = NULL;
		return err;
	}

	// Mode 1: sorted array + binary search
	unsigned int lookupMode = 1;
	err = cudaMemcpyToSymbol(_LOOKUP_MODE, &lookupMode, sizeof(unsigned int));
	if(err) {
		cudaFree(_sortedTargetsPtr);
		_sortedTargetsPtr = NULL;
		return err;
	}

	return cudaSuccess;
}

void CudaHashLookup::initializeBloomFilter(const std::vector<struct hash160> &targets, unsigned int *filter, unsigned int mask)
{
	// Use the low 16 bits of each word in the hash as the index into the bloom filter
	for(unsigned int i = 0; i < targets.size(); i++) {

		unsigned int h[5];

		undoRMD160FinalRound(targets[i].h, h);

		for(int j = 0; j < 5; j++) {
			unsigned int idx = h[j] & mask;

			filter[idx / 32] |= (0x01 << (idx % 32));
		}

	}
}

void CudaHashLookup::initializeBloomFilter64(const std::vector<struct hash160> &targets, unsigned int *filter, unsigned long long mask)
{
	for(unsigned int k = 0; k < targets.size(); k++) {

		unsigned int hash[5];

		unsigned long long idx[5];

		undoRMD160FinalRound(targets[k].h, hash);

		idx[0] = ((unsigned long long)hash[0] << 32 | hash[1]) & mask;
		idx[1] = ((unsigned long long)hash[2] << 32 | hash[3]) & mask;
		idx[2] = ((unsigned long long)(hash[0]^hash[1]) << 32 | (hash[1]^hash[2])) & mask;
		idx[3] = ((unsigned long long)(hash[2]^hash[3]) << 32 | (hash[3] ^ hash[4])) & mask;
		idx[4] = ((unsigned long long)(hash[0]^hash[3]) << 32 | (hash[1]^hash[3])) & mask;

		for(int i = 0; i < 5; i++) {

			filter[idx[i] / 32] |= (0x01 << (idx[i] % 32));
		}
	}
}

/**
Populates the bloom filter with the target hashes
*/
cudaError_t CudaHashLookup::setTargetBloomFilter(const std::vector<struct hash160> &targets)
{
	unsigned int bloomFilterBits = getOptimalBloomFilterBits(1.0e-9, targets.size());

	unsigned long long bloomFilterSizeWords = (unsigned long long)1 << (bloomFilterBits - 5);
	unsigned long long bloomFilterBytes = (unsigned long long)1 << (bloomFilterBits - 3);
	unsigned long long bloomFilterMask = (((unsigned long long)1 << bloomFilterBits) - 1);

	Logger::log(LogLevel::Info, "Allocating bloom filter (" + util::format("%.1f", (double)bloomFilterBytes/(double)(1024*1024)) + "MB)");

	unsigned int *filter = NULL;
	
	try {
		filter = new unsigned int[bloomFilterSizeWords];
	} catch(std::bad_alloc) {
		Logger::log(LogLevel::Error, "Out of system memory");

		return cudaErrorMemoryAllocation;
	}

	cudaError_t err = cudaMalloc(&_bloomFilterPtr, bloomFilterBytes);

	if(err) {
		Logger::log(LogLevel::Error, "Device error: " + std::string(cudaGetErrorString(err)));
		delete[] filter;
		return err;
	}

	memset(filter, 0, sizeof(unsigned int) * bloomFilterSizeWords);
	if(bloomFilterBits > 32) {
		initializeBloomFilter64(targets, filter, bloomFilterMask);
	} else {
		initializeBloomFilter(targets, filter, (unsigned int)bloomFilterMask);
	}

	// Copy to device
	err = cudaMemcpy(_bloomFilterPtr, filter, sizeof(unsigned int) * bloomFilterSizeWords, cudaMemcpyHostToDevice);
	if(err) {
		cudaFree(_bloomFilterPtr);
		_bloomFilterPtr = NULL;
		delete[] filter;
		return err;
	}

	// Set L2 cache persistence for Bloom filter (RTX 4090 has 72MB L2)
	err = setL2CachePersistence(_bloomFilterPtr, bloomFilterBytes);
	if(err != cudaSuccess) {
		Logger::log(LogLevel::Warning, "Could not set L2 persistence for Bloom filter: " +
		           std::string(cudaGetErrorString(err)));
		// Continue anyway, this is just an optimization
	}

	// Copy device memory pointer to constant memory
	err = cudaMemcpyToSymbol(_BLOOM_FILTER, &_bloomFilterPtr, sizeof(unsigned int *));
	if(err) {
		cudaFree(_bloomFilterPtr);
		_bloomFilterPtr = NULL;
		delete[] filter;
		return err;
	}

	// Copy device memory pointer to constant memory
	if(bloomFilterBits <= 32) {
		err = cudaMemcpyToSymbol(_BLOOM_FILTER_MASK, &bloomFilterMask, sizeof(unsigned int *));
		if(err) {
			cudaFree(_bloomFilterPtr);
			_bloomFilterPtr = NULL;
			delete[] filter;
			return err;
		}
	} else {
		err = cudaMemcpyToSymbol(_BLOOM_FILTER_MASK64, &bloomFilterMask, sizeof(unsigned long long *));
		if(err) {
			cudaFree(_bloomFilterPtr);
			_bloomFilterPtr = NULL;
			delete[] filter;
			return err;
		}
	}

	// Mode 2: bloom filter (32-bit), Mode 3: bloom filter (64-bit)
	unsigned int lookupMode = bloomFilterBits <= 32 ? 2 : 3;
	err = cudaMemcpyToSymbol(_LOOKUP_MODE, &lookupMode, sizeof(unsigned int));

	delete[] filter;

	if(err == cudaSuccess) {
		Logger::log(LogLevel::Info, "Bloom filter mode " +
		           util::format(lookupMode) + " initialized");
	}

	return err;
}

/**
 * Copies the target hashes to the most efficient storage based on target count:
 * - ≤16 targets: Constant memory (fastest, but limited size)
 * - 17-1024 targets: Sorted array with binary search + L2 cache pinning
 * - >1024 targets: Bloom filter + L2 cache pinning
 */
cudaError_t CudaHashLookup::setTargets(const std::vector<struct hash160> &targets)
{
	cleanup();

	size_t count = targets.size();

	if(count <= MAX_TARGETS_CONSTANT_MEM) {
		// Small sets: use constant memory (legacy, fastest for ≤16 targets)
		return setTargetConstantMemory(targets);
	} else if(count <= MAX_TARGETS_SORTED_MEM) {
		// Medium sets: use sorted array + binary search with L2 persistence
		return setTargetSortedMemory(targets);
	} else {
		// Large sets: use Bloom filter with L2 persistence
		return setTargetBloomFilter(targets);
	}
}

void CudaHashLookup::cleanup()
{
	if(_bloomFilterPtr != NULL) {
		cudaFree(_bloomFilterPtr);
		_bloomFilterPtr = NULL;
	}

	if(_sortedTargetsPtr != NULL) {
		cudaFree(_sortedTargetsPtr);
		_sortedTargetsPtr = NULL;
	}

	if(_stream != NULL) {
		cudaStreamDestroy(_stream);
		_stream = NULL;
	}
}

/**
 * Device-side binary search for sorted targets
 * Uses L2-cached sorted array for O(log n) lookup
 */
__device__ bool checkSortedTargets(const unsigned int hash[5])
{
	unsigned int count = *_NUM_SORTED_TARGETS;
	unsigned int *targets = _SORTED_TARGETS[0];

	int left = 0;
	int right = count - 1;

	while(left <= right) {
		int mid = (left + right) / 2;
		unsigned int *midHash = &targets[mid * 5];

		// Compare lexicographically
		int cmp = 0;
		for(int i = 0; i < 5; i++) {
			if(hash[i] < midHash[i]) {
				cmp = -1;
				break;
			}
			if(hash[i] > midHash[i]) {
				cmp = 1;
				break;
			}
		}

		if(cmp == 0) {
			return true;  // Found exact match
		} else if(cmp < 0) {
			right = mid - 1;
		} else {
			left = mid + 1;
		}
	}

	return false;  // Not found
}

__device__ bool checkBloomFilter(const unsigned int hash[5])
{
	bool foundMatch = true;

	unsigned int mask = _BLOOM_FILTER_MASK[0];
	unsigned int *bloomFilter = _BLOOM_FILTER[0];

	for(int i = 0; i < 5; i++) {
        unsigned int idx = hash[i] & mask;

        unsigned int f = bloomFilter[idx / 32];

		if((f & (0x01 << (idx % 32))) == 0) {
			foundMatch = false;
		}
	}

	return foundMatch;
}

__device__ bool checkBloomFilter64(const unsigned int hash[5])
{
	bool foundMatch = true;

	unsigned long long mask = _BLOOM_FILTER_MASK64[0];
	unsigned int *bloomFilter = _BLOOM_FILTER[0];
	unsigned long long idx[5];

	idx[0] = ((unsigned long long)hash[0] << 32 | hash[1]) & mask;
	idx[1] = ((unsigned long long)hash[2] << 32 | hash[3]) & mask;
	idx[2] = ((unsigned long long)(hash[0] ^ hash[1]) << 32 | (hash[1] ^ hash[2])) & mask;
	idx[3] = ((unsigned long long)(hash[2] ^ hash[3]) << 32 | (hash[3] ^ hash[4])) & mask;
	idx[4] = ((unsigned long long)(hash[0] ^ hash[3]) << 32 | (hash[1] ^ hash[3])) & mask;

	for(int i = 0; i < 5; i++) {
		unsigned int f = bloomFilter[idx[i] / 32];

		if((f & (0x01 << (idx[i] % 32))) == 0) {
			foundMatch = false;
		}
	}

	return foundMatch;
}


/**
 * Main hash checking function - routes to appropriate lookup method
 * Mode 0: Constant memory linear search (≤16 targets)
 * Mode 1: Sorted array + binary search (17-1024 targets)
 * Mode 2: Bloom filter 32-bit (>1024 targets, small keyspace)
 * Mode 3: Bloom filter 64-bit (>1024 targets, large keyspace)
 */
__device__ bool checkHash(const unsigned int hash[5])
{
	unsigned int mode = *_LOOKUP_MODE;

	switch(mode) {
		case 0: {
			// Constant memory - linear search (fastest for ≤16 targets)
			bool foundMatch = false;
			for(int j = 0; j < *_NUM_TARGET_HASHES; j++) {
				bool equal = true;
				for(int i = 0; i < 5; i++) {
					equal &= (hash[i] == _TARGET_HASH[j][i]);
				}
				foundMatch |= equal;
			}
			return foundMatch;
		}

		case 1:
			// Sorted array - binary search with L2 cache persistence
			return checkSortedTargets(hash);

		case 2:
			// Bloom filter 32-bit
			return checkBloomFilter(hash);

		case 3:
			// Bloom filter 64-bit
			return checkBloomFilter64(hash);

		default:
			return false;
	}
}