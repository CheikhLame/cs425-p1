#ifndef LAB_H
#define LAB_H

#include <stddef.h>
#include <stdio.h>

/* Exit codes, per the assignment spec */
#define EXIT_USAGE 1
#define EXIT_SMTP_FAIL 2

/* Error codes shared by layers 2 and 3. Zero and positive values are
 * reserved for real SMTP status codes (2xx/3xx/4xx/5xx), so these are
 * all negative and can never collide with one. */
#define SMTP_ERR_IO        (-1) /* transport read/write failed */
#define SMTP_ERR_EOF       (-2) /* transport closed before a complete line/reply arrived */
#define SMTP_ERR_TOO_LONG  (-3) /* a line exceeded the reader's fixed buffer */
#define SMTP_ERR_MALFORMED (-4) /* reply text didn't parse as "ddd[- ]..." */

/* =====================================================================
 * Layer 1 -- pure protocol helpers. No I/O anywhere in here: every
 * function takes strings (and lengths) and returns strings or status
 * codes, so they can be unit tested directly with no transport at all.
 * ===================================================================== */

/*
 * Parses the 3-digit status code from the start of a reply line.
 * `len` is the line's length, not counting any line terminator.
 * Returns the code (100-599) on success, or SMTP_ERR_MALFORMED if the
 * line doesn't start with three digits followed by ' ' or '-'.
 */
int smtp_parse_reply_code(const char *line, size_t len);

/*
 * Returns 1 if `line` is the final line of a (possibly multi-line)
 * reply -- RFC 5321's "code SP text" form -- 0 if it's a continuation
 * line ("code - text"), or SMTP_ERR_MALFORMED if the line is too
 * short or doesn't start with three digits.
 */
int smtp_is_final_reply_line(const char *line, size_t len);

/*
 * Appends CRLF to `text` and returns a malloc'd copy for sending as a
 * command line. Returns NULL on allocation failure.
 */
char *smtp_build_command_line(const char *text);

/*
 * Dot-stuffs `body` per RFC 5321 4.5.2: splits on '\n' (a line may
 * already end in "\r\n" or just "\n"), doubles a leading '.' on any
 * line, CRLF-terminates every line, and appends the terminating
 * ".\r\n" line. Returns a malloc'd, NUL-terminated buffer with
 * *out_len set to its byte length (safe even if the body contains
 * embedded NULs -- callers should use *out_len, not strlen). NULL on
 * allocation failure.
 */
char *smtp_dot_stuff_body(const char *body, size_t *out_len);

/*
 * Builds the complete DATA payload: "From:", "To:", "Subject:"
 * headers, a blank line, the dot-stuffed body (via
 * smtp_dot_stuff_body), and the terminating ".\r\n" line. Returns a
 * malloc'd buffer with *out_len set to its length. NULL on OOM.
 */
char *smtp_build_data_payload(const char *from, const char *to,
                               const char *subject, const char *body,
                               size_t *out_len);

/* =====================================================================
 * Layer 2 -- the session, over a transport you can swap out. Every
 * read and write goes through the two callbacks below rather than
 * calling recv/send directly, so tests can plug in a scripted
 * in-memory transport and drive a complete session -- and every one
 * of its error paths -- with no network at all.
 * ===================================================================== */

/*
 * Read up to `len` bytes into `buf`. Same contract as recv(): >0 is
 * the number of bytes read, 0 means the peer closed cleanly, <0 is an
 * error.
 */
typedef long (*smtp_read_fn)(void *ctx, char *buf, size_t len);

/*
 * Write up to `len` bytes from `buf`. Same contract as send(): >=0 is
 * the number of bytes actually written (may be less than `len`), <0
 * is an error.
 */
typedef long (*smtp_write_fn)(void *ctx, const char *buf, size_t len);

typedef struct
{
    smtp_read_fn read;
    smtp_write_fn write;
    void *ctx;
} smtp_transport_t;

/* Fixed line-buffer capacity for smtp_reader_t. A reply line longer
 * than this is a protocol violation from the server's side; the
 * reader reports it rather than growing without bound. */
#define SMTP_RBUF_SIZE 1024

typedef struct
{
    smtp_transport_t transport;
    char buf[SMTP_RBUF_SIZE];
    size_t start; /* offset of unconsumed data in buf */
    size_t end;   /* offset just past unconsumed data in buf */
} smtp_reader_t;

/* Initializes `reader` to read from `transport`. Does no I/O. */
void smtp_reader_init(smtp_reader_t *reader, smtp_transport_t transport);

