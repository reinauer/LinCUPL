module and2(input a, b, output y);
    assign y = a & b;
endmodule

module adder(input [3:0] a, b, input ci, output [3:0] sum, output co);
    assign {co, sum} = a + b + ci;
endmodule

module counter(input clk, rst, en, output reg [3:0] q);
    always @(posedge clk or posedge rst)
        if (rst) q <= 0;
        else if (en) q <= q + 1'b1;
endmodule

module preset(input clk, rst_n, input [1:0] d, output reg [1:0] q);
    always @(negedge clk or negedge rst_n)
        if (!rst_n) q <= 2'b11;
        else q <= d;
endmodule

module latch(input gate, d, output reg q);
    always @* if (gate) q = d;
endmodule

module tristate(input a, b, oe, output y);
    assign y = oe ? (a ^ b) : 1'bz;
endmodule

module fsm(input clk, reset, go, ack, output busy, done);
    reg [1:0] state;
    always @(posedge clk)
        if (reset) state <= 0;
        else case (state)
            0: if (go) state <= 1;
            1: state <= 2;
            2: if (ack) state <= 0;
            default: state <= 0;
        endcase
    assign busy = state == 1;
    assign done = state == 2;
endmodule

module offset(input [2:5] a, output [5:2] q);
    assign q = ~a;
endmodule
