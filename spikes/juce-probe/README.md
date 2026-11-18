# juce-probe (WP-0132 spike — throwaway, not product code)

JUCE 9.0.5 window + audio-device probe used as evidence for ADR-0007. Never linked into `src/`.

```bash
cmake -S spikes/juce-probe -B spikes/juce-probe/build -DCMAKE_BUILD_TYPE=Release
cmake --build spikes/juce-probe/build -j6
B="spikes/juce-probe/build/dz_spike_artefacts/Release/DhunZilla Spike"
DZ_NO_WINDOW=1 DZ_DEVICE="PipeWire Sound Server" DZ_SR=48000 DZ_BUFFER=128 "$B"   # probe only
DZ_SPIKE_EXIT=1 DZ_SNAPSHOT=/tmp/w.png "$B"                                      # window + PNG, then exit
```
Plays silence only. Results: `evidence/`.
