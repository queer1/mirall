/*
 * libcsync -- a library to sync a directory with another
 *
 * Copyright (c) 2011      by Andreas Schneider <asn@cryptomilk.org>
 * Copyright (c) 2012      by Klaas Freitag <freitag@owncloud.com>
 *
 * This program is free software = NULL, you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation = NULL, either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY = NULL, without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program = NULL, if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <neon/ne_basic.h>
#include <neon/ne_socket.h>
#include <neon/ne_session.h>
#include <neon/ne_request.h>
#include <neon/ne_props.h>
#include <neon/ne_auth.h>
#include <neon/ne_dates.h>

#include "c_lib.h"
#include "vio/csync_vio_module.h"
#include "vio/csync_vio_file_stat.h"

#define DEBUG_WEBDAV(x) printf x

enum resource_type {
    resr_normal = 0,
    resr_collection,
    resr_reference,
    resr_error
};

#ifdef HAVE_UNSIGNED_LONG_LONG
typedef unsigned long long dav_size_t;
#define FMT_DAV_SIZE_T "ll"
#ifdef HAVE_STRTOULL
#define DAV_STRTOL strtoull
#endif
#else
typedef unsigned long dav_size_t;
#define FMT_DAV_SIZE_T "l"
#endif

#ifndef DAV_STRTOL
#define DAV_STRTOL strtol
#endif

/* Struct to store data for each resource found during an opendir operation.
 * It represents a single file entry.
 */
typedef struct resource {
    char *uri;           /* The complete uri */
    char *name;          /* The filename only */

    enum resource_type type;
    dav_size_t         size;
    time_t             modtime;

    struct resource    *next;
} resource;

/* Struct to hold the context of a WebDAV PropFind operation to fetch
 * a directory listing from the server.
 */
struct listdir_context {
    struct resource *list;           /* The list of result resources */
    struct resource *currResource;   /* A pointer to the current resource */
    char             *target;        /* Request-URI of the PROPFIND */
    unsigned int     include_target; /* Do you want the uri in the result list? */
    unsigned int     result_count;   /* number of elements stored in list */
};

/*
 * context to store info about a temp file for GET and PUT requests
 * which store the data in a local file to save memory and secure the
 * transmission.
 */
struct transfer_context {
    ne_request *req;            /* the neon request */
    int         fd;             /* file descriptor of the file to read or write from */
    char        *tmpFileName;   /* the name of the temp file */
    size_t      bytes_written;  /* the amount of bytes written or read */
    char        method[4];      /* the HTTP method, either PUT or GET  */
};

/* Struct with the WebDAV session */
struct dav_session_s {
    ne_session *ctx;
    char *user;
    char *pwd;
};

/* The list of properties that is fetched in PropFind on a collection */
static const ne_propname ls_props[] = {
    { "DAV:", "getlastmodified" },
    { "DAV:", "getcontentlength" },
    { "DAV:", "resourcetype" },
    { "DAV:", "getcontenttype" },
    { NULL, NULL }
};

/*
 * local variables.
 */

struct dav_session_s dav_session; /* The DAV Session, initialised in dav_connect */
int _connected;                   /* flag to indicate if a connection exists, ie.
                                     the dav_session is valid */
csync_vio_file_stat_t _fs;

csync_auth_callback _authcb;
void *_userdata;

/* ***************************************************************************** */
static int ne_session_error_errno(ne_session *session)
{
    const char *p = ne_get_error(session);
    char *q;
    int err;

    err = strtol(p, &q, 10);
    if (p == q) {
        return EIO;
    }
    DEBUG_WEBDAV(("Session error string %s\n", p));
    DEBUG_WEBDAV(("Session Error: %d\n", err ));

    switch(err) {
    case 200:           /* OK */
    case 201:           /* Created */
    case 202:           /* Accepted */
    case 203:           /* Non-Authoritative Information */
    case 204:           /* No Content */
    case 205:           /* Reset Content */
    case 207:           /* Multi-Status */
    case 304:           /* Not Modified */
        return 0;
    case 401:           /* Unauthorized */
    case 402:           /* Payment Required */
    case 407:           /* Proxy Authentication Required */
        return EPERM;
    case 301:           /* Moved Permanently */
    case 303:           /* See Other */
    case 404:           /* Not Found */
    case 410:           /* Gone */
        return ENOENT;
    case 408:           /* Request Timeout */
    case 504:           /* Gateway Timeout */
        return EAGAIN;
    case 423:           /* Locked */
        return EACCES;
    case 400:           /* Bad Request */
    case 403:           /* Forbidden */
    case 405:           /* Method Not Allowed */
    case 409:           /* Conflict */
    case 411:           /* Length Required */
    case 412:           /* Precondition Failed */
    case 414:           /* Request-URI Too Long */
    case 415:           /* Unsupported Media Type */
    case 424:           /* Failed Dependency */
    case 501:           /* Not Implemented */
        return EINVAL;
    case 413:           /* Request Entity Too Large */
    case 507:           /* Insufficient Storage */
        return ENOSPC;
    case 206:           /* Partial Content */
    case 300:           /* Multiple Choices */
    case 302:           /* Found */
    case 305:           /* Use Proxy */
    case 306:           /* (Unused) */
    case 307:           /* Temporary Redirect */
    case 406:           /* Not Acceptable */
    case 416:           /* Requested Range Not Satisfiable */
    case 417:           /* Expectation Failed */
    case 422:           /* Unprocessable Entity */
    case 500:           /* Internal Server Error */
    case 502:           /* Bad Gateway */
    case 503:           /* Service Unavailable */
    case 505:           /* HTTP Version Not Supported */
        return EIO;
    default:
        return EIO;
    }

    return EIO;
}

