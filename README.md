# Rallonge TCP/UDP tunnel

Rallonge is a program designed to tunnel UDP / TCP messages through a TCP and UDP channel managed by rallonge.
The tunnel is not secured in any way.
The initial objective of rallonge is to overcome network limitations caused by Network Address Translation that hinder communication with UDP.

## UDP bypassing
The option -ub or --udp-bypass enables the bypassing of udp : udp messages are passed through a tcp connection, so udp streams can be emulated using tcp only

This is useful if your isp blocks udp traffic
