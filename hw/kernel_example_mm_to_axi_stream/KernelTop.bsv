import Axi4LiteControllerXrt::*;
import Axi4MemoryMaster::*;
import Axi4Stream::*;

import FIFO::*;
import Clocks :: *;

import KernelMain::*;

interface KernelTopIfc;
    (* always_ready *)
    interface Axi4StreamSlavePinsIfc#(512) s_axis;
    (* always_ready *)
    interface Axi4MemoryMasterPinsIfc#(64,512) out;
    (* always_ready *)
    interface Axi4LiteControllerXrtPinsIfc#(12,32) s_axi_control;
    (* always_ready *)
    method Bool interrupt;
endinterface

(* synthesize *)
(* default_reset="ap_rst_n", default_clock_osc="ap_clk" *)
module kernel (KernelTopIfc);
    Clock defaultClock <- exposeCurrentClock;
    Reset defaultReset <- exposeCurrentReset;

    Axi4LiteControllerXrtIfc#(12,32) axi4control <- mkAxi4LiteControllerXrt(defaultClock, defaultReset);
    Axi4MemoryMasterIfc#(64,512)     axi4out     <- mkAxi4MemoryMaster_64_512;
    Axi4StreamSlaveIfc#(512)         axisIn      <- mkAxi4StreamSlave_512;

    KernelMainIfc kernelMain <- mkKernelMain(axisIn);

    Reg#(Bool) started <- mkReg(False);
    rule assertControl;
        if ( !started ) axi4control.ap_idle;
    endrule

    FIFO#(Bit#(32)) startQ <- mkFIFO;
    Reg#(Bool) last_ap_start <- mkReg(False);
    rule checkStart;
        if ( !last_ap_start && axi4control.ap_start ) startQ.enq(axi4control.scalar00);
        last_ap_start <= axi4control.ap_start;
    endrule
    rule relayStart (!started);
        startQ.deq;
        kernelMain.start(startQ.first);
        started <= True;
    endrule

    rule checkDone ( started );
        Bool done <- kernelMain.done;
        if ( done ) begin
            axi4control.ap_done();
            axi4control.ap_ready;
            started <= False;
        end
    endrule

    rule relayWriteReq ( started );
        let r <- kernelMain.mem.writeReq;
        axi4out.writeReq(axi4control.mem_addr + r.addr, zeroExtend(r.bytes));
    endrule
    rule relayWriteWord ( started );
        let w <- kernelMain.mem.writeWord;
        axi4out.write(w);
    endrule

    interface s_axis        = axisIn.pins;
    interface out           = axi4out.pins;
    interface s_axi_control = axi4control.pins;
    interface interrupt     = axi4control.interrupt;
endmodule
