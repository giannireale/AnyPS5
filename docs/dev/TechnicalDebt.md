# Project technical debt

### Build

- Building on Windows requires a specific version of mingw - MinGW-w64 GCC 15.2.0 (`winlibs-gcc15`, `x86_64-ucrt-posix-seh`)
- Even compiled prx libraries on Windows require nearby (static linking of these dependencies causes conflicts):
  - libgcc_s_seh-1.dll
  - libstdc++-6.dll
  - libwinpthread-1.dll

### Silent stubs

Throughout the project, every function at every stage either **does exactly what it's supposed to or throws an exception**. Everywhere... except:
- [libSceSaveDataDialog.native](../../core/libs/prx/libSceSaveDataDialog.native/Export.cpp)
- [libSceCommonDialog](../../core/libs/prx/libSceCommonDialog/Export.cpp)
- [libSceHmd](../../core/libs/prx/libSceHmd/Export.cpp) implements only the disconnected-headset path: initialization succeeds, device queries report `NotDetected`, and opening a device returns `DeviceDisconnected`. Headset support, tracking, rendering and additional HMD exports are not implemented; SDK-level ABI compatibility and in-game behaviour remain unverified.
- [libSceAudioPropagation](../../core/libs/prx/libSceAudioPropagation/Export.cpp) implements only the no-acoustic-propagation path: the system keeps a 16-byte header in the title's CPU memory (no GPU memory), rooms, portals, sources and materials are tracked handles, attributes are validated and have no effect, no rays are requested and sources report no audio paths. Ray results, path calculation and audio paths are accepted only when empty and throw otherwise; `sceAudioPropagationSourceRender` writes silence (zeroes) to each source's output buffer for the given size, without a dry signal. Objects still alive when their system is destroyed become unusable, and their later destroy or unregister is a no-op: PPSA21567 releases its reference-counted room, portal and source owners just before `sceAudioPropagationSystemDestroy`, so the order for every object is not proven. Occlusion, reflections and reverb from level geometry are absent; in-game behaviour remains unverified.
- [libSceNpCommerce](../../core/libs/prx/libSceNpCommerce/Export.cpp) - the PS Store icon show/hide calls do nothing
- [sceVideoOutOpen](../../core/libs/prx/libSceVideoOut/src/Output.cpp) validates the priority and CPU affinity that the open param requests for the VideoOut service thread but does not apply them: the port's present and vblank threads are host threads, and guest priorities and affinities do not reach host scheduling
- The shader recompiler [skips baryctric coordinates](../../core/shader/recompiler/Recompiler.cpp) (is not even passed to SpirvTargetOptions at row 212).
- [libSceAvPlayer](../../core/libs/prx/libSceAvPlayer/Export.cpp): `sceAvPlayerSetLogCallback` accepts a callback that is never called, as the player produces no log messages, and `sceAvPlayerSetAvailableBandwidth` has no effect, as it governs HLS sources, which `sceAvPlayerAddSource` does not implement.
- [sceAvPlayerSetTrickSpeed](../../core/libs/prx/libSceAvPlayer/src/Source.cpp) (libSceAvPlayer) with a negative speed runs the clock backwards but delivers no frames; when a forward speed is set again, playback resumes from the rewound time.

### Unknown function info

