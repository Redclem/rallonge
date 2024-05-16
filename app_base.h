#ifndef APP_BASE_H
#define APP_BASE_H

#include "classes.h"
#include "socket.hpp"
#include "ral_proto.h"

#include <vector>
#include <array>

struct CombinedAddressSocket
{
	Socket sck;
	Address addr;
};

class AppBase : public NoCopy
{
protected:
	Socket m_tcp_proto_conn, m_udp_proto_conn;
	Address m_proto_udp_address;
	std::vector<pollfd> m_pfds;
	std::vector<unsigned char> m_message_buffer;

	std::vector<CombinedAddressSocket> m_udp_sockets;

	uint16_t m_udp_port;
	bool m_udp_established = false;
	bool m_udp_est_resend = true;
public:

	static constexpr size_t message_buffer_size = 16384;

	AppBase() : m_message_buffer(message_buffer_size) {}

	constexpr static int n_initial_messages = 16;

	void discard_udp_message();
	void discard_tcp_message();

	void establish_udp_connection();
	void process_udp_message();
};


template<size_t Size>
struct StatVec : public std::array<unsigned char, Size>
{
public:
	size_t dyn_size;

	void resize(size_t s) {dyn_size = s;}
};

static_assert(resizable<std::vector<unsigned char>>::value, "This type should be resizable");
static_assert(resizable<StatVec<2>>::value, "This type should be resizable");


#endif
