#include "harness/unity.h"
#include "../src/lab.h"

#include <string.h>
#include <stdlib.h>

/* =====================================================================
 * A scripted in-memory transport. `script` is exactly the bytes a
 * real server would have sent; reads hand it out `chunk_size` bytes
 * at a time (0 means "as much as fits/remains"), so the same script
 * can be replayed either as one big read or dribbled out a few bytes
 * at a time. Writes are captured into `sent` so a test can check
 * exactly what the client sent.
 * ===================================================================== */

typedef struct
{
    const char *script;
    size_t script_len;
    size_t pos;
    size_t chunk_size;
    char sent[8192];
    size_t sent_len;
} mock_t;

static void mock_init(mock_t *m, const char *script, size_t chunk_size)
{
    m->script = script;
    m->script_len = strlen(script);
    m->pos = 0;
    m->chunk_size = chunk_size;
    m->sent_len = 0;
    m->sent[0] = '\0';
}

static long mock_read(void *ctx, char *buf, size_t len)
{
    mock_t *m = (mock_t *) ctx;
    size_t remaining = m->script_len - m->pos;
    if (remaining == 0)
    {
        return 0; /* EOF */
    }
    size_t n = remaining;
    if (m->chunk_size != 0 && n > m->chunk_size)
    {
        n = m->chunk_size;
    }
    if (n > len)
    {
        n = len;
    }
    memcpy(buf, m->script + m->pos, n);
    m->pos += n;
    return (long) n;
}

static long mock_write(void *ctx, const char *buf, size_t len)
{
    mock_t *m = (mock_t *) ctx;
    if (m->sent_len + len < sizeof(m->sent))
    {
        memcpy(m->sent + m->sent_len, buf, len);
        m->sent_len += len;
        m->sent[m->sent_len] = '\0';
    }
    return (long) len;
}

static smtp_transport_t mock_transport(mock_t *m)
{
    smtp_transport_t t;
    t.read = mock_read;
    t.write = mock_write;
    t.ctx = m;
    return t;
}

void setUp(void) {}
void tearDown(void) {}

/* =====================================================================
 * Layer 1: pure protocol helpers
 * ===================================================================== */

static void test_parse_reply_code_valid(void)
{
    TEST_ASSERT_EQUAL_INT(250, smtp_parse_reply_code("250 Ok", 6));
    TEST_ASSERT_EQUAL_INT(250, smtp_parse_reply_code("250-more", 8));
    TEST_ASSERT_EQUAL_INT(550, smtp_parse_reply_code("550 no such user", 17));
}

static void test_parse_reply_code_malformed(void)
{
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_MALFORMED, smtp_parse_reply_code("25", 2));
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_MALFORMED, smtp_parse_reply_code("25x ok", 6));
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_MALFORMED, smtp_parse_reply_code("250xok", 6));
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_MALFORMED, smtp_parse_reply_code(NULL, 0));
}

static void test_is_final_reply_line(void)
{
    TEST_ASSERT_EQUAL_INT(1, smtp_is_final_reply_line("250 Ok", 6));
    TEST_ASSERT_EQUAL_INT(0, smtp_is_final_reply_line("250-more", 8));
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_MALFORMED, smtp_is_final_reply_line("25", 2));
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_MALFORMED, smtp_is_final_reply_line("25x more", 8));
}

static void test_build_command_line(void)
{
    char *line = smtp_build_command_line("HELO onyx.boisestate.edu");
    TEST_ASSERT_NOT_NULL(line);
    TEST_ASSERT_EQUAL_STRING("HELO onyx.boisestate.edu\r\n", line);
    free(line);
}

static void test_dot_stuff_body_basic(void)
{
    size_t len = 0;
    char *out = smtp_dot_stuff_body("hello\nworld", &len);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING("hello\r\nworld\r\n.\r\n", out);
    TEST_ASSERT_EQUAL_size_t(strlen(out), len);
    free(out);
}

static void test_dot_stuff_body_leading_dot(void)
{
    size_t len = 0;
    char *out = smtp_dot_stuff_body(".leading\nnormal\n.also leading", &len);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING("..leading\r\nnormal\r\n..also leading\r\n.\r\n", out);
    free(out);
}

static void test_dot_stuff_body_empty(void)
{
    size_t len = 0;
    char *out = smtp_dot_stuff_body("", &len);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING(".\r\n", out);
    free(out);
}

static void test_build_data_payload(void)
{
    size_t len = 0;
    char *out = smtp_build_data_payload("me@example.com", "you@example.com",
                                         "hi", "body line", &len);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING(
        "From: me@example.com\r\n"
        "To: you@example.com\r\n"
        "Subject: hi\r\n"
        "\r\n"
        "body line\r\n"
        ".\r\n",
        out);
    free(out);
}