static int ne_error_to_errno(int ne_err)
{
    switch (ne_err) {
    case NE_OK:
    case NE_ERROR:
        return 0;
    case NE_AUTH:
    case NE_PROXYAUTH:
        return EACCES;
    case NE_CONNECT:
    case NE_TIMEOUT:
    case NE_RETRY:
        return EAGAIN;
    case NE_FAILED:
        return EINVAL;
    case NE_REDIRECT:
        return ENOENT;
    case NE_LOOKUP:
        return EIO;
    default:
        return EIO;
    }

    return EIO;
}

/*
 * Authentication callback. Is set by ne_set_server_auth to be called
 * from the neon lib to authenticate a request.
 */
static int ne_auth( void *userdata, const char *realm, int attempt,
                    char *username, char *password)
{
    char buf[NE_ABUFSIZ];

    (void) userdata;
    (void) realm;

    // DEBUG_WEBDAV(( "Authentication required %s\n", realm ));
    if( username && password ) {
        DEBUG_WEBDAV(( "Authentication required %s\n", username ));
        if( dav_session.user ) {
            /* allow user without password */
            strncpy( username, dav_session.user, NE_ABUFSIZ);
            if( dav_session.pwd ) {
                strncpy( password, dav_session.pwd, NE_ABUFSIZ );
            }
        } else if( _authcb != NULL ){
            /* call the csync callback */
            DEBUG_WEBDAV(("Call the csync callback for %s\n", realm ));
            memset( buf, 0, NE_ABUFSIZ );
            (*_authcb) ("Enter your username: ", buf, NE_ABUFSIZ-1, 1, 0, userdata );
            strncpy( username, buf, NE_ABUFSIZ );
            memset( buf, 0, NE_ABUFSIZ );
            (*_authcb) ("Enter your password: ", buf, NE_ABUFSIZ-1, 0, 0, userdata );
            strncpy( password, buf, NE_ABUFSIZ );
        } else {
            DEBUG_WEBDAV(("I can not authenticate!\n"));
        }
    }
    return attempt;
}

/*
 * Connect to a DAV server
 * This function sets the flag _connected if the connection is established
 * and returns if the flag is set, so calling it frequently is save.
 */
static int dav_connect(const char *base_url) {
    int timeout = 30;
    ne_uri uri;
    int rc;
    char *p;
    char protocol[6];

    if (_connected) {
        return 0;
    }

    rc = ne_uri_parse(base_url, &uri);
    if (rc < 0) {
        goto out;
    }

    DEBUG_WEBDAV(("* Userinfo: %s\n", uri.userinfo ));
    DEBUG_WEBDAV(("* scheme %s\n", uri.scheme ));
    DEBUG_WEBDAV(("* host %s\n", uri.host ));
    DEBUG_WEBDAV(("* port %d\n", uri.port ));
    DEBUG_WEBDAV(("* path %s\n", uri.path ));
    DEBUG_WEBDAV(("* fragment %s\n", uri.fragment));

    if( strcmp( uri.scheme, "owncloud" ) == 0 ) {
        strncpy( protocol, "http", 6);
    } else if( strcmp( uri.scheme, "ownclouds" )) {
        strncpy( protocol, "https", 6 );
    }

    if( uri.userinfo ) {
        p = strchr( uri.userinfo, ':');
        if( p ) {
            *p = '\0'; /* cut the strings for strdup */
            p = p+1;

             dav_session.user = c_strdup( uri.userinfo );
             if( p ) dav_session.pwd  = c_strdup( p );
             *(p-1) = ':'; /* unite the strings again */
        }
    }
    DEBUG_WEBDAV(("* user %s\n", dav_session.user ? dav_session.user : ""));
    /* DEBUG_WEBDAV(("* passwd %s\n", dav_session.pwd ? dav_session.pwd : "" )); */

    if (uri.port == 0) {
        uri.port = ne_uri_defaultport(protocol);
    }

    rc = ne_sock_init();
    DEBUG_WEBDAV(("ne_sock_init: %d\n", rc ));
    if (rc < 0) {
        rc = -1;
        goto out;
    }

    /* FIXME: Check for https connections */
    dav_session.ctx = ne_session_create( protocol, uri.host, uri.port);

    if (dav_session.ctx == NULL) {
        rc = -1;
        goto out;
    }

    ne_set_read_timeout(dav_session.ctx, timeout);
    ne_set_useragent( dav_session.ctx, "csync_owncloud" );
    ne_set_server_auth(dav_session.ctx, ne_auth, 0 );

    _connected = 1;
    rc = 0;
out:
    ne_uri_free( &uri );
    return rc;
}

