#pragma once

#include "MimeBuilder.h"

#include <QList>

namespace zmail::zip {

// PLAN §4.5.1: would zipping get an over-limit message under the limit?
struct Probe
{
    qint64 rawBytes = 0;          // sum of attachment sizes
    qint64 estimatedZipBytes = 0; // deflate estimate of the archive
    bool worthwhile = false;      // saves at least 10%
};

// Already-compressed formats (jpg, png, mp4, zip, docx, pdf...) are counted
// at full size; the rest are probed by deflating up to the first 1 MB.
Probe probe(const QList<OutgoingAttachment> &attachments);
bool isAlreadyCompressed(const QString &fileName, const QString &mimeType);

// Builds one deflated archive (UTF-8 names, duplicates renamed "a (2).txt").
// Returns an empty QByteArray and sets *error on failure.
QByteArray makeArchive(const QList<OutgoingAttachment> &attachments, QString *error = nullptr);

// Names inside an archive (tests / verification).
QStringList listArchive(const QByteArray &zip, QString *error = nullptr);
QByteArray extract(const QByteArray &zip, const QString &name);

} // namespace zmail::zip
