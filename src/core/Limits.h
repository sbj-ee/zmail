#pragma once

// Message size limits that mirror Google's. Keep every size limit zmail
// enforces here, so a change on Google's side is a one-line edit.
// Checked against Google's documentation on 2026-10-03.

#include <cstdint>

namespace zmail::limits {

// Gmail send limit for personal accounts: "For personal Gmail accounts, the
// limit is 25 MB." Applied to the total encoded MIME message (headers, bodies
// and base64-encoded attachments), using decimal MB to stay on the safe side.
// https://support.google.com/mail/answer/6584
inline constexpr std::int64_t kSendLimitBytes = 25'000'000;

// Start warning in the compose size meter at 80% of the send limit.
inline constexpr int kSendWarnPercent = 80;

// Gmail API upload cap for users.messages.send / drafts.create / drafts.update
// / drafts.send: mediaUpload.maxSize = 36700160 bytes (35 MiB), from the Gmail
// API discovery document. Above kSendLimitBytes, so the send limit is binding.
// https://gmail.googleapis.com/$discovery/rest?version=v1
// https://developers.google.com/workspace/gmail/api/reference/rest/v1/users.messages/send
inline constexpr std::int64_t kApiSendUploadMaxBytes = 36'700'160;

// Receive limit: "You can receive emails of up to 50 MB ... These values are
// the limit after encoding." Google documents this for Workspace; the
// personal-Gmail help page doesn't state a receive limit. zmail uses it only
// to size buffers and timeouts, never to reject mail.
// https://support.google.com/a/answer/1366776
inline constexpr std::int64_t kReceiveLimitBytes = 50'000'000;

// Messages larger than this are fetched in stages (headers and text first,
// attachments on demand) and streamed to disk rather than held in memory.
inline constexpr std::int64_t kLargeMessageBytes = 5'000'000;

// Gmail API "simple" path for messages.send / drafts.*: the message goes
// base64url-encoded in the JSON body ({"raw": ...}, plus threadId). Google
// recommends the upload endpoints for larger messages, so anything at or
// above this many encoded MIME bytes uses the resumable upload protocol
// (/upload/gmail/v1/..., uploadType=resumable), up to kApiSendUploadMaxBytes.
// https://developers.google.com/workspace/gmail/api/guides/uploads
inline constexpr std::int64_t kSimpleUploadMaxBytes = 5'000'000;

// MIME base64 line length (RFC 2045): 76 characters, then CRLF.
inline constexpr int kBase64LineLength = 76;

// Bytes a part occupies once base64-encoded for MIME: 4 output bytes per 3
// input bytes (rounded up), plus CRLF after every full 76-character line.
// That's about 1.37x (Google quotes "about a 37% increase").
constexpr std::int64_t base64MimeSize(std::int64_t rawBytes)
{
    if (rawBytes <= 0) {
        return 0;
    }
    const std::int64_t encoded = 4 * ((rawBytes + 2) / 3);
    const std::int64_t lineBreaks = (encoded - 1) / kBase64LineLength; // between lines
    return encoded + 2 * lineBreaks;
}

enum class SizeLevel { Ok, Warn, Blocked };

// Classify a total encoded message size for the compose size meter.
constexpr SizeLevel classifySendSize(std::int64_t encodedBytes)
{
    if (encodedBytes > kSendLimitBytes) {
        return SizeLevel::Blocked;
    }
    if (encodedBytes * 100 >= kSendLimitBytes * kSendWarnPercent) {
        return SizeLevel::Warn;
    }
    return SizeLevel::Ok;
}

static_assert(kSendLimitBytes <= kApiSendUploadMaxBytes,
              "the send limit must fit within the Gmail API upload cap");
static_assert(kSimpleUploadMaxBytes < kSendLimitBytes);
static_assert(base64MimeSize(3) == 4);
static_assert(base64MimeSize(57) == 76);      // exactly one full line
static_assert(base64MimeSize(58) == 76 + 2 + 4);

} // namespace zmail::limits
