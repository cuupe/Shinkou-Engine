# Shinkou Audio Mixer Test

`shinkou_audio_mixer_test` is a native Win32-control diagnostic utility for
the engine audio system. It does not create an engine window or use a render
backend. On startup it creates four temporary WAV fixtures, loads them through
`shinkou::audio::AudioSystem`, and routes them through the engine's Music, SFX,
Voice, and Ambient buses.

The UI can:

- start/stop individual voices or all voices together;
- change Master and per-bus volume and mute state;
- change individual voice volume, pan, and looping;
- insert realtime Low-pass, High-pass, Compressor, Limiter, Delay, and Reverb
  effects on any bus, including Master;
- tune cutoff/threshold, wet mix, delay time, and feedback while audio is
  running;
- load and play an external WAV file on a selected bus;
- show active voice and loaded asset counts.

The effect rack is backed by `AudioEffectChain` and a custom miniaudio node,
so it processes the live mix graph rather than rewriting the source files. The
same engine API also exposes track-level effect chains for editor/runtime use.

For the recommended follow-up order, agent task boundaries, realtime-thread
constraints, and acceptance criteria, see `docs/AudioMixingExpansionPlan.md`.

Build on Windows with:

```powershell
cmake --preset mingw-debug
cmake --build out/build/mingw-debug --target shinkou_audio_mixer_test
```

The executable is written to `out/build/mingw-debug/shinkou_audio_mixer_test.exe`.
