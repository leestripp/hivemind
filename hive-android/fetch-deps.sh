#!/usr/bin/env bash
# Fetch the whisper.cpp source + q5_1 ggml models the app bundles
# (kept out of git for size). Safe to re-run.
set -euo pipefail
cd "$(dirname "$0")"
DEST=app/src/main
D="$DEST/assets/models/stt"
mkdir -p "$D"

# ── whisper.cpp STT (replaces sherpa-onnx) ───────────────────────────
# Pinned source release, extracted into app/src/main/cpp/whisper-cpp/
# (gitignored — fetched, not vendored into git).
WV=1.9.4
WDIR=app/src/main/cpp/whisper-cpp
if [ ! -f "$WDIR/src/whisper.cpp" ]; then
    echo "== whisper.cpp v$WV source =="
    curl -sfSL -o /tmp/whisper-cpp.tar.gz \
      "https://github.com/ggml-org/whisper.cpp/archive/refs/tags/v$WV.tar.gz"
    rm -rf "$WDIR" /tmp/whisper.cpp-$WV
    tar -xzf /tmp/whisper-cpp.tar.gz -C /tmp
    mkdir -p app/src/main/cpp
    mv "/tmp/whisper.cpp-$WV" "$WDIR"
fi

# q8_0 quantized (the FAST ggml ARM dotprod kernel — q5_1 has no
# optimized NEON path and decodes slower than fp16 on-device):
# tiny.en-q8_0 (~44MB) = live partials, base.en-q8_0 (~82MB) = final pass.
WM=https://huggingface.co/ggerganov/whisper.cpp/resolve/main
for m in ggml-base.en-q8_0.bin ggml-tiny.en-q8_0.bin; do
    [ -s "$D/$m" ] || { echo "== $m =="; curl -sfSL -o "$D/$m" "$WM/$m"; }
done

# ── audio.cpp TTS (pocket-tts — same engine/voices as the desktop) ──
# Pinned to the exact commit ChimpFlow serves on the desktop (v0.9.0).
AC=795c45fb
ADIR=app/src/main/cpp/audio-cpp
if [ ! -f "$ADIR/include/audiocpp.h" ]; then
    echo "== audio.cpp @ $AC source =="
    curl -sfSL -o /tmp/audio-cpp.tar.gz \
      "https://github.com/0xShug0/audio.cpp/archive/$AC.tar.gz"
    rm -rf "$ADIR" /tmp/audio.cpp-$AC*
    tar -xzf /tmp/audio-cpp.tar.gz -C /tmp
    mkdir -p app/src/main/cpp
    mv /tmp/audio.cpp-$AC* "$ADIR"
fi

# pocket-tts model + the voice embeddings matching the desktop app's
# per-agent mapping (~/.config/hive-client-voice/prefs.json). Sourced
# from the local ChimpFlow install (provenance: audio.cpp GGUF repo on
# HF) — copied, not re-downloaded, to keep this script fast/offline.
T="$DEST/assets/models/tts"
mkdir -p "$T/embeddings"
SRC=/projects/audio-cpp-test/models
[ -s "$T/pocket-tts-english-q8_0.gguf" ] || { echo "== pocket-tts model =="; cp "$SRC/pocket-tts-english-q8_0.gguf" "$T/"; }
for v in alba anna bill_boerst charles eponine jean mary paul peter_yearsley vera; do
    [ -s "$T/embeddings/$v.safetensors" ] || cp "$SRC/embeddings/$v.safetensors" "$T/embeddings/"
done

# ── libaudiocpp.so prebuilt per ABI ──────────────────────────────────
# Built OUTSIDE the app project: whisper.cpp and audio.cpp both vendor
# ggml → target-name collisions (CMP0002) if add_subdirectory'd together.
# Rebuilt only when the pinned source changes (stamp file).
NDK="$HOME/Android/Sdk/ndk/27.0.12077973"
for abi in arm64-v8a x86_64; do
    OUT=app/src/main/jniLibs/$abi
    mkdir -p "$OUT"
    STAMP="$OUT/.audiocpp-stamp"
    if [ -s "$OUT/libaudiocpp.so" ] && [ "$(cat "$STAMP" 2>/dev/null)" = "$AC" ]; then
        continue
    fi
    echo "== libaudiocpp.so $abi (one-time, ~4 min) =="
    BD="build/audiocpp-$abi"
    cmake -S "$ADIR" -B "$BD" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI="$abi" -DANDROID_PLATFORM=android-26 \
      -DCMAKE_BUILD_TYPE=Release \
      -DENGINE_ENABLE_CUDA=OFF -DENGINE_ENABLE_HIP=OFF \
      -DENGINE_ENABLE_VULKAN=OFF -DENGINE_ENABLE_METAL=OFF \
      -DENGINE_ENABLE_NATIVE_CPU=OFF \
      -DENGINE_BUILD_EXAMPLES=OFF -DENGINE_BUILD_TESTS=OFF \
      -DAUDIOCPP_MODEL_SET=custom -DAUDIOCPP_MODELS=pocket_tts \
      -DAUDIOCPP_BUILD_C_API=ON \
      -DCMAKE_SHARED_LINKER_FLAGS="-llog"
      # sentencepiece's protobuf-lite logs via __android_log_write;
      # upstream has no Android build so nothing links liblog (2026-10-14).
    cmake --build "$BD" --target audiocpp -j"$(nproc)"
    cp "$BD/bin/libaudiocpp.so" "$OUT/"
    echo "$AC" > "$STAMP"
done

# NDK OpenMP runtime — audio.cpp links it (ENGINE_ENABLE_OPENMP default
# ON; even OFF the ggml/sentencepiece objects still need libomp), and
# Android doesn't ship it. Bundled per-ABI into jniLibs.
for abi in arm64-v8a x86_64; do
    case "$abi" in
        arm64-v8a) pre=aarch64 ;; x86_64) pre=x86_64 ;;
    esac
    SRCOMP="$NDK/toolchains/llvm/prebuilt/linux-x86_64/lib/clang/18/lib/linux/$pre/libomp.so"
    OUT=app/src/main/jniLibs/$abi
    mkdir -p "$OUT"
    [ -s "$OUT/libomp.so" ] || { echo "== libomp $abi =="; cp "$SRCOMP" "$OUT/"; }
done

echo "deps ready"
