#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <stdlib.h>
#include <stdio.h>
#include <netdb.h>
#include <fcntl.h>
#include <dirent.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <snet.h>

#include "cparse.h"
#include "logname.h"
#include "monster.h"

/* 90 minutes */
int		idle = 45000;
int		interval = 20;
int             debug = 0;
extern char	*cosign_version;

static void (*logger)( char * ) = NULL;
static struct timeval           timeout = { 10 * 60, 0 };


int decision( char *, struct timeval *, time_t * );

    int
main( int ac, char **av )
{
    DIR			*dirp;
    struct dirent	*de;
    struct timeval	tv, now;
    struct hostent	*he;
    struct cl		*head = NULL, **tail = NULL, **cur, *new = NULL;
    char                login[ MAXPATHLEN ];
    time_t		cookie_itime = 0;
    char		*prog, *line;
    int			c, i, port = htons( 6663 ), err = 0;
    int			rc;
    char           	*cosign_dir = _COSIGN_DIR;
    char           	*cosign_host = _COSIGN_HOST;
    char		*cryptofile = _COSIGN_TLS_KEY;
    char		*certfile = _COSIGN_TLS_CERT;
    char		*cadir = _COSIGN_TLS_CADIR;
    int                 facility = _COSIGN_LOG;
    SSL_CTX		*ctx = NULL;
    extern int          optind;
    extern char         *optarg;

    if (( prog = strrchr( av[ 0 ], '/' )) == NULL ) {
	prog = av[ 0 ];
    } else {
	prog++;
    }

    while (( c = getopt( ac, av, "dh:i:I:L:p:x:y:z:" )) != EOF ) {
	switch ( c ) {
	case 'd' :		/* debug */
	    debug++;
	    break;

	case 'h' :		/* host running cosignd */
	    cosign_host = optarg;
	    break;

	case 'i' :              /* idle timeout in seconds*/
	    idle = atoi( optarg );
	    break;

	case 'I' :              /* timestamp pushing interval*/
	    interval = atoi( optarg );
	    break;

	case 'L' :              /* syslog facility */
	    if (( facility = syslogname( optarg )) == -1 ) {
		fprintf( stderr, "%s: %s: unknown syslog facility\n",
			prog, optarg );
		exit( 1 );
	    }
	    break;

	case 'p' :              /* TCP port */
	     port = htons( atoi( optarg ));
	     break;

	case 'x' :		/* ca dir */
	    cadir = optarg;
	    break;
	
	case 'y' :		/* cert */
	    certfile = optarg;
	    break;
	
	case 'z' :		/* private key file */
	    cryptofile = optarg;
	    break;
	
	case '?':
            err++;
            break;

        default:
            err++;
            break;
	}
    }

    if ( err || optind != ac ) {
	fprintf( stderr, "Usage: monster [ -dV ] [ -h cosignd host ] ");
	fprintf( stderr, "[ -i idletimeinsecs  ] [ -I update interval ] " );
	fprintf( stderr, "[ -L syslog facility] [ -p port ] [ -x ca dir ] " );
	fprintf( stderr, "[ -y cert file] [ -z private key file ]\n" );
	exit( -1 );
    }

    if (( he = gethostbyname( cosign_host )) == NULL ) {
	fprintf( stderr, "%s no wanna give hostnames\n", cosign_host );
	exit( 1 );
    }
    tail = &head;
    for ( i = 0; he->h_addr_list[ i ] != NULL; i++ ) {
	if (( new = ( struct cl * ) malloc( sizeof( struct cl ))) == NULL ) {
	    perror( "cl build" );
	    exit( 1 );
	}

        memset( &new->cl_sin, 0, sizeof( struct sockaddr_in ));
        new->cl_sin.sin_family = AF_INET;
        new->cl_sin.sin_port = port;
        memcpy( &new->cl_sin.sin_addr.s_addr,
                he->h_addr_list[ i ], (unsigned int)he->h_length );
        new->cl_sn = NULL;
	new->cl_last_time = 0;
        *tail = new;
        tail = &new->cl_next;
    }
    *tail = NULL;

    if ( access( cryptofile, R_OK ) != 0 ) {
        perror( cryptofile );
        exit( 1 );
    }

    if ( access( certfile, R_OK ) != 0 ) {
        perror( certfile );
        exit( 1 );
    }

    if ( access( cadir, R_OK ) != 0 ) {
        perror( cadir );
        exit( 1 );
    }

    if ( cryptofile != NULL ) {
	SSL_load_error_strings();
	SSL_library_init();

	if (( ctx = SSL_CTX_new( SSLv23_client_method())) == NULL ) {
	    fprintf( stderr, "SSL_CTX_new: %s\n",
		    ERR_error_string( ERR_get_error(), NULL ));
	    exit( 1 );
	}

	if ( SSL_CTX_use_PrivateKey_file( ctx, cryptofile, SSL_FILETYPE_PEM )
		!= 1 ) {
	    fprintf( stderr, "SSL_CTX_use_PrivateKey_file: %s: %s\n",
		    cryptofile, ERR_error_string( ERR_get_error(), NULL));
	    exit( 1 );
	}
	if ( SSL_CTX_use_certificate_chain_file( ctx, certfile ) != 1) {
	    fprintf( stderr, "SSL_CTX_use_certificate_chain_file: %s: %s\n",
		    cryptofile, ERR_error_string( ERR_get_error(), NULL));
	    exit( 1 );
	}
	if ( SSL_CTX_check_private_key( ctx ) != 1 ) {
	    fprintf( stderr, "SSL_CTX_check_private_key: %s\n",
		    ERR_error_string( ERR_get_error(), NULL ));
	    exit( 1 );
	}

	if ( SSL_CTX_load_verify_locations( ctx, NULL, cadir ) != 1 ) {
	    fprintf( stderr, "SSL_CTX_load_verify_locations: %s: %s\n",
		    cryptofile, ERR_error_string( ERR_get_error(), NULL));
	    exit( 1 );
	}
	SSL_CTX_set_verify( ctx,
		SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    }

    if ( chdir( cosign_dir ) < 0 ) {
	perror( cosign_dir );
	exit( 1 );
    }


    /*
     * Disassociate from controlling tty.
     */
    if ( !debug ) {
	int		i, dt;

	switch ( fork()) {
	case 0 :
	    if ( setsid() < 0 ) {
		perror( "setsid" );
		exit( 1 );
	    }
	    dt = getdtablesize();
	    if (( i = open( "/", O_RDONLY, 0 )) == 0 ) {
		dup2( i, 1 );
		dup2( i, 2 );
	    }
	    break;
	case -1 :
	    perror( "fork" );
	    exit( 1 );
	default :
	    exit( 0 );
	}
    }

    /*
     * Start logging.
     */
#ifdef ultrix
    openlog( prog, LOG_NOWAIT|LOG_PID );
#else /* ultrix */
    openlog( prog, LOG_NOWAIT|LOG_PID, LOG_DAEMON );
#endif /* ultrix */


    syslog( LOG_INFO, "restart %s", cosign_version );

	for (;;) {

    sleep( interval );

    if (( dirp = opendir( cosign_dir )) == NULL ) {
	syslog( LOG_ERR, "%s: %m", cosign_dir);
	exit( -1 );
    }

    if ( gettimeofday( &now, NULL ) != 0 ){
	syslog( LOG_ERR, "gettimeofday: %m" );
	exit( -1 );
    }

    for ( cur = &head; *cur != NULL; cur = &(*cur)->cl_next ) {
	if ( (*cur)->cl_sn == NULL ) {
	    if ( connect_sn( *cur, ctx, cosign_host ) != 0 ) {
		(*cur)->cl_sn = NULL;
		continue;
	    }
	}

	if ( snet_writef( (*cur)->cl_sn, "TIME\r\n" ) < 0 ) {
	    syslog( LOG_ERR, "snet_writef failed on TIME\n");
	    if ( snet_close( (*cur)->cl_sn ) != 0 ) {
		syslog( LOG_ERR, "snet_close: %m\n" );
	    }
	    (*cur)->cl_sn = NULL;
	    continue;
	}

	tv = timeout;
	if (( line = snet_getline_multi( (*cur)->cl_sn, logger, &tv ))
		== NULL ) {
	    if ( !snet_eof( (*cur)->cl_sn )) {
		syslog( LOG_ERR, "snet_getline_multi: %m\n" );
	    }
	    if ( snet_close( (*cur)->cl_sn ) != 0 ) {
		syslog( LOG_ERR, "snet_close: %m\n" );
	    }
	    (*cur)->cl_sn = NULL;
	    continue;
	}

	if ( *line != '3' ) {
	    syslog( LOG_ERR, "snet_getline_multi: %m\n" );
	    if ( snet_close( (*cur)->cl_sn ) != 0 ) {
		syslog( LOG_ERR, "snet_close: %m\n" );
	    }
	    (*cur)->cl_sn = NULL;
	    continue;
	}
    }

    while (( de = readdir( dirp )) != NULL ) {
	/* is a login cookie */
	if ( strncmp( de->d_name, "cosign=", 7 ) == 0 ) {
	    if (( rc = decision( de->d_name, &now, &cookie_itime )) < 0 ) {
		syslog( LOG_ERR, "decision failure: %s\n", de->d_name );
		continue;
	    }
	    for ( cur = &head; *cur != NULL; cur = &(*cur)->cl_next ) {
		if ( cookie_itime > (*cur)->cl_last_time ) {
		    if ( snet_writef( (*cur)->cl_sn, "%s %d\r\n",
			    de->d_name, cookie_itime ) < 0 ) {
			if ( snet_close( (*cur)->cl_sn ) != 0 ) {
			    syslog( LOG_ERR, "snet_close: %m\n" );
			}
			(*cur)->cl_sn = NULL;
			continue;
		    }
		}
	    }
	} else if ( strncmp( de->d_name, "cosign-", 7 ) == 0 ) {

	    if ( service_to_login( de->d_name, login ) != 0 ) {
		syslog( LOG_ERR, "service to login: %s\n", de->d_name );
		continue;
	    }
	    if (( rc = decision( login, &now, &cookie_itime )) < 0 ) {
		syslog( LOG_ERR, "decision failure: %s\n", login );
		continue;
	    }
	    if ( rc == 0 ) {
		if ( unlink( de->d_name ) != 0 ) {
		    syslog( LOG_ERR, "%s: %m\n", de->d_name );
		}
	    }
	} else {
	    continue;
	}
    }
    closedir( dirp );

    for ( cur = &head; *cur != NULL; cur = &(*cur)->cl_next ) {
	if ( (*cur)->cl_sn != NULL ) {
	    snet_writef( (*cur)->cl_sn, ".\r\n" );
	    if (( line = snet_getline_multi( (*cur)->cl_sn, logger, &tv ))
		     == NULL ) {
		if ( !snet_eof( (*cur)->cl_sn )) {
		    syslog( LOG_ERR, "snet_getline_multi: %m\n" );
		}
		if ( snet_close( (*cur)->cl_sn ) != 0 ) {
		    syslog( LOG_ERR, "snet_close: %m\n" );
		}
		(*cur)->cl_sn = NULL;
		continue;
	    }

	    if ( *line != '2' ) {
		syslog( LOG_ERR, "snet_getline_multi: %m\n" );
		if ( snet_close( (*cur)->cl_sn ) != 0 ) {
		    syslog( LOG_ERR, "snet_close: %m\n" );
		}
		(*cur)->cl_sn = NULL;
		continue;
	    }
	    (*cur)->cl_last_time = now.tv_sec;
	}

    }

	}
}

    int
decision( char *name, struct timeval *now, time_t *itime )
{
    struct cinfo	ci = { 0, 0, "\0","\0","\0", "\0","\0", 0, };
    int			rc;
    extern int		errno;


    if (( rc = read_cookie( name, &ci )) < 0 ) {
	syslog( LOG_ERR, "read_cookie error: %s", name );
	return( -1 );
    }

    /* login cookie gave us an ENOENT do we think it's gone */
    if ( rc == 1 ) {
	syslog( LOG_DEBUG, "%s already gone", name );
	return( 0 );
    }

    if ( !ci.ci_state ) {
	goto delete_stuff;
    }

    if ( now->tv_sec - ci.ci_itime  > idle ) {
	goto delete_stuff;
    }

    *itime = ci.ci_itime; 
    return( 1 );

delete_stuff:

    /* clean up ticket and file */
    if ( strcmp( ci.ci_krbtkt, "\0" ) != 0 ) {
	if ( unlink( ci.ci_krbtkt ) != 0 ) {
	    syslog( LOG_ERR, "%s: %m\n", ci.ci_krbtkt );
	}
    }
    if ( unlink( name ) != 0 ) {
	syslog( LOG_ERR, "%s: %m\n", name );
    } 

    return( 0 );
}
