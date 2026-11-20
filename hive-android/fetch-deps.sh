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

# ── audio.cpp TTS (Kokoro 82M — same engine/voices as ChimpFlow) ──
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

# Kokoro 82M BF16 GGUF — all 54 voice packs ship INSIDE the model file,
# so no embeddings dir is needed. Sourced from the local ChimpFlow
# install (copied, not re-downloaded, to keep this script fast/offline).
T="$DEST/assets/models/tts"
mkdir -p "$T"
SRC=/workspace/AI/ChimpFlow/engine/models/Kokoro-82M-GGUF
[ -s "$T/kokoro-82m-bf16.gguf" ] || { echo "== kokoro model =="; cp "$SRC/kokoro-82m-bf16.gguf" "$T/"; }

NDK="$HOME/Android/Sdk/ndk/27.0.12077973"
CLANG="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"

# ── eSpeak-ng phonemizer for Kokoro ─────────────────────────────────
# Android has no system eSpeak. Use Termux's upstream 1.52.0 arm64/x86_64
# build (same upstream version as Ubuntu's desktop libespeak-ng1), plus a
# local no-op pcaudio stub: eSpeak links pcaudiolib only for playback,
# which audio.cpp's phonemizer never performs. This avoids pcaudiolib's
# PulseAudio dependency chain. Data is zipped because recursive Android
# asset-directory enumeration is unreliable; KokoroTts extracts it with
# tmp+rename and self-heals incomplete installations.
EV=1.52.0-1
for abi in arm64-v8a x86_64; do
    case "$abi" in
        arm64-v8a) pre=aarch64; tarch=aarch64 ;;
        x86_64) pre=x86_64; tarch=x86_64 ;;
    esac
    OUT=app/src/main/jniLibs/$abi
    mkdir -p "$OUT"
    if [ ! -s "$OUT/libespeak-ng.so" ] || [ ! -s "$T/espeak-ng-data.zip" ]; then
        echo "== eSpeak-ng $EV $abi =="
        BD="build/espeak-$abi"
        rm -rf "$BD"
        mkdir -p "$BD/root"
        curl -sfSL -o "$BD/espeak.deb" \
          "https://packages.termux.dev/apt/termux-main/pool/main/e/espeak/espeak_${EV}_${pre}.deb"
        dpkg-deb -x "$BD/espeak.deb" "$BD/root"
        EU="$BD/root/data/data/com.termux/files/usr"
        cp "$EU/lib/libespeak-ng.so" "$OUT/"
        if [ ! -s "$T/espeak-ng-data.zip" ]; then
            python3 - "$EU/share/espeak-ng-data" "$T/espeak-ng-data.zip" <<'PY'
import os, sys, zipfile
src, out = sys.argv[1:]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    for root, _, files in os.walk(src):
        for name in files:
            path = os.path.join(root, name)
            z.write(path, os.path.relpath(path, src))
PY
        fi
    fi
    "$CLANG/${tarch}-linux-android26-clang" -shared -fPIC \
      -Wl,-soname,libpcaudio.so \
      -o "$OUT/libpcaudio.so" app/src/main/cpp/pcaudio_stub.c
    [ -s "$OUT/libc++_shared.so" ] || cp \
      "$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/${tarch}-linux-android/libc++_shared.so" \
      "$OUT/"
done

# ── libaudiocpp.so prebuilt per ABI ──────────────────────────────────
# Built OUTSIDE the app project: whisper.cpp and audio.cpp both vendor
# ggml → target-name collisions (CMP0002) if add_subdirectory'd together.
# Rebuilt only when the pinned source changes (stamp file).
for abi in arm64-v8a x86_64; do
    OUT=app/src/main/jniLibs/$abi
    mkdir -p "$OUT"
    STAMP="$OUT/.audiocpp-stamp"
    if [ -s "$OUT/libaudiocpp.so" ] && [ "$(cat "$STAMP" 2>/dev/null)" = "$AC-kokoro_tts" ]; then
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
      -DAUDIOCPP_MODEL_SET=custom -DAUDIOCPP_MODELS=kokoro_tts \
      -DAUDIOCPP_BUILD_C_API=ON \
      -DCMAKE_SHARED_LINKER_FLAGS="-llog"
      # sentencepiece's protobuf-lite logs via __android_log_write;
      # upstream has no Android build so nothing links liblog (2026-10-14).
    cmake --build "$BD" --target audiocpp -j"$(nproc)"
    cp "$BD/bin/libaudiocpp.so" "$OUT/"
    echo "$AC-kokoro_tts" > "$STAMP"
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
