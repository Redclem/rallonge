#include "app_base.h"
#include "ral_proto.h"
#include "socket.hpp"
#include <array>
#include <iostream>

void AppBase::discard_udp_message()
{
	std::array<unsigned char, 1> opcode;
	CHECK_RET(m_udp_proto_conn.Recv(opcode))

	switch(Proto::OpCode(opcode[0]))
	{
	case Proto::OpCode::NOP:
		return;
	case Proto::OpCode::MESSAGE:
		{
			std::array<unsigned char, 6> head;
			CHECK_RET(m_udp_proto_conn.Recv(head))
			uint32_t len = DECODE_UINT32(head.data() + 2);
			std::vector<unsigned char> vdat(len);
			CHECK_RET(m_udp_proto_conn.Recv(vdat));
			return;
		}
	case Proto::OpCode::UDP_CONNECTED:
		m_udp_established = true;
		return;

	case Proto::OpCode::CONFIG:
	case Proto::OpCode::CONNECT:
	default:
		throw NetworkError("Unexpected OpCode on UDP");
	}
}

void AppBase::establish_udp_connection()
{

	std::array<Proto::OpCode, 1> init_msg = {Proto::OpCode::NOP};

	for(int i = 0; i != n_initial_messages; ++i)
	{
		CHECK_RET(m_udp_proto_conn.Sendto(init_msg, m_proto_udp_address))
	}

	pollfd pfd = {m_udp_proto_conn.socket(), POLLIN, 0};

	int recv_count = 0;

	while(recv_count < n_initial_messages || !m_udp_established)
	{
		if(!m_udp_established)
			CHECK_RET(m_udp_proto_conn.Sendto(init_msg, m_proto_udp_address))
		
		int pres;

		do
		{
			pres = poll(&pfd, 1, 10);

			if(pfd.revents & pollmask)
			{
				pfd.revents = 0;
				discard_udp_message();
				recv_count++;

				if(recv_count == n_initial_messages)
				{
					init_msg[0] = Proto::OpCode::UDP_CONNECTED;
				}
			}
		}
		while(pres > 0);

		CHECK_RET(pres == 0)
	}
}

void AppBase::process_udp_message()
{
	m_message_buffer.resize(m_message_buffer.capacity());
	CHECK_RET(m_udp_proto_conn.Recv(m_message_buffer))

	switch(Proto::OpCode(m_message_buffer[0]))
	{
	case Proto::OpCode::NOP:
		return;
	case Proto::OpCode::MESSAGE:
		{
			uint16_t bridge = DECODE_UINT16(m_message_buffer.data() + 1);
			uint32_t len = DECODE_UINT32(m_message_buffer.data() + 3);

			m_udp_sockets[bridge].sck.Sendto_raw(m_message_buffer.data() + 7, len, m_udp_sockets[bridge].addr);
			return;
		}
	case Proto::OpCode::UDP_CONNECTED:
		m_udp_established = true;
		if(m_udp_est_resend)
			CHECK_RET(m_udp_proto_conn.Sendto(m_message_buffer, m_proto_udp_address))

		m_udp_est_resend = !m_udp_est_resend;
		return;

	case Proto::OpCode::CONFIG:
	case Proto::OpCode::CONNECT:
	default:
		throw NetworkError("Unexpected OpCode on UDP");
	}
}

void AppBase::process_bypassed_message()
{
	std::array<unsigned char, 6> buf;
	m_tcp_proto_conn.Recv(buf);

	uint16_t bridge = DECODE_UINT16(buf.data());
	uint32_t len = DECODE_UINT32(buf.data() + 2);

	m_message_buffer.resize(len);

	LOG("Processing bypassed udp with size " << len << std::endl)

	m_tcp_proto_conn.Recv(m_message_buffer, MSG_WAITALL);

	m_udp_sockets[bridge].sck.Sendto(m_message_buffer, m_udp_sockets[bridge].addr);
}

void AppBase::send_udp(uint16_t bridge, uint32_t size)
{
	m_message_buffer.resize(size + Proto::udp_message_header_size);

	ENCODE_UINT16(bridge, m_message_buffer.data() + 2)
	ENCODE_UINT32(size, m_message_buffer.data() + 4)

	if(m_bypass_udp)
	{
		m_message_buffer[1] = (unsigned char)(Proto::Protocol::UDP);
		m_message_buffer[0] = (unsigned char)(Proto::OpCode::MESSAGE);

		m_tcp_proto_conn.Send(m_message_buffer);
	}
	else
	{
		m_message_buffer[1] = (unsigned char)(Proto::OpCode::MESSAGE);

		m_udp_proto_conn.Sendto_raw(m_message_buffer.data() + 1, m_message_buffer.size() - 1, m_proto_udp_address);
	}

	update_udp_ka();
}

bool AppBase::check_conn_pfd(std::vector<pollfd>::iterator iter_pfd)
	{
		auto conn = m_connections.find(key_sock_uni_t(iter_pfd->fd));

		while(iter_pfd->revents & pollmask)
		{
			Socket::recv_res_t recres;

			if(iter_pfd->revents & POLLERR)
			{
				recres = 0;
			}
			else
			{
				// If poll gives hangup, we still need to receive last data
				// So we process hangup here if recres is 0

				m_message_buffer.resize(m_message_buffer.capacity());
				
				recres = conn->second.sck.Recv_raw(m_message_buffer.data() + Proto::tcp_message_header_size, m_message_buffer.size() - Proto::tcp_message_header_size, 0);
				CHECK_RET(recres >= 0);
			}
			
			if(recres == 0) // Connection loss
			{
				LOG("Connection " << conn->first.sk << ',' << conn->second.key << " Hung up." << std::endl);
				if(disconnect_tcp<true>(conn))
					return true;

				// The iter_sck structure has changed. Update connection and poll again.
				conn = m_connections.find(key_sock_uni_t(iter_pfd->fd));
			}
			else // Message
			{
				m_message_buffer.resize(recres + Proto::tcp_message_header_size);

				ENCODE_KEY(conn->second.key, &m_message_buffer[2])
				ENCODE_KEY(conn->first.uk, &m_message_buffer[10])

				ENCODE_UINT32(recres, &m_message_buffer[18])

				if(m_bypass_udp)
				{
					m_message_buffer[1] = (unsigned char)(Proto::Protocol::TCP);
					m_message_buffer[0] = (unsigned char)(Proto::OpCode::MESSAGE);
					CHECK_RET(m_tcp_proto_conn.Send(m_message_buffer))
				}
				else
				{
					m_message_buffer[1] = (unsigned char)(Proto::OpCode::MESSAGE);
					CHECK_RET(m_tcp_proto_conn.Send_raw(m_message_buffer.data() + 1, m_message_buffer.size() - 1));
				}
			}

			CHECK_RET(poll(&(*iter_pfd), 1, 0) >= 0)
		}

		return false;
	}

void AppBase::create_udp_socket()
{	
	std::cout << "Creating UDP Socket ..." << std::endl;
	CHECK_RET(m_udp_proto_conn.create(AF_INET, SOCK_DGRAM))
	CHECK_RET(m_udp_proto_conn.bind(Address(AF_INET, SOCK_DGRAM, "0.0.0.0", 0)))

	auto [res, udp_plug_adr] = m_udp_proto_conn.getsockname();
	CHECK_RET(res);

	m_udp_port = udp_plug_adr.port();

	std::cout << "UDP Socket created at port " << m_udp_port << '.' << std::endl;

	m_pfds[1].fd = m_udp_proto_conn.socket();
}
