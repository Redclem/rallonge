#include "server.h"
#include "ral_proto.h"
#include "socket.hpp"

#include <cerrno>
#include <iostream>
#include <iterator>
#include <stdexcept>
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

	CHECK_RET(m_tcp_proto_conn.send(port))
	CHECK_RET(m_tcp_proto_conn.recv(port))
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

	CombinedAddressSocket sck = {
		{},
		{AF_INET, SOCK_STREAM, hostname, dst_port}
	};

	if(proto == Proto::Protocol::TCP)
	{
		CHECK_RET(sck.sck.create(sck.addr.af(), SOCK_STREAM))

		// Add pfd
		m_pfds.insert(m_pfds.end() - m_udp_sockets.size(), {~sck.sck.socket(), POLLIN, 0});

		m_tcp_sockets.push_back(std::move(sck));
	}
	else if(proto == Proto::Protocol::UDP)
	{
		CHECK_RET(sck.sck.create(sck.addr.af(), SOCK_DGRAM))

		m_pfds.push_back({sck.sck.socket(), POLLIN, 0});
		m_udp_sockets.push_back(std::move(sck));
	}
	else
		throw std::runtime_error("Unknown protocol in CONFIG message");
}

void Server::proc_loop()
{
	while(true)
	{
		// Poll
		int rpoll;

		do
		{
			// UDP Keepalive
			std::array<Proto::OpCode, 1> ka{Proto::OpCode::NOP};
			m_udp_proto_conn.sendto(ka, m_proto_udp_address);

			rpoll = poll(m_pfds.data(), m_pfds.size(), 1000);
		} while(rpoll == 0);

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
		for(auto & sck : m_tcp_sockets)
		{
			while(
				iter_pfd->revents
#ifdef WIN32
      				& (~POLLNVAL)
#endif
			)
			{
				decltype(sck.sck.recv_raw(m_message_buffer.data(), m_message_buffer.size())) recres;

				if(iter_pfd->revents & (POLLHUP | POLLERR))
				{
					recres = 0;
				}
				else
				{
					m_message_buffer.resize(m_message_buffer.capacity());
					
					recres = sck.sck.recv_raw(m_message_buffer.data() + 7, m_message_buffer.size() - 7);
					CHECK_RET(recres >= 0);
				}
				
				if(recres == 0) // Connection loss
				{
					std::cout << "Bridge " << bridge << " Hung up." << std::endl;

					disconnect_tcp(bridge);

					std::array<unsigned char, 3> cod = {(unsigned char)(Proto::OpCode::TCP_DISCONNECTED)};
					ENCODE_UINT16(bridge, &cod[1])
					
					CHECK_RET(m_tcp_proto_conn.send(cod))

					// Do not poll, as struct contains a disabled socket.
					iter_pfd->revents = 0;
				}
				else // Message
				{
					m_message_buffer.resize(recres + 7);
					m_message_buffer[0] = (unsigned char)(Proto::OpCode::MESSAGE);

					ENCODE_UINT16(bridge, m_message_buffer.data() + 1)
					ENCODE_UINT32(recres, m_message_buffer.data() + 3)

					CHECK_RET(m_tcp_proto_conn.send(m_message_buffer))

					CHECK_RET(poll(&(*iter_pfd), 1, 0) >= 0)
				}
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
				auto recres = sck.sck.recv_raw(m_message_buffer.data() + 7, m_message_buffer.size() - 7);
				CHECK_RET(recres >= 0);

				m_message_buffer.resize(recres + 7);

				m_message_buffer[0] = (unsigned char)(Proto::OpCode::MESSAGE);

				ENCODE_UINT16(bridge, m_message_buffer.data() + 1)
				ENCODE_UINT32(recres, m_message_buffer.data() + 3)

				m_udp_proto_conn.sendto(m_message_buffer, m_proto_udp_address);

				CHECK_RET(poll(&(*iter_pfd), 1, 0) >= 0)
			}

			iter_pfd++;
			bridge++;
		}
	}
}


void Server::process_tcp_message()
{

	StatVec<1> opcode;
	m_tcp_proto_conn.recv(opcode);

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
			CHECK_RET(m_tcp_proto_conn.recv(size_dat))

			uint16_t size = DECODE_UINT16(size_dat.data());
			
			m_message_buffer.resize(size);
			CHECK_RET(m_tcp_proto_conn.recv(m_message_buffer))

			add_endpoint(Proto::Protocol(m_message_buffer[0]), DECODE_UINT16(m_message_buffer.data() + 1), reinterpret_cast<char*>(m_message_buffer.data() + 3));
		}
		return;
	case Proto::OpCode::MESSAGE:
		{
			std::array<unsigned char, 6> hdr;
			CHECK_RET(m_tcp_proto_conn.recv(hdr))

			uint16_t bridge = DECODE_UINT16(hdr);
			uint32_t dat_size = DECODE_UINT32(hdr.data() + 2);

			m_message_buffer.resize(dat_size);
			CHECK_RET(m_tcp_proto_conn.recv(m_message_buffer, MSG_WAITALL))

			CHECK_RET(m_tcp_sockets[bridge].sck.send(m_message_buffer))
			return;
		}
	case Proto::OpCode::CONNECT:
		{
			std::array<unsigned char, 2> bridge_dat;
			CHECK_RET(m_tcp_proto_conn.recv(bridge_dat))
			
			uint16_t bridge = DECODE_UINT16(bridge_dat);

			if(m_pfds[2 + bridge].fd == m_tcp_sockets[bridge].sck.socket())
			{
				std::cout << "Double connect of bridge " << bridge << std::endl;
				return;
			}

			std::cout << "TCP bridge " << bridge << " connected." << std::endl;

			if(m_tcp_sockets[bridge].sck.connect(m_tcp_sockets[bridge].addr))
				m_pfds[2 + bridge].fd = ~m_pfds[2 + bridge].fd;
			else if(
#ifdef __unix__
				errno == ECONNREFUSED
#elif defined(WIN32)
				WSAGetLastError() == WSAECONNREFUSED
#endif
			) {
				// Connection refused
				m_tcp_sockets[bridge].sck.destroy();
				CHECK_RET(m_tcp_sockets[bridge].sck.create(AF_INET, SOCK_STREAM))
				std::cout << "Connection refused on bridge " << bridge << std::endl;
				std::array<unsigned char, 3> msg;

				msg[0] = (unsigned char)(Proto::OpCode::TCP_DISCONNECTED);
				ENCODE_UINT16(bridge, &msg[1]);
				
				m_tcp_proto_conn.send(msg);
			}
			else
				throw std::runtime_error("connect failed");


			return;
		}
	case Proto::OpCode::TCP_DISCONNECTED:
		{
			std::array<unsigned char, 2> bridge_dat;
			CHECK_RET(m_tcp_proto_conn.recv(bridge_dat))
			
			disconnect_tcp(DECODE_UINT16(bridge_dat));

			return;
		}
	default:
	case Proto::OpCode::UDP_CONNECTED:
		throw NetworkError("Unexpected OpCode on TCP");
	}
}


