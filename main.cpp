#include "server.h"
#include "client.h"
#include "socket.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>

constexpr const char * usage =
	"Rallonge TCP / UDP tunnel\n\n"

	"Usage : rallonge <client / server> <options>\n\n"

	"Client usage:\n"
	"rallonge client <server hostname> <server port> <config file>\n\n"

	"Server usage:\n"
	"rallonge server <tcp port>\n"
;

int main(int argc, char * argv[])
{
	if(argc < 2)
	{
		std::cout << usage;
		return 0;
	}

#ifdef WIN32
	{
		WSADATA d;
		WSAStartup(2, &d);
	}
#endif

	try
	{
		if(strcmp(argv[1], "client") == 0)
		{
			if(argc != 5)
			{
				std::cout << usage;
				return 0;
			}

			Client cl(argv[2], port_t(atoi(argv[3])), argv[4]);
			cl.run();
		}
		else if (strcmp(argv[1],  "server") == 0)
		{
			if(argc != 3)
			{
				std::cout << usage;
				return 0;
			}

			Server srv(port_t(atoi(argv[2])));
			srv.run();
		}
		else
		{
			std::cout << usage;
			return 0;
		}
	}
	catch (const std::runtime_error & e)
	{
		std::cout << e.what() << std::endl;
		std::cout << "error number : " << net_err << std::endl;
#ifdef __unix__
		std::cout << strerror(net_err) << std::endl;
#endif
	}

#ifdef WIN32
	WSACleanup();
#endif
}