/*
 * helper function to sort the result resource list. Called from the
 * results function to build up the result list.
 */
static int compare_resource(const struct resource *r1,
                            const struct resource *r2)
{
    /* DEBUG_WEBDAV(( "C1 %d %d\n", r1, r2 )); */
    int re = -1;
    /* Sort errors first, then collections, then alphabetically */
    if (r1->type == resr_error) {
        // return -1;
    } else if (r2->type == resr_error) {
        re = 1;
    } else if (r1->type == resr_collection) {
        if (r2->type != resr_collection) {
            // return -1;
        } else {
            re = strcmp(r1->uri, r2->uri);
        }
    } else {
        if (r2->type != resr_collection) {
            re = strcmp(r1->uri, r2->uri);
        } else {
            re = 1;
        }
    }
    /* DEBUG_WEBDAV(( "C2 = %d\n", re )); */
    return re;

}

/*
 * result parsing list.
 * This function is called to parse the result of the propfind request
 * to list directories on the WebDAV server. I takes a single resource
 * and fills a resource struct and stores it to the result list which
 * is stored in the listdir_context.
 */
static void results(void *userdata,
                    const ne_uri *uri,
                    const ne_prop_result_set *set)
{
    struct listdir_context *fetchCtx = userdata;
    struct resource *current = 0;
    struct resource *previous = 0;
    struct resource *newres = 0;
    const char *clength, *modtime = NULL;
    const char *resourcetype = NULL;
    const char *contenttype = NULL;
    const ne_status *status = NULL;
    char *path = ne_path_unescape( uri->path );

    (void) status;

    DEBUG_WEBDAV(("** propfind result found: %s\n", path ));
    if( ! fetchCtx->target ) {
        DEBUG_WEBDAV(("error: target must not be zero!\n" ));
        return;
    }

    if (ne_path_compare(fetchCtx->target, uri->path) == 0 && !fetchCtx->include_target) {
        /* This is the target URI */
        DEBUG_WEBDAV(( "Skipping target resource.\n"));
        /* Free the private structure. */
        SAFE_FREE( path );
        return;
    }

    /* Fill the resource structure with the data about the file */
    newres = c_malloc(sizeof(struct resource));
    newres->uri =  path; /* no need to strdup because ne_path_unescape already allocates */
    newres->name = c_basename( path );

    modtime      = ne_propset_value( set, &ls_props[0] );
    clength      = ne_propset_value( set, &ls_props[1] );
    resourcetype = ne_propset_value( set, &ls_props[2] );
    contenttype  = ne_propset_value( set, &ls_props[3] );
    DEBUG_WEBDAV(("Contenttype: %s\n", contenttype ? contenttype : "" ));

    newres->type = resr_normal;
    if( clength == NULL && resourcetype && strncmp( resourcetype, "<DAV:collection>", 16 ) == 0) {
        newres->type = resr_collection;
    }

    if (modtime)
        newres->modtime = ne_httpdate_parse(modtime);

    if (clength) {
        char *p;

        newres->size = DAV_STRTOL(clength, &p, 10);
        if (*p) {
            newres->size = 0;
        }
    }

    if( contenttype ) {

    }

    /* put the new resource into the result list */
    for (current = fetchCtx->list, previous = NULL; current != NULL;
         previous = current, current=current->next) {
        if (compare_resource(current, newres) >= 0) {
            break;
        }
    }
    if (previous) {
        previous->next = newres;
    } else {
        fetchCtx->list = newres;
    }
    newres->next = current;

    fetchCtx->result_count = fetchCtx->result_count + 1;
    DEBUG_WEBDAV(( "results for URI %s: %d %d\n", newres->name, (int)newres->size, (int)newres->modtime ));
    // DEBUG_WEBDAV(( "Leaving result for resource %s\n", newres->uri ));

}

static const char *_cleanUrl( const char* uri ) {
    int rc = 0;

    ne_uri url;
    rc = ne_uri_parse( uri, &url );
    if( rc < 0 ) {
        return NULL;
    }

    return ne_path_escape( url.path );
}

/*
 * fetches a resource list from the WebDAV server. This is equivalent to list dir.
 */

static int fetch_resource_list( const char *curi,
                                int depth,
                                struct listdir_context *fetchCtx )
{
    int ret = 0;

    /* do a propfind request and parse the results in the results function, set as callback */
    ret = ne_simple_propfind( dav_session.ctx, curi, depth, ls_props, results, fetchCtx );

    if( ret == NE_OK ) {
        DEBUG_WEBDAV(("Simple propfind OK.\n" ));
        fetchCtx->currResource = fetchCtx->list;
    }
    return ret;
}

/*
 * helper: convert a resource struct to file_stat struct.
 */
