#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include <atomic>

// No network is emulated: contexts, templates and requests can be created, but any request
// that would touch the network fails with the library's network error.
static constexpr int ERROR_NETWORK = static_cast<int>(0x80435001);
static std::atomic<int> g_nextHandle{1};

namespace {

constexpr int ERROR_NOT_FOUND = static_cast<int>(0x8095F004);
constexpr int ERROR_INVALID_ARG = static_cast<int>(0x8095177A);

struct SslData {
    char* ptr;
    size_t size;
};

struct SslCaCerts {
    SslData* certs;
    size_t num;
    void* pool;
};

}

extern "C" {

int APS5_VABI sceSslFreeCaCerts(int ssl_ctx_id, void* ca_certs) {
    (void)ssl_ctx_id;
    if (!ca_certs) return ERROR_INVALID_ARG;
    *static_cast<SslCaCerts*>(ca_certs) = {};
    return 0;
}

int APS5_VABI sceSslGetCaCerts(int ssl_ctx_id, void* ca_certs) {
    (void)ssl_ctx_id;
    if (!ca_certs) return ERROR_INVALID_ARG;
    *static_cast<SslCaCerts*>(ca_certs) = {};
    return ERROR_NOT_FOUND;
}

int APS5_VABI sceSslInit_nid_postfix(uint64_t pool_size) {
    (void)pool_size;
    return g_nextHandle.fetch_add(1, std::memory_order_relaxed);
}

int APS5_VABI sceSslTerm_nid_postfix(int ssl_ctx_id) {
    (void)ssl_ctx_id;
    return 0;
}

int APS5_VABI sceSslClose() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceSslGetSerialNumber() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceSslGetMemoryPoolStats(int ssl_ctx_id, void* stats) {
    (void)ssl_ctx_id;
    (void)stats;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSslLoadCert() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSslGetSubjectName(int ssl_ctx_id, const void* cert) {
    (void)ssl_ctx_id;
    (void)cert;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSslGetIssuerName(int ssl_ctx_id, const void* cert) {
    (void)ssl_ctx_id;
    (void)cert;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSslGetNameEntryCount(int ssl_ctx_id, const void* name) {
    (void)ssl_ctx_id;
    (void)name;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSslGetNameEntryInfo(int ssl_ctx_id, const void* name, int entry_num, char* oid, uint64_t max_oid_len, uint8_t* value, uint64_t max_value_len, uint64_t* value_len) {
    (void)ssl_ctx_id;
    (void)name;
    (void)entry_num;
    (void)oid;
    (void)max_oid_len;
    (void)value;
    (void)max_value_len;
    (void)value_len;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSslFreeSslCertName(int ssl_ctx_id, void* name) {
    (void)ssl_ctx_id;
    (void)name;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceSslGetPem(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
