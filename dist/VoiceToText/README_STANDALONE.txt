VoiceToText standalone package

Run VoiceToText.exe.

On first run the app creates config.json next to the executable.
If the selected KB Whisper model is missing, the app downloads it into the
models folder automatically.

GPU mode is used when all of these are true:
- an NVIDIA driver is installed
- ggml-cuda.dll is included in this folder
- the required cuBLAS DLLs are included in this folder

If GPU is unavailable, the app falls back to CPU.
