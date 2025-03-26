#include <stdlib.h>

#include "hxm.h"

int main(void) {
    setenv("HXM_LOG_MONO", "1", 1);
    hxm_log(LOG_INFO, "mono");
    hxm_log(LOG_INFO, NULL);
    hxm_log(LOG_ERROR, "mono error");
    return 0;
}
