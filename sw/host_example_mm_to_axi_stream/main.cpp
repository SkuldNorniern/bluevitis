#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "xrt/xrt_bo.h"
#include <experimental/xrt_xclbin.h>
#include "xrt/xrt_device.h"
#include "xrt/xrt_kernel.h"

using namespace std;

namespace {
    constexpr unsigned int kDeviceId  = 0;
    constexpr uint32_t     kWordCount = 16;
    constexpr size_t       kWordBytes = 64;
    constexpr size_t       kBufBytes  = kWordCount * kWordBytes;
    constexpr size_t       kU32PerBuf = kBufBytes / sizeof(uint32_t);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        cerr << "Usage: " << argv[0] << " <XCLBIN file>\n";
        return EXIT_FAILURE;
    }
    const string xclbin_file = argv[1];

    cout << "[AXI4-Stream example: mm2s -> kernel s_axis (AxiStreamSlave) -> out]\n";
    xrt::device device{kDeviceId};
    xrt::uuid uuid = device.load_xclbin(xclbin_file);

    auto krnl = xrt::kernel(device, uuid, "kernel:{kernel_1}");
    auto mm2s = xrt::kernel(device, uuid, "mm2s:{mm2s_1}");

    // mm2s args: arg0=mem (PLRAM[0]), arg2=words  (arg1 is the AXIS stream port).
    // kernel args: arg0=word_count (scalar), arg1=out (PLRAM[1]).
    auto boIn  = xrt::bo(device, kBufBytes, mm2s.group_id(0));
    auto boOut = xrt::bo(device, kBufBytes, krnl.group_id(1));
    auto in    = boIn.map<uint32_t*>();
    auto out   = boOut.map<uint32_t*>();

    for (size_t k = 0; k < kU32PerBuf; ++k) in[k] = 0xC0DE0000u + static_cast<uint32_t>(k);
    memset(out, 0, kBufBytes);

    boIn.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    boOut.sync(XCL_BO_SYNC_BO_TO_DEVICE);

    cout << "streaming " << kWordCount << " x 512-bit words through external AXI4-Stream...\n";
    auto k_run = krnl(kWordCount, boOut);    // kernel waits on s_axis
    auto m_run = mm2s(boIn, kWordCount);     // data mover feeds the stream
    m_run.wait();
    k_run.wait();
    boOut.sync(XCL_BO_SYNC_BO_FROM_DEVICE);

    size_t mismatches = 0;
    for (size_t k = 0; k < kU32PerBuf; ++k) {
        if (out[k] != in[k]) {
            if (mismatches < 8)
                cout << "  mismatch at u32[" << k << "]: in=0x" << hex << in[k]
                     << " out=0x" << out[k] << dec << "\n";
            ++mismatches;
        }
    }

    if (mismatches == 0) {
        cout << "all " << kU32PerBuf << " u32 lanes matched through the external stream\n";
        cout << "\nTEST PASSED\n";
        return EXIT_SUCCESS;
    }
    cout << mismatches << " / " << kU32PerBuf << " lanes mismatched\n";
    cout << "\nTEST FAILED\n";
    return EXIT_FAILURE;
}
