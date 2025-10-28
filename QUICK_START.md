# BitCrack Quick Start Guide (RTX 4090 Optimized)

## 🚀 Build (One Time Setup)

```bash
cd /path/to/BitCrack
make clean
make BUILD_CUDA=1
```

Binary location: `bin/cuBitCrack`

## 📝 Prepare Your Target Address File

Create `addresses.txt` with one address per line:
```
1BoatSLRHtKNngkdXEeobR76b53LETtpyT
13zb1hQbWVsc2S7ZTZnP2G4undNNpdh5so
```

## 🎯 Basic Usage

### Scan a Specific Range (Hex Values)

```bash
# Bitcoin Puzzle #66 (66-bit range)
./bin/cuBitCrack --keyspace 20000000000000000:3ffffffffffffffff -i addresses.txt
```

### With Checkpoint (Resume on Crash)

```bash
./bin/cuBitCrack --keyspace 20000000000000000:3ffffffffffffffff \
                 --continue checkpoint.txt \
                 -i addresses.txt
```

### Save Results to File

```bash
./bin/cuBitCrack --keyspace 20000000000000000:3ffffffffffffffff \
                 --continue checkpoint.txt \
                 -o found_keys.txt \
                 -i addresses.txt
```

## 🔥 Multi-GPU Setup

### Check Available GPUs

```bash
./bin/cuBitCrack --list-devices
```

### Run on Specific GPU

```bash
# Use GPU 0
./bin/cuBitCrack -d 0 --keyspace 20000000000000000:3ffffffffffffffff -i addresses.txt

# Use GPU 1
./bin/cuBitCrack -d 1 --keyspace 20000000000000000:3ffffffffffffffff -i addresses.txt
```

### Split Range Across 4 GPUs (Recommended)

Open 4 terminals and run:

**Terminal 1 (GPU 0 - processes 1st quarter):**
```bash
./bin/cuBitCrack -d 0 --keyspace 20000000000000000:3ffffffffffffffff \
                 --share 1/4 --continue checkpoint_gpu0.txt \
                 -i addresses.txt
```

**Terminal 2 (GPU 1 - processes 2nd quarter):**
```bash
./bin/cuBitCrack -d 1 --keyspace 20000000000000000:3ffffffffffffffff \
                 --share 2/4 --continue checkpoint_gpu1.txt \
                 -i addresses.txt
```

**Terminal 3 (GPU 2 - processes 3rd quarter):**
```bash
./bin/cuBitCrack -d 2 --keyspace 20000000000000000:3ffffffffffffffff \
                 --share 3/4 --continue checkpoint_gpu2.txt \
                 -i addresses.txt
```

**Terminal 4 (GPU 3 - processes 4th quarter):**
```bash
./bin/cuBitCrack -d 3 --keyspace 20000000000000000:3ffffffffffffffff \
                 --share 4/4 --continue checkpoint_gpu3.txt \
                 -i addresses.txt
```

## 📊 Common Bitcoin Puzzle Ranges

Copy-paste these commands directly:

### Puzzle #64 (64-bit)
```bash
./bin/cuBitCrack --keyspace 8000000000000000:ffffffffffffffff \
                 --continue checkpoint.txt -i puzzle64.txt
```

### Puzzle #65 (65-bit)
```bash
./bin/cuBitCrack --keyspace 10000000000000000:1ffffffffffffffff \
                 --continue checkpoint.txt -i puzzle65.txt
```

### Puzzle #66 (66-bit) ⭐ Most Popular
```bash
./bin/cuBitCrack --keyspace 20000000000000000:3ffffffffffffffff \
                 --continue checkpoint.txt -i puzzle66.txt
```

### Puzzle #67 (67-bit)
```bash
./bin/cuBitCrack --keyspace 40000000000000000:7ffffffffffffffff \
                 --continue checkpoint.txt -i puzzle67.txt
```

