// mm2s: reads words 512-bit values from memory and streams them out as AXI4-Stream.
// tlast is asserted on the final word. Feeds the RTL kernel's s_axis slave port.
#include <ap_int.h>
#include <hls_stream.h>
#include <ap_axi_sdata.h>

typedef ap_axiu<512, 0, 0, 0> word512_t;

void mm2s(ap_uint<512>* mem,
          hls::stream<word512_t>& s,
          uint32_t words)
{
#pragma HLS INTERFACE m_axi     port=mem    offset=slave bundle=gmem
#pragma HLS INTERFACE axis      port=s
#pragma HLS INTERFACE s_axilite port=mem    bundle=control
#pragma HLS INTERFACE s_axilite port=words  bundle=control
#pragma HLS INTERFACE s_axilite port=return bundle=control

    for (uint32_t i = 0; i < words; i++) {
#pragma HLS PIPELINE II=1
        word512_t x;
        x.data = mem[i];
        x.keep = (ap_uint<64>)-1;
        x.last = (i == words - 1);
        s.write(x);
    }
}
