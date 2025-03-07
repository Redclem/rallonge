#include "server.h"
#include "app_base.h"
#include "ral_proto.h"
#include "socket.hpp"

#include <algorithm>
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
	m_pfds = {{0, POLLIN, 0}, {0, POLLIN, 0}};

	connect_proto_tcp(true);

	Proto::UDPBypass ub;
	CHECK_RET(m_tcp_proto_conn.Recv(ub));
	m_bypass_udp = ub == Proto::UDPBypass::BYPASS;

	if(!m_bypass_udp)
	{
		create_udp_socket();
	}

	init_post_connection();

	m_last_tcp_packet = m_cur_time = time(nullptr);
}

bool Server::connect_proto_tcp(bool fresh)
{
	Socket tcp_plug;
	Address tcp_adr_rec(AF_INET, SOCK_STREAM, "0.0.0.0", m_tcp_port);

	CHECK_RET(tcp_plug.create(AF_INET, SOCK_STREAM))
	CHECK_RET(tcp_plug.bind(tcp_adr_rec))

	std::cout << "Listening on port " << m_tcp_port << ". Waiting for client." << std::endl;

	CHECK_RET(tcp_plug.listen(1))
	std::tie(m_tcp_proto_conn, m_proto_udp_address) = tcp_plug.accept_addr();
	CHECK_RET(m_tcp_proto_conn.valid())

	Proto::OpCode opcode = Proto::OpCode::ESTABLISH;
	m_tcp_proto_conn.Send(opcode);
	do
	{
		m_tcp_proto_conn.Recv(opcode);
	} while (opcode != Proto::OpCode::ESTABLISH);

	std::cout << "Client connected : " << m_proto_udp_address.str() << std::endl;

	m_pfds.front().fd = m_tcp_proto_conn.socket();


	Proto::Connection cn(fresh ? Proto::Connection::FRESH : Proto::Connection::RESUME);
	m_tcp_proto_conn.Send(cn);
	m_tcp_proto_conn.Recv(cn);
	return cn == Proto::Connection::FRESH;
}

