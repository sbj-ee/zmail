// MessageParser on a Gmail API message resource (format=full JSON).
#include "FuzzCommon.h"
#include "core/MessageParser.h"

#include <QJsonDocument>
#include <QJsonObject>

extern "C" int LLVMFuzzerInitialize(int *, char ***)
{
    zmail::fuzz::initQt();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    using namespace zmail;
    const QByteArray in(reinterpret_cast<const char *>(data), static_cast<qsizetype>(size));
    const QJsonObject msg = QJsonDocument::fromJson(in).object();
    (void)MessageParser::fromMetadata(msg);
    (void)MessageParser::bodyFromFull(msg);
    (void)MessageParser::splitAddress(QString::fromUtf8(in.left(4096)));
    (void)MessageParser::decodeBase64Url(QString::fromLatin1(in));
    return 0;
}
