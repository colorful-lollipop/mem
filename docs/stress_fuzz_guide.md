# Stress & Fuzz Guide

## Build (Release + Stress)

```bash
cmake -S . -B build_stress -DMEMRPC_ENABLE_STRESS_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build_stress
```

## Run 2h Random Stress

```bash
MEMRPC_STRESS_DURATION_SEC=7200 \
MEMRPC_STRESS_WARMUP_SEC=600 \
MEMRPC_STRESS_THREADS=8 \
MEMRPC_STRESS_ECHO_WEIGHT=70 \
MEMRPC_STRESS_ADD_WEIGHT=20 \
MEMRPC_STRESS_SLEEP_WEIGHT=10 \
MEMRPC_STRESS_PAYLOAD_SIZES=0,16,128,512,1024,2048 \
./build_stress/tests/apps/minirpc/memrpc_minirpc_stress_runner
```

## ASan/UBSan/LSan Build

```bash
cmake -S . -B build_asan -DMEMRPC_ENABLE_STRESS_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build_asan
ctest --test-dir build_asan -L stress --output-on-failure
```

## TSan Build

```bash
cmake -S . -B build_tsan -DMEMRPC_ENABLE_STRESS_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build_tsan
ctest --test-dir build_tsan -L stress --output-on-failure
```

## Fuzz Build (Clang)

```bash
cmake -S . -B build_fuzz -DMEMRPC_ENABLE_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build build_fuzz
./build_fuzz/tests/fuzz/minirpc_codec_fuzz -max_total_time=300
```

## RSS Sampling Script

```bash
MEMRPC_STRESS_DURATION_SEC=3600 \
SAMPLE_INTERVAL_SEC=5 \
bash tools/stress/run_minirpc_stress.sh
```

Output in `stress_out/`: `stress.log`, `rss.csv`, `pids.txt`.

## Environment Variables

- `MEMRPC_STRESS_DURATION_SEC` / `MEMRPC_STRESS_WARMUP_SEC`
- `MEMRPC_STRESS_THREADS`
- `MEMRPC_STRESS_ECHO_WEIGHT` / `MEMRPC_STRESS_ADD_WEIGHT` / `MEMRPC_STRESS_SLEEP_WEIGHT`
- `MEMRPC_STRESS_PAYLOAD_SIZES`
- `MEMRPC_STRESS_HIGH_PRIORITY_PCT`
- `MEMRPC_STRESS_MAX_SLEEP_MS`
- `MEMRPC_STRESS_BURST_INTERVAL_MS` / `MEMRPC_STRESS_BURST_DURATION_MS` / `MEMRPC_STRESS_BURST_MULTIPLIER`
- `MEMRPC_STRESS_NO_PROGRESS_SEC`
- `MEMRPC_STRESS_PID_FILE`
- `MEMRPC_STRESS_SEED`
- `MEMRPC_STRESS_MAX_REQUEST_BYTES` / `MEMRPC_STRESS_MAX_RESPONSE_BYTES`