static csync_vio_file_stat_t *resourceToFileStat( struct resource *res )
{
    csync_vio_file_stat_t *lfs = NULL;

    if( ! res ) {
        return NULL;
    }

    lfs = c_malloc(sizeof(csync_vio_file_stat_t));
    if (lfs == NULL) {
        // free readdir list?
        return NULL;
    }

    lfs->name = c_strdup( res->name );

    lfs->fields = CSYNC_VIO_FILE_STAT_FIELDS_NONE;
    if( res->type == resr_normal ) {
        lfs->fields |= CSYNC_VIO_FILE_STAT_FIELDS_TYPE;
        lfs->type = CSYNC_VIO_FILE_TYPE_REGULAR;
    } else if( res->type == resr_collection ) {
        lfs->fields |= CSYNC_VIO_FILE_STAT_FIELDS_TYPE;
        lfs->type = CSYNC_VIO_FILE_TYPE_DIRECTORY;
    }

    lfs->mtime = res->modtime;
    lfs->fields |= CSYNC_VIO_FILE_STAT_FIELDS_MTIME;
    lfs->size  = res->size;
    lfs->fields |= CSYNC_VIO_FILE_STAT_FIELDS_SIZE;

    return lfs;
}

/*
 * file functions
 */
static int _stat(const char *uri, csync_vio_file_stat_t *buf) {
    /* get props:
     *   modtime
     *   creattime
     *   size
     */
    int rc = 0;
    csync_vio_file_stat_t *lfs = NULL;
    struct listdir_context  *fetchCtx = NULL;
    char *curi = NULL;

    DEBUG_WEBDAV(("__stat__ %s called\n", uri ));

    buf->name = c_basename(uri);

    if (buf->name == NULL) {
        csync_vio_file_stat_destroy(buf);
        errno = ENOMEM;
        return -1;
    }

    /* check if the data in the static 'cache' fs is for the same file.
     * The cache is filled by readdir which is often called directly before
     * stat. If the cache matches, a http call is saved.
     */
    if( _fs.name && strcmp( buf->name, _fs.name ) == 0 ) {
        buf->fields  = CSYNC_VIO_FILE_STAT_FIELDS_NONE;
        buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_TYPE;
        buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_SIZE;
        buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_MTIME;
        buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_PERMISSIONS;

        buf->fields = _fs.fields;
        buf->type   = _fs.type;
        buf->mtime  = _fs.mtime;
        buf->size   = _fs.size;
        buf->mode   = _stat_perms( _fs.type );
    } else {
        // fetch data via a propfind call.
        DEBUG_WEBDAV(("I have no stat cache, call propfind.\n"));

        fetchCtx = c_malloc( sizeof( struct listdir_context ));
        if( ! fetchCtx ) {
            errno = ne_error_to_errno( NE_ERROR );
            csync_vio_file_stat_destroy(buf);
            return -1;
        }

        curi = _cleanPath( uri );
        fetchCtx->list = NULL;
        fetchCtx->target = curi;
        fetchCtx->include_target = 1;
        fetchCtx->currResource = NULL;

        DEBUG_WEBDAV(("fetchCtx good.\n" ));

        rc = fetch_resource_list( curi, NE_DEPTH_ONE, fetchCtx );
        if( rc != NE_OK ) {
            errno = ne_session_error_errno( dav_session.ctx );

            DEBUG_WEBDAV(("stat fails with errno %d\n", errno ));
            SAFE_FREE(fetchCtx);
            // csync_vio_file_stat_destroy(buf);
            return -1;
        }

        if( fetchCtx ) {
            fetchCtx->currResource = fetchCtx->list;
            lfs = resourceToFileStat( fetchCtx->currResource );
            if( lfs ) {
                buf->fields = CSYNC_VIO_FILE_STAT_FIELDS_NONE;
                buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_TYPE;
                buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_SIZE;
                buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_MTIME;
                buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_PERMISSIONS;

                buf->fields = lfs->fields;
                buf->type   = lfs->type;
                buf->mtime  = lfs->mtime;
                buf->size   = lfs->size;
                buf->mode   = _stat_perms( lfs->type );

                csync_vio_file_stat_destroy( lfs );
            }
            SAFE_FREE( fetchCtx );
        }
    }
    DEBUG_WEBDAV(("STAT result: %s, type=%d\n", buf->name, buf->type ));

    return 0;
}

static ssize_t _write(csync_vio_method_handle_t *fhandle, const void *buf, size_t count) {
    struct transfer_context *writeCtx = NULL;
    size_t written = 0;

    if (fhandle == NULL) {
        return -1;
    }

    writeCtx = (struct transfer_context*) fhandle;
    if( writeCtx->fd > -1 ) {
        written = write( writeCtx->fd, buf, count );
        if( written != count ) {
            DEBUG_WEBDAV(("Written bytes not equal to count\n"));
        } else {
            writeCtx->bytes_written = writeCtx->bytes_written + written;
            /* DEBUG_WEBDAV(("Successfully written %d\n", written )); */
        }
    } else {
        /* problem: the file descriptor is not valid. */
        DEBUG_WEBDAV(("Not a valid file descriptor in write\n"));
    }

    DEBUG_WEBDAV(("Wrote %d bytes.\n", written ));
    return written;
}

