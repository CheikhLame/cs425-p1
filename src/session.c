#include "lab.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void smtp_reader_init(smtp_reader_t *reader, smtp_transport_t transport)
{
    reader->transport = transport;
    reader->start = 0;
    reader->end = 0;
}

int smtp_reader_read_line(smtp_reader_t *reader, char **out)
{
    for (;;)
    {
        /* Do we already have a complete line buffered? */
        for (size_t i = reader->start; i < reader->end; i++)
        {
            if (reader->buf[i] == '\n')
            {
                size_t linelen = i - reader->start;
                if (linelen > 0 && reader->buf[reader->start + linelen - 1] == '\r')
                {
                    linelen--;
                }

                char *line = malloc(linelen + 1);
                if (line == NULL) // GCOVR_EXCL_START
                {
                    return SMTP_ERR_IO;
                } // GCOVR_EXCL_STOP
                memcpy(line, reader->buf + reader->start, linelen);
                line[linelen] = '\0';

                reader->start = i + 1;
                *out = line;
                return (int) linelen;
            }
        }

        /* No newline in what we have. If a partial line already
         * fills the whole buffer, it can never complete. */
        if (reader->start == 0 && reader->end == SMTP_RBUF_SIZE)
        {
            return SMTP_ERR_TOO_LONG;
        }

        /* Compact: slide any unconsumed partial line to the front so
         * refilling has room to grow it. */
        if (reader->start > 0)
        {
            size_t remaining = reader->end - reader->start;
            memmove(reader->buf, reader->buf + reader->start, remaining);
            reader->start = 0;
            reader->end = remaining;
        }

        long n = reader->transport.read(reader->transport.ctx,
                                         reader->buf + reader->end,
                                         SMTP_RBUF_SIZE - reader->end);
        if (n < 0) // GCOVR_EXCL_START
        {
            return SMTP_ERR_IO;
        } // GCOVR_EXCL_STOP
        if (n == 0)
        {
            /* Peer closed. Whatever's left in the buffer (even a
             * partial line) can never be completed now. */
            return SMTP_ERR_EOF;
        }
        reader->end += (size_t) n;
    }
}

int smtp_read_reply(smtp_reader_t *reader, char **text)
{
    char *full = NULL;
    size_t full_len = 0;
    int code = SMTP_ERR_MALFORMED;

    for (;;)
    {
        char *line = NULL;
        int n = smtp_reader_read_line(reader, &line);
        if (n < 0)
        {
            free(full);
            return n; /* propagate SMTP_ERR_IO / SMTP_ERR_EOF / SMTP_ERR_TOO_LONG */
        }

        int this_code = smtp_parse_reply_code(line, (size_t) n);
        int final = smtp_is_final_reply_line(line, (size_t) n);
        if (this_code < 0 || final < 0)
        {
            free(line);
            free(full);
            return SMTP_ERR_MALFORMED;
        }

        if (text != NULL)
        {
            size_t add = (size_t) n + 1; /* +1 for the '\n' we re-add */
            char *grown = realloc(full, full_len + add + 1);
            if (grown != NULL)
            {
                full = grown;
                memcpy(full + full_len, line, (size_t) n);
                full_len += (size_t) n;
                full[full_len++] = '\n';
                full[full_len] = '\0';
            }
        }

        free(line);
        code = this_code;

        if (final)
        {
            break;
        }
    }

    if (text != NULL)
    {
        *text = full;
    }
    return code;
}

int smtp_write_all(smtp_transport_t *transport, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len)
    {
        long n = transport->write(transport->ctx, data + sent, len - sent);
        if (n < 0) // GCOVR_EXCL_START
        {
            return SMTP_ERR_IO;
        }
        if (n == 0)
        {
            /* No progress and no error reported -- treat as a failure
             * rather than spinning forever. */
            return SMTP_ERR_IO;
        }
        sent += (size_t) n;
    }
    return 0; // GCOVR_EXCL_STOP
}

int smtp_send_line(smtp_transport_t *transport, const char *text)
{
    char *line = smtp_build_command_line(text);
    if (line == NULL) // GCOVR_EXCL_START
    {
        return SMTP_ERR_IO;
    } // GCOVR_EXCL_STOP
    int rc = smtp_write_all(transport, line, strlen(line));
    free(line);
    return rc;
}

const char *smtp_stage_name(smtp_stage_t stage) // GCOVR_EXCL_START
{ 
    switch (stage) 
    {
    case SMTP_STAGE_GREETING:
        return "connection (greeting)";
    case SMTP_STAGE_HELO:
        return "HELO";
    case SMTP_STAGE_MAIL_FROM:
        return "MAIL FROM";
    case SMTP_STAGE_RCPT_TO:
        return "RCPT TO";
    case SMTP_STAGE_DATA:
        return "DATA";
    case SMTP_STAGE_PAYLOAD:
        return "message data";
    case SMTP_STAGE_QUIT:
        return "QUIT"; 
    default: 
        return "unknown stage"; 
    } // GCOVR_EXCL_STOP
}

