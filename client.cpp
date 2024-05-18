#include "client.h"
#include "app_base.h"
#include "ral_proto.h"
#include "socket.hpp"
#include <fstream>
#include <iostream>
#include <array>
#include <stdexcept>
#include <sys/poll.h>

void Client::run()
{
	initiate();
	load_config();
	proc_loop();
}

void Client::initiate()
{
	Address tcp_srv(AF_INET, SOCK_STREAM, m_hostname, m_tcp_port);
	m_proto_udp_address = tcp_srv;

	CHECK_RET(m_tcp_proto_conn.create(AF_INET, SOCK_STREAM))

	std::cout << "Connecting to server at " << m_hostname << ':'
		<< m_tcp_port << " ..." << std::endl;

	CHECK_RET(m_tcp_proto_conn.connect(tcp_srv))

	std::cout << "Connected to server." << std::endl;
	std::cout << "Creating UDP Socket ..." << std::endl;
	
	CHECK_RET(m_udp_proto_conn.create(AF_INET, SOCK_DGRAM))
	CHECK_RET(m_udp_proto_conn.bind(Address(AF_INET, SOCK_DGRAM, "0.0.0.0", 0)))

	auto [res, udp_plug_adr] = m_udp_proto_conn.getsockname();
	CHECK_RET(res);

	m_udp_port = udp_plug_adr.port();

	std::cout << "UDP Socket created at port " << m_udp_port << ". Initializing connection..." << std::endl;

	std::array<unsigned char, 2> port;

	ENCODE_UINT16(m_udp_port, port)

	CHECK_RET(m_tcp_proto_conn.Send(port))
	CHECK_RET(m_tcp_proto_conn.Recv(port))
	CHECK_RET(!port.empty());

	port_t client_udp_port = DECODE_UINT16(port);
	m_proto_udp_address.set_port(client_udp_port);

	std::cout << "TCP exchange OK." << std::endl;
	std::cout << "Waiting for server UDP connection at port "
		<< client_udp_port << std::endl;

	establish_udp_connection();

	std::cout << "UDP connect OK." << std::endl;
	
	m_pfds = {{m_tcp_proto_conn.socket(), POLLIN, 0}, {m_udp_proto_conn.socket(), POLLIN, 0}};
}

void Client::proc_loop()
{
	while(m_run)
	{
		// Poll
		int rpoll;
		
		// UDP Keepalive
		if(ka_message())
		{
			std::array<Proto::OpCode, 1> ka{Proto::OpCode::NOP};
			m_udp_proto_conn.Sendto(ka, m_proto_udp_address);
		}

		rpoll = poll(m_pfds.data(), m_pfds.size(), 5000);
		
		if(rpoll == 0) continue;

		CHECK_RET(rpoll);

		// Check server TCP / UDP
		
		while(m_pfds.front().revents)
		{
			if(m_pfds.front().revents & (POLLERR | POLLHUP))
			{
				std::cout << "Server disconnected. Quitting." << std::endl;
				return;
			}

			process_tcp_message();

			if(!m_tcp_proto_conn.valid())
			{
				std::cout << "Server disconnected. Quitting." << std::endl;
				return;
			}

			// /!\ with TCP message, the pfd vector might have been reallocated

			CHECK_RET(poll(&m_pfds.front(), 1, 0) >= 0)
		}

		// pfd vector won't change ahead

		auto iter_pfd = std::next(m_pfds.begin());

		while(iter_pfd->revents)
		{
			process_udp_message();
			CHECK_RET(poll(&(*iter_pfd), 1, 0) >= 0)
		}
		iter_pfd++;

		// Check endpoints
		
		// TCP
		uint16_t bridge(0);
		for(auto & sck : m_tcp_listener_sockets)
		{
			while(iter_pfd->revents)
			{
				// Add a connection

				Connection nco;
				nco.sck = sck.accept();
				CHECK_RET(nco.sck.valid())

				std::cout << "New connection on bridge " << bridge << ", key " << nco.sck.socket() << std::endl;

				nco.pfd_index = m_pfds.size();

				m_pfds.push_back({nco.sck.socket(), 0, 0});
				// VECTOR INVALIDATION !!!
				
				iter_pfd = m_pfds.begin() + 2 + bridge;

				// Do not poll for input before connection is confirmed


				std::array<unsigned char, 11> msg = {(unsigned char)(Proto::OpCode::CONNECT)};
				ENCODE_UINT16(bridge, &msg[1])
				ENCODE_KEY(nco.sck.socket(), &msg[3])

				m_connections.emplace(nco.sck.socket(), std::move(nco));

				CHECK_RET(m_tcp_proto_conn.Send(msg))

				CHECK_RET(poll(&(*iter_pfd), 1, 0) >= 0)
			}

			iter_pfd++;
			bridge++;
		}

		// UDP
		bridge = 0;
		for(auto & sck : m_udp_sockets)
		{
			while(iter_pfd->revents)
			{
				m_message_buffer.resize(m_message_buffer.capacity());

				decltype(sck.sck.Recv_raw(m_message_buffer.data() + 7, m_message_buffer.size() - 7)) recres;

				if(sck.addr.empty())
				{
					recres = sck.sck.Recvfrom_raw(m_message_buffer.data() + 7, m_message_buffer.size() - 7, sck.addr);

					std::cout << "First message on UDP bridge " << bridge << std::endl;
				}
				else
					recres = sck.sck.Recv_raw(m_message_buffer.data() + 7, m_message_buffer.size() - 7);

				CHECK_RET(recres >= 0);

				m_message_buffer.resize(7 + recres);

				m_message_buffer[0] = (unsigned char)(Proto::OpCode::MESSAGE);

				ENCODE_UINT16(bridge, m_message_buffer.data() + 1)
				ENCODE_UINT32(recres, m_message_buffer.data() + 3)

				m_udp_proto_conn.Sendto(m_message_buffer, m_proto_udp_address);

				CHECK_RET(poll(&(*iter_pfd), 1, 0) >= 0)
			}

			iter_pfd++;
			bridge++;
		}

		// TCP connections

		for(;iter_pfd < m_pfds.end(); ++iter_pfd)
		{
			if(check_conn_pfd(iter_pfd)) return;
		}
	}
}