/*
 * Reads one line, refilling the internal buffer from the transport
 * only when it doesn't already hold a complete line (a reply may
 * arrive split across several reads, or several replies may arrive
 * in one -- this handles both). The terminating '\n' (and a preceding
 * '\r', if present) is stripped.
 *
 * On success returns the line length (>= 0) and sets *out to a
 * malloc'd, NUL-terminated copy the caller must free. On failure
 * returns one of SMTP_ERR_IO, SMTP_ERR_EOF (transport closed with an
 * incomplete or absent line pending), or SMTP_ERR_TOO_LONG (line
 * would exceed SMTP_RBUF_SIZE); *out is left unset.
 */
int smtp_reader_read_line(smtp_reader_t *reader, char **out);

/*
 * Reads one full (possibly multi-line) SMTP reply, following
 * continuation lines via smtp_is_final_reply_line. Returns the
 * status code on success. On failure returns one of the SMTP_ERR_*
 * constants (propagated from smtp_reader_read_line, or
 * SMTP_ERR_MALFORMED if a line fails to parse). If `text` is
 * non-NULL, *text receives a malloc'd copy of the full raw reply
 * (newline-joined, as the server sent it) for error messages --
 * useful on both success and failure; the caller frees it. *text is
 * left NULL if nothing was read.
 */
int smtp_read_reply(smtp_reader_t *reader, char **text);

/* Writes `len` bytes to `transport`, looping over short writes.
 * Returns 0 on success, SMTP_ERR_IO on failure. */
int smtp_write_all(smtp_transport_t *transport, const char *data, size_t len);

/* Builds `text` into a command line (via smtp_build_command_line) and
 * writes it to `transport`. Returns 0 on success, SMTP_ERR_IO on a
 * write failure, or SMTP_ERR_IO if smtp_build_command_line fails (OOM
 * -- there is no more specific code, and it's fatal either way). */
int smtp_send_line(smtp_transport_t *transport, const char *text);

/* Which stage of the session a failure occurred in. */
typedef enum
{
    SMTP_STAGE_GREETING = 0,
    SMTP_STAGE_HELO,
    SMTP_STAGE_MAIL_FROM,
    SMTP_STAGE_RCPT_TO,
    SMTP_STAGE_DATA,
    SMTP_STAGE_PAYLOAD,
    SMTP_STAGE_QUIT,
} smtp_stage_t;

typedef struct
{
    int ok;             /* 1 if every stage got its expected reply, 0 otherwise */
    smtp_stage_t stage; /* the stage that failed (meaningless when ok == 1) */
    int status;         /* the reply code received, or an SMTP_ERR_* on I/O failure */
    char *reply_text;   /* malloc'd raw reply text for the failing stage, or NULL;
                            caller must free when non-NULL */
} smtp_session_result_t;

/* Human-readable name for a stage, e.g. "MAIL FROM". For error messages. */
const char *smtp_stage_name(smtp_stage_t stage);

/*
 * Runs one complete SMTP session -- greeting, HELO, MAIL FROM, RCPT
 * TO, DATA, the message payload, and QUIT -- checking the expected
 * status code at every stage (220, 250, 250, 250, 354, 250, 221) and
 * following continuation lines. Every byte in and out goes through
 * `transport`'s callbacks, so this function never touches a socket
 * and can be driven entirely by a scripted in-memory transport.
 *
 * Stops at the first stage that doesn't get its expected reply,
 * except that QUIT is still attempted whenever DATA's payload was
 * sent (so the server sees a clean end to the session even after a
 * QUIT-reply mismatch is what gets reported).
 */
smtp_session_result_t smtp_run_session(smtp_transport_t *transport,
                                        const char *helo_host,
                                        const char *from, const char *to,
                                        const char *subject,
                                        const char *body);

/* =====================================================================
 * Layer 3 -- the socket transport. Thin wrappers over getaddrinfo,
 * connect, recv and send that satisfy the two callbacks above. Not
 * unit tested against a live mail server -- that's the whole point of
 * layer 2 -- but exercised against a local loopback socket.
 * ===================================================================== */

/*
 * Resolves `host`/`port` (port may be numeric or a service name) with
 * getaddrinfo and returns a connected TCP socket, or -1 on failure
 * (a message is already printed to stderr).
 */
int smtp_socket_connect(const char *host, const char *port);

/*
 * Wraps a connected socket as an smtp_transport_t. `fd` must outlive
 * the transport (it's referenced by pointer, not copied) and the
 * transport does not take ownership -- the caller still closes it.
 */
smtp_transport_t smtp_socket_transport(int *fd);

void usage(FILE *out, const char *prog);

#endif /* LAB_H */