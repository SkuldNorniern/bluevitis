// Host for the AXI4-Stream *packet* example.
//
//   mm2s_packet  reads one PacketDesc per packet and emits one AXIS packet
//                (TLAST per packet, TKEEP byte mask on the tail beat,
//                 TUSER = packet_id).
//   kernel       consumes the stream, and for each packet writes a 16-byte
//                summary {packet_id, beats, bytes, error} to memory using
//                TLAST for packet boundaries and popcount(TKEEP) for bytes.
//
// This proves the consumer understands packet boundaries instead of just
// copying stream payload back to memory.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <random>
#include <vector>
#include <chrono>
#include <thread>

#include "xrt/xrt_bo.h"
#include <experimental/xrt_xclbin.h>
#include "xrt/xrt_device.h"
#include "xrt/xrt_kernel.h"

using namespace std;

namespace {
    constexpr unsigned int kDeviceId = 0;
    constexpr size_t       kBeatBytes = 64;             // 512-bit AXIS beat
    constexpr size_t       kSlotBytes = 64;             // one summary record per 64B slot

    struct PacketDesc {
        uint32_t offset_bytes;   // 64B-aligned payload offset in data buffer
        uint32_t len_bytes;
        uint32_t packet_id;      // carried on TUSER
        uint32_t reserved;
    };

    struct ResultRec {           // matches the 128 low bits the kernel writes
        uint32_t packet_id;
        uint32_t beats;
        uint32_t bytes;
        uint32_t error;
    };

    inline uint32_t roundup64(uint32_t n) { return (n + 63u) & ~63u; }
    inline uint32_t beats_for(uint32_t len) { return (len + 63u) / 64u; }

    // Poll a run to completion with a timeout, so a hang is reported instead
    // of blocking forever. Returns true if the run completed.
    bool wait_run(xrt::run& r, const char* name, int timeout_s) {
        for (int i = 0; i < timeout_s * 20; ++i) {
            ert_cmd_state s = r.state();
            if (s == ERT_CMD_STATE_COMPLETED) return true;
            if (s == ERT_CMD_STATE_ERROR || s == ERT_CMD_STATE_ABORT) {
                cerr << "  [" << name << "] run entered error state " << s << "\n";
                return false;
            }
            this_thread::sleep_for(chrono::milliseconds(50));
        }
        cerr << "  [" << name << "] TIMEOUT after " << timeout_s << "s (state="
             << r.state() << ")\n";
        return false;
    }
}

