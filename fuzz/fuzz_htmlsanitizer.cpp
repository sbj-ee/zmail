// HtmlSanitizer on hostile HTML, with and without remote images kept.
#include "FuzzCommon.h"
#include "core/HtmlSanitizer.h"

extern "C" int LLVMFuzzerInitialize(int *, char ***)
{
    zmail::fuzz::initQt();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const QString html = QString::fromUtf8(reinterpret_cast<const char *>(data), static_cast<qsizetype>(size));
    int blocked = 0;
    (void)zmail::html::sanitize(html, &blocked, false);
    (void)zmail::html::sanitize(html, nullptr, true);
    return 0;
}
