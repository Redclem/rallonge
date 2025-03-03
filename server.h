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

	std::vector<Address> m_tcp_addresses;
public:

	Server(port_t tp) : m_tcp_port(tp) {}

	void run();

	void initiate();
	void proc_loop();

	void process_tcp_message();

	void add_endpoint(Proto::Protocol proto, port_t dst_port, const char * hostname);
	
	void on_timeout();

	bool connect_proto_tcp(bool fresh);

	// Initilization after connecting tcp and establishing if connection is fresh
	void init_post_connection();
};

#endif
