#include "http.h"

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <time.h>

static const char* const error_msgs[] =
{
    "400 Bad Request",
    "404 Not Found",
    "405 Not Allowed",
    "403 Forbidden",
    "406 Not Acceptable",
    "413 Payload Too Large",
    "500 Internal Server Error",
    "408 Request Time-out",
};

static const char* err_page_fmt = "<!DOCTYPE html>"
                                  "<html><head><title>%s</title></head>"
                                  "<body><h1>%s</h1></body></html>";

static const char* header_fmt = "HTTP/1.1 %s\r\n"
                                "Server: HTTP toaster\r\n"
                                "X-Powered-By: Electricity\r\n"
                                "Accept-Encoding: gzip, deflate\r\n"
                                "Connection: keep-alive\r\n";

static const char* header_content = "Content-Type: %s\r\n"
                                    "Content-Length: %lu\r\n";

static const char* http_date_fmt = "%a, %d %b %Y %H:%M:%S %Z";

static const struct { const char* field; int length; } hdrfields[] =
{
    { "Host: ",               6 },
    { "Content-Length: ",    16 },
    { "Content-Type: ",      14 },
    { "Cookie: ",             8 },
    { "If-Modified-Since: ", 19 },
    { "Accept-Encoding: ",   17 },
    { "Content-Encoding: ",  18 },
};

static const struct { const char* str; int length; } methods[] =
{
    { "GET ",    4 },
    { "HEAD ",   5 },
    { "POST ",   5 },
    { "PUT ",    4 },
    { "DELETE ", 7 },
};

static int hextoi( int c )
{
    return isdigit(c) ? (c-'0') : (isupper(c) ? (c-'A'+10) : (c-'a'+10));
}

static int check_path( char* path )
{
    unsigned int len;
    char* ptr;

    while( *path && (ptr = strchrnul(path, '/')) )
    {
        len = ptr - path;
        if( path[0]=='.' && (len==1 || (len==2 && path[1]=='.')) )
            return 0;
        path = *ptr ? ptr+1 : ptr;
    }

    return 1;
}

/****************************************************************************/

size_t gen_error_page( int fd, int errorid )
{
    const char* error = error_msgs[ errorid-1 ];
    size_t count = strlen(err_page_fmt) - 4 + strlen(error)*2;

    count = dprintf(fd, header_fmt, error);
    count += dprintf(fd, header_content, "text/html", (unsigned long)count);
    count += dprintf( fd, "\r\n" );
    count += dprintf( fd, err_page_fmt, error, error );
    return count;
}

size_t http_ok( int fd, const http_file_info* info,
                const char* setcookies )
{
    char buffer[ 128 ];
    struct tm stm;
    size_t count;
    time_t t;

    if( info->flags & FLAG_UNCHANGED )
    {
        count = dprintf(fd, header_fmt, "304 Not Modified");
    }
    else
    {
        count = dprintf(fd, header_fmt, "200 Ok");
        count += dprintf(fd, header_content, info->type, info->size);

        if( info->encoding )
            count += dprintf(fd, "Content-Encoding: %s\r\n", info->encoding);
    }

    if( setcookies )
        count += dprintf(fd, "Set-Cookie: %s\r\n", setcookies);

    if( info->last_mod )
    {
        t = info->last_mod;
        localtime_r( &t, &stm );
        strftime(buffer, sizeof(buffer), http_date_fmt, &stm);
        count += dprintf(fd, "Last-Modified: %s\r\n", buffer);
    }

    if( info->flags & FLAG_STATIC )
        count += dprintf(fd, "Cache-Control: max-age=3600\r\n");
    else if( info->flags & FLAG_DYNAMIC )
        count += dprintf(fd, "Cache-Control: no-store, must-revalidate\r\n");

    count += dprintf(fd, "\r\n");
    return count;
}

