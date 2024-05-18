#ifndef CLIENT_H
#define CLIENT_H

#include "app_base.h"
#include "socket.hpp"
#include <iostream>


class Client : public AppBase
{
	const char * m_hostname, * m_config_path;
	port_t m_tcp_port;

	std::vector<Socket> m_tcp_listener_sockets;


public:
	Client(const char * hostname, port_t port, const char * cfg_file) : 
		m_hostname(hostname), m_config_path(cfg_file), m_tcp_port(port)
	{}

	void run();

	void initiate();
	void proc_loop();

	void process_tcp_message();

	void load_config();
};


#endif

