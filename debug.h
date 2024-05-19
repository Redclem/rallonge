#include <iostream>

#ifdef NDEBUG
	#define LOG(stat) ((void*)0)
#else
	#define LOG(stat) std::cout << stat;
#endif
