// mm2s_packet: descriptor-driven memory-to-stream packetizer.
//
// Reads one PacketDesc per packet from `desc`, then streams that packet's
// payload out of `mem` as a single AXI4-Stream packet:
//   - one descriptor  == one AXIS packet
//   - TLAST asserted on the final beat of each packet
//   - TKEEP is the valid-byte mask (full on whole beats, partial on the tail)
//   - TUSER carries packet_id as a debug-friendly sideband
//
// Payload for a packet lives at a 64-byte-aligned byte offset in `mem`
// (offset_bytes must be a multiple of 64).
#include <ap_int.h>
#include <hls_stream.h>
#include <ap_axi_sdata.h>

// ap_axiu<Data, User, Id, Dest>: 512b data, 32b user (packet_id), no id/dest.
typedef ap_axiu<512, 32, 0, 0> axis512_t;

struct PacketDesc {
    uint32_t offset_bytes;   // 64B-aligned byte offset of payload in `mem`
    uint32_t len_bytes;      // payload length in bytes
    uint32_t packet_id;      // carried out on TUSER
    uint32_t reserved;
};

void mm2s_packet(const ap_uint<512>* mem,
                 const PacketDesc*   desc,
                 hls::stream<axis512_t>& s,
                 uint32_t num_packets)
{
#pragma HLS INTERFACE m_axi     port=mem    offset=slave bundle=gmem0
#pragma HLS INTERFACE m_axi     port=desc   offset=slave bundle=gmem1
#pragma HLS INTERFACE axis      port=s
#pragma HLS INTERFACE s_axilite port=mem         bundle=control
#pragma HLS INTERFACE s_axilite port=desc        bundle=control
#pragma HLS INTERFACE s_axilite port=num_packets bundle=control
#pragma HLS INTERFACE s_axilite port=return      bundle=control

    for (uint32_t p = 0; p < num_packets; p++) {
        PacketDesc d = desc[p];
        uint32_t beats     = (d.len_bytes + 63) / 64;
        uint32_t word_base = d.offset_bytes >> 6;   // 64B-aligned word index
        for (uint32_t b = 0; b < beats; b++) {
#pragma HLS PIPELINE II=1
            axis512_t x;
            uint32_t remaining = d.len_bytes - b * 64;
            x.data = mem[word_base + b];
            if (remaining >= 64) {
                x.keep = ~ap_uint<64>(0);
            } else {
                x.keep = (ap_uint<64>(1) << remaining) - 1;
            }
            x.strb = x.keep;
            x.last = (b == beats - 1);
            x.user = d.packet_id;
            s.write(x);
        }
    }
}
