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
    method Action start(Bit#(32) wordCount);
    method ActionValue#(Bool) done;
    interface MemPortIfc mem;
endinterface

module mkKernelMain#(Axi4StreamSlaveIfc#(512) axisIn)(KernelMainIfc);
    FIFO#(Bit#(32))   startQ     <- mkFIFO;
    FIFO#(Bool)       doneQ      <- mkFIFO;
    FIFO#(MemPortReq) writeReqQ  <- mkFIFO;
    FIFO#(Bit#(512))  writeWordQ <- mkFIFO;

    Reg#(Bool)     started   <- mkReg(False);
    Reg#(Bit#(32)) total     <- mkReg(0);
    Reg#(Bit#(32)) outCount  <- mkReg(0);
    Reg#(Bit#(8))  drainWait <- mkReg(0);

    rule systemStart(!started);
        startQ.deq;
        let n = startQ.first;
        if (n == 0) begin
            doneQ.enq(True);
        end else begin
            total     <= n;
            outCount  <= 0;
            drainWait <= 0;
            writeReqQ.enq(MemPortReq{ addr: 64'd0, bytes: n << 6 });
            started <= True;
        end
    endrule

    rule drainStream(started && outCount < total);
        let beat <- axisIn.get;
        writeWordQ.enq(tpl_1(beat));
        outCount <= outCount + 1;
    endrule

    rule startDrainWait(started && outCount == total && total != 0 && drainWait == 0);
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

    method Action start(Bit#(32) wordCount) if (!started);
        startQ.enq(wordCount);
    endmethod
    method ActionValue#(Bool) done;
        let d = doneQ.first; doneQ.deq; return d;
    endmethod
endmodule
