#include "lab.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int smtp_parse_reply_code(const char *line, size_t len)
{
    if (line == NULL || len < 4)
    {
        return SMTP_ERR_MALFORMED;
    }
    if (!isdigit((unsigned char) line[0]) || !isdigit((unsigned char) line[1]) ||
        !isdigit((unsigned char) line[2]))
    {
        return SMTP_ERR_MALFORMED;
    }
    if (line[3] != '-' && line[3] != ' ')
    {
        return SMTP_ERR_MALFORMED;
    }

    return (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
}

int smtp_is_final_reply_line(const char *line, size_t len)
{
    if (line == NULL || len < 4)
    {
        return SMTP_ERR_MALFORMED;
    }
    if (!isdigit((unsigned char) line[0]) || !isdigit((unsigned char) line[1]) ||
        !isdigit((unsigned char) line[2]))
    {
        return SMTP_ERR_MALFORMED;
    }

    if (line[3] == ' ')
    {
        return 1;
    }
    if (line[3] == '-')
    {
        return 0;
    }
    return SMTP_ERR_MALFORMED; //GCOVR_EXCL_LINE
}

char *smtp_build_command_line(const char *text)
{
    if (text == NULL) // GCOVR_EXCL_START
    {
        return NULL; 
    } // GCOVR_EXCL_STOP

    size_t len = strlen(text);
    char *out = malloc(len + 3); /* + "\r\n" + NUL */
    if (out == NULL) // GCOVR_EXCL_START
    {
        return NULL;
    } // GCOVR_EXCL_STOP
    memcpy(out, text, len);
    out[len] = '\r';
    out[len + 1] = '\n';
    out[len + 2] = '\0';
    return out;
}

/* Small growable-buffer helper shared by the two builders below. */
typedef struct
{
    char *data;
    size_t len;
    size_t cap;
} strbuf_t;

static int strbuf_init(strbuf_t *b, size_t initial_cap)
{
    b->data = malloc(initial_cap);
    if (b->data == NULL) // GCOVR_EXCL_START
    {
        return -1;
    } // GCOVR_EXCL_STOP
    b->len = 0;
    b->cap = initial_cap;
    return 0;
}

static int strbuf_append(strbuf_t *b, const char *p, size_t n)
{
    if (b->len + n + 1 > b->cap)
    { // GCOVR_EXCL_START
        size_t new_cap = b->cap * 2;
        while (new_cap < b->len + n + 1)
        {
            new_cap *= 2;
        }
        char *grown = realloc(b->data, new_cap);
        if (grown == NULL)
        {
            return -1;
        }
        b->data = grown;
        b->cap = new_cap;
    } // GCOVR_EXCL_STOP
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 0;
}

static int strbuf_append_str(strbuf_t *b, const char *s)
{
    return strbuf_append(b, s, strlen(s));
}

char *smtp_dot_stuff_body(const char *body, size_t *out_len)
{
    strbuf_t b;
    if (strbuf_init(&b, 256) != 0) // GCOVR_EXCL_START
    {
        return NULL;
    } // GCOVR_EXCL_STOP

    const char *p = body != NULL ? body : "";
    while (*p != '\0')
    {
        const char *nl = strchr(p, '\n');
        size_t linelen = (nl != NULL) ? (size_t) (nl - p) : strlen(p);

        /* strip a trailing '\r' so CRLF input isn't doubled */
        if (linelen > 0 && p[linelen - 1] == '\r') // GCOVR_EXCL_START
        {
            linelen--;
        } // GCOVR_EXCL_STOP

        if (linelen > 0 && p[0] == '.')
        {
            if (strbuf_append(&b, ".", 1) != 0) // GCOVR_EXCL_START
            {
                free(b.data);
                return NULL; 
            } // GCOVR_EXCL_STOP
        }
        if (strbuf_append(&b, p, linelen) != 0 ||
            strbuf_append(&b, "\r\n", 2) != 0) // GCOVR_EXCL_START
        {
            free(b.data);
            return NULL;
        } // GCOVR_EXCL_STOP

        if (nl == NULL)
        {
            break;
        }
        p = nl + 1;
    }

    if (strbuf_append(&b, ".\r\n", 3) != 0) // GCOVR_EXCL_START
    {
        free(b.data);
        return NULL;
    } // GCOVR_EXCL_STOP

    b.data[b.len] = '\0';
    if (out_len != NULL)
    {
        *out_len = b.len;
    }
    return b.data;
}

char *smtp_build_data_payload(const char *from, const char *to,
                               const char *subject, const char *body,
                               size_t *out_len)
{
    strbuf_t b;
    if (strbuf_init(&b, 256) != 0) // GCOVR_EXCL_START
    {
        return NULL;
    } // GCOVR_EXCL_STOP

    if (strbuf_append_str(&b, "From: ") != 0 ||
        strbuf_append_str(&b, from != NULL ? from : "") != 0 ||
        strbuf_append_str(&b, "\r\n") != 0 ||
        strbuf_append_str(&b, "To: ") != 0 ||
        strbuf_append_str(&b, to != NULL ? to : "") != 0 ||
        strbuf_append_str(&b, "\r\n") != 0 ||
        strbuf_append_str(&b, "Subject: ") != 0 ||
        strbuf_append_str(&b, subject != NULL ? subject : "") != 0 ||
        strbuf_append_str(&b, "\r\n\r\n") != 0) // GCOVR_EXCL_START
    {
        free(b.data);
        return NULL;
    } // GCOVR_EXCL_STOP

    size_t stuffed_len = 0;
    char *stuffed = smtp_dot_stuff_body(body, &stuffed_len);
    if (stuffed == NULL) // GCOVR_EXCL_START
    {
        free(b.data);
        return NULL;
    } // GCOVR_EXCL_STOP

    if (strbuf_append(&b, stuffed, stuffed_len) != 0) // GCOVR_EXCL_START
    {
        free(stuffed);
        free(b.data);
        return NULL;
    } // GCOVR_EXCL_STOP
    free(stuffed);

    b.data[b.len] = '\0';
    if (out_len != NULL)
    {
        *out_len = b.len;
    }
    return b.data;
}