/* =====================================================================
 * Layer 2: smtp_reader_read_line
 * ===================================================================== */

static void test_reader_single_line(void)
{
    mock_t m;
    mock_init(&m, "250 Ok\r\n", 0);
    smtp_reader_t r;
    smtp_reader_init(&r, mock_transport(&m));

    char *line = NULL;
    int n = smtp_reader_read_line(&r, &line);
    TEST_ASSERT_EQUAL_INT(6, n);
    TEST_ASSERT_EQUAL_STRING("250 Ok", line);
    free(line);
}

static void test_reader_multiple_lines_in_one_read(void)
{
    mock_t m;
    mock_init(&m, "250-first\r\n250 second\r\n", 0);
    smtp_reader_t r;
    smtp_reader_init(&r, mock_transport(&m));

    char *line = NULL;
    TEST_ASSERT_EQUAL_INT(9, smtp_reader_read_line(&r, &line));
    TEST_ASSERT_EQUAL_STRING("250-first", line);
    free(line);

    TEST_ASSERT_EQUAL_INT(10, smtp_reader_read_line(&r, &line));
    TEST_ASSERT_EQUAL_STRING("250 second", line);
    free(line);
}

static void test_reader_line_a_few_bytes_at_a_time(void)
{
    const char *expected = "250 chunked reply";
    mock_t m;
    mock_init(&m, "250 chunked reply\r\n", 3); /* 3 bytes per underlying read */
    smtp_reader_t r;
    smtp_reader_init(&r, mock_transport(&m));

    char *line = NULL;
    int n = smtp_reader_read_line(&r, &line);
    TEST_ASSERT_EQUAL_INT((int) strlen(expected), n);
    TEST_ASSERT_EQUAL_STRING(expected, line);
    free(line);
}

static void test_reader_line_too_long(void)
{
    /* one byte over SMTP_RBUF_SIZE, no newline in sight */
    char *huge = malloc(SMTP_RBUF_SIZE + 2);
    memset(huge, 'a', SMTP_RBUF_SIZE + 1);
    huge[SMTP_RBUF_SIZE + 1] = '\0';

    mock_t m;
    mock_init(&m, huge, 0);
    smtp_reader_t r;
    smtp_reader_init(&r, mock_transport(&m));

    char *line = NULL;
    int n = smtp_reader_read_line(&r, &line);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_TOO_LONG, n);

    free(huge);
}

static void test_reader_clean_eof_no_data(void)
{
    mock_t m;
    mock_init(&m, "", 0);
    smtp_reader_t r;
    smtp_reader_init(&r, mock_transport(&m));

    char *line = NULL;
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_EOF, smtp_reader_read_line(&r, &line));
}

static void test_reader_eof_mid_line(void)
{
    /* server hangs up after sending a partial line, no trailing \n */
    mock_t m;
    mock_init(&m, "250 partial", 0);
    smtp_reader_t r;
    smtp_reader_init(&r, mock_transport(&m));

    char *line = NULL;
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_EOF, smtp_reader_read_line(&r, &line));
}

/* =====================================================================
 * Layer 2: smtp_read_reply
 * ===================================================================== */

static void test_read_reply_multiline(void)
{
    mock_t m;
    mock_init(&m, "250-smtp.example.com\r\n250-PIPELINING\r\n250 SIZE 10240000\r\n", 0);
    smtp_reader_t r;
    smtp_reader_init(&r, mock_transport(&m));

    char *text = NULL;
    int code = smtp_read_reply(&r, &text);
    TEST_ASSERT_EQUAL_INT(250, code);
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_EQUAL_STRING(
        "250-smtp.example.com\n250-PIPELINING\n250 SIZE 10240000\n", text);
    free(text);
}

static void test_read_reply_malformed_line(void)
{
    mock_t m;
    mock_init(&m, "not a reply\r\n", 0);
    smtp_reader_t r;
    smtp_reader_init(&r, mock_transport(&m));

    char *text = NULL;
    int code = smtp_read_reply(&r, &text);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_MALFORMED, code);
}

/* =====================================================================
 * Layer 2: smtp_run_session -- happy path
 * ===================================================================== */

