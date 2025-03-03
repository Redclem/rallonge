#include "client.h"
#include "app_base.h"
#include "ral_proto.h"
#include "socket.hpp"
#include <fstream>
#include <iostream>
#include <array>
#include <limits>
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
	m_pfds = {{0, POLLIN, 0}, {0, POLLIN, 0}};
	
	connect_proto_tcp(true);

	if(!m_bypass_udp)
	{
		create_udp_socket();
	}

	init_post_connection();
	
	m_last_tcp_packet = m_cur_time = time(nullptr);
}

bool Client::connect_proto_tcp(bool fresh)
{
	Address tcp_srv(AF_INET, SOCK_STREAM, m_hostname, m_tcp_port);
	m_proto_udp_address = tcp_srv;

	CHECK_RET(m_tcp_proto_conn.create(AF_INET, SOCK_STREAM))

	std::cout << "Connecting to server at " << m_hostname << ':'
		<< m_tcp_port << " ..." << std::endl;

	CHECK_RET(m_tcp_proto_conn.connect(tcp_srv))

	std::cout << "Connected to server." << std::endl;

	m_pfds.front().fd = m_tcp_proto_conn.socket();

	Proto::Connection cn(fresh ? Proto::Connection::FRESH : Proto::Connection::RESUME);
	m_tcp_proto_conn.Send(cn);
	m_tcp_proto_conn.Recv(cn);
	return cn == Proto::Connection::FRESH;
}

void Client::init_post_connection()
{
	std::cout << "Initializing connection" << std::endl;
	auto bp = m_bypass_udp ? Proto::UDPBypass::BYPASS : Proto::UDPBypass::NO_BYPASS;
	CHECK_RET(m_tcp_proto_conn.Send(bp));

	if(!m_bypass_udp)
	{

		std::array<unsigned char, 2> port;

		ENCODE_UINT16(m_udp_port, port)

		CHECK_RET(m_tcp_proto_conn.Send(port))
		CHECK_RET(m_tcp_proto_conn.Recv(port))

		port_t client_udp_port = DECODE_UINT16(port);
		m_proto_udp_address.set_port(client_udp_port);

		std::cout << "TCP exchange OK." << std::endl;
	
		std::cout << "Waiting for server UDP connection at port "
			<< client_udp_port << std::endl;

		establish_udp_connection();

		std::cout << "UDP connect OK." << std::endl;
	}
	else
	{
		std::cout << "UDP bypass enabled." << std::endl;
		m_udp_ka_time = std::numeric_limits<time_t>::max();
	}
}

void Client::proc_loop()
{
	while(m_run)
	{
		if(check_tcp_timeout())
			on_timeout();

		auto rpoll = poll_pfds();
		
		if(rpoll == 0) continue;

		CHECK_RET(rpoll);

		// Check server TCP / UDP
		
		while(m_pfds.front().revents & pollmask)
		{
			if(m_pfds.front().revents & (POLLIN | POLLRDNORM))
			{
				process_tcp_message();
			}
			else if(m_pfds.front().revents & (POLLERR | POLLHUP))
			{
				std::cout << "Server disconnected. Quitting." << std::endl;
				return;
			}

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
		
		
		while (iter_pfd->revents & pollmask)
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
			while(iter_pfd->revents & pollmask)
			{
				// Add a connection

				Connection nco;
				nco.sck = sck.accept();
				nco.key = 0; // Will receive true value when connection established message is received
				CHECK_RET(nco.sck.valid())

				LOG("New connection on bridge " << bridge << ", key " << key_sock_uni_t(nco.sck.socket()) << std::endl);

				nco.pfd_index = m_pfds.size();

				m_pfds.push_back({nco.sck.socket(), 0, 0});
				// VECTOR INVALIDATION !!!
				
				iter_pfd = m_pfds.begin() + 2 + bridge;

				// Do not poll for input before connection is confirmed

				key_sock_uni_t unkey = next_unique_key();

				std::array<unsigned char, 19> msg = {(unsigned char)(Proto::OpCode::CONNECT)};
				ENCODE_UINT16(bridge, &msg[1])
				ENCODE_KEY(nco.sck.socket(), &msg[3])
				ENCODE_KEY(unkey, &msg[11])

				ComKey ck{key_sock_uni_t(nco.sck.socket()), unkey};

				m_connections.emplace(ck, std::move(nco));

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
			while(iter_pfd->revents & pollmask)
			{
				m_message_buffer.resize(m_message_buffer.capacity());

				// Offset by 8 to ensure bypassed header fits
				auto recres = sck.sck.Recvfrom_raw(m_message_buffer.data() + Proto::udp_message_header_size, m_message_buffer.size() - Proto::udp_message_header_size, sck.addr);

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
				LOG("Sending UDP with size " << recres << " to server" << std::endl)

				send_udp(bridge, recres);
				
				CHECK_RET(poll(&(*iter_pfd), 1, 0) >= 0)
			}

			iter_pfd++;
			bridge++;
		}

		// TCP connections

		for(;iter_pfd < m_pfds.end(); ++iter_pfd)
		{
			if(check_conn_pfd(iter_pfd)) break;
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

	m_last_tcp_packet = m_cur_time;

	switch(Proto::OpCode(opcode[0]))
	{
	case Proto::OpCode::NOP:
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

			ComKey ck{DECODE_KEY(&hdr[0]), DECODE_KEY(&hdr[8])};

			uint32_t dat_size = DECODE_UINT32(&hdr[16]);

			m_message_buffer.resize(dat_size);
			CHECK_RET(m_tcp_proto_conn.Recv(m_message_buffer, MSG_WAITALL))

			auto iter_co = m_connections.find(ck);
			
			if(iter_co == m_connections.end())
			{
				LOG("Message on dead connection " << ck.sk << std::endl);
				return;
			}

			CHECK_RET(iter_co->second.sck.Send(m_message_buffer))
			return;
		}
	case Proto::OpCode::TCP_DISCONNECTED:
		{
			std::array<unsigned char, 16> bridge_dat;
			CHECK_RET(m_tcp_proto_conn.Recv(bridge_dat))

			disconnect_tcp<false>({DECODE_KEY(&bridge_dat[0]), DECODE_KEY(&bridge_dat[8])});

			return;
		}
	case Proto::OpCode::TCP_ESTABLISHED:
		{
			std::array<unsigned char, 24> keys;

			CHECK_RET(m_tcp_proto_conn.Recv(keys))

			auto iter_co = m_connections.find(ComKey{DECODE_KEY(&keys[0]), DECODE_KEY(&keys[8])});

			if(iter_co == m_connections.end())
			{
				LOG("Dead connection established" << std::endl);
				return;
			}

			iter_co->second.key = DECODE_KEY(&keys[16]);

			m_pfds[iter_co->second.pfd_index].events = POLLIN;

			LOG("Connection " << iter_co->second.key << " established" << std::endl);
		}
		return;
	case Proto::OpCode::TCP_TIMEOUT:
		on_timeout();
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


void Client::on_timeout()
{
	std::cout << "Timeout!" << std::endl;

	m_connections.clear();
	m_pfds.resize(2 + m_udp_sockets.size() + m_tcp_listener_sockets.size());

	m_tcp_proto_conn.destroy();

	if(connect_proto_tcp(false))
	{
		m_udp_sockets.clear();

		m_tcp_listener_sockets.clear();
		m_connections.clear();

		m_pfds.resize(2);

		init_post_connection();

		load_config();
	}

	m_pfds.front().fd = m_tcp_proto_conn.socket();
	m_last_tcp_packet = m_cur_time = time(nullptr);
}
