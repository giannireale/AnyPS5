#include <cstdio>
#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/DirectMemory/DirectMemory.hpp"
#include "prx/libkernel/KernelErrors.hpp"

#include <cstring>
#include <stdexcept>
#include <string_view>
#include <mutex>
#include <set>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h>
#endif

int StartDynamicGuestModule_nid_no_patch(void* handle, size_t args, const void* argp,
                                       int (*initialize)(void*, size_t, const void*));

extern "C" {
void* APS5_VABI dlopen_nid_postfix(const char* path, int flags);
void* APS5_VABI dlsym_nid_postfix(void* handle, const char* name);
int APS5_VABI dlclose_nid_postfix(void* handle);
}

namespace {
constexpr int kRtldNow = 2;
}

extern "C" {

int APS5_VABI sceKernelDlsym(KernelModule handle, const char* symbol, void** addr) {
 if (!symbol || !addr) return SCE_KERNEL_ERROR_EFAULT;
 void* found = dlsym_nid_postfix(reinterpret_cast<void*>(static_cast<intptr_t>(handle)), symbol);
 if (!found) return SCE_KERNEL_ERROR_ESRCH;
 *addr = found;
 return 0;
}

int APS5_VABI sceKernelGetModuleInfoForUnwind(uint64_t addr, int flags, ModuleInfoForUnwind* info) {
 if (flags >= 3) return SCE_KERNEL_ERROR_EINVAL;
 if (!info) return SCE_KERNEL_ERROR_EFAULT;
#ifdef _WIN32
 if (info->st_size < sizeof(ModuleInfoForUnwind)) return SCE_KERNEL_ERROR_EINVAL;
 HMODULE module = nullptr;
 if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(addr), &module)) return SCE_KERNEL_ERROR_ESRCH;
 const auto* base = reinterpret_cast<const std::uint8_t*>(module);
 const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
 const auto* section = IMAGE_FIRST_SECTION(nt);
 std::uint64_t header = 0;
 std::uint64_t ehFrameSize = 0;
 // The relinker records the guest PT_GNU_EH_FRAME as an RVA in .ehmeta; .ehfram, when present, bounds .eh_frame.
 for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
  const std::string_view name(reinterpret_cast<const char*>(section->Name), strnlen(reinterpret_cast<const char*>(section->Name), IMAGE_SIZEOF_SHORT_NAME));
  if (name == ".ehmeta") header = reinterpret_cast<std::uint64_t>(base) + *reinterpret_cast<const std::uint32_t*>(base + section->VirtualAddress);
  else if (name == ".ehfram") ehFrameSize = section->Misc.VirtualSize;
 }
 std::memset(info, 0, sizeof(ModuleInfoForUnwind));
 info->st_size = sizeof(ModuleInfoForUnwind);
 char path[MAX_PATH] = {};
 GetModuleFileNameA(module, path, sizeof(path));
 const char* slash = std::strrchr(path, 92);
 std::strncpy(info->name, slash ? slash + 1 : path, sizeof(info->name) - 1);
 info->seg0_addr = reinterpret_cast<std::uint64_t>(base);
 info->seg0_size = nt->OptionalHeader.SizeOfImage;
 if (header != 0) {
  const auto* hdr = reinterpret_cast<const std::uint8_t*>(header);
  // eh_frame_hdr: version, eh_frame_ptr_enc, fde_count_enc, table_enc, eh_frame_ptr. Guest toolchains emit pcrel|sdata4.
  if (hdr[0] != 1 || hdr[1] != 0x1b) throw std::runtime_error("sceKernelGetModuleInfoForUnwind: unsupported eh_frame_hdr encoding");
  std::int32_t delta = 0;
  std::memcpy(&delta, hdr + 4, sizeof(delta));
  info->eh_frame_hdr_addr = header;
  info->eh_frame_addr = header + 4 + static_cast<std::int64_t>(delta);
  // The unwinder asks once per frame; measure each module's .eh_frame only once.
  static std::mutex sizeLock;
  static std::unordered_map<std::uint64_t, std::uint64_t> sizes;
  std::lock_guard sizeGuard(sizeLock);
  if (ehFrameSize == 0) {
   if (const auto cached = sizes.find(info->eh_frame_addr); cached != sizes.end()) ehFrameSize = cached->second;
  }
  if (ehFrameSize == 0) {
   // Walk CIE/FDE records up to the zero terminator.
   const auto* p = reinterpret_cast<const std::uint8_t*>(info->eh_frame_addr);
   const auto* limit = base + nt->OptionalHeader.SizeOfImage;
   while (p + 4 <= limit) {
    std::uint32_t length = 0;
    std::memcpy(&length, p, sizeof(length));
    if (length == 0) { p += 4; break; }
    if (length == 0xffffffffu) throw std::runtime_error("sceKernelGetModuleInfoForUnwind: 64-bit eh_frame records are not supported");
    p += 4 + static_cast<std::size_t>(length);
   }
   ehFrameSize = static_cast<std::uint64_t>(p - reinterpret_cast<const std::uint8_t*>(info->eh_frame_addr));
   sizes.emplace(info->eh_frame_addr, ehFrameSize);
  }
  info->eh_frame_size = ehFrameSize;
 }
 return 0;