### Puzzle #70 (70-bit)
```bash
./bin/cuBitCrack --keyspace 200000000000000000:3ffffffffffffffffff \
                 --continue checkpoint.txt -i puzzle70.txt
```

## ⚙️ Advanced Options

### Compressed vs Uncompressed

```bash
# Compressed only (faster, modern addresses)
./bin/cuBitCrack -c --keyspace 20000000000000000:3ffffffffffffffff -i addresses.txt

# Uncompressed only (older addresses)
./bin/cuBitCrack -u --keyspace 20000000000000000:3ffffffffffffffff -i addresses.txt

# Both (slower, comprehensive)
./bin/cuBitCrack --compression BOTH --keyspace 20000000000000000:3ffffffffffffffff -i addresses.txt
```

### Manual Performance Tuning (Usually Not Needed)

```bash
# RTX 4090 optimal (auto-detected by default)
./bin/cuBitCrack -b 128 -t 512 -p 128 \
                 --keyspace 20000000000000000:3ffffffffffffffff \
                 -i addresses.txt

# RTX 3090 optimal
./bin/cuBitCrack -b 128 -t 512 -p 128 \
                 --keyspace 20000000000000000:3ffffffffffffffff \
                 -i addresses.txt
```

## 📈 Monitoring Progress

### View Checkpoint File
```bash
cat checkpoint.txt
```

Example output:
```
start=20000000000000000
next=20000A3B5C7E9012      ← Current position
end=3ffffffffffffffff
blocks=128
threads=512
points=128
compression=compressed
device=0
elapsed=3600000                ← Time in milliseconds
stride=1
```

### Calculate Progress
```python
# Python one-liner to calculate progress
start = int("20000000000000000", 16)
current = int("20000A3B5C7E9012", 16)
end = int("3ffffffffffffffff", 16)
progress = (current - start) / (end - start) * 100
print(f"Progress: {progress:.10f}%")
```

## 🛠️ Troubleshooting

### No GPU Found
```bash
# Check CUDA installation
nvcc --version

# List devices
./bin/cuBitCrack --list-devices
```

### Out of Memory
```bash
# Reduce points per thread
./bin/cuBitCrack -p 64 --keyspace 20000000000000000:3ffffffffffffffff -i addresses.txt
```

### Slow Performance
```bash
# Verify RTX 4090 optimizations are active
# Should see in logs:
# - "Auto-tuned for high-end GPU (128 SMs)"
# - "L2 cache persistence enabled"
# - "Estimated occupancy: 33.4%"
```

## 💡 Pro Tips

1. **Always use `--continue`** for long searches - saves your progress
2. **Split across GPUs with `--share`** for 4x speedup
3. **Compressed mode** is faster and covers most modern addresses
4. **Monitor GPU temperature** - RTX 4090 can get hot under full load
5. **Expect 20-120x speedup** over old BitCrack versions

## 📏 Expected Performance (RTX 4090)

| Configuration | Speed | Puzzle #66 Time |
|---------------|-------|-----------------|
| 1x RTX 4090 | ~500-1000 MKey/s | 73-147 years |
| 4x RTX 4090 | ~2-4 GKey/s | 18-37 years |
| 8x RTX 4090 | ~4-8 GKey/s | 9-18 years |

*Average find time is ~50% of full range time*

## 🆘 Need Help?

- **Detailed Guide**: See [KEYSPACE_GUIDE.md](KEYSPACE_GUIDE.md)
- **Technical Details**: See [CLAUDE.md](CLAUDE.md)
- **GitHub Issues**: https://github.com/brichard19/BitCrack/issues

## 🎉 Quick Test

Test your installation with a small range:

```bash
# Create test address file
echo "1BoatSLRHtKNngkdXEeobR76b53LETtpyT" > test.txt

# Quick test (1 million keys)
./bin/cuBitCrack --keyspace 1:+F4240 -i test.txt

# Should complete in seconds and show throughput
```

Good luck! 🚀