static void test_run_session_happy_path(void)
{
    mock_t m;
    mock_init(&m,
              "220 test.local ready\r\n"
              "250 Hi\r\n"
              "250 2.1.0 Ok\r\n"
              "250 2.1.5 Ok\r\n"
              "354 End data with .\r\n"
              "250 2.0.0 Ok: queued\r\n"
              "221 Bye\r\n",
              0);
    smtp_transport_t t = mock_transport(&m);

    smtp_session_result_t result =
        smtp_run_session(&t, "onyx.boisestate.edu", "me@boisestate.edu",
                          "you@example.com", "hi", "hello\n.dot line");

    TEST_ASSERT_EQUAL_INT(1, result.ok);
    TEST_ASSERT_NULL(result.reply_text);

    TEST_ASSERT_TRUE(strstr(m.sent, "HELO onyx.boisestate.edu\r\n") != NULL);
    TEST_ASSERT_TRUE(strstr(m.sent, "MAIL FROM:<me@boisestate.edu>\r\n") != NULL);
    TEST_ASSERT_TRUE(strstr(m.sent, "RCPT TO:<you@example.com>\r\n") != NULL);
    TEST_ASSERT_TRUE(strstr(m.sent, "DATA\r\n") != NULL);
    TEST_ASSERT_TRUE(strstr(m.sent, "..dot line\r\n") != NULL);
    TEST_ASSERT_TRUE(strstr(m.sent, "QUIT\r\n") != NULL);

    free(result.reply_text);
}

/* =====================================================================
 * Layer 2: smtp_run_session -- every wrong status code in the sequence
 * ===================================================================== */

/* Builds a server transcript with a correct reply for every stage up
 * to `bad_stage`, where it substitutes `bad_line`. Stages after the
 * bad one are omitted -- the session should never get that far. */
static char *build_bad_transcript(smtp_stage_t bad_stage, const char *bad_line)
{
    static const char *good[] = {
        "220 test.local ready\r\n", /* GREETING */
        "250 Hi\r\n",               /* HELO */
        "250 2.1.0 Ok\r\n",         /* MAIL FROM */
        "250 2.1.5 Ok\r\n",         /* RCPT TO */
        "354 End data with .\r\n",  /* DATA */
        "250 2.0.0 Ok: queued\r\n", /* PAYLOAD */
        "221 Bye\r\n",              /* QUIT */
    };

    size_t cap = 512;
    char *buf = malloc(cap);
    buf[0] = '\0';
    size_t len = 0;

    for (int i = 0; i <= (int) bad_stage; i++)
    {
        const char *line = (i == (int) bad_stage) ? bad_line : good[i];
        size_t linelen = strlen(line);
        while (len + linelen + 1 > cap)
        {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        memcpy(buf + len, line, linelen);
        len += linelen;
        buf[len] = '\0';
    }
    return buf;
}

static void test_run_session_wrong_status_each_stage(void)
{
    struct
    {
        smtp_stage_t stage;
        const char *bad_line;
        int expected_status;
    } cases[] = {
        {SMTP_STAGE_GREETING, "421 service not available\r\n", 421},
        {SMTP_STAGE_HELO, "500 syntax error\r\n", 500},
        {SMTP_STAGE_MAIL_FROM, "550 mailbox unavailable\r\n", 550},
        {SMTP_STAGE_RCPT_TO, "550 no such user\r\n", 550},
        {SMTP_STAGE_DATA, "503 bad sequence\r\n", 503},
        {SMTP_STAGE_PAYLOAD, "552 storage exceeded\r\n", 552},
        {SMTP_STAGE_QUIT, "500 huh?\r\n", 500},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        char *script = build_bad_transcript(cases[i].stage, cases[i].bad_line);
        mock_t m;
        mock_init(&m, script, 0);
        smtp_transport_t t = mock_transport(&m);

        smtp_session_result_t result = smtp_run_session(
            &t, "helo.local", "me@example.com", "you@example.com", "subj",
            "body");

        TEST_ASSERT_EQUAL_INT(0, result.ok);
        TEST_ASSERT_EQUAL_INT(cases[i].stage, result.stage);
        TEST_ASSERT_EQUAL_INT(cases[i].expected_status, result.status);

        free(result.reply_text);
        free(script);
    }
}

/* =====================================================================
 * Layer 2: smtp_run_session -- server hangs up mid-session
 * ===================================================================== */

static void test_run_session_hangup_mid_session(void)
{
    /* Server accepts greeting, HELO, MAIL FROM, then closes the
     * connection before replying to RCPT TO. */
    mock_t m;
    mock_init(&m,
              "220 test.local ready\r\n"
              "250 Hi\r\n"
              "250 2.1.0 Ok\r\n",
              0);
    smtp_transport_t t = mock_transport(&m);

    smtp_session_result_t result = smtp_run_session(
        &t, "helo.local", "me@example.com", "you@example.com", "subj", "body");

    TEST_ASSERT_EQUAL_INT(0, result.ok);
    TEST_ASSERT_EQUAL_INT(SMTP_STAGE_RCPT_TO, result.stage);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_EOF, result.status);
    TEST_ASSERT_NULL(result.reply_text);
}

/* =====================================================================
 * Layer 2: smtp_run_session -- reply the buffer cannot hold
 * ===================================================================== */

