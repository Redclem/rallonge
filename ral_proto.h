#ifndef RAL_PROTO_H
#define RAL_PROTO_H

#include <cstddef>

#define ENCODE_UINT16(n, loc) (loc)[0] = n & 255; (loc)[1] = n >> 8;
#define DECODE_UINT16(loc) (uint16_t((loc)[0]) | uint16_t((loc)[1]) << 8)

#define ENCODE_UINT32(n, loc) (loc)[0] = n & 255; (loc)[1] = (n >> 8) & 255; (loc)[2] = (n >> 16) & 255; (loc)[3] = (n >> 24);
#define DECODE_UINT32(loc) uint32_t((loc)[0]) | uint32_t((loc)[1]) << 8 | uint32_t((loc)[2]) << 16 | uint32_t((loc)[3]) << 24

namespace Proto
{
	enum class OpCode : unsigned char
	{
		NOP = 0,
		CONFIG = 1,
		MESSAGE = 2,
		CONNECT = 3,
		UDP_CONNECTED = 4,
		TCP_DISCONNECTED = 5,
		TCP_ESTABLISHED = 6,
	};
	
	enum class Protocol : unsigned char
	{
		TCP = 0,
		UDP = 1,
	};

	constexpr size_t tcp_message_header_size = 13;
	constexpr size_t udp_message_header_size = 7;
}

#endif
