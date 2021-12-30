#include <stdlib.h>
#include <sys/types.h>
#include "RandomHelper.h"

void CRandomHelper::GenerateBuffer(void* buffer, size_t size)
{
    size_t icount = size / sizeof(int);
    int *ibuf = static_cast<int *>(buffer);
    char *chbuf = static_cast<char *>(buffer);

    for (size_t offset = 0; offset < icount; offset++)
        ibuf[offset] = ::random();

    for (size_t offset = (icount * sizeof(int)); offset < size; offset++)
        chbuf[offset] = static_cast<char>(::random() & 0xFF);
}

int CRandomHelper::GenerateInt()
{
    return ::random();
}