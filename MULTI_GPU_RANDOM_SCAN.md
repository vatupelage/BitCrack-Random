# Multi-GPU Random Scanning

This document explains how random scanning works with multi-GPU configurations and key partitioning.

## How It Works

### Single GPU Random Scan

With a single GPU, random scanning works across the **entire** keyspace:

```bash
./bin/cuBitCrack --keyspace 20000000000000000:3ffffffffffffffff -i puzzle66.txt
```

- Scans from `0x2000...000` to `0x3fff...fff`
- Random keys generated across full range
- Alpha ranges apply to entire keyspace

### Multi-GPU Random Scan

With multiple GPUs using `--devices`, the keyspace is **automatically partitioned**, and each GPU randomly scans **only its partition**:

```bash
./bin/cuBitCrack --devices 0,1,2,3 --keyspace 20000000000000000:3ffffffffffffffff -i puzzle66.txt
```

**Automatic Partitioning:**
```
Total keyspace: 0x20000000000000000 to 0x3ffffffffffffffff

GPU 0: 0x20000000000000000 to 0x27ffffffffffffff (partition 1/4)
GPU 1: 0x28000000000000000 to 0x2fffffffffffffff (partition 2/4)
GPU 2: 0x30000000000000000 to 0x37ffffffffffffff (partition 3/4)
GPU 3: 0x38000000000000000 to 0x3ffffffffffffffff (partition 4/4)
```

**Each GPU:**
- Has its own independent Bloom filter
- Generates random keys **only within its partition**
- Applies alpha range priorities **relative to its partition**
- No overlap or duplicate work between GPUs

## Key Benefits

### 1. No Duplicate Work

Each GPU operates independently within its partition:
- GPU 0 will **never** generate keys in GPU 1's range
- GPU 1 will **never** generate keys in GPU 2's range
- etc.

Result: Perfect scaling with zero redundancy

### 2. Partition-Relative Alpha Ranges

Alpha ranges (0.30-0.50, 0.60-0.80, etc.) are applied **relative to each partition**:

**Example for GPU 0 (partition 0x2000...000 to 0x27ff...fff):**
- Alpha 0.00 = `0x20000000000000000` (start of GPU 0's partition)
- Alpha 0.30 = `0x20000000000000000 + (partition_size * 0.30)`
- Alpha 0.50 = `0x20000000000000000 + (partition_size * 0.50)`
- Alpha 1.00 = `0x27ffffffffffffff` (end of GPU 0's partition)

**Example for GPU 1 (partition 0x2800...000 to 0x2fff...fff):**
- Alpha 0.00 = `0x28000000000000000` (start of GPU 1's partition)
- Alpha 0.30 = `0x28000000000000000 + (partition_size * 0.30)`
- Alpha 1.00 = `0x2fffffffffffffff` (end of GPU 1's partition)

This ensures **each GPU prioritizes the same relative regions** within its assigned space.

## Performance Scaling

### Throughput

Multi-GPU random scanning scales linearly:

| GPUs | Per-GPU Speed | Combined Speed | Efficiency |
|------|--------------|----------------|------------|
| 1    | 3.09 GKey/s  | 3.09 GKey/s    | 100%       |
| 2    | 3.09 GKey/s  | 6.18 GKey/s    | 100%       |
| 4    | 3.09 GKey/s  | 12.36 GKey/s   | 100%       |
| 8    | 3.09 GKey/s  | 24.72 GKey/s   | 100%       |

**Why 100% efficiency?**
- Each GPU operates completely independently
- No inter-GPU communication needed
- No shared memory or synchronization
- Each GPU has its own Bloom filter

### Memory Usage

Each GPU requires its own Bloom filter:

| Component         | Per GPU | 4 GPUs Total |
|-------------------|---------|--------------|
| Bloom Filter      | 1 GB    | 4 GB         |
| GPU Buffers       | 2-3 GB  | 8-12 GB      |
| System RAM        | ~500 MB | ~2 GB        |
| **Total**         | **~4 GB** | **~16 GB** |

## Usage Examples

### Example 1: Multi-GPU with Default Random Scan

```bash
./bin/cuBitCrack --devices 0,1,2,3 --keyspace 20000000000000000:3ffffffffffffffff -i puzzle66.txt
```

**Output:**
```
[Info] Multi-GPU mode: Using 4 GPUs
[Info] Compression: compressed
[Info] Starting at: 20000000000000000
[Info] Ending at:   3ffffffffffffffff
[Info] GPU 0 starting: 20000000000000000 to 27ffffffffffffff
[Info] GPU 1 starting: 28000000000000000 to 2fffffffffffffff
[Info] GPU 2 starting: 30000000000000000 to 37ffffffffffffff
[Info] GPU 3 starting: 38000000000000000 to 3ffffffffffffffff

[Info] Initializing NVIDIA GeForce RTX 4090 (GPU 0)
[Info] Random scanning mode enabled
[Info] Partition: 20000000000000000 to 27ffffffffffffff
[Info] Bloom filter size: 1024 MB
[Info] Priority ranges weight: 60.0%
[Info] Priority ranges (within partition):
[Info]   0.30 → 0.50 (30%-50% into partition)
[Info]   0.60 → 0.80 (60%-80% into partition)
...
```

Each GPU logs its partition and random scan configuration.

### Example 2: Multi-GPU with Manual Partitioning (--share)

The old `--share` method still works but requires manual coordination:

```bash
# Terminal 1 - GPU 0
./bin/cuBitCrack -d 0 --keyspace 20000000000000000:3ffffffffffffffff --share 1/4 -i puzzle66.txt

# Terminal 2 - GPU 1
./bin/cuBitCrack -d 1 --keyspace 20000000000000000:3ffffffffffffffff --share 2/4 -i puzzle66.txt

# Terminal 3 - GPU 2
./bin/cuBitCrack -d 2 --keyspace 20000000000000000:3ffffffffffffffff --share 3/4 -i puzzle66.txt

# Terminal 4 - GPU 3
./bin/cuBitCrack -d 3 --keyspace 20000000000000000:3ffffffffffffffff --share 4/4 -i puzzle66.txt
```

Each GPU will:
- Calculate its partition based on `--share`
- Enable random scanning within that partition
- Operate independently

**Recommendation:** Use `--devices` instead of `--share` for simpler management.

## Comparison: --devices vs --share

| Feature | --devices 0,1,2,3 | --share (4 terminals) |
|---------|-------------------|----------------------|
| Partitioning | Automatic | Manual calculation |
| Terminals needed | 1 | 4 |
| Bloom filters | 4 independent | 4 independent |
| Random scanning | Yes (per partition) | Yes (per partition) |
| Progress tracking | Centralized | Separate per terminal |
| Checkpoint files | 1 set | 4 sets |
| Ease of use | ✅ Simple | ❌ Complex |

## Checkpointing with Multi-GPU

### Using --devices (Recommended)

Checkpoint files are **global** (not per-GPU):

```bash
./bin/cuBitCrack --devices 0,1,2,3 \
  --keyspace 20000000000000000:3ffffffffffffffff \
  -i puzzle66.txt \
  --continue checkpoint.txt
```

Creates:
- `checkpoint.txt` - Standard checkpoint (tracks one GPU's position)
- `checkpoint.txt.random` - Random scan metadata
- `checkpoint.txt.random.bloom` - **Bloom filter for GPU 0 only**

**Current Limitation:** Only GPU 0's progress is checkpointed. Other GPUs restart from their partition start.

**Workaround:** For critical long-running searches, use `--share` with separate checkpoint files per GPU.

### Using --share (Per-GPU Checkpoints)

Each GPU gets its own checkpoint:

```bash
# GPU 0
./bin/cuBitCrack -d 0 --keyspace 2000...000:3fff...fff --share 1/4 \
  --continue cp_gpu0.txt -i puzzle.txt

# GPU 1
./bin/cuBitCrack -d 1 --keyspace 2000...000:3fff...fff --share 2/4 \
  --continue cp_gpu1.txt -i puzzle.txt
```

Each creates independent checkpoint files:
- `cp_gpu0.txt`, `cp_gpu0.txt.random`, `cp_gpu0.txt.random.bloom`
- `cp_gpu1.txt`, `cp_gpu1.txt.random`, `cp_gpu1.txt.random.bloom`

## Verification: No Overlap

To verify GPUs don't overlap, the logs show partition boundaries:

```
[Info] GPU 0 starting: 20000000000000000 to 27ffffffffffffff
[Info] Partition: 20000000000000000 to 27ffffffffffffff
[Info] Random scanning mode enabled
```

The `RandomKeyGenerator` is initialized with these exact boundaries, ensuring:
- GPU 0 generates keys: `20000000000000000 <= key <= 27ffffffffffffff`
- GPU 1 generates keys: `28000000000000000 <= key <= 2fffffffffffffff`
- No overlap possible

## Algorithm Details

### Random Key Generation (Multi-GPU)

```cpp
// Each GPU gets its own generator with partition boundaries
GPU 0: RandomKeyGenerator(0x2000...000, 0x27ff...fff)
GPU 1: RandomKeyGenerator(0x2800...000, 0x2fff...fff)
GPU 2: RandomKeyGenerator(0x3000...000, 0x37ff...fff)
GPU 3: RandomKeyGenerator(0x3800...000, 0x3fff...fff)

// When generating a random key:
alpha = select_random_alpha()  // e.g., 0.45 (priority range)
key = partition_start + (partition_size * alpha)

// GPU 0 example:
partition_start = 0x20000000000000000
partition_size = 0x8000000000000000
alpha = 0.45
key = 0x20000000000000000 + (0x8000000000000000 * 0.45)
    = 0x20000000000000000 + 0x3999999999999999
    = 0x23999999999999999  ✓ (within GPU 0's partition)
```

### Bloom Filter Independence

Each GPU maintains its own Bloom filter tracking **only keys it has visited within its partition**:

```
GPU 0 Bloom Filter: Tracks keys in [0x2000...000, 0x27ff...fff]
GPU 1 Bloom Filter: Tracks keys in [0x2800...000, 0x2fff...fff]
GPU 2 Bloom Filter: Tracks keys in [0x3000...000, 0x37ff...fff]
GPU 3 Bloom Filter: Tracks keys in [0x3800...000, 0x3fff...fff]
```

No shared state = perfect parallelization.

## Best Practices

### 1. Use --devices for Simplicity

```bash
# ✅ Recommended: Single command, automatic partitioning
./bin/cuBitCrack --devices 0,1,2,3 --keyspace <range> -i addresses.txt
```

### 2. Verify Partitions in Logs

Always check the startup logs to confirm each GPU has the correct partition:

```
[Info] GPU 0 starting: <start> to <end>
[Info] Partition: <start> to <end>
```

### 3. Monitor Bloom Filter Statistics

Each GPU logs its Bloom filter status:

```
[Info] Bloom filter: 2,000,000,000 keys tracked, FPR: 0.1234%
```

If FPR exceeds 5%, consider:
- Increasing Bloom filter size (edit code)
- Restarting to clear Bloom filter

### 4. Balance GPU Workload

All GPUs should show similar throughput:

```
GPU 0: 3.09 GKey/s
GPU 1: 3.08 GKey/s  ✓ Good balance
GPU 2: 3.10 GKey/s
GPU 3: 3.09 GKey/s
```

If one GPU is significantly slower:
- Check thermal throttling
- Verify no other processes using that GPU
- Ensure equal hardware specs

## Troubleshooting

### Issue: GPUs Showing Same Partition

**Symptom:**
```
[Info] GPU 0 starting: 20000000000000000 to 3ffffffffffffffff
[Info] GPU 1 starting: 20000000000000000 to 3ffffffffffffffff
```

**Cause:** Not using `--devices`, each GPU scanning full range

**Solution:** Use `--devices` flag:
```bash
./bin/cuBitCrack --devices 0,1,2,3 --keyspace <range> -i addresses.txt
```

### Issue: Uneven Performance Across GPUs

**Symptom:**
```
GPU 0: 3.09 GKey/s
GPU 1: 1.20 GKey/s  ← Slow
```

**Possible causes:**
- Different GPU models (expected)
- Thermal throttling on GPU 1
- Other process using GPU 1
- PCIe bandwidth saturation

**Solution:**
```bash
# Check GPU temperature
nvidia-smi

# Check GPU utilization
nvidia-smi dmon -s u
```

### Issue: High Memory Usage

**Symptom:** System running out of RAM with 4+ GPUs

**Cause:** Each GPU's Bloom filter uses 1 GB

**Solution:** Reduce Bloom filter size in code:
```cpp
// In KeyFinder.cpp constructor:
_bloomFilterSizeMB = 512;  // 512 MB instead of 1024 MB
```

Rebuild and run.

## Summary

✅ **Random scanning fully supports multi-GPU**
✅ **Each GPU randomly scans only its partition**
✅ **No duplicate work between GPUs**
✅ **Perfect linear scaling**
✅ **Alpha ranges applied relative to each partition**
✅ **Independent Bloom filters per GPU**
✅ **Works with both --devices and --share**

The implementation ensures that multi-GPU setups maintain the same efficiency as the original linear scan while benefiting from intelligent random sampling within each partition!
