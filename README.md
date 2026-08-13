# KeyGuesser

## Build Process

### 1. Generate model files

1. Download `model.safetensors` from the [MuScriptor-small repo on Hugging Face](https://huggingface.co/MuScriptor/muscriptor-small/tree/main) and place it in the same folder as the Python scripts.
2. Run `exporter.py`. This generates:
   - `prefill.onnx`
   - `prefill.onnx.data`
   - `step.onnx`
   - `step.onnx.data`
3. These files will be moved into the correct directory once the plugin binary is compiled in step 3.

### 2. Add dependencies

- **JUCE** — download from [juce.com](https://juce.com/). This project uses version 8.
- **libsamplerate** — download and build from [libsndfile/libsamplerate](https://github.com/libsndfile/libsamplerate).
- **ONNX Runtime (static)** — download from the [onnxruntime-libs releases page](https://github.com/csukuangfj/onnxruntime-libs/releases/tag/v1.25.0):
  - Windows: `onnxruntime-win-x64-static_lib-MT-Release-1.25.0.tar.bz2`
  - macOS: `onnxruntime-osx-x86_64-static_lib-1.25.0.zip`

Make sure the file paths match what's expected in `CMakeLists.txt`.

### 3. Compile with CMake

`CMakeLists.txt` may need to be edited to point to JUCE's location on your machine.

```bash
mkdir cmake_build
cd cmake_build
cmake ..
cmake --build . --config Release
```

## Citation

If you use MuScriptor, please cite:

```bibtex
@misc{rouard2026muscriptoropenmodelmultiinstrument,
      title={MuScriptor: An Open Model for Multi-Instrument Music Transcription}, 
      author={Simon Rouard and Michael Krause and Axel Roebel and Carl-Johann Simon-Gabriel and Alexandre Défossez},
      year={2026},
      eprint={2607.08168},
      archivePrefix={arXiv},
      primaryClass={cs.SD},
      url={https://arxiv.org/abs/2607.08168}, 
}
```