int http_request_parse( char* buffer, http_request* rq )
{
    char c, *out = buffer;
    struct tm stm;
    size_t j;

    memset( rq, 0, sizeof(*rq) );

    /* parse method */
    for( rq->method=-1, j=0; j<sizeof(methods)/sizeof(methods[0]); ++j )
    {
        if( !strncmp(buffer, methods[j].str, methods[j].length) )
        {
            rq->method = j;
            buffer += methods[j].length;
            break;
        }
    }

    if( rq->method < 0 )
        return 0;

    /* isolate path */
    while( *buffer=='/' || *buffer=='\\' ) { ++buffer; }
    rq->path = out;

    while( !isspace(*buffer) && *buffer )
    {
        c = *(buffer++);
        if( c=='\\' )
            c = '/';
        while( c=='/' && (*buffer=='/' || *buffer=='\\') )
            ++buffer;
        if( c=='/' && (!(*buffer) || isspace(*buffer)) )
            break;

        if( c=='%' && isxdigit(buffer[0]) && isxdigit(buffer[1]) )
        {
            c = (hextoi(buffer[0])<<4) | hextoi(buffer[1]);
            buffer += 2;
        }
        else if( (c=='?' && !rq->numargs) || (c=='&' && rq->numargs) )
        {
            if( c=='?' )
                rq->getargs = out + 1;
            c = '\0';
            ++rq->numargs;
        }
        *(out++) = c;
    }

    *(out++) = '\0';
    if( !check_path( rq->path ) )
        return 0;

    /* parse fields */
    while( (buffer = strchr( buffer, '\n' )) )
    {
        ++buffer;

        for( j=0; j<sizeof(hdrfields)/sizeof(hdrfields[0]); ++j )
        {
            if( !strncmp( buffer, hdrfields[j].field, hdrfields[j].length ) )
            {
                buffer += hdrfields[j].length;
                break;
            }
        }

        switch( j )
        {
        case FIELD_HOST:
            rq->host = out;
            while( isgraph(*buffer)&&*buffer!=':' ) { *(out++)=*(buffer++); }
            *(out++) = '\0';
            break;
        case FIELD_LENGTH:
            rq->length = strtol( buffer, NULL, 10 );
            break;
        case FIELD_TYPE:
            rq->type = out;
            while( !isspace(*buffer) && *buffer ) { *(out++)=*(buffer++); }
            *(out++) = '\0';
            break;
        case FIELD_COOKIE:
            rq->cookies = out;
            rq->numcookies = 1;
            while( *buffer && *buffer!='\n' && *buffer!='\r' )
            {
                c = *(buffer++);
                if( !isspace(c) )
                    continue;
                if( c==';' )
                {
                    c = '\0';
                    ++rq->numcookies;
                }
                *(out++) = c;
            }
            *(out++) = '\0';
            break;
        case FIELD_IFMOD:
            memset( &stm, 0, sizeof(stm) );
            strptime(buffer, http_date_fmt, &stm);
            rq->ifmod = mktime(&stm);
            break;
        case FIELD_ACCEPT:
            rq->accept = 0;
            while( *buffer && *buffer!='\n' && *buffer!='\r' )
            {
                if( isspace(*buffer) || *buffer==',' )
                {
                    ++buffer;
                    continue;
                }
                if( !strncmp( buffer, "gzip", 4 ) && !isalnum(buffer[4]) )
                {
                    rq->accept |= ENC_GZIP;
                    buffer += 4;
                    continue;
                }
                if( !strncmp( buffer, "deflate", 7 ) && !isalnum(buffer[7]) )
                {
                    rq->accept |= ENC_DEFLATE;
                    buffer += 7;
                    continue;
                }
                while( !isspace(*buffer) )
                    ++buffer;
            }
            break;
        case FIELD_ENCODING:
            if( !strncmp( buffer, "gzip", 4 ) && !isalnum(buffer[4]) )
                rq->encoding = ENC_GZIP;
            else if( !strncmp( buffer, "deflate", 7 ) && !isalnum(buffer[7]) )
                rq->encoding = ENC_DEFLATE;
            else
                rq->encoding = -1;
            break;
        }
    }
    return 1;
}

const char* http_get_arg( const char* argstr, int args, const char* arg )
{
    const char* ptr = argstr;
    int i, len = strlen(arg);

    if( !ptr )
        return NULL;

    for( i=0; i<args; ++i )
    {
        if( !strncmp( ptr, arg, len ) && ptr[len]=='=' )
            return ptr + len + 1;

        ptr += strlen(ptr) + 1;
    }

    return NULL;
}

int http_split_args( char* argstr )
{
    int count;

    for( count=1; *argstr; ++argstr )
    {
        if( *argstr=='+' )
        {
            *argstr = ' ';
        }
        else if( *argstr=='%' && isxdigit(argstr[1]) && isxdigit(argstr[2]) )
        {
            *argstr = (hextoi(argstr[1])<<4) | hextoi(argstr[2]);
            memmove( argstr+1, argstr+3, strlen(argstr+3)+1 );
        }
        else if( *argstr=='&' )
        {
            *argstr = '\0';
            ++count;
        }
    }

    return count;
}

