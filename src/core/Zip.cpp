#include "Zip.h"

#include <QFileInfo>
#include <QSet>
#include <QTemporaryFile>
#include <zip.h>

namespace zmail::zip {

bool isAlreadyCompressed(const QString &fileName, const QString &mimeType)
{
    static const QSet<QString> exts{
        QStringLiteral("zip"), QStringLiteral("gz"), QStringLiteral("tgz"), QStringLiteral("bz2"), QStringLiteral("xz"),
        QStringLiteral("zst"), QStringLiteral("7z"), QStringLiteral("rar"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("png"), QStringLiteral("gif"), QStringLiteral("webp"), QStringLiteral("heic"), QStringLiteral("avif"),
        QStringLiteral("mp3"), QStringLiteral("m4a"), QStringLiteral("aac"), QStringLiteral("ogg"), QStringLiteral("opus"),
        QStringLiteral("flac"), QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("mov"), QStringLiteral("mkv"),
        QStringLiteral("webm"), QStringLiteral("docx"), QStringLiteral("xlsx"), QStringLiteral("pptx"), QStringLiteral("odt"),
        QStringLiteral("ods"), QStringLiteral("odp"), QStringLiteral("epub"), QStringLiteral("jar"), QStringLiteral("apk"),
        QStringLiteral("pdf")};
    if (exts.contains(QFileInfo(fileName).suffix().toLower())) {
        return true;
    }
    return (mimeType.startsWith(QLatin1String("image/")) && mimeType != QLatin1String("image/svg+xml") &&
            mimeType != QLatin1String("image/bmp")) ||
           mimeType.startsWith(QLatin1String("video/")) || mimeType.startsWith(QLatin1String("audio/"));
}

Probe probe(const QList<OutgoingAttachment> &attachments)
{
    constexpr qint64 kSample = 1'000'000;
    Probe p;
    for (const auto &a : attachments) {
        const qint64 n = a.data.size();
        p.rawBytes += n;
        qint64 est = n;
        if (!isAlreadyCompressed(a.fileName, a.mimeType) && n > 0) {
            const QByteArray sample = a.data.left(kSample);
            // qCompress = zlib deflate + 4-byte length prefix; close enough to zip's raw deflate.
            const double ratio = double(qCompress(sample, 6).size() - 4) / double(sample.size());
            est = qint64(double(n) * std::min(1.0, ratio));
        }
        // Local header + central directory entry, roughly.
        p.estimatedZipBytes += est + 76 + 2 * a.fileName.toUtf8().size();
    }
    p.estimatedZipBytes += 22;
    p.worthwhile = p.rawBytes > 0 && p.estimatedZipBytes * 10 <= p.rawBytes * 9;
    return p;
}

QByteArray makeArchive(const QList<OutgoingAttachment> &attachments, QString *error)
{
    auto fail = [error](const QString &e) {
        if (error) {
            *error = e;
        }
        return QByteArray();
    };
    QTemporaryFile tmp;
    if (!tmp.open()) {
        return fail(QStringLiteral("cannot create a temporary file"));
    }
    const QByteArray path = QFile::encodeName(tmp.fileName());
    tmp.close();
    int zerr = 0;
    zip_t *z = zip_open(path.constData(), ZIP_TRUNCATE | ZIP_CREATE, &zerr);
    if (!z) {
        return fail(QStringLiteral("zip_open failed (%1)").arg(zerr));
    }
    QSet<QString> used;
    for (const auto &a : attachments) {
        QString name = QFileInfo(a.fileName).fileName();
        if (name.isEmpty()) {
            name = QStringLiteral("attachment");
        }
        const QFileInfo fi(name);
        for (int i = 2; used.contains(name.toLower()); ++i) {
            name = fi.suffix().isEmpty() ? QStringLiteral("%1 (%2)").arg(fi.completeBaseName()).arg(i)
                                         : QStringLiteral("%1 (%2).%3").arg(fi.completeBaseName()).arg(i).arg(fi.suffix());
        }
        used.insert(name.toLower());
        // The buffers must outlive zip_close(); `attachments` does.
        zip_source_t *src = zip_source_buffer(z, a.data.constData(), zip_uint64_t(a.data.size()), 0);
        if (!src) {
            zip_discard(z);
            return fail(QStringLiteral("zip_source_buffer failed"));
        }
        const zip_int64_t idx = zip_file_add(z, name.toUtf8().constData(), src, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
        if (idx < 0) {
            zip_source_free(src);
            zip_discard(z);
            return fail(QStringLiteral("zip_file_add failed"));
        }
        zip_set_file_compression(z, zip_uint64_t(idx),
                                 isAlreadyCompressed(a.fileName, a.mimeType) ? ZIP_CM_STORE : ZIP_CM_DEFLATE, 6);
    }
    if (zip_close(z) != 0) {
        const QString e = QString::fromUtf8(zip_strerror(z));
        zip_discard(z);
        return fail(e);
    }
    QFile f(tmp.fileName());
    if (!f.open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral("cannot read the archive"));
    }
    return f.readAll();
}

namespace {
zip_t *openBuffer(const QByteArray &zipData, QString *error)
{
    zip_error_t ze;
    zip_error_init(&ze);
    zip_source_t *src = zip_source_buffer_create(zipData.constData(), zip_uint64_t(zipData.size()), 0, &ze);
    zip_t *z = src ? zip_open_from_source(src, ZIP_RDONLY | ZIP_CHECKCONS, &ze) : nullptr;
    if (!z) {
        if (error) {
            *error = QString::fromUtf8(zip_error_strerror(&ze));
        }
        if (src) {
            zip_source_free(src);
        }
    }
    zip_error_fini(&ze);
    return z;
}
} // namespace

QStringList listArchive(const QByteArray &zipData, QString *error)
{
    QStringList names;
    zip_t *z = openBuffer(zipData, error);
    if (!z) {
        return names;
    }
    const zip_int64_t n = zip_get_num_entries(z, 0);
    for (zip_int64_t i = 0; i < n; ++i) {
        names << QString::fromUtf8(zip_get_name(z, zip_uint64_t(i), ZIP_FL_ENC_GUESS));
    }
    zip_close(z);
    return names;
}

QByteArray extract(const QByteArray &zipData, const QString &name)
{
    zip_t *z = openBuffer(zipData, nullptr);
    if (!z) {
        return {};
    }
    QByteArray out;
    zip_stat_t st;
    if (zip_stat(z, name.toUtf8().constData(), ZIP_FL_ENC_UTF_8, &st) == 0) {
        if (zip_file_t *f = zip_fopen(z, name.toUtf8().constData(), 0)) {
            out.resize(qsizetype(st.size));
            const zip_int64_t got = zip_fread(f, out.data(), st.size);
            out.resize(got < 0 ? 0 : qsizetype(got));
            zip_fclose(f);
        }
    }
    zip_close(z);
    return out;
}

} // namespace zmail::zip
