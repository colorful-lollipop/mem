# Stress & Fuzz Guide

## Build (Release + Stress)

```bash
cmake -S demo/vpsdemo -B build_vps_stress -DVPSDEMO_ENABLE_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build_vps_stress --target vpsdemo_testkit_stress_runner
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
./build_vps_stress/vpsdemo_testkit_stress_runner
```

## ASan/UBSan/LSan Build

```bash
cmake -S demo/vpsdemo -B build_vps_asan -DVPSDEMO_ENABLE_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build_vps_asan
ctest --test-dir build_vps_asan -L stress --output-on-failure
```

## TSan Build

```bash
cmake -S demo/vpsdemo -B build_vps_tsan -DVPSDEMO_ENABLE_TESTS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build_vps_tsan
ctest --test-dir build_vps_tsan -L stress --output-on-failure
```

## Fuzz Build (Clang)

```bash
cmake -S demo/vpsdemo -B build_vps_fuzz \
  -DVPSDEMO_ENABLE_TESTS=ON \
  -DVPSDEMO_ENABLE_FUZZ=ON \
  -DCMAKE_CXX_COMPILER=clang++
cmake --build build_vps_fuzz --target vpsdemo_codec_fuzz vpsdemo_testkit_codec_fuzz
./build_vps_fuzz/tests/fuzz/vpsdemo_testkit_codec_fuzz -max_total_time=300
```

也可以直接跑 fuzz smoke：

```bash
ctest --test-dir build_vps_fuzz -L fuzz --output-on-failure
```

## PID / Log Sampling

```bash
mkdir -p stress_out
MEMRPC_STRESS_DURATION_SEC=3600 \
MEMRPC_STRESS_PID_FILE=stress_out/pids.txt \
./build_vps_stress/vpsdemo_testkit_stress_runner | tee stress_out/stress.log
```

## Environment Variables

- `MEMRPC_STRESS_DURATION_SEC` / `MEMRPC_STRESS_WARMUP_SEC`
- `MEMRPC_STRESS_THREADS`
- `MEMRPC_STRESS_ECHO_WEIGHT` / `MEMRPC_STRESS_ADD_WEIGHT` / `MEMRPC_STRESS_SLEEP_WEIGHT`
- `MEMRPC_STRESS_PAYLOAD_SIZES`
- `MEMRPC_STRESS_HIGH_PRIORITY_PCT`
- `MEMRPC_STRESS_MAX_SLEEP_MS`
- `MEMRPC_STRESS_BURST_INTERVAL_MS` / `MEMRPC_STRESS_BURST_durationMs` / `MEMRPC_STRESS_BURST_MULTIPLIER`
- `MEMRPC_STRESS_NO_PROGRESS_SEC`
- `MEMRPC_STRESS_PID_FILE`
- `MEMRPC_STRESS_SEED`
- `MEMRPC_STRESS_MAX_REQUEST_BYTES` / `MEMRPC_STRESS_MAX_RESPONSE_BYTES`
