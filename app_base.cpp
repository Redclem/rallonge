#include "app_base.h"
#include "ral_proto.h"
#include "socket.hpp"
#include <array>
#include <iostream>

void AppBase::discard_udp_message()
{
	std::array<unsigned char, 1> opcode;
	CHECK_RET(m_udp_proto_conn.recv(opcode))

	switch(Proto::OpCode(opcode[0]))
	{
	case Proto::OpCode::NOP:
		return;
	case Proto::OpCode::MESSAGE:
		{
			std::array<unsigned char, 6> head;
			CHECK_RET(m_udp_proto_conn.recv(head))
			uint32_t len = DECODE_UINT32(head.data() + 2);
			std::vector<unsigned char> vdat(len);
			CHECK_RET(m_udp_proto_conn.recv(vdat));
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

void AppBase::discard_tcp_message()
{
	std::array<unsigned char, 1> opcode;
	CHECK_RET(m_tcp_proto_conn.recv(opcode))

	switch(Proto::OpCode(opcode[0]))
	{
	case Proto::OpCode::NOP:
		return;
	case Proto::OpCode::MESSAGE:
		{
			std::array<unsigned char, 6> head;
			CHECK_RET(m_tcp_proto_conn.recv(head))
			uint32_t len = DECODE_UINT32(head.data() + 2);

			m_message_buffer.resize(len);
			CHECK_RET(m_tcp_proto_conn.recv(m_message_buffer));
			return;
		}
	case Proto::OpCode::CONFIG:
		{
			std::cout << "WARNING : TCP config message discarded !" << std::endl;

			std::array<unsigned char, 2> head;
			CHECK_RET(m_tcp_proto_conn.recv(head))
			uint16_t len = DECODE_UINT16(head);

			m_message_buffer.resize(len);
			CHECK_RET(m_tcp_proto_conn.recv(m_message_buffer))
		}
		return;
		
	case Proto::OpCode::CONNECT:
		{
			std::array<unsigned char, 2> trash;
			CHECK_RET(m_tcp_proto_conn.recv(trash))
		}
		return;
	case Proto::OpCode::UDP_CONNECTED:
	default:
		throw NetworkError("Unexpected OpCode on TCP");
	}
}

void AppBase::establish_udp_connection()
{

	std::array<Proto::OpCode, 1> init_msg = {Proto::OpCode::NOP};

	for(int i = 0; i != n_initial_messages; ++i)
	{
		CHECK_RET(m_udp_proto_conn.sendto(init_msg, m_proto_udp_address))
	}

	pollfd pfd = {m_udp_proto_conn.socket(), POLLIN, 0};

	int recv_count = 0;

	while(recv_count < n_initial_messages || !m_udp_established)
	{
		if(!m_udp_established)
			CHECK_RET(m_udp_proto_conn.sendto(init_msg, m_proto_udp_address))
		
		int pres;

		do
		{
			pres = poll(&pfd, 1, 10);

			if(pfd.revents)
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
	CHECK_RET(m_udp_proto_conn.recv(m_message_buffer))

	switch(Proto::OpCode(m_message_buffer[0]))
	{
	case Proto::OpCode::NOP:
		return;
	case Proto::OpCode::MESSAGE:
		{
			uint16_t bridge = DECODE_UINT16(m_message_buffer.data() + 1);
			uint32_t len = DECODE_UINT32(m_message_buffer.data() + 3);

			m_udp_sockets[bridge].sck.sendto_raw(m_message_buffer.data() + 7, len, m_udp_sockets[bridge].addr);
			return;
		}
	case Proto::OpCode::UDP_CONNECTED:
		m_udp_established = true;
		if(m_udp_est_resend)
			CHECK_RET(m_udp_proto_conn.sendto(m_message_buffer, m_proto_udp_address))

		m_udp_est_resend = !m_udp_est_resend;
		return;

	case Proto::OpCode::CONFIG:
	case Proto::OpCode::CONNECT:
	default:
		throw NetworkError("Unexpected OpCode on UDP");
	}
}
