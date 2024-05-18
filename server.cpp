#include "server.h"
#include "app_base.h"
#include "ral_proto.h"
#include "socket.hpp"

#include <cerrno>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <sys/socket.h>
#include <tuple>
#include <array>

void Server::run()
{
	initiate();
	proc_loop();
}

void Server::initiate()
{
	Socket tcp_plug;
	Address tcp_adr_rec(AF_INET, SOCK_STREAM, "0.0.0.0", m_tcp_port);

	CHECK_RET(tcp_plug.create(AF_INET, SOCK_STREAM))
	CHECK_RET(tcp_plug.bind(tcp_adr_rec))

	std::cout << "Listening on port " << m_tcp_port << ". Waiting for client." << std::endl;

	CHECK_RET(tcp_plug.listen(1))
	std::tie(m_tcp_proto_conn, m_proto_udp_address) = tcp_plug.accept_addr();
	CHECK_RET(m_tcp_proto_conn.valid())

	std::cout << "Client connected : " << m_proto_udp_address.str() << std::endl;
	std::cout << "Creating UDP Socket ..." << std::endl;
	
	CHECK_RET(m_udp_proto_conn.create(AF_INET, SOCK_DGRAM))
	CHECK_RET(m_udp_proto_conn.bind(Address(AF_INET, SOCK_DGRAM, "0.0.0.0", 0)))

	auto [res, udp_plug_adr] = m_udp_proto_conn.getsockname();
	CHECK_RET(res);

	m_udp_port = udp_plug_adr.port();

	std::cout << "UDP Socket created at port " << m_udp_port << ". Initializing connection..." << std::endl;

	std::array<unsigned char, 2> port;

	port[0] = m_udp_port;
	port[1] = m_udp_port >> 8;

	CHECK_RET(m_tcp_proto_conn.Send(port))
	CHECK_RET(m_tcp_proto_conn.Recv(port))
	CHECK_RET(!port.empty());

	port_t client_udp_port = port[0] | port[1] << 8;
	m_proto_udp_address.set_port(client_udp_port);

	std::cout << "TCP exchange OK." << std::endl;
	std::cout << "Waiting for client UDP connection at port "
		<< client_udp_port << std::endl;

	establish_udp_connection();

	std::cout << "UDP connect OK." << std::endl;

	m_pfds = {{m_tcp_proto_conn.socket(), POLLIN, 0}, {m_udp_proto_conn.socket(), POLLIN, 0}};
}

void Server::add_endpoint(Proto::Protocol proto, port_t dst_port, const char * hostname)
{
	std::cout << "Adding endpoint for protocol " << (proto == Proto::Protocol::TCP ? "TCP" : "UDP") << " at " << hostname << ':' << dst_port << std::endl;

	Address adr{AF_INET, SOCK_STREAM, hostname, dst_port};

	if(proto == Proto::Protocol::TCP)
	{
		m_tcp_addresses.push_back(std::move(adr));
	}
	else if(proto == Proto::Protocol::UDP)
	{
		CombinedAddressSocket sck{{}, std::move(adr)};

		CHECK_RET(sck.sck.create(sck.addr.af(), SOCK_DGRAM))

		m_pfds.push_back({sck.sck.socket(), POLLIN, 0});
		m_udp_sockets.push_back(std::move(sck));
	}
	else
		throw std::runtime_error("Unknown protocol in CONFIG message");
}

void Server::proc_loop()
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


		// Check client TCP / UDP
		
		while(m_pfds.front().revents)
		{
			if(m_pfds.front().revents & (POLLERR | POLLHUP))
			{
				std::cout << "Client disconnected. Quitting." << std::endl;
				return;
			}

			process_tcp_message();

			if(!m_tcp_proto_conn.valid())
			{
				std::cout << "Client disconnected. Quitting." << std::endl;
				return;
			}

			// /!\ with TCP message, the pfd vector might have been reallocated

			CHECK_RET(poll(&m_pfds.front(), 1, 0) >= 0)
		}

		// pfd vector won't invalidate ahead

		auto iter_pfd = std::next(m_pfds.begin());

		while(iter_pfd->revents)
		{
			process_udp_message();
			CHECK_RET(poll(&(*iter_pfd), 1, 0) >= 0)
		}
		iter_pfd++;

		// Check endpoints
		
		
		// UDP
		uint16_t bridge = 0;
		for(auto & sck : m_udp_sockets)
		{
			while(iter_pfd->revents)
			{
				m_message_buffer.resize(m_message_buffer.capacity());
				auto recres = sck.sck.Recv_raw(m_message_buffer.data() + 7, m_message_buffer.size() - 7);
				CHECK_RET(recres >= 0);

				m_message_buffer.resize(recres + 7);

				m_message_buffer[0] = (unsigned char)(Proto::OpCode::MESSAGE);

				ENCODE_UINT16(bridge, m_message_buffer.data() + 1)
				ENCODE_UINT32(recres, m_message_buffer.data() + 3)

				m_udp_proto_conn.Sendto(m_message_buffer, m_proto_udp_address);

				CHECK_RET(poll(&(*iter_pfd), 1, 0) >= 0)
			}

			iter_pfd++;
			bridge++;
		}

		// TCP
		
		for(;iter_pfd != m_pfds.end(); iter_pfd++)
		{
			if(check_conn_pfd(iter_pfd)) break;
		}

	}
}


