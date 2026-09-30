#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

int APS5_VABI sceNgs2CalcWaveformBlock(const Ngs2WaveformFormat* format, uint32_t sample_pos, uint32_t num_samples, Ngs2WaveformBlock* block) {
 (void)format;
 (void)sample_pos;
 (void)num_samples;
 (void)block;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceNgs2GeomApply(const Ngs2GeomListenerWork* listener, const Ngs2GeomSourceParam* source, Ngs2GeomAttribute* out_attrib, uint32_t flags) {
 (void)listener;
 (void)source;
 (void)out_attrib;
 (void)flags;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceNgs2GeomCalcListener(const Ngs2GeomListenerParam* param, Ngs2GeomListenerWork* out_work, uint32_t flags) {
 (void)param;
 (void)out_work;
 (void)flags;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceNgs2GeomResetListenerParam(Ngs2GeomListenerParam* out_listener_param) {
 (void)out_listener_param;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceNgs2GeomResetSourceParam(Ngs2GeomSourceParam* out_source_param) {
 (void)out_source_param;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceNgs2PanGetVolumeMatrix(Ngs2PanWork* work, const Ngs2PanParam* params, uint32_t num_params, uint32_t matrix_format, float* out_volume_matrix) {
 (void)work;
 (void)params;
 (void)num_params;
 (void)matrix_format;
 (void)out_volume_matrix;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceNgs2PanInit(Ngs2PanWork* work, const float* speaker_angles, float unit_angle, uint32_t num_speakers) {
 (void)work;
 (void)speaker_angles;
 (void)unit_angle;
 (void)num_speakers;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceNgs2ParseWaveformData(const void* data, size_t data_size, Ngs2WaveformInfo* info) {
 (void)data;
 (void)data_size;
 (void)info;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

APS5_EXPORT("WCayTgob7-o", sceNgs2Stub_WCayTgob7_o);
int APS5_VABI sceNgs2Stub_WCayTgob7_o() { NotImplemented_nid_no_patch(__func__); return 0; }
APS5_EXPORT("9eic4AmjGVI", sceNgs2Stub_9eic4AmjGVI);
int APS5_VABI sceNgs2Stub_9eic4AmjGVI() { NotImplemented_nid_no_patch(__func__); return 0; }
}
