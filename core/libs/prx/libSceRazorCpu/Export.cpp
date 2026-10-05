#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

uint32_t APS5_VABI sceRazorCpuIsCapturing(void) {
 return 0;
}

int APS5_VABI sceRazorCpuJobManagerDispatch(const void* args) {
 (void)args;
 return 0;
}

int APS5_VABI sceRazorCpuJobManagerJob(const void* args) {
 (void)args;
 return 0;
}

int APS5_VABI sceRazorCpuJobManagerSequence(const void* args) {
 (void)args;
 return 0;
}

}
