#ifndef _EC_H
#define _EC_H

#include <cuda.h>
#include <cuda_runtime.h>

#include <vector>
#include "secp256k1.h"

// Forward declaration
class CudaShuffleIndex;

class CudaDeviceKeys {

private:
	int _blocks;

	int _threads;

	int _pointsPerThread;

	unsigned int _numKeys;

	unsigned int *_devX;

	unsigned int *_devY;

	unsigned int *_devPrivate;

	unsigned int *_devChain;

	unsigned int *_devBasePointX;

	unsigned int *_devBasePointY;

	int _step;

	// Reservoir mode support (persistent buffer for shuffle-walk)
	bool _reservoirMode;
	unsigned int *_reservoirX;      // Persistent base points X (never freed in reservoir mode)
	unsigned int *_reservoirY;      // Persistent base points Y
	uint64_t _reservoirEpoch;       // Rotation counter

	// Shuffle-index mode (Phase 2 optimization - zero-copy)
	bool _shuffleIndexMode;
	CudaShuffleIndex *_shuffleIndex;

	int getIndex(int block, int thread, int idx);

	void splatBigInt(unsigned int *dest, int block, int thread, int idx, const secp256k1::uint256 &i);

	secp256k1::uint256 readBigInt(unsigned int *src, int block, int thread, int idx);

	cudaError_t allocateChainBuf(unsigned int count);

	cudaError_t initializePublicKeys(size_t count);

	cudaError_t initializeBasePoints();


public:

	CudaDeviceKeys()
	{
		_numKeys = 0;
		_devX = NULL;
		_devY = NULL;
		_devPrivate = NULL;
		_devChain = NULL;
		_devBasePointX = NULL;
		_devBasePointY = NULL;
		_step = 0;
		_reservoirMode = false;
		_reservoirX = NULL;
		_reservoirY = NULL;
		_reservoirEpoch = 0;
		_shuffleIndexMode = false;
		_shuffleIndex = NULL;
	}

	~CudaDeviceKeys()
	{
		clearPublicKeys();
		clearPrivateKeys();
	}

	cudaError_t init(int blocks, int threads, int pointsPerThread, const std::vector<secp256k1::uint256> &privateKeys);

	bool selfTest(const std::vector<secp256k1::uint256> &privateKeys);

	cudaError_t doStep();

	void clearPrivateKeys();

	void clearPublicKeys();

	// Reservoir mode methods
	void enableReservoirMode(bool enable);
	bool isReservoirMode() const { return _reservoirMode; }
	cudaError_t initReservoir(int blocks, int threads, int pointsPerThread, const std::vector<secp256k1::uint256> &baseKeys);
	cudaError_t copyFromReservoir();  // Copy reservoir → working buffers (fast!)
	cudaError_t rotateReservoir(float fraction);  // Partial rotation

	// Shuffle-index mode methods (Phase 2 - zero-copy)
	void enableShuffleIndexMode(bool enable);
	bool isShuffleIndexMode() const { return _shuffleIndexMode; }
	cudaError_t initShuffleIndex(int blocks);
	cudaError_t shuffleIndices();
	const uint32_t* getIndexLUT() const;
	unsigned int* getReservoirX() const { return _reservoirX; }
	unsigned int* getReservoirY() const { return _reservoirY; }

};

#endif