static csync_vio_method_handle_t *_open(const char *durl,
                                        int flags,
                                        mode_t mode) {
    char *uri = NULL;
    char *dir = NULL;
    char getUrl[PATH_MAX];
    int put = 0;
    int rc = NE_OK;
    struct transfer_context *writeCtx = NULL;
    struct stat statBuf;

    (void) mode; /* unused on webdav server */
    DEBUG_WEBDAV(( "=> open called for %s!\n", durl ));

    /* uri = ne_path_escape(durl);
     * escaping lets the ne_request_create fail, even though its documented
     * differently :-(
     */
    uri = _cleanPath( durl );
    if( ! uri ) {
        rc = NE_ERROR;
    } else {
        DEBUG_WEBDAV(("uri: %s\n", uri ));
    }

    if( rc == NE_OK )
        dav_connect( durl );

    if (flags & O_WRONLY) {
        put = 1;
    }
    if (flags & O_RDWR) {
        put = 1;
    }
    if (flags & O_CREAT) {
        put = 1;
    }


    if( rc == NE_OK && put ) {
        /* check if the dir name exists. Otherwise return ENOENT */
        dir = c_dirname( durl );
        DEBUG_WEBDAV(("Stating directory %s\n", dir ));
        if( _stat( dir, (csync_vio_method_handle_t*)(&statBuf) ) == 0 ) {
            // Success!
            DEBUG_WEBDAV(("Directory of file to open exists.\n"));
        } else {
            DEBUG_WEBDAV(("Directory %s of file to open does NOT exist.\n", dir ));
            /* the directory does not exist. That is an ENOENT */
            errno = ENOENT;
            SAFE_FREE( dir );
            return NULL;
        }
    }

    writeCtx = c_malloc( sizeof(struct transfer_context) );
    writeCtx->bytes_written = 0;
    if( rc == NE_OK ) {
        /* open a temp file to store the incoming data */
        writeCtx->tmpFileName = c_strdup( "/tmp/csync.XXXXXX" );
        writeCtx->fd = mkstemp( writeCtx->tmpFileName );
        DEBUG_WEBDAV(("opening temp directory %s\n", writeCtx->tmpFileName ));
        if( writeCtx->fd == -1 ) {
            rc = NE_ERROR;
        }
    }

    if( rc == NE_OK && put) {
        DEBUG_WEBDAV(("PUT request on %s!\n", uri));

        writeCtx->req = ne_request_create(dav_session.ctx, "PUT", uri);
        if( writeCtx->req ) {
            rc = ne_begin_request( writeCtx->req );
            if (rc != NE_OK) {
                DEBUG_WEBDAV(("Can not open a request, bailing out.\n"));
            }
        }

        strncpy( writeCtx->method, "PUT", 3 );
    }


    if( rc == NE_OK && ! put ) {
        writeCtx->req = 0;
        strncpy( writeCtx->method, "GET", 3 );

        /* Download the data into a local temp file. */
        /* the download via the get function requires a full uri */
        snprintf( getUrl, PATH_MAX, "%s://%s%s", ne_get_scheme( dav_session.ctx),
                  ne_get_server_hostport( dav_session.ctx ), uri );
        DEBUG_WEBDAV(("GET request on %s\n", getUrl ));

        rc = ne_get( dav_session.ctx, getUrl, writeCtx->fd ); // FIX_ESCAPE
        if( rc != NE_OK ) {
            DEBUG_WEBDAV(("Download to local file failed: %d.\n", rc));
        }
        if( close( writeCtx->fd ) == -1 ) {
            DEBUG_WEBDAV(("Close of local download file failed.\n"));
            writeCtx->fd = -1;
            rc = NE_ERROR;
        }

        writeCtx->fd = -1;
    }

    if( rc != NE_OK ) {
        ne_error_to_errno( rc );
        SAFE_FREE( writeCtx );
    }

    SAFE_FREE( uri );
    SAFE_FREE(dir);

    return (csync_vio_method_handle_t *) writeCtx;
}

static csync_vio_method_handle_t *_creat(const char *durl, mode_t mode) {

    csync_vio_method_handle_t *handle = _open(durl, O_CREAT|O_WRONLY|O_TRUNC, mode);

    /* on create, the file needs to be created empty */
    _write( handle, NULL, 0 );

    return handle;
}