#else
 (void)addr;
 NotImplemented_nid_no_patch(__func__);
 return 0;
#endif
}

int APS5_VABI sceKernelGetModuleInfoFromAddr(uint64_t addr, int n, ModuleInfo* r) {
 (void)addr;
 (void)n;
 (void)r;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

}
#ifdef _WIN32
namespace {
// Guest modules the executable does not need keep their initializer (crt init, then module_start)
// for sceKernelLoadStartModule, which hands them the caller's arguments. Each runs once; a module's
// deferred dependencies start first, without arguments, as the loader starts dependencies.
using ModuleInit = int (APS5_VABI*)(size_t, const void*, void*);
std::recursive_mutex g_startLock;
std::set<HMODULE> g_started;

int StartDeferredGuestModule(void* native, size_t args, const void* argp) {
 const auto module = static_cast<HMODULE>(native);
 std::lock_guard lock(g_startLock);
 const auto init = reinterpret_cast<ModuleInit>(reinterpret_cast<void*>(GetProcAddress(module, "__aps5_module_init")));
 if (init == nullptr || !g_started.insert(module).second) return 0;
 // The relinker lists the deferred guest modules this one imports from (consecutive strings).
 if (const auto* names = reinterpret_cast<const char*>(GetProcAddress(module, "__aps5_module_deps"))) {
  for (; *names != 0; names += std::strlen(names) + 1) {
   if (const HMODULE dependency = GetModuleHandleA(names)) StartDeferredGuestModule(dependency, 0, nullptr);
  }
 }
 return init(args, argp, nullptr);
}
}
#endif

extern "C" {

KernelModule APS5_VABI sceKernelLoadStartModule(const char* module_file_name, size_t args, const void* argp, uint32_t flags, const KernelLoadModuleOpt* opt, int* res) {
 (void)flags;
 (void)opt;
 if (res) *res = 0;
 if (!module_file_name) return static_cast<KernelModule>(SCE_KERNEL_ERROR_EFAULT);
 void* handle = dlopen_nid_postfix(module_file_name, kRtldNow);
 if (!handle) return static_cast<KernelModule>(SCE_KERNEL_ERROR_ENOENT);
#ifdef _WIN32
 const int result = StartDynamicGuestModule_nid_no_patch(handle, args, argp, StartDeferredGuestModule);
 if (res) *res = result;
#else
 (void)args;
 (void)argp;
#endif
 return static_cast<KernelModule>(reinterpret_cast<intptr_t>(handle));
}

int APS5_VABI sceKernelStopUnloadModule(KernelModule handle, size_t args, const void* argp, uint32_t flags, const KernelUnloadModuleOpt* opt, int* res) {
 (void)args;
 (void)argp;
 (void)flags;
 (void)opt;
 if (res) *res = 0;
 return dlclose_nid_postfix(reinterpret_cast<void*>(static_cast<intptr_t>(handle))) == 0 ? 0 : SCE_KERNEL_ERROR_ESRCH;
}

}

extern "C" {

int APS5_VABI __elf_phdr_match_addr_nid_postfix(ModuleInfo* module, std::uint64_t address) {
    (void)module;
    (void)address;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

// unknown signature
std::int32_t APS5_VABI sceKernelInternalMemoryGetModuleSegmentInfo_nid_postfix(void* result) {
    (void)result;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
