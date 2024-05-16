#ifndef CLASSES_H
#define CLASSES_H

class NoCopy
{
public:
	NoCopy() {}

	NoCopy & operator=(const NoCopy &) = delete;
	NoCopy(const NoCopy &) = delete;
};

#endif
