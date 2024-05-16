#ifndef SERVER_H
#define SERVER_H

#include "ral_proto.h"
#include "socket.hpp"
#include "app_base.h"

#include "classes.h"

#include <iostream>


class Server : public AppBase
{
	uint16_t m_tcp_port;

	std::vector<CombinedAddressSocket> m_tcp_sockets;

public:

	Server(port_t tp) : m_tcp_port(tp) {}

	void run();

	void initiate();
	void proc_loop();

	void process_tcp_message();

	void add_endpoint(Proto::Protocol proto, port_t dst_port, const char * hostname);

	void disconnect_tcp(uint16_t bridge)
	{
		if(m_pfds[bridge + 2].fd != m_tcp_sockets[bridge].sck.socket())
		{
			std::cout << "Double disconnect of bridge " << bridge << std::endl;
			return;
		}

		m_pfds[bridge + 2].fd = ~m_pfds[bridge + 2].fd;

		m_tcp_sockets[bridge].sck.destroy();
		m_tcp_sockets[bridge].sck.create(m_tcp_sockets[bridge].addr.af(), SOCK_STREAM);

		std::cout << "TCP bridge " << bridge << " disconnected." << std::endl;
	}

};

#endif