// Run one batch of packets through the design and check the per-packet
// summaries the kernel produced. Returns true on success.
static bool run_batch(xrt::device& device, xrt::kernel& krnl, xrt::kernel& mm2s,
                      const vector<pair<uint32_t,uint32_t>>& pkts,  // (len_bytes, packet_id)
                      const char* label, bool verbose)
{
    const uint32_t n = static_cast<uint32_t>(pkts.size());

    // Lay payloads out at 64B-aligned offsets and build descriptors.
    vector<PacketDesc> desc(n);
    uint32_t off = 0;
    for (uint32_t i = 0; i < n; ++i) {
        desc[i] = { off, pkts[i].first, pkts[i].second, 0 };
        off += roundup64(pkts[i].first);
    }
    const uint32_t data_bytes = (off == 0) ? kBeatBytes : off;

    auto boData = xrt::bo(device, data_bytes,             mm2s.group_id(0));
    auto boDesc = xrt::bo(device, n * sizeof(PacketDesc), mm2s.group_id(1));
    auto boOut  = xrt::bo(device, n * kSlotBytes,         krnl.group_id(1));

    auto data = boData.map<uint8_t*>();
    auto dsc  = boDesc.map<PacketDesc*>();
    auto out  = boOut.map<uint8_t*>();

    // Fill payloads with a recognizable per-packet pattern (content is not
    // checked by the consumer, but makes waveforms readable).
    memset(data, 0, data_bytes);
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t b = 0; b < pkts[i].first; ++b)
            data[desc[i].offset_bytes + b] = static_cast<uint8_t>(pkts[i].second + b);
    memcpy(dsc, desc.data(), n * sizeof(PacketDesc));
    memset(out, 0, n * kSlotBytes);

    boData.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    boDesc.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    boOut.sync(XCL_BO_SYNC_BO_TO_DEVICE);

    // Bind arguments explicitly by index (kernel.xml ids):
    //   mm2s_packet: mem=0, desc=1, s=2 (stream, host-unset), num_packets=3
    //   kernel:      num_packets=0, out=1, s_axis=2 (stream, host-unset)
    xrt::run k_run(krnl);
    k_run.set_arg(0, n);
    k_run.set_arg(1, boOut);
    k_run.start();

    xrt::run m_run(mm2s);
    m_run.set_arg(0, boData);
    m_run.set_arg(1, boDesc);
    m_run.set_arg(3, n);
    m_run.start();

    bool ran = wait_run(m_run, "mm2s_packet", 120);
    ran &= wait_run(k_run, "kernel", 120);
    if (!ran) {
        cout << "  [" << label << "] FAILED (run did not complete)\n";
        return false;
    }
    boOut.sync(XCL_BO_SYNC_BO_FROM_DEVICE);

    if (verbose)
        cout << "  id        bytes  beats  error\n";

    size_t fails = 0;
    uint64_t total_bytes = 0;
    for (uint32_t i = 0; i < n; ++i) {
        ResultRec r;
        memcpy(&r, out + i * kSlotBytes, sizeof(r));
        const uint32_t exp_beats = beats_for(pkts[i].first);
        const uint32_t exp_bytes = pkts[i].first;
        const uint32_t exp_id    = pkts[i].second;

        bool ok = (r.packet_id == exp_id) && (r.beats == exp_beats) &&
                  (r.bytes == exp_bytes) && (r.error == 0);
        if (!ok) ++fails;
        total_bytes += r.bytes;

        if (verbose) {
            cout << "  0x" << setw(6) << setfill('0') << hex << r.packet_id << dec
                 << setfill(' ') << setw(7) << r.bytes
                 << setw(7) << r.beats
                 << setw(7) << r.error;
            if (!ok) cout << "   <-- MISMATCH (exp id=0x" << hex << exp_id << dec
                          << " bytes=" << exp_bytes << " beats=" << exp_beats << ")";
            cout << "\n";
        }
    }

    // Aggregate invariants: TLAST count == num_packets (we got n records),
    // and total bytes counted from TKEEP == sum of descriptor lengths.
    uint64_t exp_total = 0;
    for (auto& p : pkts) exp_total += p.first;
    if (total_bytes != exp_total) {
        cout << "  [" << label << "] aggregate byte mismatch: got " << total_bytes
             << " expected " << exp_total << "\n";
        ++fails;
    }

    cout << "  [" << label << "] " << (fails == 0 ? "OK" : "FAILED")
         << " (" << n << " packets, " << exp_total << " bytes)\n";
    return fails == 0;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        cerr << "Usage: " << argv[0] << " <XCLBIN file>\n";
        return EXIT_FAILURE;
    }
    const string xclbin_file = argv[1];

    cout << "[AXI4-Stream packet example: mm2s_packet -> kernel s_axis -> summaries]\n";
    xrt::device device{kDeviceId};
    xrt::uuid uuid = device.load_xclbin(xclbin_file);

    auto krnl = xrt::kernel(device, uuid, "kernel:{kernel_1}");
    auto mm2s = xrt::kernel(device, uuid, "mm2s_packet:{mm2s_packet_1}");

    bool all_ok = true;

    // ---- Fixed test cases (len_bytes, packet_id) -------------------------
    // 20 -> 1 beat partial; 64 -> 1 beat full; 65 -> 2 beats (tail=1);
    // 1500 -> 24 beats (tail=28). Non-sequential ids prove TUSER is carried.
    cout << "\n-- fixed cases --\n";
    vector<pair<uint32_t,uint32_t>> fixed = {
        {  20, 0xCAFE0 },
        {  64, 0x1234A },
        {  65, 0x00042 },
        {1500, 0xBEEF7 },
    };
    all_ok &= run_batch(device, krnl, mm2s, fixed, "fixed", /*verbose=*/true);
    if (!all_ok) {
        cout << "\nTEST FAILED (fixed cases)\n";   // skip randomized on first failure
        return EXIT_FAILURE;
    }

    // ---- Randomized cases ------------------------------------------------
    cout << "\n-- randomized cases --\n";
    mt19937 rng(0xA11CE);
    uniform_int_distribution<uint32_t> len_dist(1, 4096);
    uniform_int_distribution<uint32_t> id_dist(0, 0xFFFFF);
    for (int t = 0; t < 5; ++t) {
        uniform_int_distribution<uint32_t> cnt_dist(1, 8);
        uint32_t n = cnt_dist(rng);
        vector<pair<uint32_t,uint32_t>> pkts;
        for (uint32_t i = 0; i < n; ++i)
            pkts.emplace_back(len_dist(rng), id_dist(rng));
        string label = "rand#" + to_string(t);
        all_ok &= run_batch(device, krnl, mm2s, pkts, label.c_str(), /*verbose=*/false);
    }

    cout << "\n" << (all_ok ? "TEST PASSED" : "TEST FAILED") << "\n";
    return all_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
