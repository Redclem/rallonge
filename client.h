#ifndef CLIENT_H
#define CLIENT_H

#include "app_base.h"
#include "socket.hpp"


class Client : public AppBase
{
	const char * m_hostname, * m_config_path;
	port_t m_tcp_port;

	std::vector<Socket> m_tcp_listener_sockets;

	key_sock_uni_t m_next_key = 0;

public:	

	key_sock_uni_t next_unique_key()
	{
		return m_next_key++;
	}

	Client(const char * hostname, port_t port, const char * cfg_file, bool ub) : AppBase(ub),
		m_hostname(hostname), m_config_path(cfg_file), m_tcp_port(port)
	{}

	void run();

	void initiate();
	void proc_loop();

	void process_tcp_message();

	void load_config();

	bool connect_proto_tcp(bool fresh = true);
	
	void on_timeout();

	// Initilization after connecting tcp and establishing if connection is fresh
	void init_post_connection();
};


#endif