static int _close(csync_vio_method_handle_t *fhandle) {
    struct transfer_context *writeCtx;
    struct stat st;
    int rc;
    int ret = 0;

    writeCtx = (struct transfer_context*) fhandle;

    if (fhandle == NULL) {
        ret = -1;
    }

    /* handle the PUT request, means write to the WebDAV server */
    if( ret != -1 && strcmp( writeCtx->method, "PUT" ) == 0 ) {

        /* if there is a valid file descriptor, close it, reopen in read mode and start the PUT request */
        if( writeCtx->fd > -1 ) {
            if( close( writeCtx->fd ) < 0 ) {
                DEBUG_WEBDAV(("Could not close file %s\n", writeCtx->tmpFileName ));
                ret = -1;
            }

            /* and open it again to read from */
            if (( writeCtx->fd = open( writeCtx->tmpFileName, O_RDONLY )) < 0) {
                ret = -1;
            } else {
                if (fstat( writeCtx->fd, &st ) < 0) {
                    DEBUG_WEBDAV(("Could not stat file %s\n", writeCtx->tmpFileName ));
                    ret = -1;
                }

                /* successfully opened for read. Now start the request via ne_put */
                ne_set_request_body_fd( writeCtx->req, writeCtx->fd, 0, st.st_size );
                rc = ne_request_dispatch( writeCtx->req );
                close( writeCtx->fd );
                if (rc == NE_OK) {
                    if ( ne_get_status( writeCtx->req )->klass != 2 ) {
                        DEBUG_WEBDAV(("Error - PUT status value no 2xx\n"));
                    }
                } else {
                    DEBUG_WEBDAV(("ERROR %d!\n", rc ));
                    ne_session_error_errno( dav_session.ctx );
                    ret = -1;
                }
            }
        }
        ne_request_destroy( writeCtx->req );
    } else  {
        /* Its a GET request, not much to do in close. */
        if( writeCtx->fd > -1) {
            close( writeCtx->fd );
        }
    }
    /* Remove the local file. */
    unlink( writeCtx->tmpFileName );

    /* free mem. Note that the request mem is freed by the ne_request_destroy call */
    SAFE_FREE( writeCtx->tmpFileName );
    SAFE_FREE( writeCtx );

    return ret;
}

static ssize_t _read(csync_vio_method_handle_t *fhandle, void *buf, size_t count) {
    struct transfer_context *writeCtx;
    ssize_t len = 0;
    struct stat st;

    writeCtx = (struct transfer_context*) fhandle;

    /* DEBUG_WEBDAV(( "read called on %s!\n", writeCtx->tmpFileName )); */
    if( ! fhandle ) {
        return -1;
    }

    if( writeCtx->fd == -1 ) {
        /* open the downloaded file to read from */
        if (( writeCtx->fd = open( writeCtx->tmpFileName, O_RDONLY )) < 0) {
             DEBUG_WEBDAV(("Could not open local file %s\n", writeCtx->tmpFileName ));
            return -1;
        } else {
            if (fstat( writeCtx->fd, &st ) < 0) {
                DEBUG_WEBDAV(("Could not stat file %s\n", writeCtx->tmpFileName ));
                return -1;
            }

            DEBUG_WEBDAV(("local downlaod file size=%d\n", (int) st.st_size ));
        }
    }

    if( writeCtx->fd ) {
        len = read( writeCtx->fd, buf, count );
        writeCtx->bytes_written = writeCtx->bytes_written + len;
    }

    /* DEBUG_WEBDAV(( "read len: %d\n", len )); */

    return len;
}

static off_t _lseek(csync_vio_method_handle_t *fhandle, off_t offset, int whence) {
    (void) fhandle;
    (void) offset;
    (void) whence;

    return -1;
}

/*
 * directory functions
 */
static csync_vio_method_handle_t *_opendir(const char *uri) {
    int rc;
    struct listdir_context *fetchCtx = NULL;
    struct resource *reslist = NULL;
    char *curi = _cleanPath( uri );

    DEBUG_WEBDAV(("opendir method called on %s\n", uri ));

    dav_connect( uri );

    fetchCtx = c_malloc( sizeof( struct listdir_context ));

    fetchCtx->list = reslist;
    fetchCtx->target = curi;
    fetchCtx->include_target = 0;
    fetchCtx->currResource = NULL;

    rc = fetch_resource_list( curi, NE_DEPTH_ONE, fetchCtx );
    if( rc != NE_OK ) {
        errno = ne_session_error_errno( dav_session.ctx );
        return NULL;
    } else {
        fetchCtx->currResource = fetchCtx->list;
        DEBUG_WEBDAV(("opendir returning handle %p\n", (void*) fetchCtx ));
        return fetchCtx;
    }
    /* no freeing of curi because its part of the fetchCtx and gets freed later */
}

static int _closedir(csync_vio_method_handle_t *dhandle) {

    struct listdir_context *fetchCtx = dhandle;
    struct resource *r = fetchCtx->list;
    struct resource *rnext = NULL;

    DEBUG_WEBDAV(("closedir method called %p!\n", dhandle));

    while( r ) {
        rnext = r->next;
        SAFE_FREE(r->uri);
        SAFE_FREE(r->name);
        SAFE_FREE(r);
        r = rnext;
    }
    SAFE_FREE( fetchCtx->target );

    SAFE_FREE( dhandle );
    return 0;
}

