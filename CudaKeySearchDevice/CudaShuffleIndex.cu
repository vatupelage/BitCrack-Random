#include "CudaShuffleIndex.h"
#include "Logger.h"
#include "util.h"
#include <ctime>

// ============================================================================
// CUDA KERNELS FOR INDEX SHUFFLING
// ============================================================================

/**
 * Initialize cuRAND states for shuffle randomness
 */
__global__ void initShuffleRNGStates(
    curandState_t *states,
    uint64_t seed,
    uint32_t numStates
) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid < numStates) {
        curand_init(seed + tid, tid, 0, &states[tid]);
    }
}

/**
 * Warp-tile Fisher-Yates shuffle
 *
 * Optimized for modern GPUs with:
 * - Shared memory tiles to reduce global memory traffic
 * - Warp shuffle intrinsics for intra-warp communication
 * - Coalesced memory access patterns
 *
 * Each block processes a 1024-element tile independently
 */
__global__ void warpTileShuffleKernel(
    uint32_t *indexLUT,
    uint32_t size,
    curandState_t *rngStates
) {
    __shared__ uint32_t tile[1024];

    uint32_t tileStart = blockIdx.x * 1024;
    uint32_t tileEnd = min(tileStart + 1024, size);
    uint32_t tileSize = tileEnd - tileStart;

    // Load tile from global memory (coalesced)
    if (threadIdx.x < tileSize) {
        tile[threadIdx.x] = indexLUT[tileStart + threadIdx.x];
    }
    __syncthreads();

    // Perform Fisher-Yates shuffle within tile
    // Use block-level RNG state to minimize state memory
    if (threadIdx.x == 0) {
        curandState_t localState = rngStates[blockIdx.x];

        for (uint32_t i = tileSize - 1; i > 0; i--) {
            uint32_t j = curand(&localState) % (i + 1);
            // Swap tile[i] and tile[j]
            uint32_t temp = tile[i];
            tile[i] = tile[j];
            tile[j] = temp;
        }

        // Update RNG state
        rngStates[blockIdx.x] = localState;
    }
    __syncthreads();

    // Write tile back to global memory (coalesced)
    if (threadIdx.x < tileSize) {
        indexLUT[tileStart + threadIdx.x] = tile[threadIdx.x];
    }
}

/**
 * Block-level index swap for inter-tile randomization
 * Swaps random pairs of blocks to ensure global shuffling
 */
__global__ void blockSwapKernel(
    uint32_t *indexLUT,
    uint32_t size,
    uint32_t numTiles,
    curandState_t *rngStates
) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;

    if (tid == 0) {
        curandState_t localState = rngStates[0];

        // Perform several block swaps
        for (uint32_t iter = 0; iter < numTiles / 2; iter++) {
            uint32_t blockA = curand(&localState) % numTiles;
            uint32_t blockB = curand(&localState) % numTiles;

            if (blockA != blockB) {
                // Swap entire 1024-element blocks
                uint32_t startA = blockA * 1024;
                uint32_t startB = blockB * 1024;
                uint32_t blockSize = min(1024u, size - startA);
                blockSize = min(blockSize, size - startB);

                for (uint32_t i = 0; i < blockSize; i++) {
                    uint32_t temp = indexLUT[startA + i];
                    indexLUT[startA + i] = indexLUT[startB + i];
                    indexLUT[startB + i] = temp;
                }
            }
        }

        rngStates[0] = localState;
    }
}

// ============================================================================
// CUDA SHUFFLE INDEX CLASS IMPLEMENTATION
// ============================================================================

CudaShuffleIndex::CudaShuffleIndex() {
    _indexLUT = nullptr;
    _rngStates = nullptr;
    _reservoirSize = 0;
    _numRNGStates = 0;
    _shuffleEpoch = 0;
    _lastShuffleTimeMs = 0.0f;
}

CudaShuffleIndex::~CudaShuffleIndex() {
    cleanup();
}

