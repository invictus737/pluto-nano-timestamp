// SPDX-License-Identifier: GPL-2.0-or-later
`timescale 1ns/1ps
// Functional FIFO/CDC stubs: exercise the REAL timestamp module's handshake.
// Equal clocks deliberately isolate the DMA tag bug; this is NOT a CDC/XPM
// timing, synthesis, placement or deployed-bitstream qualification.
module xpm_fifo_async #(
  parameter FIFO_MEMORY_TYPE="block", FIFO_READ_LATENCY=0,
  FIFO_WRITE_DEPTH=16, READ_DATA_WIDTH=130, READ_MODE="fwft",
  SIM_ASSERT_CHK=1, USE_ADV_FEATURES="0000", WRITE_DATA_WIDTH=130
)(input wr_clk, rst, wr_en, input [WRITE_DATA_WIDTH-1:0] din,
  output full, wr_rst_busy, input rd_clk, rd_en,
  output empty, rd_rst_busy, output [READ_DATA_WIDTH-1:0] dout, input sleep);
  reg [WRITE_DATA_WIDTH-1:0] mem[0:FIFO_WRITE_DEPTH-1];
  integer head=0, tail=0, used=0;
  assign full=used==FIFO_WRITE_DEPTH;
  assign empty=used==0;
  assign wr_rst_busy=0;
  assign rd_rst_busy=0;
  assign dout=empty ? 0 : mem[head];
  always @(posedge wr_clk) begin
    if (rst) begin head<=0; tail<=0; used<=0; end
    else begin
      if (wr_en) begin
        if(full) $fatal(1,"FIFO overflow");
        mem[tail]<=din; tail<=(tail+1)%FIFO_WRITE_DEPTH;
      end
      if (rd_en) begin
        if(empty) $fatal(1,"FIFO underflow");
        head<=(head+1)%FIFO_WRITE_DEPTH;
      end
      case ({wr_en,rd_en})
        2'b10: used<=used+1;
        2'b01: used<=used-1;
      endcase
    end
  end
endmodule

module binary_to_grey #(parameter WIDTH=64)(input [WIDTH-1:0] in_binary, output [WIDTH-1:0] out_grey);
  assign out_grey=in_binary^(in_binary>>1);
endmodule
module grey_to_binary #(parameter WIDTH=64)(input [WIDTH-1:0] in_grey, output [WIDTH-1:0] out_binary);
  genvar j;
  generate for(j=0;j<WIDTH;j=j+1) begin
    assign out_binary[j]=^(in_grey >> j);
  end endgenerate
endmodule
module cdc_sync_bits #(parameter NUM_BITS=64)(input clk_out, reset,
  input [NUM_BITS-1:0] bits_in, output reg [NUM_BITS-1:0] bits_out=0);
  reg [NUM_BITS-1:0] stage=0;
  always @(posedge clk_out) begin stage<=bits_in; bits_out<=stage; end
endmodule

module pluto_timestamp_bubble_test;
  reg clk=0;
  always #5 clk=~clk;
  reg reset=0, valid=0, xfer=0, permit=1;
  reg [63:0] timestamp=0, data=0;
  reg [31:0] every=2;
  wire ready, out_valid, reset_upack;
  wire [63:0] out_data;
  wire [31:0] discarded;
  // This models util_upack2_impl.v:86-89: downstream ready is gated by valid.
  wire out_ready=permit && out_valid;
  util_upack2_timestamp dut(.dma_clk(clk),.dac_clk(clk),.reset(reset),
    .reset_upack(reset_upack),.timestamp(timestamp),.timestamp_every(every),
    .discarded_block_count(discarded),.s_axis_valid(valid),.s_axis_ready(ready),
    .s_axis_xfer_req(xfer),.s_axis_data(data),.m_axis_valid(out_valid),
    .m_axis_ready(out_ready),.m_axis_data(out_data));
  integer received=0;
  always @(negedge clk) if($test$plusargs("TRACE")) $display("t=%0t valid=%b ready=%b counter=%0d tag=%b wr=%b used=%0d out=%b now=%0d dma=%0d reset=%b",$time,valid,ready,dut.timestamp_counter,dut.timestamp_valid,dut.fifo_wr_en,dut.fifo.used,out_valid,timestamp,dut.timestamp_dma,reset_upack);
  reg [63:0] seen[0:127];
  always @(posedge clk) if(out_valid && out_ready) begin
    if(timestamp<out_data[63:32])
      $fatal(1,"EARLY payload: time=%0d wanted=%0d word=%h",timestamp,out_data[63:32],out_data);
    seen[received]=out_data;
    received=received+1;
  end
  task send(input [63:0] word);
    begin
      valid=1; data=word;
      @(posedge clk);
      while(ready!==1'b1) @(posedge clk);
      @(negedge clk); valid=0;
    end
  endtask
  task idle(input integer cycles);
    repeat(cycles) @(negedge clk);
  endtask
  task payload(input integer due, input integer index);
    send((64'(due)<<32)|64'(index));
  endtask
  task check(input integer count, input integer due, input integer first);
    integer j;
    begin
      idle(40);
      if(received!=count) $fatal(1,"payload count: actual=%0d expected=%0d",received,count);
      for(j=0;j<count;j=j+1)
        if(seen[j]!==((64'(due)<<32)|64'(first+j))) $fatal(1,"payload order at %0d",j);
    end
  endtask
  string scenario;
  integer i;
  initial begin
    if(!$value$plusargs("CASE=%s",scenario)) $fatal(1,"CASE required");
    idle(4); timestamp=1; idle(4); timestamp=0; idle(10); xfer=1;
    if(scenario=="bubble") begin
      send(20); idle(2); payload(20,1); payload(20,2);
      idle(12);
      if(received!=0) $fatal(1,"future payload escaped");
      timestamp=20; check(2,20,1);
    end else if(scenario=="consecutive") begin
      send(20); payload(20,1); payload(20,2);
      send(30); payload(30,3); payload(30,4);
      idle(12); if(received!=0) $fatal(1,"future first frame escaped");
      timestamp=20; idle(30);
      if(received!=2 || seen[0]!==((64'd20<<32)|1) || seen[1]!==((64'd20<<32)|2)) $fatal(1,"first frame mismatch");
      timestamp=30; idle(30);
      if(received!=4 || seen[2]!==((64'd30<<32)|3) || seen[3]!==((64'd30<<32)|4)) $fatal(1,"second frame mismatch");
    end else if(scenario=="backpressure") begin
      every=20; send(100);
      for(i=0;i<16;i=i+1) payload(100,i);
      fork
        begin for(i=16;i<20;i=i+1) payload(100,i); end
        begin idle(8); if(received!=0) $fatal(1,"backpressure leaked payload"); timestamp=100; end
      join
      check(20,100,0);
    end else if(scenario=="discard") begin
      timestamp=10; idle(10);
      send(5); payload(5,91); payload(5,92);
      send(20); idle(2); payload(20,1); payload(20,2);
      idle(12);
      if(discarded!=1 || received!=0) $fatal(1,"discarded frame leaked/count mismatch");
      timestamp=20; check(2,20,1);
    end else if(scenario=="header_full_after_discard") begin
      every=16;
      send(20);
      for(i=0;i<16;i=i+1) payload(20,i);
      if(!dut.fifo.full) $fatal(1,"fixture must fill FIFO with future payload");
      // An invalid header/payload can drain while the FIFO remains full.
      send(300);
      for(i=0;i<16;i=i+1) payload(300,40+i);
      if(discarded!=1 || !dut.fifo.full) $fatal(1,"discard must not alter full FIFO");
      // The previous discard flag permits this future header to be accepted
      // without FIFO space. Its VALID-high first payload then stalls normally.
      send(30);
      if(!dut.fifo.full || !dut.timestamp_valid) $fatal(1,"future header was not retained initially");
      fork
        begin for(i=0;i<16;i=i+1) payload(30,100+i); end
        begin
          #1;
          if(ready!==1'b0 || valid!==1'b1) $fatal(1,"first payload must be VALID-high and backpressured");
          idle(8);
          if(received!=0) $fatal(1,"future FIFO leaked before timestamp");
          timestamp=20;
        end
      join
      idle(20);
      if(received!=16 || discarded!=1) $fatal(1,"second future frame bypassed timestamp/count mismatch");
      timestamp=30; idle(40);
      if(received!=32) $fatal(1,"stalled-frame output count mismatch");
      for(i=0;i<16;i=i+1) begin
        if(seen[i]!==((64'd20<<32)|64'(i))) $fatal(1,"first FIFO frame order");
        if(seen[16+i]!==((64'd30<<32)|64'(100+i))) $fatal(1,"stalled FIFO frame order");
      end
    end else if(scenario=="disable") begin
      send(20); every=0; payload(0,101); payload(0,102);
      check(2,0,101);
    end else if(scenario=="abort") begin
      send(20); xfer=0; idle(3); xfer=1;
      send(30); payload(30,1); payload(30,2);
      idle(12); if(received!=0) $fatal(1,"aborted header leaked");
      timestamp=30; check(2,30,1);
    end else $fatal(1,"unknown CASE");
    $display("PASS %s",scenario); $finish;
  end
  initial begin #100000; $fatal(1,"simulation watchdog"); end
endmodule
