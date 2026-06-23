import FIFO::*;
import Axi4Stream::*;

typedef struct {
    Bit#(64) addr;
    Bit#(32) bytes;
} MemPortReq deriving (Eq, Bits);

interface MemPortIfc;
    method ActionValue#(MemPortReq) writeReq;
    method ActionValue#(Bit#(512))  writeWord;
endinterface

interface KernelMainIfc;
    method Action start(Bit#(32) numPackets);
    method ActionValue#(Bool) done;
    interface MemPortIfc mem;
endinterface

// popcount of a 64-bit TKEEP mask -> number of valid bytes in the beat.
function Bit#(32) popcount64(Bit#(64) x);
    Bit#(32) c = 0;
    for (Integer i = 0; i < 64; i = i + 1)
        c = c + zeroExtend(x[i]);
    return c;
endfunction

// mkKernelMain consumes one AXIS packet per descriptor and writes a 16-byte
// summary record per packet to memory (one 64-byte slot each):
//   word[31:0]    = packet_id   (from TUSER)
//   word[63:32]   = beats        (beats observed for the packet)
//   word[95:64]   = bytes        (sum of popcount(TKEEP) across beats)
//   word[127:96]  = error_flags  (bit0: a non-final beat had a partial TKEEP)
// It uses TLAST to find packet boundaries and TKEEP to count valid bytes,
// rather than copying stream payload back to memory.
module mkKernelMain#(Axi4StreamSlaveIfc#(512, 32) axisIn)(KernelMainIfc);
    FIFO#(Bit#(32))   startQ     <- mkFIFO;
    FIFO#(Bool)       doneQ      <- mkFIFO;
    FIFO#(MemPortReq) writeReqQ  <- mkFIFO;
    FIFO#(Bit#(512))  writeWordQ <- mkFIFO;

    Reg#(Bool)     started     <- mkReg(False);
    Reg#(Bit#(32)) total       <- mkReg(0);   // packets expected
    Reg#(Bit#(32)) packetsSeen <- mkReg(0);   // packets completed (TLAST seen)
    Reg#(Bit#(8))  drainWait   <- mkReg(0);

    // per-packet accumulators, reset on each TLAST
    Reg#(Bit#(32)) beatsAcc <- mkReg(0);
    Reg#(Bit#(32)) bytesAcc <- mkReg(0);
    Reg#(Bit#(32)) errAcc   <- mkReg(0);

    rule systemStart(!started);
        startQ.deq;
        let n = startQ.first;
        if (n == 0) begin
            doneQ.enq(True);
        end else begin
            total       <= n;
            packetsSeen <= 0;
            beatsAcc    <= 0;
            bytesAcc    <= 0;
            errAcc      <= 0;
            drainWait   <= 0;
            // one 64-byte summary slot per packet
            writeReqQ.enq(MemPortReq{ addr: 64'd0, bytes: n << 6 });
            started <= True;
        end
    endrule

    rule drainStream(started && packetsSeen < total);
        match {.data, .keep, .last, .usr} <- axisIn.get;
        let beats = beatsAcc + 1;
        let bytes = bytesAcc + popcount64(keep);
        // a partial beat is only legal as the final beat of a packet
        let err   = errAcc | ((!last && keep != '1) ? 32'h1 : 32'h0);

        if (last) begin
            Bit#(32) pid = usr;
            Bit#(512) result = zeroExtend({err, bytes, beats, pid});
            writeWordQ.enq(result);
            packetsSeen <= packetsSeen + 1;
            beatsAcc <= 0;
            bytesAcc <= 0;
            errAcc   <= 0;
        end else begin
            beatsAcc <= beats;
            bytesAcc <= bytes;
            errAcc   <= err;
        end
    endrule

    rule startDrainWait(started && packetsSeen == total && total != 0 && drainWait == 0);
        drainWait <= 64;
    endrule
    rule countDrainWait(started && drainWait > 0);
        drainWait <= drainWait - 1;
        if (drainWait == 1) begin
            started <= False;
            doneQ.enq(True);
        end
    endrule

    interface MemPortIfc mem;
        method ActionValue#(MemPortReq) writeReq;
            let r = writeReqQ.first; writeReqQ.deq; return r;
        endmethod
        method ActionValue#(Bit#(512)) writeWord;
            let w = writeWordQ.first; writeWordQ.deq; return w;
        endmethod
    endinterface

    method Action start(Bit#(32) numPackets) if (!started);
        startQ.enq(numPackets);
    endmethod
    method ActionValue#(Bool) done;
        let d = doneQ.first; doneQ.deq; return d;
    endmethod
endmodule
