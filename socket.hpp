#ifndef SOCKET_HPP
#define SOCKET_HPP

#include "classes.h"

#ifdef __unix__

#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <unistd.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netinet/in.h>

typedef int socket_t;
typedef uint16_t port_t;

constexpr auto close_socket = close;

constexpr socket_t null_socket = 0;

#define net_err errno

#endif

#ifdef WIN32

#include <winsock2.h>
#include <ws2tcpip.h>

typedef SOCKET socket_t;
typedef u_short port_t;

constexpr auto close_socket = closesocket;
constexpr auto poll = WSAPoll;

constexpr socket_t null_socket = INVALID_SOCKET;

#define net_err WSAGetLastError()

#endif

#include <utility>
#include <stdexcept>
#include <cstdlib>
#include <cstring>

static char exc_buf[500];

#define CHECK_RET(call) if(!(call))\
{\
	throw std::runtime_error((sprintf(exc_buf, "CHECK_RET failed : %d", net_err), exc_buf));\
}\


template <typename T, typename = int>
struct resizable : std::false_type {};

template <typename T>
struct resizable <T, decltype((void) std::declval<T>().resize(1), 0)> : std::true_type {};

struct NetworkError :  std::runtime_error {
	NetworkError() : std::runtime_error("Network Error") {}

	NetworkError(const std::string & s) : std::runtime_error("Network Error : " + s) {}
};

class Socket;

class Address : public NoCopy
{
	sockaddr * m_sa = nullptr;
	socklen_t m_alen = 0;
public:
	Address(Address && rhs) : m_sa(std::exchange(rhs.m_sa, nullptr)), m_alen(rhs.m_alen) {}
	Address(const Address & rhs) : m_sa(reinterpret_cast<sockaddr*>(malloc(rhs.m_alen))), m_alen(rhs.m_alen) {
		memcpy(m_sa, rhs.m_sa, m_alen);
	}

	Address(socklen_t len) : m_sa(reinterpret_cast<sockaddr*>(malloc(len))), m_alen(len) {}

	Address() = default;
	~Address() {destroy();}

	Address(int af, int socktype, const char * adr_str) {
		addrinfo * pnfo, hint = {};
		hint.ai_family = af;
		hint.ai_socktype = socktype;

		auto res = getaddrinfo(adr_str, nullptr, &hint, &pnfo);
		if(res)
		{
			freeaddrinfo(pnfo);
			throw NetworkError(gai_strerror(res));
		}

		destroy();
		m_sa = reinterpret_cast<sockaddr*>(malloc(pnfo->ai_addrlen));
		m_alen = pnfo->ai_addrlen;
		memcpy(m_sa, pnfo->ai_addr, m_alen);
	}

	Address(int af, int socktype, const char * adr_str, port_t port) : Address(af, socktype, adr_str) {
		set_port(port);
	}

	Address & operator=(Address && rhs) {
		destroy();
		m_sa = rhs.m_sa;
		rhs.m_sa = nullptr;
		m_alen = rhs.m_alen;
		return *this;
	}

	Address & operator=(const Address & rhs)
	{
		if(rhs.m_alen == m_alen)
			memcpy(m_sa, rhs.m_sa, m_alen);
		else
		{
			if(m_sa) free(m_sa);
			m_sa = reinterpret_cast<sockaddr*>(malloc(rhs.m_alen));
			m_alen = rhs.m_alen;
			memcpy(m_sa, rhs.m_sa, m_alen);
		}
		return *this;
	}

	void destroy()
	{
		if(m_sa)
		{
			free(m_sa);
			m_sa = nullptr;
			m_alen = 0;
		}
	}
	
	void set_port(port_t port)
	{
		if(m_sa->sa_family == AF_INET || m_sa->sa_family == AF_INET6)
		{
			reinterpret_cast<sockaddr_in*>(m_sa)->sin_port = htons(port);
		}
	}

	port_t port() const
	{
		if(m_sa->sa_family == AF_INET || m_sa->sa_family == AF_INET6)
		{
			return ntohs(reinterpret_cast<sockaddr_in*>(m_sa)->sin_port);
		}
		else return 0;
	}

	int af() const 
	{
		return m_sa->sa_family;
	}

	const sockaddr * addr() const {return m_sa;}
	socklen_t addr_len() const {return m_alen;}

