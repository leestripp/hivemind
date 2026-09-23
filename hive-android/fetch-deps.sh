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

echo "deps ready"