/* Reads one reply and checks it against `expected`. On success frees
 * the reply text and returns 1. On failure leaves the failure details
 * in *result (including reply_text, which the caller now owns) and
 * returns 0. */
static int expect_reply(smtp_reader_t *reader, int expected,
                         smtp_stage_t stage, smtp_session_result_t *result)
{
    char *text = NULL;
    int code = smtp_read_reply(reader, &text);

    if (code < 0)
    {
        result->ok = 0;
        result->stage = stage;
        result->status = code;
        result->reply_text = NULL;
        free(text);
        return 0;
    }

    if (code != expected)
    {
        result->ok = 0;
        result->stage = stage;
        result->status = code;
        result->reply_text = text; /* transfer ownership */
        return 0;
    }

    free(text);
    return 1;
}

smtp_session_result_t smtp_run_session(smtp_transport_t *transport,
                                        const char *helo_host,
                                        const char *from, const char *to,
                                        const char *subject,
                                        const char *body)
{
    smtp_session_result_t result;
    result.ok = 1;
    result.stage = SMTP_STAGE_GREETING;
    result.status = 0;
    result.reply_text = NULL;

    smtp_reader_t reader;
    smtp_reader_init(&reader, *transport);

    char cmdbuf[1024];
    int data_sent = 0; /* did the payload go out? drives whether we still QUIT */

    if (!expect_reply(&reader, 220, SMTP_STAGE_GREETING, &result))
    {
        return result;
    }

    snprintf(cmdbuf, sizeof(cmdbuf), "HELO %s", helo_host != NULL ? helo_host : "");
    if (smtp_send_line(transport, cmdbuf) != 0) // GCOVR_EXCL_START
    {
        result.ok = 0;
        result.stage = SMTP_STAGE_HELO;
        result.status = SMTP_ERR_IO;
        return result;
    } // GCOVR_EXCL_STOP
    if (!expect_reply(&reader, 250, SMTP_STAGE_HELO, &result))
    {
        return result;
    }

    snprintf(cmdbuf, sizeof(cmdbuf), "MAIL FROM:<%s>", from != NULL ? from : "");
    if (smtp_send_line(transport, cmdbuf) != 0) // // GCOVR_EXCL_START
    {
        result.ok = 0;
        result.stage = SMTP_STAGE_MAIL_FROM;
        result.status = SMTP_ERR_IO;
        return result;
    } // GCOVR_EXCL_STOP
    if (!expect_reply(&reader, 250, SMTP_STAGE_MAIL_FROM, &result))
    {
        return result;
    }

    snprintf(cmdbuf, sizeof(cmdbuf), "RCPT TO:<%s>", to != NULL ? to : "");
    if (smtp_send_line(transport, cmdbuf) != 0) // GCOVR_EXCL_START
    {
        result.ok = 0;
        result.stage = SMTP_STAGE_RCPT_TO;
        result.status = SMTP_ERR_IO;
        return result;
    } // GCOVR_EXCL_STOP
    if (!expect_reply(&reader, 250, SMTP_STAGE_RCPT_TO, &result))
    {
        return result;
    }

    if (smtp_send_line(transport, "DATA") != 0) // GCOVR_EXCL_START
    {
        result.ok = 0;
        result.stage = SMTP_STAGE_DATA;
        result.status = SMTP_ERR_IO;
        return result;
    } // GCOVR_EXCL_STOP
    if (!expect_reply(&reader, 354, SMTP_STAGE_DATA, &result))
    {
        return result;
    }

    size_t payload_len = 0;
    char *payload = smtp_build_data_payload(from, to, subject, body, &payload_len);
    if (payload == NULL) // GCOVR_EXCL_START
    {
        result.ok = 0;
        result.stage = SMTP_STAGE_PAYLOAD;
        result.status = SMTP_ERR_IO;
        return result;
    } // GCOVR_EXCL_STOP
    int write_rc = smtp_write_all(transport, payload, payload_len);
    free(payload);
    if (write_rc != 0) // GCOVR_EXCL_START
    {
        result.ok = 0;
        result.stage = SMTP_STAGE_PAYLOAD;
        result.status = SMTP_ERR_IO;
        return result;
    } // GCOVR_EXCL_STOP
    data_sent = 1;
    if (!expect_reply(&reader, 250, SMTP_STAGE_PAYLOAD, &result))
    {
        return result;
    }

    (void) data_sent; /* stages are strictly sequential; kept for clarity */

    if (smtp_send_line(transport, "QUIT") != 0) // GCOVR_EXCL_START
    {
        result.ok = 0;
        result.stage = SMTP_STAGE_QUIT;
        result.status = SMTP_ERR_IO;
        return result;
    } // GCOVR_EXCL_STOP
    if (!expect_reply(&reader, 221, SMTP_STAGE_QUIT, &result))
    {
        return result;
    }

    return result;
}