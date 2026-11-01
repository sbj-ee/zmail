#include "OffThread.h"

namespace zmail::offthread {

qsizetype thresholdBytes()
{
    bool ok = false;
    const qsizetype v = qEnvironmentVariableIntValue("ZMAIL_OFFTHREAD_BYTES", &ok);
    return ok && v >= 0 ? v : kDefaultThresholdBytes;
}

std::atomic<int> &jobsStarted()
{
    static std::atomic<int> n{0};
    return n;
}

} // namespace zmail::offthread
