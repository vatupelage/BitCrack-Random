#ifndef _HASH_LOOKUP_HOST_H
#define _HASH_LOOKUP_HOST_H

#include <cuda_runtime.h>

class CudaHashLookup {

private:
	unsigned int *_bloomFilterPtr;
	unsigned int *_sortedTargetsPtr;  // For medium-sized target sets (17-1024)
	cudaStream_t _stream;  // For async operations and L2 persistence

	cudaError_t setTargetBloomFilter(const std::vector<struct hash160> &targets);

	cudaError_t setTargetConstantMemory(const std::vector<struct hash160> &targets);

	cudaError_t setTargetSortedMemory(const std::vector<struct hash160> &targets);

	unsigned int getOptimalBloomFilterBits(double p, size_t n);

	void cleanup();

	void initializeBloomFilter(const std::vector<struct hash160> &targets, unsigned int *filter, unsigned int mask);

	void initializeBloomFilter64(const std::vector<struct hash160> &targets, unsigned int *filter, unsigned long long mask);

	cudaError_t setL2CachePersistence(void *ptr, size_t bytes);

public:

	CudaHashLookup()
	{
		_bloomFilterPtr = NULL;
		_sortedTargetsPtr = NULL;
		_stream = NULL;
	}

	~CudaHashLookup()
	{
		cleanup();
	}

	cudaError_t setTargets(const std::vector<struct hash160> &targets);
};

#endif