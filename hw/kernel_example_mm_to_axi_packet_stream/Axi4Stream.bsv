package Axi4Stream;

import FIFOF::*;

interface Axi4StreamMasterPinsIfc#(numeric type dataSz);
	(* always_ready, result="tvalid" *)
	method Bool tvalid;
	(* always_ready, always_enabled, prefix = "" *)
	method Action data_ready ((* port="tready" *) Bool tready);
	(* always_ready, result="tdata" *)
	method Bit#(dataSz) tdata;
	(* always_ready, result="tkeep" *)
	method Bit#(TDiv#(dataSz,8)) tkeep;
	(* always_ready, result="tlast" *)
	method Bool tlast;
endinterface

interface Axi4StreamMasterIfc#(numeric type dataSz);
	interface Axi4StreamMasterPinsIfc#(dataSz) pins;
	method Action put(Bit#(dataSz) data, Bit#(TDiv#(dataSz,8)) keep, Bool last);
endinterface

(* synthesize *)
module mkAxi4StreamMaster_512 (Axi4StreamMasterIfc#(512));
	let m_ <- mkAxi4StreamMaster;
	return m_;
endmodule

module mkAxi4StreamMaster (Axi4StreamMasterIfc#(dataSz));
	FIFOF#(Tuple3#(Bit#(dataSz), Bit#(TDiv#(dataSz,8)), Bool)) outQ <- mkFIFOF;

	RWire#(Tuple3#(Bit#(dataSz), Bit#(TDiv#(dataSz,8)), Bool)) beatW <- mkRWire;
	PulseWire readyW <- mkPulseWire;

	rule presentBeat ( outQ.notEmpty );
		beatW.wset(outQ.first);
	endrule

	rule consumeBeat ( readyW && outQ.notEmpty );
		outQ.deq;
	endrule

	interface Axi4StreamMasterPinsIfc pins;
		method Bool tvalid;
			return isValid(beatW.wget);
		endmethod
		method Action data_ready (Bool tready);
			if ( tready ) readyW.send;
		endmethod
		method Bit#(dataSz) tdata;
			return tpl_1(fromMaybe(?, beatW.wget));
		endmethod
		method Bit#(TDiv#(dataSz,8)) tkeep;
			return tpl_2(fromMaybe(?, beatW.wget));
		endmethod
		method Bool tlast;
			return tpl_3(fromMaybe(?, beatW.wget));
		endmethod
	endinterface

	method Action put(Bit#(dataSz) data, Bit#(TDiv#(dataSz,8)) keep, Bool last);
		outQ.enq(tuple3(data, keep, last));
	endmethod
endmodule

// Beat carried out of the slave: (tdata, tkeep, tlast, tuser).
typedef Tuple4#(Bit#(dataSz), Bit#(TDiv#(dataSz,8)), Bool, Bit#(userSz)) Axis4Beat#(numeric type dataSz, numeric type userSz);

interface Axi4StreamSlavePinsIfc#(numeric type dataSz, numeric type userSz);
	(* always_ready, always_enabled, prefix = "" *)
	method Action data_valid ((* port="tvalid" *) Bool tvalid);
	(* always_ready, result="tready" *)
	method Bool tready;
	(* always_ready, always_enabled, prefix = "" *)
	method Action data ((* port="tdata" *) Bit#(dataSz) tdata);
	(* always_ready, always_enabled, prefix = "" *)
	method Action keep ((* port="tkeep" *) Bit#(TDiv#(dataSz,8)) tkeep);
	(* always_ready, always_enabled, prefix = "" *)
	method Action user ((* port="tuser" *) Bit#(userSz) tuser);
	(* always_ready, always_enabled, prefix = "" *)
	method Action data_last ((* port="tlast" *) Bool tlast);
endinterface

interface Axi4StreamSlaveIfc#(numeric type dataSz, numeric type userSz);
	interface Axi4StreamSlavePinsIfc#(dataSz, userSz) pins;
	method ActionValue#(Axis4Beat#(dataSz, userSz)) get;
endinterface

(* synthesize *)
module mkAxi4StreamSlave_512_32 (Axi4StreamSlaveIfc#(512, 32));
	let m_ <- mkAxi4StreamSlave;
	return m_;
endmodule

module mkAxi4StreamSlave (Axi4StreamSlaveIfc#(dataSz, userSz));
	FIFOF#(Axis4Beat#(dataSz, userSz)) inQ <- mkFIFOF;

	PulseWire                     validW <- mkPulseWire;
	RWire#(Bit#(dataSz))          dataW  <- mkRWire;
	RWire#(Bit#(TDiv#(dataSz,8))) keepW  <- mkRWire;
	RWire#(Bit#(userSz))          userW  <- mkRWire;
	RWire#(Bool)                  lastW  <- mkRWire;

	rule captureBeat ( inQ.notFull && validW );
		inQ.enq(tuple4(fromMaybe(?, dataW.wget),
		               fromMaybe(?, keepW.wget),
		               fromMaybe(False, lastW.wget),
		               fromMaybe(0, userW.wget)));
	endrule

	interface Axi4StreamSlavePinsIfc pins;
		method Action data_valid (Bool tvalid);
			if ( tvalid ) validW.send;
		endmethod
		method Bool tready;
			return inQ.notFull;
		endmethod
		method Action data (Bit#(dataSz) tdata);
			dataW.wset(tdata);
		endmethod
		method Action keep (Bit#(TDiv#(dataSz,8)) tkeep);
			keepW.wset(tkeep);
		endmethod
		method Action user (Bit#(userSz) tuser);
			userW.wset(tuser);
		endmethod
		method Action data_last (Bool tlast);
			lastW.wset(tlast);
		endmethod
	endinterface

	method ActionValue#(Axis4Beat#(dataSz, userSz)) get;
		inQ.deq;
		return inQ.first;
	endmethod
endmodule

endpackage
