#include "lab.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef TEST
#define main main_exclude
#endif

void usage(FILE *out, const char *prog)
{
    fprintf(out,
            "Usage: %s -f <from> -t <to> [-s subject] [-b body] [-p port]\n"
            "          [-H helo-host] <server>\n"
            "\n"
            "  -f <from>       envelope sender, for example you@example.com\n"
            "  -t <to>         envelope recipient\n"
            "  -s <subject>    subject line (default: empty)\n"
            "  -b <body>       message body (default: read from stdin)\n"
            "  -p <port>       port or service name (default: 25)\n"
            "  -H <helo-host>  host name sent with HELO (default: localhost)\n"
            "  <server>        host name or address of the mail server\n",
            prog);
}

static char *read_stdin_body(void)
{
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (buf == NULL)
    {
        return NULL;
    }

    size_t n;
    while ((n = fread(buf + len, 1, cap - len, stdin)) > 0)
    {
        len += n;
        if (len == cap)
        {
            cap *= 2;
            char *grown = realloc(buf, cap);
            if (grown == NULL)
            {
                free(buf);
                return NULL;
            }
            buf = grown;
        }
    }
    buf[len] = '\0';
    return buf;
}

static void report_failure(const smtp_session_result_t *result)
{
    if (result->status == SMTP_ERR_IO)
    {
        fprintf(stderr, "myapp: connection failed during %s\n",
                smtp_stage_name(result->stage));
    }
    else if (result->status == SMTP_ERR_EOF)
    {
        fprintf(stderr, "myapp: server closed the connection during %s\n",
                smtp_stage_name(result->stage));
    }
    else if (result->status == SMTP_ERR_TOO_LONG)
    {
        fprintf(stderr, "myapp: server's reply to %s was too long\n",
                smtp_stage_name(result->stage));
    }
    else if (result->status == SMTP_ERR_MALFORMED)
    {
        fprintf(stderr, "myapp: server's reply to %s was malformed\n",
                smtp_stage_name(result->stage));
    }
    else
    {
        fprintf(stderr, "myapp: server rejected %s:\n%s",
                smtp_stage_name(result->stage),
                result->reply_text != NULL ? result->reply_text
                                            : "(no reply)\n");
    }
}

int main(int argc, char *argv[])
{
    if (argc == 1)
    {
        usage(stdout, argv[0]);
        return 0;
    }

    const char *from = NULL;
    const char *to = NULL;
    const char *subject = "";
    const char *body_arg = NULL;
    const char *port = "25";
    const char *helo_host = "localhost";

    int opt;
    while ((opt = getopt(argc, argv, "f:t:s:b:p:H:h")) != -1)
    {
        switch (opt)
        {
        case 'f':
            from = optarg;
            break;
        case 't':
            to = optarg;
            break;
        case 's':
            subject = optarg;
            break;
        case 'b':
            body_arg = optarg;
            break;
        case 'p':
            port = optarg;
            break;
        case 'H':
            helo_host = optarg;
            break;
        case 'h':
            usage(stdout, argv[0]);
            return 0;
        default:
            usage(stderr, argv[0]);
            return EXIT_USAGE;
        }
    }

    if (optind >= argc)
    {
        fprintf(stderr, "myapp: missing <server>\n");
        usage(stderr, argv[0]);
        return EXIT_USAGE;
    }
    const char *server = argv[optind];

    if (from == NULL || to == NULL)
    {
        fprintf(stderr, "myapp: -f and -t are required\n");
        usage(stderr, argv[0]);
        return EXIT_USAGE;
    }

    char *body_owned = NULL;
    const char *body = body_arg;
    if (body == NULL)
    {
        body_owned = read_stdin_body();
        if (body_owned == NULL)
        {
            fprintf(stderr, "myapp: failed reading message body from stdin\n");
            return EXIT_SMTP_FAIL;
        }
        body = body_owned;
    }

    int fd = smtp_socket_connect(server, port);
    if (fd == -1)
    {
        free(body_owned);
        return EXIT_SMTP_FAIL;
    }

    smtp_transport_t transport = smtp_socket_transport(&fd);
    smtp_session_result_t result =
        smtp_run_session(&transport, helo_host, from, to, subject, body);

    close(fd);
    free(body_owned);

    if (!result.ok)
    {
        report_failure(&result);
        free(result.reply_text);
        return EXIT_SMTP_FAIL;
    }

    return 0;
}