static csync_vio_file_stat_t *_readdir(csync_vio_method_handle_t *dhandle) {

    struct listdir_context *fetchCtx = dhandle;
    csync_vio_file_stat_t *lfs = NULL;

    if( fetchCtx->currResource ) {
        DEBUG_WEBDAV(("readdir method called for %s\n", fetchCtx->currResource->uri));
    } else {
        /* DEBUG_WEBDAV(("An empty dir or at end\n")); */
        return NULL;
    }

    if( fetchCtx && fetchCtx->currResource ) {
        /* FIXME: Who frees the allocated mem for lfs, allocated in the helper func? */
        lfs = resourceToFileStat( fetchCtx->currResource );

        // set pointer to next element
        fetchCtx->currResource = fetchCtx->currResource->next;

        /* fill the static stat buf as input for the stat function */
        _fs.name   = lfs->name;
        _fs.mtime  = lfs->mtime;
        _fs.fields = lfs->fields;
        _fs.type   = lfs->type;
        _fs.size   = lfs->size;
    }

    DEBUG_WEBDAV(("LFS fields: %s: %d\n", lfs->name, lfs->type ));
    return lfs;
}

static int _mkdir(const char *uri, mode_t mode) {
    int rc = NE_OK;
    char *path = _cleanPath( uri );
    (void) mode; /* unused */

    rc = dav_connect(uri);
    if (rc < 0) {
        errno = EINVAL;
    }

    /* the suri path is required to have a trailing slash */
    if( rc >= 0 ) {
      DEBUG_WEBDAV(("MKdir on %s\n", path ));
      rc = ne_mkcol(dav_session.ctx, path );
        if (rc != NE_OK ) {
            errno = ne_error_to_errno(rc);
        }
    }
    SAFE_FREE( path );

    if( rc < 0 || rc != NE_OK ) {
        return -1;
    }
    return 0;
}

static int _rmdir(const char *uri) {
    int rc = NE_OK;
    char* curi = _cleanPath( uri );

    rc = dav_connect(uri);
    if (rc < 0) {
        errno = EINVAL;
    }

    if( rc >= 0 ) {
        rc = ne_delete(dav_session.ctx, curi);
        if (rc != 0) {
            errno = ne_error_to_errno(rc);
        }
    }
    SAFE_FREE( curi );
    if( rc < 0 || rc != NE_OK ) {
        return -1;
    }

    return 0;
}

/* WebDAV does not deliver permissions. We set a default here. */
static int _stat_perms( int type ) {
    int ret = 0;

    if( type == CSYNC_VIO_FILE_TYPE_DIRECTORY ) {
        DEBUG_WEBDAV(("Setting mode in stat (dir)\n"));
        /* directory permissions */
        ret = S_IFDIR | S_IRUSR | S_IWUSR | S_IXUSR /* directory, rwx for user */
                | S_IRGRP | S_IXGRP                       /* rx for group */
                | S_IROTH | S_IXOTH;                      /* rx for others */
    } else {
        /* regualar file permissions */
        DEBUG_WEBDAV(("Setting mode in stat (file)\n"));
        ret = S_IFREG | S_IRUSR | S_IWUSR /* regular file, user read & write */
                | S_IRGRP                         /* group read perm */
                | S_IROTH;                        /* others read perm */
    }
    return ret;
}

static int _stat(const char *uri, csync_vio_file_stat_t *buf) {
    /* get props:
     *   modtime
     *   creattime
     *   size
     */
    int rc = 0;
    csync_vio_file_stat_t *lfs = NULL;
    struct listdir_context  *fetchCtx = NULL;

    DEBUG_WEBDAV(("__stat__ %s called\n", uri ));

    buf->name = c_basename(uri);

    if (buf->name == NULL) {
        csync_vio_file_stat_destroy(buf);
        errno = ENOMEM;
        return -1;
    }

    /* check if the data in the static 'cache' fs is for the same file.
     * The cache is filled by readdir which is often called directly before
     * stat. If the cache matches, a http call is saved.
     */
    if( _fs.name && strcmp( buf->name, _fs.name ) == 0 ) {
        buf->fields  = CSYNC_VIO_FILE_STAT_FIELDS_NONE;
        buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_TYPE;
        buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_SIZE;
        buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_MTIME;
        buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_PERMISSIONS;

        buf->fields = _fs.fields;
        buf->type   = _fs.type;
        buf->mtime  = _fs.mtime;
        buf->size   = _fs.size;
        buf->mode   = _stat_perms( _fs.type );
    } else {
        // fetch data via a propfind call.
        DEBUG_WEBDAV(("I have no stat cache, call propfind.\n"));

        fetchCtx = c_malloc( sizeof( struct listdir_context ));
        if( ! fetchCtx ) {
            errno = ne_error_to_errno( NE_ERROR );
            csync_vio_file_stat_destroy(buf);
            return -1;
        }

        fetchCtx->list = NULL;
        fetchCtx->target = _cleanPath( uri );
        fetchCtx->include_target = 1;
        fetchCtx->currResource = NULL;

        DEBUG_WEBDAV(("fetchCtx good.\n" ));

        rc = fetch_resource_list( uri, NE_DEPTH_ONE, fetchCtx );
        if( rc != NE_OK ) {
            errno = ne_error_to_errno( rc );
            SAFE_FREE(fetchCtx);
            csync_vio_file_stat_destroy(buf);
            return -1;
        }

        if( fetchCtx ) {
            fetchCtx->currResource = fetchCtx->list;
            lfs = resourceToFileStat( fetchCtx->currResource );
            if( lfs ) {
                buf->fields = CSYNC_VIO_FILE_STAT_FIELDS_NONE;
                buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_TYPE;
                buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_SIZE;
                buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_MTIME;
                buf->fields |= CSYNC_VIO_FILE_STAT_FIELDS_PERMISSIONS;

                buf->fields = lfs->fields;
                buf->type   = lfs->type;
                buf->mtime  = lfs->mtime;
                buf->size   = lfs->size;
                buf->mode   = _stat_perms( lfs->type );

                csync_vio_file_stat_destroy( lfs );
            }
            SAFE_FREE( fetchCtx );
        }
    }
    DEBUG_WEBDAV(("STAT result: %s, type=%d\n", buf->name, buf->type ));

    return 0;
}

