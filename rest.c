#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include "rest.h"
#include "html.h"



static int echo_demo( int fd, const http_request* req );
static int form_get( int fd, const http_request* req );
static int form_post( int fd, const http_request* req );
static int cookie_get( int fd, const http_request* req );



static const struct
{
    int method;             /* method to map to, negative value for all */
    const char* path;       /* sub-path of request to map to */
    const char* host;       /* if set, only allow for this requested host */
    const char* accept;     /* content type that is accepted */

    int(* callback )( int fd, const http_request* req );
}
restmap[] =
{
    {-1,       "echo",  NULL,NULL,                               echo_demo },
    {HTTP_GET, "form",  NULL,NULL,                               form_get  },
    {HTTP_POST,"form",  NULL,"application/x-www-form-urlencoded",form_post },
    {HTTP_GET, "cookie",NULL,NULL,                               cookie_get},
};



int rest_handle_request( int fd, const http_request* req )
{
    const char* ptr;
    int error;
    size_t i;

    /* path must be absolute */
    error = ERR_FORBIDDEN;

    for( ptr=req->path; ptr; )
    {
        if( ptr[0]=='.' && ptr[1]=='.' && (ptr[2]=='/' || !ptr[2]) )
            goto fail;
        if( ptr[0]=='.' && (ptr[1]=='/' || !ptr[1]) )
            goto fail;
        ptr = strchr( ptr, '/' );
        ptr = ptr ? ptr + 1 : NULL;
    }

    /* find rest callback */
    error = ERR_NOT_FOUND;

    for( i=0; i<sizeof(restmap)/sizeof(restmap[0]); ++i )
    {
        if( restmap[i].host && strcmp(req->host, restmap[i].host) )
            continue;

        if( strcmp(req->path, restmap[i].path) )
            continue;

        error = ERR_METHOD;
        if( restmap[i].method>=0 && req->method != restmap[i].method )
            continue;

        error = ERR_TYPE;
        if( restmap[i].accept && strcmp(req->type, restmap[i].accept) )
            continue;

        error = restmap[i].callback( fd, req );

        if( !error )
            return 1;
    }
fail:
    gen_error_page( fd, error );
    return 0;
}

/****************************************************************************/

static int echo_demo( int fd, const http_request* req )
{
    const char* method = "-unknown-";
    html_page page;

    switch( req->method )
    {
    case HTTP_GET:    method = "GET"; break;
    case HTTP_HEAD:   method = "HEAD"; break;
    case HTTP_POST:   method = "POST"; break;
    case HTTP_PUT:    method = "PUT"; break;
    case HTTP_DELETE: method = "DELETE"; break;
    }

    html_page_init( &page, HTML_4 );
    html_page_begin( &page, "echo", NULL );
    html_append_raw( &page, "<h1>REST API - Echo demo</h1>" );
    html_table_begin( &page, "border: 1px solid black;", STYLE_INLINE );
    html_table_row( &page, 2, "Method", method );
    html_table_row( &page, 2, "Path", req->path );
    html_table_row( &page, 2, "Host", req->host );
    html_table_end( &page );
    html_page_end( &page );

    http_ok( fd, "text/html", page.used, NULL );
    write( fd, page.data, page.used );
    html_page_cleanup( &page );
    return 0;
}

static int form_get( int fd, const http_request* req )
{
    const char *first, *second;
    html_page page;

    first = http_get_arg( req->getargs, req->numargs, "str1" );
    second = http_get_arg( req->getargs, req->numargs, "str2" );

    html_page_init( &page, HTML_4 );
    html_page_begin( &page, "form", NULL );
    html_append_raw( &page, "<h1>GET arguments</h1>" );
    html_table_begin( &page, "border: 1px solid black;", STYLE_INLINE );
    html_table_row( &page, 2, "First Argument", first );
    html_table_row( &page, 2, "Second Argument", second );
    html_table_end( &page );
    html_page_end( &page );
    http_ok( fd, "text/html", page.used, NULL );
    write( fd, page.data, page.used );
    html_page_cleanup( &page );
    return 0;
}

static int form_post( int fd, const http_request* req )
{
    const char *first, *second;
    char buffer[128];
    html_page page;
    int count;

    if( req->length > (sizeof(buffer)-1) )
        return ERR_SIZE;

    read( fd, buffer, req->length );
    buffer[ req->length ] = '\0';

    count = http_split_args( buffer );
    first = http_get_arg( buffer, count, "str1" );
    second = http_get_arg( buffer, count, "str2" );

    html_page_init( &page, HTML_4 );
    html_page_begin( &page, "form", NULL );
    html_append_raw( &page, "<h1>POST arguments</h1>" );
    html_table_begin( &page, "border: 1px solid black;", STYLE_INLINE );
    html_table_row( &page, 2, "First Argument", first );
    html_table_row( &page, 2, "Second Argument", second );
    html_table_end( &page );
    html_page_end( &page );

    http_ok( fd, "text/html", page.used, NULL );
    write( fd, page.data, page.used );
    html_page_cleanup( &page );
    return 0;
}

static int cookie_get( int fd, const http_request* req )
{
    char cookiebuffer[ 512 ];
    int setcookie = 0;
    const char* value;
    html_page page;

    html_page_init( &page, HTML_4 );
    html_page_begin( &page, "Cookie", NULL );
    html_append_raw( &page, "<h1>HTTP cookies</h1>" );

    value = http_get_arg( req->cookies, req->numcookies, "magic" );

    if( value )
    {
        html_append_raw( &page, "Cookie is set to: " );
        html_append_raw( &page, value );
    }
    else
    {
        value = http_get_arg( req->getargs, req->numargs, "magic" );

        if( value )
        {
            html_append_raw( &page, "Setting cookie to: " );
            html_append_raw( &page, value );
            html_append_raw( &page, "<br>" );
            html_append_raw( &page, "<a href=\"/rest/cookie\">refresh</a>" );
            sprintf( cookiebuffer, "magic=%s", value );
            setcookie = 1;
        }
        else
        {
            html_append_raw( &page, "Cookie is not set" );

            html_table_begin(&page, "border: 1px solid black;", STYLE_INLINE);
            html_form_begin( &page, NULL, HTTP_GET );
                html_table_row( &page, 0 );
                    html_table_element( &page );
                        html_append_raw( &page, "Set Cookie to:" );
                    html_table_end_element( &page );
                    html_table_element( &page );
                        html_form_input( &page, INP_TEXT, 0, "magic", NULL );
                    html_table_end_element( &page );
                html_table_end_row( &page );
                html_table_row( &page, 0 );
                    html_table_element( &page );
                        html_append_raw( &page, "&nbsp;" );
                    html_table_end_element( &page );
                    html_table_element( &page );
                        html_form_input( &page, INP_SUBMIT, 0, NULL, "Ok" );
                    html_table_end_element( &page );
                html_table_end_row( &page );
            html_form_end( &page );
            html_table_end( &page );
        }
    }

    html_page_end( &page );

    http_ok( fd, "text/html", page.used, setcookie ? cookiebuffer : NULL );
    write( fd, page.data, page.used );
    html_page_cleanup( &page );
    return 0;
}
