#include <iostream>

#ifdef NDEBUG
	#define LOG(stat)
#else
	#define LOG(stat) std::cout << stat;
#endif
