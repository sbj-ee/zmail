// RawMime: raw RFC 822 as IMAP hands it over, then MessageParser on the
// payload it builds (the IMAP read path end to end).
#include "FuzzCommon.h"
#include "core/MessageParser.h"
#include "core/RawMime.h"

#include <QJsonObject>

extern "C" int LLVMFuzzerInitialize(int *, char ***)
{
    zmail::fuzz::initQt();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    using namespace zmail;
    const QByteArray raw(reinterpret_cast<const char *>(data), static_cast<qsizetype>(size));
    QJsonObject json;
    json.insert(QStringLiteral("id"), QStringLiteral("fuzz"));
    json.insert(QStringLiteral("payload"), RawMime::toGmailPayload(raw));
    (void)MessageParser::fromMetadata(json);
    (void)MessageParser::bodyFromFull(json);
    (void)RawMime::parseHeaders(raw.left(64 * 1024));
    (void)RawMime::decodeEncodedWords(raw.left(64 * 1024));
    (void)RawMime::decodeQuotedPrintable(raw, size & 1);
    (void)RawMime::headerParam(QString::fromUtf8(raw.left(8 * 1024)), QStringLiteral("filename"));
    return 0;
}
