#include "honch/core/storage.h"

#include <assert.h>
#include <string.h>

int main(void)
{
    honch_storage_reader_t reader = {0};
    assert(reader.total_size == 0u);
    assert(reader.sequence == 0u);
    assert(reader.read == 0);
    return 0;
}