void Server::init_post_connection()
{
	std::cout << "Initializing connection" << std::endl;

	if(!m_bypass_udp)
	{
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
	}
	else
	{
		std::cout << "UDP bypass enabled." << std::endl;
		set_bypass();
	}
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
		if(check_tcp_timeout())
			on_timeout();

		auto rpoll = poll_pfds();
		
		if(rpoll == 0) continue;

		CHECK_RET(rpoll);


		// Check client TCP / UDP
		
		while(m_pfds.front().revents & pollmask)
		{
			if(m_pfds.front().revents & (POLLIN | POLLRDNORM))
			{
				process_tcp_message();
			}
			else if(m_pfds.front().revents & (POLLERR | POLLHUP))
			{
				std::cout << "Lost connection. Reconnecting." << std::endl;
				send_timeout_message();
				on_timeout();
			}


			if(!m_tcp_proto_conn.valid())
			{
				std::cout << "Lost connection. Reconnecting." << std::endl;
				send_timeout_message();
				on_timeout();
			}

			// /!\ with TCP message, the pfd vector might have been reallocated

			CHECK_RET(poll(&m_pfds.front(), 1, 0) >= 0)
		}

		// pfd vector won't invalidate ahead

		auto iter_pfd = std::next(m_pfds.begin());

		
		while (iter_pfd->revents & pollmask)
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
			while(iter_pfd->revents & pollmask)
			{
				m_message_buffer.resize(m_message_buffer.capacity());
				auto recres = sck.sck.Recv_raw(m_message_buffer.data() + Proto::udp_message_header_size, m_message_buffer.size() - Proto::udp_message_header_size);
#ifdef WIN32
				if(recres < 0)
				{
					auto err = WSAGetLastError();
					if(err == WSAECONNRESET)
					{
						LOG("UDP port unreachable on bridge " << bridge << std::endl);
					}
					else
						{
							std::cout << "err on UDP recv : " << err << std::endl;
							throw std::runtime_error("Error on UDP recv");
						}
				}
#else
				CHECK_RET(recres >= 0);
#endif
				send_udp(bridge, recres);

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

	m_last_tcp_packet = m_cur_time;	

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
			if(m_bypass_udp) // Check proto!
			{
				Proto::Protocol p;
				CHECK_RET(m_tcp_proto_conn.Recv(p));

				if(p == Proto::Protocol::UDP) // It is UDP
				{
					process_bypassed_message();
					return;
				}
				// Otherwise, do as usual...
			}

			std::array<unsigned char, 20> hdr;
			CHECK_RET(m_tcp_proto_conn.Recv(hdr))

			ComKey comkey{DECODE_KEY(&hdr[0]), DECODE_KEY(&hdr[8])};

			uint32_t dat_size = DECODE_UINT32(&hdr[16]);

			m_message_buffer.resize(dat_size);
			CHECK_RET(m_tcp_proto_conn.Recv(m_message_buffer, MSG_WAITALL))

			auto conn = m_connections.find(comkey);

			if(conn == m_connections.end())
			{
				LOG("Message on dead connection " << comkey.sk << std::endl);
				return;
			}

			CHECK_RET(conn->second.sck.Send(m_message_buffer))
			return;
		}
	case Proto::OpCode::CONNECT:
		{
			std::array<unsigned char, 18> bridge_dat;
			CHECK_RET(m_tcp_proto_conn.Recv(bridge_dat))
			
			uint16_t bridge = DECODE_UINT16(bridge_dat);
			key_sock_uni_t key = DECODE_KEY(&bridge_dat[2]);
			key_sock_uni_t unkey = DECODE_KEY(&bridge_dat[10]);

			Connection newcon{{}, key, m_pfds.size()};

			CHECK_RET(newcon.sck.create(AF_INET, SOCK_STREAM))

			if(newcon.sck.connect(m_tcp_addresses[bridge]))
			{
				// Send established
				
				std::array<unsigned char, 25> msg_estab = {(unsigned char)(Proto::OpCode::TCP_ESTABLISHED)};
				ENCODE_KEY(key, &msg_estab[1])
				ENCODE_KEY(unkey, &msg_estab[9])
				ENCODE_KEY(newcon.sck.socket(), &msg_estab[17])

				CHECK_RET(m_tcp_proto_conn.Send(msg_estab))

				LOG("TCP bridge " << bridge << " connected, key " << key << ", " << newcon.sck.socket() << std::endl);

				m_pfds.push_back({newcon.sck.socket(), POLLIN, 0});

				m_connections.emplace(ComKey{key_sock_uni_t(newcon.sck.socket()), unkey}, std::move(newcon));
			}
			else if(
#ifdef __unix__
				errno == ECONNREFUSED
#elif defined(WIN32)
				WSAGetLastError() == WSAECONNREFUSED
#endif
			) {
				// Connection refused
				LOG("Connection refused on bridge " << bridge << ", key : " << key << std::endl);
				std::array<unsigned char, 17> msg = {(unsigned char)(Proto::OpCode::TCP_DISCONNECTED)};

				ENCODE_KEY(key, &msg[1]);
				ENCODE_KEY(unkey, &msg[9]);
				
				m_tcp_proto_conn.Send(msg);

				return;
			}
			else
				throw std::runtime_error("connect failed");
	
			return;
		}
	case Proto::OpCode::TCP_DISCONNECTED:
		{
			std::array<unsigned char, 16> bridge_dat;
			CHECK_RET(m_tcp_proto_conn.Recv(bridge_dat))

			disconnect_tcp<false>(ComKey{DECODE_KEY(&bridge_dat[0]), DECODE_KEY(&bridge_dat[8])});

			return;
		}
	case Proto::OpCode::TCP_TIMEOUT:
		on_timeout();
		return;
	default:
	case Proto::OpCode::UDP_CONNECTED:
		throw NetworkError("Unexpected OpCode on TCP");
	}
}

void Server::on_timeout()
{
	std::cout << "Timeout!" << std::endl;

	m_connections.clear();
	m_pfds.resize(2 + m_udp_sockets.size());
	// Reset established TCPS
	m_tcp_proto_conn.destroy();
	
	if(connect_proto_tcp(false))
	{
		m_udp_sockets.clear();
		m_connections.clear();
		m_pfds.resize(2);

		Proto::UDPBypass ub;
		CHECK_RET(m_tcp_proto_conn.Recv(ub));
		m_bypass_udp = ub == Proto::UDPBypass::BYPASS;

		if(!m_bypass_udp && !m_udp_proto_conn.valid())
		{
			create_udp_socket();
			m_pfds[1].fd = m_udp_proto_conn.socket();
		}
		else if(m_bypass_udp)
			m_pfds[0].fd = 0;
		
		init_post_connection();
	}

	
	m_pfds.front().fd = m_tcp_proto_conn.socket();
	
	m_last_tcp_packet = m_cur_time = time(nullptr);
}