void Client::process_tcp_message()
{
	StatVec<1> opcode;
	m_tcp_proto_conn.Recv(opcode);

	if(opcode.dyn_size == 0)
	{
		m_tcp_proto_conn.destroy();
		return;
	}

	switch(Proto::OpCode(opcode[0]))
	{
	case Proto::OpCode::NOP:
		return;
	case Proto::OpCode::MESSAGE:
		{
			std::array<unsigned char, 12> hdr;
			CHECK_RET(m_tcp_proto_conn.Recv(hdr))

			key_sock_t conn = DECODE_KEY(hdr.data());
			uint32_t dat_size = DECODE_UINT32(&hdr[8]);

			m_message_buffer.resize(dat_size);
			CHECK_RET(m_tcp_proto_conn.Recv(m_message_buffer, MSG_WAITALL))

			CHECK_RET(m_connections[conn].sck.Send(m_message_buffer))
			return;
		}
	case Proto::OpCode::TCP_DISCONNECTED:
		{
			std::array<unsigned char, 8> bridge_dat;
			CHECK_RET(m_tcp_proto_conn.Recv(bridge_dat))

			disconnect_tcp<false>(DECODE_KEY(bridge_dat.data()));

			return;
		}
	case Proto::OpCode::TCP_ESTABLISHED:
		{
			std::array<unsigned char, 16> keys;

			CHECK_RET(m_tcp_proto_conn.Recv(keys))

			Connection & con = m_connections.at(DECODE_KEY(keys.data()));

			con.key = DECODE_KEY(&keys[8]);

			m_pfds[con.pfd_index].events = POLLIN;

			std::cout << "Connection " << con.key << " established" << std::endl;
		}
		return;
	case Proto::OpCode::CONNECT:
	case Proto::OpCode::UDP_CONNECTED:
	case Proto::OpCode::CONFIG:
	default:
		throw NetworkError("Unexpected OpCode on TCP");
	}
}

void Client::load_config()
{
	std::ifstream cfg_file(m_config_path);
	std::string proto;
	std::string chost, shost;
	port_t cport, sport;
	while(cfg_file >> proto >> chost >> cport >> shost >> sport)
	{

		Proto::Protocol p;

		if(proto == "tcp")
		{
			Socket listener;

			Address bind_addr(AF_INET, SOCK_STREAM, chost.c_str(), cport);

			CHECK_RET(listener.create(bind_addr.af(), SOCK_STREAM))
			CHECK_RET(listener.bind(bind_addr))
			CHECK_RET(listener.listen(16))

			m_pfds.insert(m_pfds.end() - m_udp_sockets.size(),
				{listener.socket(), POLLIN, 0});

			m_tcp_listener_sockets.push_back(std::move(listener));

			p = Proto::Protocol::TCP;
		}
		else if (proto == "udp")
		{
			CombinedAddressSocket cs;

			Address bind_addr(AF_INET, SOCK_DGRAM, chost.c_str(), cport);

			CHECK_RET(cs.sck.create(bind_addr.af(), SOCK_DGRAM))
			CHECK_RET(cs.sck.bind(bind_addr))
			
			m_pfds.push_back({cs.sck.socket(), POLLIN, 0});

			m_udp_sockets.push_back(std::move(cs));

			p = Proto::Protocol::UDP;
		}
		else
			throw std::runtime_error("Unknown protocol in config file");

		std::cout << "Adding bridge " << chost << ':' << cport
			<< " -> " << shost << ':' << sport
			<< " on protocol " << proto << std::endl;

		// Send message to server
		uint16_t len = 4 + shost.size();
		std::vector<unsigned char> data;
		data.reserve(3 + len);
		data.resize(6);

		data[0] = (unsigned char)(Proto::OpCode::CONFIG);
		ENCODE_UINT16(len, data.data() + 1)
		data[3] = (unsigned char)(p);
		ENCODE_UINT16(sport, data.data() + 4)
		data.insert(data.end(), shost.begin(), shost.end());
		data.push_back(0);

		CHECK_RET(m_tcp_proto_conn.Send(data))
	}

	if(!cfg_file.eof())
		throw std::runtime_error("Error reading config file");

	std::cout << "Configuration loaded successfully." << std::endl;
}
