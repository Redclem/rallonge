#include <iostream>

#ifndef NDEBUG
#define EXT_LOGGING
#endif

#ifndef EXT_LOGGING
	#define LOG(stat)
#else
	#define LOG(stat) std::cout << stat;
#endif