	std::string str() const
	{
		std::string s;
		if(m_sa->sa_family != AF_INET && m_sa->sa_family != AF_INET6) return "<not INET>";

		s.resize(512);

		if(m_sa->sa_family == AF_INET)
		{
			inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(m_sa)->sin_addr, s.data(), s.size());
		}
		else
			inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(m_sa)->sin6_addr, s.data(), s.size());

		return s;
	}

	bool empty() const {return m_sa == nullptr;}

	friend class Socket;
};

class Socket : public NoCopy
{
private:

	socket_t m_sck;

	Socket(socket_t s) : m_sck(s) {}

public:

	Socket() : m_sck(null_socket) {}

	Socket(Socket && rhs) : m_sck(std::exchange(rhs.m_sck, null_socket)) {}

	~Socket() {destroy();}

	Socket & operator=(Socket && rhs) {
		destroy();
		m_sck = rhs.m_sck;
		rhs.m_sck = null_socket;
		return *this;
	}


	void destroy()
	{
		if(m_sck)
		{
			close_socket(m_sck);
			m_sck = null_socket;
		}
	}

	bool valid() const {return m_sck != null_socket;}

	socket_t socket() {return m_sck;}

	bool create(int af, int type, int protocol = 0)
	{
		destroy();
		m_sck = ::socket(af, type, protocol);
		return m_sck != null_socket;
	}

	bool connect(const Address & add)
	{
		return ::connect(m_sck, add.addr(), add.addr_len()) == 0;
	}
	
	bool bind(const Address & add)
	{
		return ::bind(m_sck, add.addr(), add.addr_len()) == 0;
	}

	bool listen(int backlog)
	{
		return ::listen(m_sck, backlog) == 0;
	}

	std::pair<Socket, Address> accept_addr()
	{
		Address adr(sizeof(sockaddr_in6));
		Socket sck(::accept(m_sck, adr.m_sa, &adr.m_alen));

		return std::make_pair(std::move(sck), std::move(adr));
	}

	Socket accept()
	{
		Socket sck(::accept(m_sck, nullptr, nullptr));

		return sck;
	}

	std::pair<bool, Address> getsockname()
	{
		Address adr(sizeof(sockaddr_in6));

		bool r = ::getsockname(m_sck, adr.m_sa, &adr.m_alen) == 0;

		return {r, std::move(adr)};
	}

	typedef std::invoke_result_t<decltype(recv), socket_t, char*, size_t, int> recv_res_t;

	recv_res_t Recv_raw(void * buf, size_t size, int flags = 0)
	{
		return ::recv(m_sck, reinterpret_cast<char*>(buf), size, flags);
	}

	recv_res_t Recvfrom_raw(void * buf, size_t size, Address & addr, int flags = 0)
	{
		if(addr.empty())
			addr = Address(sizeof(sockaddr_in6));
		return ::recvfrom(m_sck, reinterpret_cast<char*>(buf), size, flags, addr.m_sa, &addr.m_alen);
	}

	template<typename Cont>
	bool Recv(Cont & buf, int flags = 0)
	{
		int res = ::recv(m_sck, reinterpret_cast<char*>(buf.data()), buf.size(), flags);
		CHECK_RET(res != -1);
		if constexpr(resizable<Cont>::value) buf.resize(res);
		return true;
	}


	template<typename Cont>
	std::pair<bool, Address> recvfrom(Cont & buf, int flags = 0)
	{
		Address adr(sizeof(sockaddr_in6));
		int res = ::recvfrom(m_sck, reinterpret_cast<char*>(buf.data()), buf.size(), flags, adr.m_sa, &adr.m_alen);
		if(res == -1) return {false, {}};
		if constexpr(resizable<Cont>::value) buf.resize(res);
		return {true, adr};
	}

	template<typename Cont>
	int Send(const Cont & buf, int flags = 0)
	{
		return ::send(m_sck, reinterpret_cast<const char*>(buf.data()), buf.size(), flags);
	}
	
	template<typename Cont>
	int Sendto(const Cont & buf, Address a, int flags = 0)
	{
		return ::sendto(m_sck, reinterpret_cast<const char*>(buf.data()), buf.size(), flags, a.m_sa, a.m_alen);
	}
	
	int Sendto_raw(const void * dat, size_t len, Address a, int flags = 0)
	{
		return ::sendto(m_sck, reinterpret_cast<const char *>(dat), len, flags, a.m_sa, a.m_alen);
	}

	bool close()
	{
		return close_socket(m_sck) == 0;
	}
};

#endif
