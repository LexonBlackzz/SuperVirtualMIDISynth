#include "svmsapi.h"

#include <cstdio>
#include <cstring>
#include <dlfcn.h>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::puts("FAIL: expected path to libsvmsapi.so");
        return 1;
    }
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        std::printf("FAIL: dlopen: %s\n", dlerror());
        return 1;
    }
    using GetInterface = SVMS_Result (SVMS_CALL *)(
        uint32_t, uint32_t, SVMS_Interface*);
    const auto getInterface = reinterpret_cast<GetInterface>(
        dlsym(library, "SVMS_GetInterface"));
    SVMS_Interface api{};
    const bool negotiated = getInterface &&
        getInterface(SVMS_ABI_VERSION_1, sizeof(api), &api) == SVMS_RESULT_OK &&
        api.abi_version == SVMS_ABI_VERSION_1 &&
        api.struct_size >= sizeof(api) &&
        (api.capabilities & SVMS_CAP_EXACT_MONOTONIC_NS) != 0u &&
        (api.capabilities & SVMS_CAP_SHORT_EVENT_BATCH) != 0u &&
        api.create_session && api.destroy_session && api.send_short_batch &&
        api.get_runtime_clock;
    uint64_t now = 0u, frequency = 0u;
    const bool clockOkay = negotiated &&
        api.get_runtime_clock(&now, &frequency) == SVMS_RESULT_OK &&
        now != 0u && frequency == 1000000000ull;
    const bool invalidSessionRejected = negotiated &&
        api.send_short(0u, 0x00643c90u) == SVMS_RESULT_NOT_INITIALIZED;
    const bool newerAbiRejected = getInterface &&
        getInterface(SVMS_ABI_VERSION_1 + 1u, sizeof(api), &api) ==
            SVMS_RESULT_UNSUPPORTED_ABI;
    dlclose(library);
    if (!negotiated || !clockOkay || !invalidSessionRejected ||
        !newerAbiRejected) {
        std::puts("FAIL: Linux native ABI negotiation contract");
        return 1;
    }
    std::puts("PASS: Linux native SVMS ABI V1 negotiation");
    return 0;
}