void Server::process_tcp_message()
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
	case Proto::OpCode::CONFIG:
		{
			std::array<unsigned char, 2> size_dat;
			CHECK_RET(m_tcp_proto_conn.Recv(size_dat))

			uint16_t size = DECODE_UINT16(size_dat.data());
			
			m_message_buffer.resize(size);
			CHECK_RET(m_tcp_proto_conn.Recv(m_message_buffer))

			add_endpoint(Proto::Protocol(m_message_buffer[0]), DECODE_UINT16(m_message_buffer.data() + 1), reinterpret_cast<char*>(m_message_buffer.data() + 3));
		}
		return;
	case Proto::OpCode::MESSAGE:
		{
			std::array<unsigned char, 12> hdr;
			CHECK_RET(m_tcp_proto_conn.Recv(hdr))

			key_sock_t idx_cn = DECODE_KEY(hdr.data());
			uint32_t dat_size = DECODE_UINT32(&hdr[8]);

			auto conn = m_connections.find(idx_cn);

			m_message_buffer.resize(dat_size);
			CHECK_RET(m_tcp_proto_conn.Recv(m_message_buffer, MSG_WAITALL))

			if(conn == m_connections.end())
			{
				std::cout << "Message on dead connection " << idx_cn << std::endl;
				return;
			}

			CHECK_RET(conn->second.sck.Send(m_message_buffer))
			return;
		}
	case Proto::OpCode::CONNECT:
		{
			std::array<unsigned char, 10> bridge_dat;
			CHECK_RET(m_tcp_proto_conn.Recv(bridge_dat))
			
			uint16_t bridge = DECODE_UINT16(bridge_dat);
			key_sock_t key = DECODE_KEY(&bridge_dat[2]);

			Connection newcon{{}, key, m_pfds.size()};

			CHECK_RET(newcon.sck.create(AF_INET, SOCK_STREAM))

			if(newcon.sck.connect(m_tcp_addresses[bridge]))
			{
				// Send established
				
				std::array<unsigned char, 17> msg_estab = {(unsigned char)(Proto::OpCode::TCP_ESTABLISHED)};
				ENCODE_KEY(key, &msg_estab[1])
				ENCODE_KEY(newcon.sck.socket(), &msg_estab[9])

				CHECK_RET(m_tcp_proto_conn.Send(msg_estab))

				std::cout << "TCP bridge " << bridge << " connected, key " << key << ", " << newcon.sck.socket() << std::endl;

				m_pfds.push_back({newcon.sck.socket(), POLLIN, 0});

				m_connections.emplace(newcon.sck.socket(), std::move(newcon));
			}
			else if(
#ifdef __unix__
				errno == ECONNREFUSED
#elif defined(WIN32)
				WSAGetLastError() == WSAECONNREFUSED
#endif
			) {
				// Connection refused
				std::cout << "Connection refused on bridge " << bridge << ", key : " << key << std::endl;
				std::array<unsigned char, 9> msg = {(unsigned char)(Proto::OpCode::TCP_DISCONNECTED)};

				ENCODE_KEY(key, &msg[1]);
				
				m_tcp_proto_conn.Send(msg);

				return;
			}
			else
				throw std::runtime_error("connect failed");
	
			return;
		}
	case Proto::OpCode::TCP_DISCONNECTED:
		{
			std::array<unsigned char, 8> bridge_dat;
			CHECK_RET(m_tcp_proto_conn.Recv(bridge_dat))

			disconnect_tcp<false>(DECODE_KEY(bridge_dat.data()));

			return;
		}
	default:
	case Proto::OpCode::UDP_CONNECTED:
		throw NetworkError("Unexpected OpCode on TCP");
	}
}