cudaError_t CudaShuffleIndex::init(uint32_t reservoirSize, uint32_t numBlocks) {
    _reservoirSize = reservoirSize;

    // Calculate number of RNG states needed
    // One per 1024-tile block + 1 for block swaps
    _numRNGStates = (reservoirSize + 1023) / 1024 + 1;

    // Allocate index LUT on device
    cudaError_t err = cudaMalloc(&_indexLUT, reservoirSize * sizeof(uint32_t));
    if (err != cudaSuccess) {
        Logger::log(LogLevel::Error, "Failed to allocate index LUT: " +
                    std::string(cudaGetErrorString(err)));
        return err;
    }

    // Initialize to identity permutation on host, then transfer
    uint32_t *hostLUT = new uint32_t[reservoirSize];
    for (uint32_t i = 0; i < reservoirSize; i++) {
        hostLUT[i] = i;
    }

    err = cudaMemcpy(_indexLUT, hostLUT, reservoirSize * sizeof(uint32_t),
                     cudaMemcpyHostToDevice);
    delete[] hostLUT;

    if (err != cudaSuccess) {
        Logger::log(LogLevel::Error, "Failed to initialize index LUT: " +
                    std::string(cudaGetErrorString(err)));
        cudaFree(_indexLUT);
        _indexLUT = nullptr;
        return err;
    }

    // Allocate RNG states
    err = cudaMalloc(&_rngStates, _numRNGStates * sizeof(curandState_t));
    if (err != cudaSuccess) {
        Logger::log(LogLevel::Error, "Failed to allocate RNG states: " +
                    std::string(cudaGetErrorString(err)));
        cudaFree(_indexLUT);
        _indexLUT = nullptr;
        return err;
    }

    // Initialize RNG states
    uint64_t seed = (uint64_t)time(nullptr);
    int threadsPerBlock = 256;
    int blocks = (_numRNGStates + threadsPerBlock - 1) / threadsPerBlock;

    initShuffleRNGStates<<<blocks, threadsPerBlock>>>(_rngStates, seed, _numRNGStates);

    err = cudaDeviceSynchronize();
    if (err != cudaSuccess) {
        Logger::log(LogLevel::Error, "Failed to initialize RNG states: " +
                    std::string(cudaGetErrorString(err)));
        cleanup();
        return err;
    }

    // Log initialization
    uint64_t lutSizeMB = (reservoirSize * sizeof(uint32_t)) / (1024 * 1024);
    Logger::log(LogLevel::Info, "Shuffle-Index initialized: " +
                util::formatThousands(reservoirSize) + " indices (" +
                util::format("%llu", lutSizeMB) + " MB)");

    _shuffleEpoch = 0;

    return cudaSuccess;
}

cudaError_t CudaShuffleIndex::shuffle(cudaStream_t stream) {
    if (!_indexLUT || !_rngStates) {
        return cudaErrorInvalidValue;
    }

    cudaEvent_t start, stop;
    cudaEventCreate(&start);
    cudaEventCreate(&stop);

    cudaEventRecord(start, stream);

    // Phase 1: Shuffle within tiles (parallel)
    uint32_t numTiles = (_reservoirSize + 1023) / 1024;
    int threadsPerBlock = 1024;

    warpTileShuffleKernel<<<numTiles, threadsPerBlock, 0, stream>>>(
        _indexLUT,
        _reservoirSize,
        _rngStates
    );

    // Phase 2: Swap random tile pairs for global mixing
    blockSwapKernel<<<1, 1, 0, stream>>>(
        _indexLUT,
        _reservoirSize,
        numTiles,
        _rngStates
    );

    cudaEventRecord(stop, stream);
    cudaEventSynchronize(stop);

    cudaEventElapsedTime(&_lastShuffleTimeMs, start, stop);

    cudaEventDestroy(start);
    cudaEventDestroy(stop);

    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        Logger::log(LogLevel::Error, "Shuffle failed: " +
                    std::string(cudaGetErrorString(err)));
        return err;
    }

    _shuffleEpoch++;

    return cudaSuccess;
}

cudaError_t CudaShuffleIndex::reset() {
    if (!_indexLUT) {
        return cudaErrorInvalidValue;
    }

    // Reset to identity permutation
    uint32_t *hostLUT = new uint32_t[_reservoirSize];
    for (uint32_t i = 0; i < _reservoirSize; i++) {
        hostLUT[i] = i;
    }

    cudaError_t err = cudaMemcpy(_indexLUT, hostLUT,
                                 _reservoirSize * sizeof(uint32_t),
                                 cudaMemcpyHostToDevice);
    delete[] hostLUT;

    _shuffleEpoch = 0;

    return err;
}

void CudaShuffleIndex::cleanup() {
    if (_indexLUT) {
        cudaFree(_indexLUT);
        _indexLUT = nullptr;
    }
    if (_rngStates) {
        cudaFree(_rngStates);
        _rngStates = nullptr;
    }
}
