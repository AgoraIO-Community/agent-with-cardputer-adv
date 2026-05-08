<goal>
M8 has been achieved on the stable G.711A path: the real Cardputer ADV microphone capture path now feeds the existing WebRTC sender on hardware. The remaining tracked work after M8 is documentation alignment and explicit recording of the current Opus blocker.
</goal>

<context>
Read these first:
- `AGENT.md`
- `README.md`
- `docs/cardputer_agora_whip_codex_task.md`
- `platformio.ini`
- `src/main.c`
- `src/app_webrtc.c`
- `src/app_audio_fake.c`
- `src/app_config.h`

Current repo facts verified on May 8, 2026:
- `pio run` succeeds locally.
- Hardware logs show the full network / media path is working:
  - Wi-Fi connects
  - HTTPS works
  - WHIP `POST` returns `201`
  - ICE connects
  - DTLS/SRTP handshake succeeds
  - fake audio frames are sent continuously
  - Cardputer ADV mic capture starts on hardware
  - real mic frames are sent continuously on the G.711A path
- The direct `esp_peer` path is in use, not the higher-level `esp_webrtc` wrapper.
- `src/app_config.h` now uses placeholder defaults and optional `src/app_config_local.h`.
- The stable hardware-proven codec is `APP_AUDIO_CODEC_G711A`.
- Experimental Opus send attempts currently fail inside the prebuilt peer-default library after connection setup.

Useful discovery commands:
- `rg --files src`
- `sed -n '1,260p' docs/cardputer_agora_whip_codex_task.md`
- `sed -n '1,260p' src/main.c`
- `sed -n '1,260p' src/app_webrtc.c`
- `sed -n '1,220p' src/app_audio_fake.c`
</context>

<constraints>
- Preserve ESP32-S3 + PlatformIO + ESP-IDF.
- Keep `BOARD_M5STACK_CARDPUTER`.
- Do not revert the working WHIP / WebRTC / fake-audio path unless required for M8.
- Do not commit real Wi-Fi credentials, Agora secrets, or local mic calibration data.
- Public C module APIs should return `esp_err_t`.
- Use ESP-IDF logging with per-module tags.
- Keep the implementation observable in serial logs.
- Avoid broad refactors unless they materially simplify M8.
- Do not modify unrelated projects outside this repo.
</constraints>

<done_when>
- `GOAL.md` reflects that M8 is complete on the stable G.711A path.
- New microphone-path files exist:
  - `src/app_audio_i2s.c`
  - `src/app_audio_i2s.h`
- The firmware builds with `pio run`.
- `app_main()` can start the real audio path in place of, or gated alongside, `app_audio_fake`.
- The real audio path captures microphone data from Cardputer ADV hardware through I2S.
- The captured data is adapted into the format expected by the existing `app_webrtc_send_audio(...)` path on the stable G.711A route.
- Serial logs clearly show microphone task startup and live frame sending attempts.
- Hardware verification demonstrates that the WebRTC path still connects and that real mic frames are being sent on the stable route.
- `README.md` is aligned with the actual repo state and documents the M8 result plus the current Opus limitation.
</done_when>

<workflow>
1. Audit the current M7 implementation and identify the clean insertion point for microphone capture.
2. Inspect the Cardputer / ESP32-S3 microphone expectations from the task doc and nearby component examples.
3. Add a narrow I2S microphone module with placeholder-safe configuration.
4. Start with raw PCM capture and logging before attempting any codec or advanced normalization work.
5. Integrate the captured frames into the current WebRTC sender path with minimal disruption.
6. Keep `app_audio_fake` available as a fallback until mic capture is verified.
7. Run `pio run` after meaningful edits.
8. Use hardware logs to drive runtime fixes.
9. Update `README.md` only after the implementation state is real.
</workflow>

<verification_loop>
- Run `pio run` after the new mic-path files are added.
- Re-run `pio run` after significant changes to I2S config or frame format handling.
- If hardware is available, use:
  - `pio run -t upload`
  - `pio device monitor`
- Confirm at minimum:
  - Wi-Fi still connects
  - WHIP / ICE / DTLS still succeeds
  - the mic task starts
  - mic frames are sent or a clear error is logged
- Do not claim M8 complete without hardware evidence or a clearly documented external blocker.
</verification_loop>

<execution_rules>
- Preserve unrelated user changes.
- Prefer `rg` over `grep`.
- Use the patch/edit tool for manual file edits.
- Run focused verification before broad changes.
- Do not paper over failures.
- Keep the final answer concise.
</execution_rules>

<output_contract>
Record the actual M8 result in tracked repo docs, keep the build green and the tracked configuration safe, and make the stable path and current Opus blocker explicit. The final response must state whether `pio run` succeeded, whether hardware upload/monitor were run, any unresolved blockers, and the concrete next step after M8.
</output_contract>
