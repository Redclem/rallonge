#ifndef APP_BASE_H
#define APP_BASE_H

#include "classes.h"
#include "socket.hpp"
#include "ral_proto.h"
#include "debug.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <type_traits>
#include <vector>
#include <array>
#include <unordered_map>
#include <iostream>
#include <ctime>
#include <cassert>

#define ENCODE_KEY(key, loc) *reinterpret_cast<key_sock_uni_t*>(loc) = key_sock_uni_t(key);
#define DECODE_KEY(loc) *reinterpret_cast<key_sock_uni_t*>(loc)



class AppBase : public NoCopy
{
public:

	struct CombinedAddressSocket
	{
		Socket sck;
		Address addr;
	};

	typedef uint64_t key_sock_uni_t;

	static_assert(sizeof(key_sock_uni_t) == 8);

	struct ComKey
	{
		key_sock_uni_t sk;
		key_sock_uni_t uk;

		bool operator==(const ComKey & a) const
		{
			return a.sk == sk && a.uk == uk;
		}
	};

	static_assert(sizeof(key_sock_uni_t) >= sizeof(socket_t), "key_sock_uni_t should be able to contain a socket");

	struct Connection
	{
		Socket sck;
		key_sock_uni_t key;
		size_t pfd_index;
	};

	struct CKHash : std::hash<key_sock_uni_t>
	{
		typedef std::true_type is_transparent;

		size_t operator()(const ComKey & ck) const
		{
			return std::hash<key_sock_uni_t>::operator()(ck.sk);
		}

		using std::hash<key_sock_uni_t>::operator();
	};

	struct CKEq : std::equal_to<ComKey>
	{
		typedef std::true_type is_transparent;

		bool operator()(const key_sock_uni_t & sk, ComKey ck) const
		{
			return ck.sk == sk;
		}

		bool operator()(const ComKey & ck, key_sock_uni_t sk) const
		{
			return ck.sk == sk;
		}

		using std::equal_to<ComKey>::operator();
	};

	typedef std::unordered_map<ComKey, Connection, CKHash, CKEq> ConnectionMap;

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
	bool m_bypass_udp = false;
public:

	static constexpr size_t message_buffer_size = 16384 + 8;

	AppBase(bool ub = false) : m_message_buffer(message_buffer_size), m_bypass_udp(ub) {
		if(ub) set_bypass();
	}

	constexpr static int n_initial_messages = 16;
	constexpr static time_t ka_interval = 5;

protected:

#undef max

	// Enable bypass
	void set_bypass()
	{
		m_next_ka_packet = std::numeric_limits<time_t>::max();
	}


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

	void establish_udp_connection();
	void process_udp_message();
	
	// Call when it has been determined that a TCP message is bypassed and directed to udp
	void process_bypassed_message();

	// Call to process UDP message in message buffer. The buffer should have space for the header reserved (should be 8 bytes at time of writing).
	void send_udp(uint16_t bridge, uint32_t size);

	template<bool Message>
	bool disconnect_tcp(ConnectionMap::iterator connex)
	{	

		LOG("Connexion " << connex->first.sk << ", " << connex->second.key << " disconnected." << std::endl);

		if constexpr (Message)
		{
			std::array<unsigned char, 17> msg = {(unsigned char)(Proto::OpCode::TCP_DISCONNECTED)};

			ENCODE_KEY(connex->second.key, &msg[1])
			ENCODE_KEY(connex->first.uk, &msg[9])

			CHECK_RET(m_tcp_proto_conn.Send(msg))
		}

		bool ate = (connex->second.pfd_index + 1) == m_pfds.size();

		if(!ate)
		{
			auto idx = connex->second.pfd_index;
			m_pfds[idx] = m_pfds.back();

			auto it_movco = m_connections.find(key_sock_uni_t(m_pfds[idx].fd));
			if(it_movco == m_connections.end())
			{
				LOG("Something strange happened ... Lost pfd." << std::endl);
			}
			else 
				it_movco->second.pfd_index = idx;

			assert(idx >= 2);
		}
	
		m_pfds.pop_back();
		m_connections.erase(connex);

		return ate;
	}

	template<bool Message>
	bool disconnect_tcp(ComKey connex)
	{
		auto iter_sck = m_connections.find(connex);

		if(iter_sck == m_connections.end())
		{
			LOG("Double disconnect of connection " << connex.sk << std::endl);
			return false; // We don't care in this case...
		}

		return disconnect_tcp<Message>(iter_sck);
	}

	bool check_conn_pfd(std::vector<pollfd>::iterator iter_pfd);
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
static_assert(!resizable<std::array<unsigned char, 3>>::value, "This type should not be resizable");

#endif