- [sceVideoOutOpen](../../core/libs/prx/libSceVideoOut/src/Output.cpp) (libSceVideoOut) - the open param's first word is unknown (PPSA21564 passes 16; it is not the byte size, since the affinity mask is at offset 16); only 16 is accepted. Whether the param continues past offset 24 is also unknown
- [AMPR WaitOnAddress / WaitOnCounter](../../core/libs/prx/libkernel/Apr/src/Apr.cpp) (libSceAmpr) - compare encoding assumed to be the WAIT_REG_MEM one (0 always, 1 <, 2 <=, 3 ==, 4 !=, 5 >=, 6 >)
- [AMPR markers](../../core/libs/prx/libSceAmpr/Export.cpp) (libSceAmpr) - signatures taken from the [ampr_emu](https://github.com/drakmor/ampr_emu) reimplementation, not confirmed on a title: `sceAmprCommandBufferSetMarkerWithColor` takes the color by pointer, the push and measure variants by value. Colors are not recorded
- [sceAgcSetSubmitMode](../../core/libs/prx/libSceAgc/Misc/src/Suspend.cpp) (libSceAgc) - mode values unknown; only 0 is accepted
- [zARR5aCmkoY](../../core/libs/prx/libSceAgc/DcbFlow/src/Control.cpp) (libSceAgc) - unknown name, signature
- [qj7QZpgr9Uw](../../core/libs/prx/libSceAgc/DcbState/src/ContextState.cpp) (libSceAgc) - unknown name
- [fd5Bp5tGTgo](../../core/libs/prx/libSceAgc/Misc/src/ShaderFusion.cpp) (libSceAgc) - unknown name
- [dolOmWH+huQ](../../core/libs/prx/libSceAgc/Misc/src/ShaderFusion.cpp) (libSceAgc) - unknown name
- [V++UgBtQhn0](../../core/libs/prx/libSceAgc/Misc/src/PacketInfo.cpp) (libSceAgc) - unknown name
- [gQkqkLttcpw](../../core/libs/prx/libSceAgc/Acb/src/Control.cpp) (libSceAgc) - unknown name, signature
- [sceKernelInternalMemoryGetModuleSegmentInfo](../../core/libs/prx/libkernel/Module/src/Module.cpp) (libkernel) - unknown signature
- [sceKernelSyncOnAddressWait](../../core/libs/prx/libkernel/SyncOnAddress/src/SyncOnAddress.cpp) (libkernel) - the only known caller passes a null timeout and a name string as the fourth argument; the timeout is assumed to point to microseconds like the other kernel waits, and the name is ignored
- [sceKernelMapNamedFlexibleMemoryInternal](../../core/libs/prx/libkernel/DirectMemory/Export.cpp) (libkernel) - flag 0x8000 unknown; only the sceKernelMapNamedFlexibleMemory flags are accepted
- [sceLibcInternalBacktraceForGame](../../core/libs/prx/libc/src/HeapDiagnostics.cpp) (libSceLibcInternal, implemented in libc) - unknown signature
- [sceLibcInternalHeapErrorReportForGame](../../core/libs/prx/libc/src/HeapDiagnostics.cpp) (libSceLibcInternal, implemented in libc) - unknown signature
- [__progname](../../core/libs/prx/libkernel/System/src/Process.cpp) (libkernel) - unknown data export
- [pthread_barrierattr_setpshared](../../core/libs/prx/libkernel/Pthread/Posix/Barrier.cpp) (libkernel) - PTHREAD_PROCESS_SHARED throws: FreeBSD 9.0 libthr rejects it with EINVAL and 11.0 accepts it, and which one the console follows is unknown
- [sceSslClose](../../core/libs/prx/libSceSsl/Export.cpp) (libSceSsl) - unknown signature
- [sceSslGetSerialNumber](../../core/libs/prx/libSceSsl/Export.cpp) (libSceSsl) - unknown signature
- [X+4jdIS75P0](../../core/libs/prx/libSceAudioIn/Export.cpp) (libSceAudioIn) - unknown name, signature
- [AOWqIYsgVHs](../../core/libs/prx/libSceContentExport/Export.cpp) (libSceContentExport) - unknown name, signature
- [GQTObcITIXI](../../core/libs/prx/libSceShare/Export.cpp) (libSceShare) - unknown name, signature
- [BnMAMrsfVWo](../../core/libs/prx/libc/src/HeapDiagnostics.cpp) (libc) - unknown name, signature
- AudioIn, NpSessionSignaling and PlayerInvitationDialog exports added without an implementation have assumed signatures
- [libSceAudioPropagation](../../core/libs/prx/libSceAudioPropagation/Export.cpp) (libSceAudioPropagation) - signatures and struct descriptors recovered from PPSA21567's calls (#481), not from documentation. The system options layout is unknown (only checked for non-null); the fourth argument of `sceAudioPropagationSourceCalculateAudioPaths` is unknown; `sceAudioPropagationSourceGetAudioPath` is assumed to output an 8-byte path handle. The meaning of the `RenderInfo` word at +0x28 is unknown (PPSA21567 and PPSA21564 pass 2); only 2 is accepted. Both titles pass one `RenderInfo` per call: a count of 0 throws, and larger counts are handled element by element without proof. No error code is known, so invalid input throws instead of returning one
- [sceAvPlayerStartEx](../../core/libs/prx/libSceAvPlayer/Export.cpp) (libSceAvPlayer) - start info layout unknown; it is ignored and playback starts as with `sceAvPlayerStart`
- [sceAvPlayerInit / sceAvPlayerInitEx](../../core/libs/prx/libSceAvPlayer/src/Player.cpp) (libSceAvPlayer) - behaviour without a memory replacement unknown; frame and sample buffers then come from the guest heap
- [SceAvPlayerVideoEx](../../core/libs/SceTypes.hpp) (libSceAvPlayer) - frame rate field and encoding unknown; it is left zero in frame and stream info
- [AJM Opus decoder](../../core/libs/prx/libSceAjm.native/src/Ajm.cpp) (libSceAjm.native, codec 24) - the initialize parameter layout (`u32 channels, u32 sample rate, u32`, seen as `2, 48000, 0`) and the little-endian `u16` byte count before each packet come from one title (The Smurfs Dreams); only 48000 Hz and a zero third word are accepted

### Functional

- The shader recompiler [ignores `s_setreg_b32`](../../core/shader/recompiler/Translation/src/ScalarInstructions.cpp): writes to MODE (float rounding and denormal controls) and the other hardware registers have no effect.
- [Shader recompilation](../../core/shader/recompiler/Recompiler.cpp) currently occurs right before it was transferred to Vulkan with caching, but should be moved to the [relinker](../../core/relinker/main.cpp) stage. For this purpose, [shader/recompiler](../../core/shader/recompiler) was written completely independently from [libs/prx](../../core/libs/prx).
- [v_fma_f64](../../core/shader/recompiler/SpirvBackend/src/SpirvAlu/SpirvAluEmitterFloat64.cpp) is fused exactly only for normal inputs: with a subnormal input it is a separate multiply and add, which rounds twice.
- The executable file that [relinker](../../core/relinker/elfpatcher/src/windows/WindowsPeWriter.cpp) generates opens the console when launched, which is inconvenient for playability.
- `--to-intel` does not lower RDPRU, whose AMD performance counters have no Intel equivalent; the [matcher](../../core/relinker/codegen/src/x86/Amd64OnlyInstructionMatcher.cpp) fails the relink instead.
- `--to-intel` fails the relink when a branch enters an AMD-only site at an address the stub does not keep as an instruction boundary, or through an eight bit displacement that cannot reach the stub; the [converter](../../core/relinker/codegen/src/Amd64OnlyConverter.cpp) names the branch in the diagnostic.
- [libSceJpegEnc](../../core/libs/prx/libSceJpegEnc/Export.cpp) encodes 4:2:2 sampling requests with 4:2:0 chroma subsampling, and grayscale requests as a 3-component JPEG with neutral chroma instead of a single-component one: the [JPEG encoder](../../core/Decoder/Jpeg/src/Jpeg.cpp) (stb) only produces 3-component 4:2:0 and 4:4:4 images.
- `--to-intel` does not lower RDPRU/MCOMMIT; the [matcher](../../core/relinker/codegen/src/x86/Amd64OnlyInstructionMatcher.cpp) fails the relink instead. SHA-1 instructions with a memory operand and SHA-256 instructions with a RIP-relative operand fail the relink.
- The length-changing path of the [instruction rewriter](../../core/relinker/codegen/src/x86/X64InstructionRewriter.cpp) is not used by the [converter](../../core/relinker/codegen/src/Amd64OnlyConverter.cpp): it does not adjust VEX/0F38/0F3A RIP-relative operands, data-to-code references (relocations, FDEs, jump tables) or segment sizes, so every substitution keeps the instruction length.
- The libc [SSE4a trap emulation](../../core/libs/prx/libc/src/specifics/windows/Sse4aEmulation.hpp) on Windows is superseded by `--to-intel` and remains only until the relinked title has been verified without it.
- `--to-intel` guest module trampolines are covered only by a synthetic relinker test; no game title has been verified with them on Linux or Windows.
- `--to-intel` replaces the approximate VEX.128 register forms of VRSQRTPS and VRCPPS with [correctly rounded](../../core/relinker/codegen/src/x86/ReciprocalLowering.cpp) 1/sqrt(x) and 1/x, not with the AMD approximation: its tables are not public. Intel's approximation of 1/sqrt(1.0) is 0x3F7FF000, and two Newton-Raphson steps on it settle on 0.99999994 instead of 1.0; a renormalised identity quaternion then turns into a NaN axis in PPSA21564, which the console does not show. Refined results now reach the exact value; unrefined ones can still differ from the console in the low bits. Each site costs an out-of-line stub (two jumps, a spill below the red zone, SQRTPS/DIVPS). The 256-bit, scalar, legacy and memory forms keep the native instruction, as does a 4-byte site with no movable instruction after it for the jump (5 sites in PPSA21564).
- [sceKeyboardGetKey2Char](../../core/libs/prx/libSceKeyboard/src/keyboard_impl.cpp) (libSceKeyboard) translates only the 101-key (US) arrangement and throws for the 106-key (Japanese) one; Ctrl and Alt do not change the character.
- [libSceAudiodec](../../core/libs/prx/libSceAudiodec/Export.cpp) throws for the 24-bit PCM word size (`iBwPcm` 0): its sample layout is unknown. ATRAC9 decoding is covered only by configuration and error tests, as no ATRAC9 encoder is available for a fixture.
- [libScePngEnc](../../core/libs/prx/libScePngEnc/Export.cpp) honours `filter_type` only as all filters (adaptive) or a single filter: the [PNG encoder](../../core/Decoder/Png/src/Png.cpp) (stb) cannot restrict adaptive filtering to a subset, so the first filter in the mask is used.
- [libScePlayerInvitationDialog](../../core/libs/prx/libScePlayerInvitationDialog/libScePlayerInvitationDialog.cpp) simulates dialog completion without displaying UI or sending invitations; its parameter ABI remains unverified.
