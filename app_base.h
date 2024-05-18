#ifndef APP_BASE_H
#define APP_BASE_H

#include "classes.h"
#include "socket.hpp"
#include "ral_proto.h"

#include <cstdint>
#include <vector>
#include <array>
#include <unordered_map>
#include <iostream>
#include <ctime>
#include <cassert>

#define ENCODE_KEY(key, loc) *reinterpret_cast<key_sock_t*>(loc) = key_sock_t(key);
#define DECODE_KEY(loc) *reinterpret_cast<key_sock_t*>(loc)

class AppBase : public NoCopy
{
public:

	struct CombinedAddressSocket
	{
		Socket sck;
		Address addr;
	};

	typedef uint64_t key_sock_t;

	static_assert(sizeof(key_sock_t) >= sizeof(socket_t), "key_sock_t should be able to contain a socket");

	struct Connection
	{
		Socket sck;
		key_sock_t key;
		size_t pfd_index;
	};

	typedef std::unordered_map<key_sock_t, Connection> ConnectionMap;

protected:
	Socket m_tcp_proto_conn, m_udp_proto_conn;
	Address m_proto_udp_address;
	std::vector<pollfd> m_pfds;
	std::vector<unsigned char> m_message_buffer;

	std::vector<CombinedAddressSocket> m_udp_sockets;
	ConnectionMap m_connections;

	time_t m_next_ka_packet = 0;

	uint16_t m_udp_port;
	bool m_udp_established = false;
	bool m_udp_est_resend = true;
	bool m_run = true;
public:

	static constexpr size_t message_buffer_size = 16384;

	AppBase() : m_message_buffer(message_buffer_size) {}

	constexpr static int n_initial_messages = 16;
	constexpr static time_t ka_interval = 5;

protected:

	bool ka_message()
	{
		auto t = time(nullptr);
		if(t >= m_next_ka_packet)
		{
			m_next_ka_packet = t + ka_interval;
			return true;
		}
		return false;
	}
	

	void discard_udp_message();
	void discard_tcp_message();

	void establish_udp_connection();
	void process_udp_message();

	template<bool Message>
	bool disconnect_tcp(ConnectionMap::iterator connex)
	{	

		std::cout << "Connexion " << connex->first << ", " << connex->second.key << " disconnected." << std::endl;

		if constexpr (Message)
		{
			std::array<unsigned char, 9> msg = {(unsigned char)(Proto::OpCode::TCP_DISCONNECTED)};

			ENCODE_KEY(connex->second.key, &msg[1])

			CHECK_RET(m_tcp_proto_conn.Send(msg))
		}

		bool ate = (connex->second.pfd_index + 1) == m_pfds.size();

		if(!ate)
		{
			auto idx = connex->second.pfd_index;
			m_pfds[idx] = m_pfds.back();
			m_connections.at(key_sock_t(m_pfds[idx].fd)).pfd_index = idx;
		}
	
		m_pfds.pop_back();
		m_connections.erase(connex);

		return ate;
	}

	template<bool Message>
	bool disconnect_tcp(key_sock_t connex)
	{
		auto iter_sck = m_connections.find(connex);

		if(iter_sck == m_connections.end())
		{
			std::cout << "Double disconnect of connection " << connex << std::endl;
			return false; // We don't care in this case...
		}

		return disconnect_tcp<Message>(iter_sck);
	}

	bool check_conn_pfd(std::vector<pollfd>::iterator iter_pfd)
	{
		auto conn = m_connections.find(key_sock_t(iter_pfd->fd));

		while(iter_pfd->revents)
		{
			Socket::recv_res_t recres;

			if(iter_pfd->revents & (POLLHUP | POLLERR))
			{
				recres = 0;
			}
			else
			{
				m_message_buffer.resize(m_message_buffer.capacity());
				
				recres = conn->second.sck.Recv_raw(m_message_buffer.data() + Proto::tcp_message_header_size, m_message_buffer.size() - Proto::tcp_message_header_size, 0);
				CHECK_RET(recres >= 0);
			}
			
			if(recres == 0) // Connection loss
			{
				std::cout << "Connection " << conn->first << ',' << conn->second.key << " Hung up." << std::endl;
				if(disconnect_tcp<true>(conn))
					return true;

				// The iter_sck structure has changed. Update connection and poll again.
				conn = m_connections.find(key_sock_t(iter_pfd->fd));
			}
			else // Message
			{
				m_message_buffer.resize(recres + Proto::tcp_message_header_size);
				m_message_buffer[0] = (unsigned char)(Proto::OpCode::MESSAGE);

				ENCODE_KEY(conn->second.key, &m_message_buffer[1])
				ENCODE_UINT32(recres, &m_message_buffer[9])

				CHECK_RET(m_tcp_proto_conn.Send(m_message_buffer))
			}

			CHECK_RET(poll(&(*iter_pfd), 1, 0) >= 0)
		}

		return false;
	}
};


template<size_t Size>
struct StatVec : public std::array<unsigned char, Size>
{
public:
	size_t dyn_size;

	void resize(size_t s) {dyn_size = s;}
};

static_assert(resizable<std::vector<unsigned char>>::value, "This type should be resizable");
static_assert(resizable<StatVec<2>>::value, "This type should be resizable");


#endif
