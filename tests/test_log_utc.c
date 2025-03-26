#include <stdlib.h>

#include "hxm.h"

int main(void) {
    setenv("HXM_LOG_UTC", "1", 1);
    hxm_log(LOG_INFO, "utc");
    return 0;
}
