#ifndef CLIENT_H
#define CLIENT_H

#include "app_base.h"
#include "socket.hpp"
#include <iostream>

struct TCPCLientSocket
{
	Socket listener, com;
	bool connected = false;
};

class Client : public AppBase
{
	const char * m_hostname, * m_config_path;
	port_t m_tcp_port;

	std::vector<TCPCLientSocket> m_tcp_sockets;

public:
	Client(const char * hostname, port_t port, const char * cfg_file) : 
		m_hostname(hostname), m_config_path(cfg_file), m_tcp_port(port)
	{}

	void run();

	void initiate();
	void proc_loop();

	void process_tcp_message();

	void load_config();

	void disconnect_tcp(uint16_t bridge)
	{
		if(m_tcp_sockets[bridge].connected == false)
		{
			std::cout << "Double disconnect of bridge " << bridge << std::endl;
			return;
		}

		m_tcp_sockets[bridge].com.destroy();
		m_tcp_sockets[bridge].connected = false;

		m_pfds[bridge + 2].fd = m_tcp_sockets[bridge].listener.socket();

		std::cout << "TCP bridge " << bridge << " disconnected." << std::endl;
	}
};


#endif