static int _rename(const char *olduri, const char *newuri) {
    char *src = NULL;
    char *target = NULL;
    int rc = NE_OK;


    rc = dav_connect(olduri);
    if (rc < 0) {
        errno = EINVAL;
    }

    src    = _cleanPath( olduri );
    target = _cleanPath( newuri );

    if( rc >= 0 ) {
        DEBUG_WEBDAV(("MOVE: %s => %s: %d\n", src, target, rc ));
        rc = ne_move(dav_session.ctx, 1, src, target );

        if (rc != NE_OK ) {
            ne_session_error_errno(dav_session.ctx);
            errno = ne_error_to_errno(rc);
        }
    }
    SAFE_FREE( src );
    SAFE_FREE( target );

    if( rc != NE_OK )
        return -1;
    return 0;
}

static int _unlink(const char *uri) {
    int rc = NE_OK;
    char *path = _cleanPath( uri );

    if( ! path ) {
        rc = NE_ERROR;
        errno = EINVAL;
    }
    if( rc == NE_OK ) {
        rc = dav_connect(uri);
        if (rc < 0) {
            errno = EINVAL;
        }
    }
    if( rc == NE_OK ) {
        rc = ne_delete( dav_session.ctx, path );
        if ( rc != NE_OK )
            errno = ne_error_to_errno(rc);
    }
    SAFE_FREE( path );

    return 0;
}

static int _chmod(const char *uri, mode_t mode) {
    (void) uri;
    (void) mode;

    return 0;
}

static int _chown(const char *uri, uid_t owner, gid_t group) {
    (void) uri;
    (void) owner;
    (void) group;

    return 0;
}

static int _utimes(const char *uri, const struct timeval *times) {

    ne_proppatch_operation ops[2];
    ne_propname pname;
    int rc = NE_OK;
    char val[255];
    char *curi;

    curi = _cleanPath( uri );

    if( ! uri || !times ) {
        errno = 1;
        return -1; // FIXME: Find good errno
    }
    pname.nspace = NULL;
    pname.name = "lastmodified";

    snprintf( val, sizeof(val), "%ld", times->tv_sec );
    DEBUG_WEBDAV(("Setting LastModified of %s to %s\n", curi, val ));

    ops[0].name = &pname;
    ops[0].type = ne_propset;
    ops[0].value = val;

    ops[1].name = NULL;

    rc = ne_proppatch( dav_session.ctx, curi, ops );
    SAFE_FREE(curi);

    if( rc != NE_OK ) {
        errno = ne_error_to_errno( rc );
        DEBUG_WEBDAV(("Error in propatch: %d\n", rc));
        return -1;
    }
    return 0;
}

csync_vio_method_t _method = {
    .method_table_size = sizeof(csync_vio_method_t),
    .open = _open,
    .creat = _creat,
    .close = _close,
    .read = _read,
    .write = _write,
    .lseek = _lseek,
    .opendir = _opendir,
    .closedir = _closedir,
    .readdir = _readdir,
    .mkdir = _mkdir,
    .rmdir = _rmdir,
    .stat = _stat,
    .rename = _rename,
    .unlink = _unlink,
    .chmod = _chmod,
    .chown = _chown,
    .utimes = _utimes
};

csync_vio_method_t *vio_module_init(const char *method_name, const char *args,
                                    csync_auth_callback cb, void *userdata) {
    DEBUG_WEBDAV(("csync_webdav - method_name: %s\n", method_name));
    DEBUG_WEBDAV(("csync_webdav - args: %s\n", args));

    (void) method_name;
    (void) args;
    _authcb = cb;
    _userdata = userdata;

    return &_method;
}

void vio_module_shutdown(csync_vio_method_t *method) {
    (void) method;

    SAFE_FREE( dav_session.user );
    SAFE_FREE( dav_session.pwd );

    ne_session_destroy( dav_session.ctx );
}



/* vim: set ts=4 sw=4 et cindent: */
