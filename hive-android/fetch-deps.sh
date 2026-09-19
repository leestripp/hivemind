#!/usr/bin/env bash
# Fetch the sherpa-onnx native libs + streaming Zipformer2 EN model that
# the app bundles (kept out of git for size). Safe to re-run.
set -euo pipefail
cd "$(dirname "$0")"
V=1.13.8
DEST=app/src/main

if [ ! -f "$DEST/jniLibs/arm64-v8a/libsherpa-onnx-jni.so" ]; then
    echo "== sherpa-onnx v$V android natives =="
    curl -sfSL -o /tmp/sherpa-android.tar.bz2 \
      "https://github.com/k2-fsa/sherpa-onnx/releases/download/v$V/sherpa-onnx-v$V-android.tar.bz2"
    tar -xjf /tmp/sherpa-android.tar.bz2 -C /tmp
    mkdir -p "$DEST/jniLibs"
    cp -r /tmp/jniLibs/arm64-v8a /tmp/jniLibs/x86_64 "$DEST/jniLibs/"
    rm -f "$DEST"/jniLibs/*/libsherpa-onnx-c-api.so \
          "$DEST"/jniLibs/*/libsherpa-onnx-cxx-api.so \
          "$DEST"/jniLibs/*/README.md
fi

M=https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-en-2023-06-26/resolve/main
D="$DEST/assets/models/stt"
mkdir -p "$D"
declare -A F=(
  [encoder.onnx]=encoder-epoch-99-avg-1-chunk-16-left-128.int8.onnx
  [decoder.onnx]=decoder-epoch-99-avg-1-chunk-16-left-128.int8.onnx
  [joiner.onnx]=joiner-epoch-99-avg-1-chunk-16-left-128.int8.onnx
  [tokens.txt]=tokens.txt
)
for out in "${!F[@]}"; do
    [ -s "$D/$out" ] || curl -sfSL -o "$D/$out" "$M/${F[$out]}"
done
echo "deps ready"