static void test_run_session_reply_too_long(void)
{
    char oversized[SMTP_RBUF_SIZE + 32];
    memcpy(oversized, "250-", 4);
    memset(oversized + 4, 'x', SMTP_RBUF_SIZE);
    oversized[4 + SMTP_RBUF_SIZE] = '\0'; /* deliberately never terminated with \r\n */

    char *script = malloc(strlen("220 test.local ready\r\n250 Hi\r\n250 2.1.0 Ok\r\n") +
                           strlen(oversized) + 1);
    strcpy(script, "220 test.local ready\r\n250 Hi\r\n250 2.1.0 Ok\r\n");
    strcat(script, oversized);

    mock_t m;
    mock_init(&m, script, 0);
    smtp_transport_t t = mock_transport(&m);

    smtp_session_result_t result = smtp_run_session(
        &t, "helo.local", "me@example.com", "you@example.com", "subj", "body");

    TEST_ASSERT_EQUAL_INT(0, result.ok);
    TEST_ASSERT_EQUAL_INT(SMTP_STAGE_RCPT_TO, result.stage);
    TEST_ASSERT_EQUAL_INT(SMTP_ERR_TOO_LONG, result.status);

    free(script);
}

/* =====================================================================
 * Layer 3: socket transport, over a local loopback socket (not a live
 * mail server -- just proof the read/write callbacks actually move
 * bytes over a real fd).
 * ===================================================================== */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static void test_socket_transport_loopback(void)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT_TRUE(listen_fd >= 0);

    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* ephemeral */

    TEST_ASSERT_EQUAL_INT(0, bind(listen_fd, (struct sockaddr *) &addr, sizeof(addr)));
    socklen_t addrlen = sizeof(addr);
    TEST_ASSERT_EQUAL_INT(0, getsockname(listen_fd, (struct sockaddr *) &addr, &addrlen));
    TEST_ASSERT_EQUAL_INT(0, listen(listen_fd, 1));

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", ntohs(addr.sin_port));

    pid_t pid = fork();
    TEST_ASSERT_TRUE(pid >= 0);

    if (pid == 0)
    {
        /* child: accept once, echo "220 loopback ready\r\n", read one
         * line, then exit */
        int conn = accept(listen_fd, NULL, NULL);
        if (conn >= 0)
        {
            const char *greeting = "220 loopback ready\r\n";
            send(conn, greeting, strlen(greeting), 0);
            char buf[64];
            recv(conn, buf, sizeof(buf), 0);
            close(conn);
        }
        close(listen_fd);
        _exit(0);
    }

    close(listen_fd);
    int fd = smtp_socket_connect("127.0.0.1", port_str);
    TEST_ASSERT_TRUE(fd >= 0);

    smtp_transport_t t = smtp_socket_transport(&fd);
    smtp_reader_t r;
    smtp_reader_init(&r, t);

    const char *expected = "220 loopback ready";
    char *line = NULL;
    int n = smtp_reader_read_line(&r, &line);
    TEST_ASSERT_EQUAL_INT((int) strlen(expected), n);
    TEST_ASSERT_EQUAL_STRING(expected, line);
    free(line);

    int wrc = smtp_write_all(&t, "QUIT\r\n", 6);
    TEST_ASSERT_EQUAL_INT(0, wrc);

    close(fd);
    int status = 0;
    waitpid(pid, &status, 0);
}

/* =====================================================================
 * runner
 * ===================================================================== */

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_parse_reply_code_valid);
    RUN_TEST(test_parse_reply_code_malformed);
    RUN_TEST(test_is_final_reply_line);
    RUN_TEST(test_build_command_line);
    RUN_TEST(test_dot_stuff_body_basic);
    RUN_TEST(test_dot_stuff_body_leading_dot);
    RUN_TEST(test_dot_stuff_body_empty);
    RUN_TEST(test_build_data_payload);

    RUN_TEST(test_reader_single_line);
    RUN_TEST(test_reader_multiple_lines_in_one_read);
    RUN_TEST(test_reader_line_a_few_bytes_at_a_time);
    RUN_TEST(test_reader_line_too_long);
    RUN_TEST(test_reader_clean_eof_no_data);
    RUN_TEST(test_reader_eof_mid_line);

    RUN_TEST(test_read_reply_multiline);
    RUN_TEST(test_read_reply_malformed_line);

    RUN_TEST(test_run_session_happy_path);
    RUN_TEST(test_run_session_wrong_status_each_stage);
    RUN_TEST(test_run_session_hangup_mid_session);
    RUN_TEST(test_run_session_reply_too_long);

    RUN_TEST(test_socket_transport_loopback);

    return UNITY_END();
}
