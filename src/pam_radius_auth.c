/*
 * $Id: pam_radius_auth.c,v 1.39 2007/03/26 05:35:31 fcusack Exp $
 * pam_radius_auth
 *      Authenticate a user via a RADIUS session
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * The original pam_radius.c code is copyright (c) Cristian Gafton, 1996,
 *                                             <gafton@redhat.com>
 *
 * Some challenge-response code is copyright (c) CRYPTOCard Inc, 1998.
 *                                              All rights reserved.
 */

#define PAM_SM_AUTH
#define PAM_SM_PASSWORD
#define PAM_SM_SESSION

#include "pam_radius_auth.h"

#define DPRINT if (debug) _pam_log

/* logging */
static void _pam_log(pam_handle_t * pamh, int err, CONST char *format, ...)
{
	va_list args;

	va_start(args, format);
	pam_vsyslog(pamh, err, format, args);
	va_end(args);

}

/* argument parsing */
static int _pam_parse(pam_handle_t * pamh, int argc, CONST char **argv,
		      radius_conf_t * conf)
{
	int ctrl = 0;

	memset(conf, 0, sizeof(radius_conf_t));	/* ensure it's initialized */

	conf->conf_file = CONF_FILE;

	/* set the default prompt */
	snprintf(conf->prompt, MAXPROMPT, "%s: ", DEFAULT_PROMPT);

	/*
	 *      If either is not there, then we can't parse anything.
	 */
	if ((argc == 0) || (argv == NULL)) {
		return ctrl;
	}

	/* step through arguments */
	for (ctrl = 0; argc-- > 0; ++argv) {

		/* generic options */
		if (!strncmp(*argv, "conf=", 5)) {
			conf->conf_file = *argv + 5;

		} else if (!strcmp(*argv, "use_first_pass")) {
			ctrl |= PAM_USE_FIRST_PASS;

		} else if (!strcmp(*argv, "try_first_pass")) {
			ctrl |= PAM_TRY_FIRST_PASS;

		} else if (!strcmp(*argv, "skip_passwd")) {
			ctrl |= PAM_SKIP_PASSWD;

		} else if (!strncmp(*argv, "retry=", 6)) {
			conf->retries = atoi(*argv + 6);

		} else if (!strcmp(*argv, "localifdown")) {
			conf->localifdown = 1;

		} else if (!strncmp(*argv, "client_id=", 10)) {
			if (conf->client_id) {
				_pam_log(pamh, LOG_WARNING, "ignoring duplicate"
					 " '%s'", *argv);
			} else {
				conf->client_id = (char *)*argv + 10;	/* point to the client-id */
			}
		} else if (!strcmp(*argv, "accounting_bug")) {
			conf->accounting_bug = TRUE;

		} else if (!strcmp(*argv, "ruser")) {
			ctrl |= PAM_RUSER_ARG;

		} else if (!strcmp(*argv, "debug")) {
			ctrl |= PAM_DEBUG_ARG;
			conf->debug = TRUE;

		} else if (!strncmp(*argv, "prompt=", 7)) {
			if (!strncmp
			    (conf->prompt, (char *)*argv + 7, MAXPROMPT)) {
				_pam_log(pamh, LOG_WARNING,
					 "ignoring duplicate" " '%s'", *argv);
			} else {
				/* truncate excessive prompts to (MAXPROMPT - 3) length */
				if (strlen((char *)*argv + 7) >=
				    (MAXPROMPT - 3)) {
					*((char *)*argv + 7 + (MAXPROMPT - 3)) =
					    0;
				}
				/* set the new prompt */
				memset(conf->prompt, 0, sizeof(conf->prompt));
				snprintf(conf->prompt, MAXPROMPT, "%s: ",
					 (char *)*argv + 7);
			}

		} else if (!strcmp(*argv, "force_prompt")) {
			conf->force_prompt = TRUE;

		} else if (!strncmp(*argv, "max_challenge=", 14)) {
			conf->max_challenge = atoi(*argv + 14);

		} else {
			_pam_log(pamh, LOG_WARNING, "unrecognized option '%s'",
				 *argv);
		}
	}

	return ctrl;
}

/* Callback function used to free the saved return value for pam_setcred. */
void _int_free(pam_handle_t * pamh, void *x, int error_status)
{
	free(x);
}

/*************************************************************************
 * SMALL HELPER FUNCTIONS
 *************************************************************************/

/*
 * A strerror_r() wrapper function to deal with its nuisances.
 */
static void get_error_string(int errnum, char *buf, size_t buflen)
{
#if !defined(__GLIBC__) || ((_POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600) && ! _GNU_SOURCE)
	/* XSI version of strerror_r(). */
	int retval = strerror_r(errnum, buf, buflen);

	/* POSIX does not state what will happen to the buffer if the function fails.
	 * Put it into a known state rather than leave it possibly uninitialized. */
	if (retval != 0 && buflen > (size_t) 0) {
		buf[0] = '\0';
	}
#else
	/* GNU version of strerror_r(). */
	char tmp_buf[BUFFER_SIZE];
	char *retval = strerror_r(errnum, tmp_buf, sizeof(tmp_buf));

	snprintf(buf, buflen, "%s", retval);
#endif
}

/*
 * Return an IP address as a struct sockaddr *.
 */
static int get_ipaddr(char *host, struct sockaddr *addr, char *port)
{
	struct addrinfo hints;
	struct addrinfo *results;
	int r;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_ADDRCONFIG;

	r = getaddrinfo(host, port && port[0] ? port : NULL, &hints, &results);
	if (r == 0) {
		memcpy(addr, results->ai_addr, results->ai_addrlen);
		freeaddrinfo(results);
	}

	return r;
}

/*
 * take server->hostname, and convert it to server->ip
 */
static int host2server(int debug, radius_server_t * server)
{
	char hostbuffer[256];
	char tmp[256];
	char *hostname;
	char *portstart;
	char *p, *port;
	int r, n;

	/* hostname might be [ipv6::address] */
	strncpy(hostbuffer, server->hostname, sizeof(hostbuffer) - 1);
	hostbuffer[sizeof(hostbuffer) - 1] = 0;
	hostname = hostbuffer;
	portstart = hostbuffer;
	if (hostname[0] == '[') {
		if ((p = strchr(hostname, ']')) != NULL) {
			hostname++;
			*p++ = 0;
			portstart = p;
		}
	}
	if ((port = strchr(portstart, ':')) != NULL) {
		*port++ = '\0';
		if (isdigit((unsigned char)*port) && server->accounting) {
			if (sscanf(port, "%d", &n) == 1) {
				snprintf(tmp, sizeof(tmp), "%d", n + 1);
				port = tmp;
			}
		}
	} else {
		if (server->accounting)
			port = "radacct";
		else
			port = "radius";
	}

	server->ip = (struct sockaddr *)&server->ip_storage;
	r = get_ipaddr(hostname, server->ip, port);
	return r;
}

/*
 * Do XOR of two buffers.
 */
static unsigned char *xor(unsigned char *p, unsigned char *q, int length)
{
	int i;
	unsigned char *retval = p;

	for (i = 0; i < length; i++) {
		*(p++) ^= *(q++);
	}
	return retval;
}

/**************************************************************************
 * MID-LEVEL RADIUS CODE
 **************************************************************************/

/*
 * get a pseudo-random vector.
 */
static void get_random_vector(unsigned char *vector)
{
#ifdef linux
	int fd = open("/dev/urandom", O_RDONLY);	/* Linux: get *real* random numbers */
	int total = 0;
	if (fd >= 0) {
		while (total < AUTH_VECTOR_LEN) {
			int bytes =
			    read(fd, vector + total, AUTH_VECTOR_LEN - total);
			if (bytes <= 0)
				break;	/* oops! Error */
			total += bytes;
		}
		close(fd);
	}

	if (total != AUTH_VECTOR_LEN)
#endif
	{			/* do this *always* on other platforms */
		MD5_CTX my_md5;
		struct timeval tv;
		struct timezone tz;
		static unsigned int session = 0;	/* make the number harder to guess */

		/* Use the time of day with the best resolution the system can
		   give us -- often close to microsecond accuracy. */
		gettimeofday(&tv, &tz);

		if (session == 0) {
			session = getppid();	/* (possibly) hard to guess information */
		}

		tv.tv_sec ^= getpid() * session++;

		/* Hash things to get maybe cryptographically strong pseudo-random numbers */
		MD5Init(&my_md5);
		MD5Update(&my_md5, (unsigned char *)&tv, sizeof(tv));
		MD5Update(&my_md5, (unsigned char *)&tz, sizeof(tz));
		MD5Final(vector, &my_md5);	/* set the final vector */
	}
}

/*
 * RFC 2139 says to do generate the accounting request vector this way.
 * However, the Livingston 1.16 server doesn't check it.	The Cistron
 * server (http://home.cistron.nl/~miquels/radius/) does, and this code
 * seems to work with it.	It also works with Funk's Steel-Belted RADIUS.
 */
static void get_accounting_vector(AUTH_HDR * request, radius_server_t * server)
{
	MD5_CTX my_md5;
	int secretlen = strlen(server->secret);
	int len = ntohs(request->length);

	memset(request->vector, 0, AUTH_VECTOR_LEN);
	MD5Init(&my_md5);
	memcpy(((char *)request) + len, server->secret, secretlen);

	MD5Update(&my_md5, (unsigned char *)request, len + secretlen);
	MD5Final(request->vector, &my_md5);	/* set the final vector */
}

/*
 * Verify the response from the server
 */
static int verify_packet(char *secret, AUTH_HDR * response, AUTH_HDR * request)
{
	MD5_CTX my_md5;
	unsigned char calculated[AUTH_VECTOR_LEN];
	unsigned char reply[AUTH_VECTOR_LEN];

	/*
	 * We could dispense with the memcpy, and do MD5's of the packet
	 * + vector piece by piece.     This is easier understand, and maybe faster.
	 */
	memcpy(reply, response->vector, AUTH_VECTOR_LEN);	/* save the reply */
	memcpy(response->vector, request->vector, AUTH_VECTOR_LEN);	/* sent vector */

	/* MD5(response packet header + vector + response packet data + secret) */
	MD5Init(&my_md5);
	MD5Update(&my_md5, (unsigned char *)response, ntohs(response->length));

	/*
	 * This next bit is necessary because of a bug in the original Livingston
	 * RADIUS server.       The authentication vector is *supposed* to be MD5'd
	 * with the old password (as the secret) for password changes.
	 * However, the old password isn't used.        The "authentication" vector
	 * for the server reply packet is simply the MD5 of the reply packet.
	 * Odd, the code is 99% there, but the old password is never copied
	 * to the secret!
	 */
	if (*secret) {
		MD5Update(&my_md5, (unsigned char *)secret, strlen(secret));
	}

	MD5Final(calculated, &my_md5);	/* set the final vector */

	/* Did he use the same random vector + shared secret? */
	if (memcmp(calculated, reply, AUTH_VECTOR_LEN) != 0) {
		return FALSE;
	}
	return TRUE;
}

/*
 * Find an attribute in a RADIUS packet.	Note that the packet length
 * is *always* kept in network byte order.
 */
static attribute_t *find_attribute(AUTH_HDR * response, unsigned char type)
{
	attribute_t *attr = (attribute_t *) & response->data;

	int len = ntohs(response->length) - AUTH_HDR_LEN;

	while (attr->attribute != type) {
		if ((len -= attr->length) <= 0) {
			return NULL;	/* not found */
		}
		attr = (attribute_t *) ((char *)attr + attr->length);
	}

	return attr;
}

/*
 * Add an attribute to a RADIUS packet.
 */
static void add_attribute(AUTH_HDR * request, unsigned char type,
			  CONST unsigned char *data, int length)
{
	attribute_t *p;

	p = (attribute_t *) ((unsigned char *)request + ntohs(request->length));
	p->attribute = type;
	p->length = length + 2;	/* the total size of the attribute */
	request->length = htons(ntohs(request->length) + p->length);
	memcpy(p->data, data, length);
}

/*
 * Add an integer attribute to a RADIUS packet.
 */
static void add_int_attribute(AUTH_HDR * request, unsigned char type, int data)
{
	int value = htonl(data);

	add_attribute(request, type, (unsigned char *)&value, sizeof(int));
}

static void add_nas_ip_address(AUTH_HDR * request, char *hostname)
{
	struct addrinfo hints;
	struct addrinfo *ai;
	int v4seen = 0, v6seen = 0;
	int r;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_ADDRCONFIG;

	r = getaddrinfo(hostname, NULL, &hints, &ai);
	if (r != 0)
		return;

	while (ai != NULL) {
		if (!v4seen && ai->ai_family == AF_INET) {
			v4seen = 1;
			r = ((struct sockaddr_in *)ai->ai_addr)->sin_addr.
			    s_addr;
			add_int_attribute(request, PW_NAS_IP_ADDRESS, ntohl(r));
		}
		if (!v6seen && ai->ai_family == AF_INET6) {
			v6seen = 1;
			add_attribute(request, PW_NAS_IPV6_ADDRESS,
				      (unsigned char *)&((struct sockaddr_in6 *)
							 ai->ai_addr)->
				      sin6_addr, 16);
		}
		ai = ai->ai_next;
	}
	freeaddrinfo(ai);
}

/*
 * Add a RADIUS password attribute to the packet.	Some magic is done here.
 *
 * If it's an PW_OLD_PASSWORD attribute, it's encrypted using the encrypted
 * PW_PASSWORD attribute as the initialization vector.
 *
 * If the password attribute already exists, it's over-written.	This allows
 * us to simply call add_password to update the password for different
 * servers.
 */
static void add_password(AUTH_HDR * request, unsigned char type,
			 CONST char *password, char *secret)
{
	MD5_CTX md5_secret, my_md5;
	unsigned char misc[AUTH_VECTOR_LEN];
	int i;
	int length = strlen(password);
	unsigned char hashed[256 + AUTH_PASS_LEN];	/* can't be longer than this */
	unsigned char *vector;
	attribute_t *attr;

	if (length > MAXPASS) {	/* shorten the password for now */
		length = MAXPASS;
	}

	memcpy(hashed, password, length);
	memset(hashed + length, 0, sizeof(hashed) - length);

	if (length == 0) {
		length = AUTH_PASS_LEN;	/* 0 maps to 16 */
	}
	if ((length & (AUTH_PASS_LEN - 1)) != 0) {
		length += (AUTH_PASS_LEN - 1);	/* round it up */
		length &= ~(AUTH_PASS_LEN - 1);	/* chop it off */
	}
	/* 16*N maps to itself */
	attr = find_attribute(request, PW_PASSWORD);

	if (type == PW_PASSWORD) {
		vector = request->vector;
	} else {
		vector = attr->data;	/* attr CANNOT be NULL here. */
	}

	/* ************************************************************ */
	/* encrypt the password */
	/* password : e[0] = p[0] ^ MD5(secret + vector) */
	MD5Init(&md5_secret);
	MD5Update(&md5_secret, (unsigned char *)secret, strlen(secret));
	my_md5 = md5_secret;	/* so we won't re-do the hash later */
	MD5Update(&my_md5, vector, AUTH_VECTOR_LEN);
	MD5Final(misc, &my_md5);	/* set the final vector */
	xor(hashed, misc, AUTH_PASS_LEN);

	/* For each step through, e[i] = p[i] ^ MD5(secret + e[i-1]) */
	for (i = 1; i < (length >> 4); i++) {
		my_md5 = md5_secret;	/* grab old value of the hash */
		MD5Update(&my_md5, &hashed[(i - 1) * AUTH_PASS_LEN],
			  AUTH_PASS_LEN);
		MD5Final(misc, &my_md5);	/* set the final vector */
		xor(&hashed[i * AUTH_PASS_LEN], misc, AUTH_PASS_LEN);
	}

	if (type == PW_OLD_PASSWORD) {
		attr = find_attribute(request, PW_OLD_PASSWORD);
	}

	if (!attr) {
		add_attribute(request, type, hashed, length);
	} else {
		memcpy(attr->data, hashed, length);	/* overwrite the packet */
	}
}

static void cleanup(radius_server_t * server)
{
	radius_server_t *next;

	while (server) {
		next = server->next;
		_pam_drop(server->hostname);
		_pam_forget(server->secret);
		_pam_drop(server);
		server = next;
	}
}

/*
 * allocate and open a local port for communication with the RADIUS
 * server
 */
static int initialize(pam_handle_t * pamh, radius_conf_t * conf, int accounting)
{
	struct sockaddr_storage salocal;
	struct sockaddr_storage salocal4;
	struct sockaddr_storage salocal6;
	char hostname[BUFFER_SIZE];
	char secret[BUFFER_SIZE];
	char vrfname[64];

	char buffer[BUFFER_SIZE];
	char *p;
	FILE *fserver;
	radius_server_t *server = NULL, *tmp;
	int timeout;
	int line = 0, scancnt;
	char src_ip[MAX_IP_LEN];
	int seen_v6 = 0;

	memset(&salocal4, 0, sizeof(salocal4));
	memset(&salocal6, 0, sizeof(salocal6));
	((struct sockaddr *)&salocal4)->sa_family = AF_INET;
	((struct sockaddr *)&salocal6)->sa_family = AF_INET6;

	/* the first time around, read the configuration file */
	if ((fserver = fopen(conf->conf_file, "r")) == (FILE *) NULL) {
		char error_string[BUFFER_SIZE];
		get_error_string(errno, error_string, sizeof(error_string));
		_pam_log(pamh, LOG_ERR, "Could not open configuration file %s:"
			 " %s\n", conf->conf_file, error_string);
		return PAM_ABORT;
	}

	vrfname[0] = '\0';
	while (!feof(fserver)
	       && (fgets(buffer, sizeof(buffer), fserver) != (char *)NULL)
	       && (!ferror(fserver))) {
		line++;
		p = buffer;

		/*
		 *      Skip whitespace
		 */
		while ((*p == ' ') || (*p == '\t'))
			p++;

		/*
		 *      Skip blank lines and comments.
		 */
		if ((*p == '\r') || (*p == '\n') || (*p == '#'))
			continue;

		/*
		 *      Error out if the text is too long.
		 */
		if (!*p) {
			_pam_log(pamh, LOG_ERR, "ERROR reading %s, line %d:"
				 " Line too long\n", conf->conf_file, line);
			break;
		}

		scancnt =
		    sscanf(p, "%s %s %d %s", hostname, secret, &timeout,
			   src_ip);

		/* is it the name of a vrf we should bind to? */
		if (!strcmp(hostname, "vrf-name")) {
			if (scancnt < 2)
				_pam_log(pamh, LOG_ERR,
					 "ERROR reading %s, line %d:"
					 " only %d fields\n", conf->conf_file,
					 line, scancnt);
			else
				snprintf(vrfname, sizeof vrfname, "%s", secret);
			continue;
		}

		/*      allow setting debug in config file as well */
		if (!strcmp(hostname, "debug")) {
			if (scancnt < 1)
				_pam_log(pamh, LOG_ERR,
					 "ERROR reading %s, line %d:"
					 " only %d fields\n", conf->conf_file,
					 line, scancnt);
			else
				conf->debug = 1;
			continue;
		}

		if (scancnt < 2) {
			_pam_log(pamh, LOG_ERR, "ERROR reading %s, line %d:"
				 " only %d fields\n", conf->conf_file, line,
				 scancnt);
			continue;	/* invalid line */
		}
		if (scancnt < 4) {
			src_ip[0] = 0;
			if (scancnt < 3)
				timeout = 3;	/*  default timeout */
		}

		/* read it in and save the data */
		tmp = malloc(sizeof(radius_server_t));
		if (server) {
			server->next = tmp;
			server = server->next;
		} else {
			conf->server = tmp;
			server = tmp;	/* first time */
		}

		/* sometime later do memory checks here */
		server->hostname = strdup(hostname);
		server->secret = strdup(secret);
		server->accounting = accounting;

		memset(&server->ip, 0, sizeof server->ip);
		if ((timeout < 1) || (timeout > 60)) {
			server->timeout = 3;
		} else {
			server->timeout = timeout;
		}
		server->next = NULL;

		if (src_ip[0]) {
			memset(&salocal, 0, sizeof(salocal));
			get_ipaddr(src_ip, (struct sockaddr *)&salocal, NULL);
			switch (salocal.ss_family) {
			case AF_INET:
				memcpy(&salocal4, &salocal, sizeof(salocal));
				break;
			case AF_INET6:
				seen_v6 = 1;
				memcpy(&salocal6, &salocal, sizeof(salocal));
				break;
			}
		}
	}
	fclose(fserver);

	if (!server) {		/* no server found, die a horrible death */
		_pam_log(pamh, LOG_ERR, "No RADIUS server found in"
			 " configuration file %s\n", conf->conf_file);
		return PAM_AUTHINFO_UNAVAIL;
	}

	/*
	 *      FIXME- we could have different source-ips for different servers, so
	 *      sockfd should probably be in the server struct, not in the conf struct.
	 */

	/* open a socket.       Dies if it fails */
	conf->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (conf->sockfd < 0) {
		char error_string[BUFFER_SIZE];
		get_error_string(errno, error_string, sizeof(error_string));
		_pam_log(pamh, LOG_ERR, "Failed to open RADIUS socket: %s\n",
			 error_string);
		return PAM_AUTHINFO_UNAVAIL;
	}
#ifndef HAVE_POLL_H
	if (conf->sockfd >= FD_SETSIZE) {
		_pam_log(pamh, LOG_ERR, "Unusable socket, FD is larger than"
			 " %d\n", FD_SETSIZE);
		close(conf->sockfd);
		return PAM_AUTHINFO_UNAVAIL;
	}
#endif

	if (vrfname[0]) {
		/*  do not fail if the bind fails, connection may succeed */
		if (setsockopt(conf->sockfd, SOL_SOCKET, SO_BINDTODEVICE,
			       vrfname, strlen(vrfname) + 1) < 0)
			_pam_log(pamh, LOG_WARNING,
				 "Binding IPv4 socket to VRF %s" " failed: %m",
				 vrfname);
		else if (conf->debug)
			_pam_log(pamh, LOG_DEBUG, "Configured IPv4 vrf as: %s",
				 vrfname);
	}

	/* set up the local end of the socket communications */
	if (bind
	    (conf->sockfd, (struct sockaddr *)&salocal4,
	     sizeof(struct sockaddr_in)) < 0) {
		char error_string[BUFFER_SIZE];
		get_error_string(errno, error_string, sizeof(error_string));
		_pam_log(pamh, LOG_ERR, "Failed binding to port: %s",
			 error_string);
		close(conf->sockfd);
		return PAM_AUTHINFO_UNAVAIL;
	}

	/* open a IPv6 socket.  Dies if it fails */
	conf->sockfd6 = socket(AF_INET6, SOCK_DGRAM, 0);
	if (conf->sockfd6 < 0) {
		if (!seen_v6)
			return PAM_SUCCESS;
		char error_string[BUFFER_SIZE];
		get_error_string(errno, error_string, sizeof(error_string));
		_pam_log(pamh, LOG_ERR, "Failed to open RADIUS IPv6 socket:"
			 " %s\n", error_string);
		close(conf->sockfd);
		return PAM_AUTHINFO_UNAVAIL;
	}
#ifndef HAVE_POLL_H
	if (conf->sockfd6 >= FD_SETSIZE) {
		_pam_log(pamh, LOG_ERR, "Unusable socket, FD is larger than"
			 " %d\n", FD_SETSIZE);
		close(conf->sockfd);
		close(conf->sockfd6);
		return PAM_AUTHINFO_UNAVAIL;
	}
#endif
	if (vrfname[0]) {
		/*  do not fail if the bind fails, connection may succeed */
		if (setsockopt(conf->sockfd6, SOL_SOCKET, SO_BINDTODEVICE,
			       vrfname, strlen(vrfname) + 1) < 0)
			_pam_log(pamh, LOG_WARNING,
				 "Binding IPv6 socket to VRF %s" " failed: %m",
				 vrfname);
		else if (conf->debug)
			_pam_log(pamh, LOG_DEBUG, "Configured IPv6 vrf as: %s",
				 vrfname);
	}

	/* set up the local end of the socket communications */
	if (bind
	    (conf->sockfd6, (struct sockaddr *)&salocal6,
	     sizeof(struct sockaddr_in6)) < 0) {
		char error_string[BUFFER_SIZE];
		get_error_string(errno, error_string, sizeof(error_string));
		_pam_log(pamh, LOG_ERR, "Failed binding to IPv6 port: %s",
			 error_string);
		close(conf->sockfd);
		close(conf->sockfd6);
		return PAM_AUTHINFO_UNAVAIL;
	}

	return PAM_SUCCESS;
}

/*
 * Helper function for building a radius packet.
 * It initializes *some* of the header, and adds common attributes.
 */
static void build_radius_packet(AUTH_HDR * request, CONST char *user,
				CONST char *password, radius_conf_t * conf)
{
	char hostname[256];

	hostname[0] = '\0';
	gethostname(hostname, sizeof(hostname) - 1);

	request->length = htons(AUTH_HDR_LEN);

	if (password) {		/* make a random authentication req vector */
		get_random_vector(request->vector);
	}

	add_attribute(request, PW_USER_NAME, (unsigned char *)user,
		      strlen(user));

	/*
	 *      Add a password, if given.
	 */
	if (password) {
		add_password(request, PW_PASSWORD, password,
			     conf->server->secret);

		/*
		 *      Add a NULL password to non-accounting requests.
		 */
	} else if (request->code != PW_ACCOUNTING_REQUEST) {
		add_password(request, PW_PASSWORD, "", conf->server->secret);
	}

	/* Perhaps add NAS IP Address (and v6 version) */
	add_nas_ip_address(request, hostname);

	/* There's always a NAS identifier */
	if (conf->client_id && *conf->client_id) {
		add_attribute(request, PW_NAS_IDENTIFIER,
			      (unsigned char *)conf->client_id,
			      strlen(conf->client_id));
	}

	/*
	 *      Add in the port (pid) and port type (virtual).
	 *
	 *      We might want to give the TTY name here, too.
	 */
	add_int_attribute(request, PW_NAS_PORT_ID, getpid());
	add_int_attribute(request, PW_NAS_PORT_TYPE, PW_NAS_PORT_TYPE_VIRTUAL);
}

/*
 * Talk RADIUS to a server.
 * Send a packet and get the response
 */
static int talk_radius(radius_conf_t * conf, AUTH_HDR * request,
		       AUTH_HDR * response, char *password, char *old_password,
		       int tries, pam_handle_t * pamh)
{
	int total_length;
#ifdef HAVE_POLL_H
	struct pollfd pollfds[1];
#else
	fd_set set;
#endif
	struct timeval tv;

	time_t now, end;
	int rcode;
	radius_server_t *server = conf->server;
	int ok;
	int server_tries;
	int retval;
	int sockfd;

	/* ************************************************************ */
	/* Now that we're done building the request, we can send it */

	/*
	   Hmm... on password change requests, all of the found server information
	   could be saved with a pam_set_data(), which means even the radius_conf_t
	   information will have to be malloc'd at some point

	   On the other hand, we could just try all of the servers again in
	   sequence, on the off chance that one may have ended up fixing itself.

	 */

	/* loop over all available servers */
	while (server != NULL) {
		/* clear the response */
		memset(response, 0, sizeof(AUTH_HDR));

		/* only look up IP information as necessary */
		retval = host2server(conf->debug, server);
		if (retval != 0) {
			_pam_log(pamh, LOG_ERR,
				 "Failed looking up IP address for RADIUS server %s (error=%s)",
				 server->hostname, gai_strerror(retval));
			ok = FALSE;
			goto next;	/* skip to the next server */
		}

		if (!password) {	/* make an RFC 2139 p6 request authenticator */
			get_accounting_vector(request, server);
		}

		sockfd =
		    server->ip->sa_family ==
		    AF_INET ? conf->sockfd : conf->sockfd6;
		total_length = ntohs(request->length);
		server_tries = tries;
 send:
		/* send the packet */
		if (sendto(sockfd, (char *)request, total_length, 0,
			   server->ip, sizeof(struct sockaddr_storage)) < 0) {
			char error_string[BUFFER_SIZE];
			get_error_string(errno, error_string,
					 sizeof(error_string));
			_pam_log(pamh, LOG_ERR,
				 "Error sending RADIUS packet to"
				 " server %s: %s", server->hostname,
				 error_string);
			ok = FALSE;
			goto next;	/* skip to the next server */
		}

		/* ************************************************************ */
		/* Wait for the response, and verify it. */
		time(&now);

		tv.tv_sec = server->timeout;	/* wait for the specified time */
		tv.tv_usec = 0;
		end = now + tv.tv_sec;

#ifdef HAVE_POLL_H
		pollfds[0].fd = sockfd;	/* wait only for the RADIUS UDP socket */
		pollfds[0].events = POLLIN;	/* wait for data to read */
#else
		FD_ZERO(&set);	/* clear out the set */
		FD_SET(sockfd, &set);	/* wait only for the RADIUS UDP socket */
#endif

		/* loop, waiting for the network to return data */
		ok = TRUE;
		while (ok) {
#ifdef HAVE_POLL_H
			rcode =
			    poll((struct pollfd *)&pollfds, 1,
				 tv.tv_sec * 1000);
#else
			rcode = select(sockfd + 1, &set, NULL, NULL, &tv);
#endif

			/* timed out */
			if (rcode == 0) {
				_pam_log(pamh, LOG_ERR, "RADIUS server %s"
					 " failed to respond",
					 server->hostname);
				if (--server_tries) {
					goto send;
				}
				ok = FALSE;
				break;	/* exit from the loop */
			} else if (rcode < 0) {

				/* poll returned an error */
				if (errno == EINTR) {	/* we were interrupted */
					time(&now);

					if (now > end) {
						_pam_log(pamh, LOG_ERR,
							 "RADIUS server %s "
							 "failed to respond",
							 server->hostname);
						if (--server_tries)
							goto send;
						ok = FALSE;
						break;	/* exit from the loop */
					}

					tv.tv_sec = end - now;
					if (tv.tv_sec == 0) {	/* keep waiting */
						tv.tv_sec = 1;
					}
				} else {	/* not an interrupt, it was a real error */
					char error_string[BUFFER_SIZE];
					get_error_string(errno, error_string,
							 sizeof(error_string));
					_pam_log(pamh, LOG_ERR,
						 "Error waiting"
						 " for response from RADIUS"
						 " server %s: %s",
						 server->hostname,
						 error_string);
					ok = FALSE;
					break;
				}

				/* the call returned OK */
#ifdef HAVE_POLL_H
			} else if (pollfds[0].revents & POLLIN) {
#else
			} else if (FD_ISSET(sockfd, &set)) {
#endif

				/* try to receive some data */
				if ((total_length =
				     recvfrom(sockfd, (void *)response,
					      BUFFER_SIZE, 0, NULL,
					      NULL)) < 0) {
					char error_string[BUFFER_SIZE];
					get_error_string(errno, error_string,
							 sizeof(error_string));
					_pam_log(pamh, LOG_ERR,
						 "error reading"
						 " RADIUS packet from server"
						 " %s: %s", server->hostname,
						 error_string);
					ok = FALSE;
					break;

					/* there's data, see if it's valid */
				} else {
					char *p = server->secret;

					if ((ntohs(response->length) !=
					     total_length)
					    || (ntohs(response->length) >
						BUFFER_SIZE)) {
						_pam_log(pamh, LOG_ERR,
							 "RADIUS packet from "
							 "server %s is "
							 "corrupted",
							 server->hostname);
						ok = FALSE;
						break;
					}

					/* Check if we have the data OK. We should also check request->id */
					if (password) {
						if (old_password) {
#ifdef LIVINGSTON_PASSWORD_VERIFY_BUG_FIXED
							p = old_password;	/* what it should be */
#else
							p = "";	/* what it really is */
#endif
						}
						/*
						 * RFC 2139 p.6 says not do do this, but the Livingston 1.16
						 * server disagrees.    If the user says he wants the bug, give in.
						 */
					} else {	/* authentication request */
						if (conf->accounting_bug) {
							p = "";
						}
					}

					if (!verify_packet
					    (p, response, request)) {
						_pam_log(pamh, LOG_ERR,
							 "packet"
							 " from RADIUS server %s"
							 " failed verification:"
							 " The shared secret is"
							 " probably incorrect.",
							 server->hostname);
						ok = FALSE;
						break;
					}

					/*
					 * Check that the response ID matches the request ID.
					 */
					if (response->id != request->id) {
						_pam_log(pamh, LOG_WARNING,
							 "Response packet ID %d"
							 " does not match the"
							 " request packet ID"
							 " %d: verification of"
							 " packet fails",
							 response->id,
							 request->id);
						ok = FALSE;
						break;
					}
				}

				/*
				 * Whew! The poll is done. It hasn't timed out, or errored out.
				 * It's our descriptor. We've got some data. It's the right size.
				 * The packet is valid.
				 * NOW, we can skip out of the loop, and process the packet
				 */
				break;
			}
			/* otherwise, we've got data on another descriptor, keep checking the network */
		}

		/* go to the next server if this one didn't respond */
 next:
		if (!ok) {
			radius_server_t *old;	/* forget about this server */

			old = server;
			server = server->next;
			conf->server = server;

			_pam_forget(old->secret);
			free(old->hostname);
			free(old);

			if (server) {	/* if there's more servers to check */
				/* get a new authentication vector, and update the passwords */
				get_random_vector(request->vector);
				request->id = request->vector[0];

				/* update passwords, as appropriate */
				if (password) {
					get_random_vector(request->vector);
					if (old_password) {	/* password change request */
						add_password(request,
							     PW_PASSWORD,
							     password,
							     old_password);
						add_password(request,
							     PW_OLD_PASSWORD,
							     old_password,
							     old_password);
					} else {	/* authentication request */
						add_password(request,
							     PW_PASSWORD,
							     password,
							     server->secret);
					}
				}
			}
			continue;

		} else {
			/* we've found one that does respond, forget about the other servers */
			cleanup(server->next);
			server->next = NULL;
			break;
		}
	}

	if (!server) {
		_pam_log(pamh, LOG_ERR, "All RADIUS servers failed to respond");
		if (conf->localifdown)
			retval = PAM_IGNORE;
		else
			retval = PAM_AUTHINFO_UNAVAIL;
	} else {
		retval = PAM_SUCCESS;
	}

	return retval;
}

/**************************************************************************
 * MIDLEVEL PAM CODE
 **************************************************************************/

/* this is our front-end for module-application conversations */

#undef PAM_FAIL_CHECK
#define PAM_FAIL_CHECK if (retval != PAM_SUCCESS) { return retval; }

static int rad_converse(pam_handle_t * pamh, int msg_style, char *message,
			char **password)
{
	CONST struct pam_conv *conv;
	struct pam_message resp_msg;
	CONST struct pam_message *msg[1];
	struct pam_response *resp = NULL;
	int retval;

	resp_msg.msg_style = msg_style;
	resp_msg.msg = message;
	msg[0] = &resp_msg;

	/* grab the password */
	retval = pam_get_item(pamh, PAM_CONV, (CONST void **)&conv);
	PAM_FAIL_CHECK;

	retval = conv->conv(1, msg, &resp, conv->appdata_ptr);
	PAM_FAIL_CHECK;

	if (password) {		/* assume msg.type needs a response */
		/* I'm not sure if this next bit is necessary on Linux */
#ifdef sun
		/* NULL response, fail authentication */
		if ((resp == NULL) || (resp->resp == NULL)) {
			return PAM_SYSTEM_ERR;
		}
#endif

		*password = resp->resp;
		free(resp);
	}

	return PAM_SUCCESS;
}

/**************************************************************************
 * GENERAL CODE
 **************************************************************************/

#undef PAM_FAIL_CHECK
#define PAM_FAIL_CHECK if (retval != PAM_SUCCESS) { \
	int *pret = malloc(sizeof(int)); \
	*pret = retval;	\
	pam_set_data(pamh, "rad_setcred_return", (void *) pret, _int_free);	\
	return retval; }

PAM_EXTERN int pam_sm_authenticate(pam_handle_t * pamh, int flags, int argc,
				   CONST char **argv)
{
	CONST char *user;
	CONST char *userinfo;
	char *password = NULL;
	CONST char *rhost;
	char *resp2challenge = NULL;
	int ctrl;
	int debug;
	int retval = PAM_AUTH_ERR;
	int num_challenge = 0;

	char recv_buffer[4096];
	char send_buffer[4096];
	AUTH_HDR *request = (AUTH_HDR *) send_buffer;
	AUTH_HDR *response = (AUTH_HDR *) recv_buffer;
	radius_conf_t config;

	ctrl = _pam_parse(pamh, argc, argv, &config);
	debug = config.debug;

	/* grab the user name */
	retval = pam_get_user(pamh, &user, NULL);
	PAM_FAIL_CHECK;

	/* check that they've entered something, and not too long, either */
	if ((user == NULL) || (strlen(user) > MAXPWNAM)) {
		int *pret = malloc(sizeof(int));
		*pret = PAM_USER_UNKNOWN;
		pam_set_data(pamh, "rad_setcred_return", (void *)pret,
			     _int_free);

		DPRINT(pamh, LOG_DEBUG, "User name was NULL, or too long");
		return PAM_USER_UNKNOWN;
	}
	DPRINT(pamh, LOG_DEBUG, "Got user name %s", user);

	if (ctrl & PAM_RUSER_ARG) {
		retval =
		    pam_get_item(pamh, PAM_RUSER, (CONST void **)&userinfo);
		PAM_FAIL_CHECK;
		DPRINT(pamh, LOG_DEBUG, "Got PAM_RUSER name %s", userinfo);

		if (!strcmp("root", user)) {
			user = userinfo;
			DPRINT(pamh, LOG_DEBUG, "Username now %s from ruser",
			       user);
		} else {
			DPRINT(pamh, LOG_DEBUG,
			       "Skipping ruser for non-root auth");
		}
	}

	/*
	 * Get the IP address of the authentication server
	 * Then, open a socket, and bind it to a port
	 */
	retval = initialize(pamh, &config, FALSE);
	PAM_FAIL_CHECK;

	/*
	 * If there's no client id specified, use the service type, to help
	 * keep track of which service is doing the authentication.
	 */
	if (!config.client_id) {
		retval =
		    pam_get_item(pamh, PAM_SERVICE,
				 (CONST void **)&config.client_id);
		PAM_FAIL_CHECK;
	}

	/* now we've got a socket open, so we've got to clean it up on error */
#undef PAM_FAIL_CHECK
#define PAM_FAIL_CHECK if (retval != PAM_SUCCESS) {goto do_next; }

	/* build and initialize the RADIUS packet */
	request->code = PW_AUTHENTICATION_REQUEST;
	get_random_vector(request->vector);
	request->id = request->vector[0];	/* this should be evenly distributed */

	/* grab the password (if any) from the previous authentication layer */
	if (!config.force_prompt) {
		DPRINT(pamh, LOG_DEBUG, "ignore last_pass, force_prompt set");
		retval =
		    pam_get_item(pamh, PAM_AUTHTOK, (CONST void **)&password);
		PAM_FAIL_CHECK;
	}

	if (password) {
		password = strdup(password);
		DPRINT(pamh, LOG_DEBUG, "Got password %s", password);
	}

	/* no previous password: maybe get one from the user */
	if (!password) {
		if (ctrl & PAM_USE_FIRST_PASS) {
			retval = PAM_AUTH_ERR;	/* use one pass only, stopping if it fails */
			goto do_next;
		}

		/* check to see if we send a NULL password the first time around */
		if (!(ctrl & PAM_SKIP_PASSWD)) {
			retval =
			    rad_converse(pamh, PAM_PROMPT_ECHO_OFF,
					 config.prompt, &password);
			PAM_FAIL_CHECK;

		} else {
			password = strdup("");
		}
	}
	/* end of password == NULL */
	build_radius_packet(request, user, password, &config);
	/* not all servers understand this service type, but some do */
	add_int_attribute(request, PW_USER_SERVICE_TYPE, PW_AUTHENTICATE_ONLY);

	/*
	 *      Tell the server which host the user is coming from.
	 *
	 *      Note that this is NOT the IP address of the machine running PAM!
	 *      It's the IP address of the client.
	 */
	retval = pam_get_item(pamh, PAM_RHOST, (CONST void **)&rhost);
	PAM_FAIL_CHECK;
	if (rhost) {
		add_attribute(request, PW_CALLING_STATION_ID,
			      (unsigned char *)rhost, strlen(rhost));
	}

	DPRINT(pamh, LOG_DEBUG, "Sending RADIUS request code %d",
	       request->code);

	retval = talk_radius(&config, request, response, password, NULL,
			     config.retries + 1, pamh);
	PAM_FAIL_CHECK;

	DPRINT(pamh, LOG_DEBUG, "Got RADIUS response code %d", response->code);

	/*
	 *      If we get an authentication failure, and we sent a NULL password,
	 *      ask the user for one and continue.
	 *
	 *      If we get an access challenge, then do a response, for as many
	 *      challenges as we receive.
	 */
	while (response->code == PW_ACCESS_CHALLENGE) {
		attribute_t *a_state, *a_reply;
		char challenge[BUFFER_SIZE];

		/* Now we do a bit more work: challenge the user, and get a response */
		if (((a_state = find_attribute(response, PW_STATE)) == NULL) ||
		    ((a_reply =
		      find_attribute(response, PW_REPLY_MESSAGE)) == NULL)) {
			/* Actually, State isn't required. */
			_pam_log(pamh, LOG_ERR, "RADIUS Access-Challenge"
				 " received with State or Reply-Message"
				 " missing");
			retval = PAM_AUTHINFO_UNAVAIL;
			goto do_next;
		}

		/*
		 *      Security fixes.
		 */
		if ((a_state->length <= 2) || (a_reply->length <= 2)) {
			_pam_log(pamh, LOG_ERR, "RADIUS Access-Challenge"
				 " received with invalid State or"
				 " Reply-Message");
			retval = PAM_AUTHINFO_UNAVAIL;
			goto do_next;
		}

		memcpy(challenge, a_reply->data, a_reply->length - 2);
		challenge[a_reply->length - 2] = 0;

		/* It's full challenge-response, we should have echo on */
		retval =
		    rad_converse(pamh, PAM_PROMPT_ECHO_ON, challenge,
				 &resp2challenge);
		PAM_FAIL_CHECK;

		/* now that we've got a response, build a new radius packet */
		build_radius_packet(request, user, resp2challenge, &config);
		/* request->code is already PW_AUTHENTICATION_REQUEST */
		request->id++;	/* one up from the request */

		if (rhost) {
			add_attribute(request, PW_CALLING_STATION_ID,
				      (unsigned char *)rhost, strlen(rhost));
		}

		/* copy the state over from the servers response */
		add_attribute(request, PW_STATE, a_state->data,
			      a_state->length - 2);

		retval =
		    talk_radius(&config, request, response, resp2challenge,
				NULL, 1, pamh);
		PAM_FAIL_CHECK;

		DPRINT(pamh, LOG_DEBUG, "Got response to challenge code %d",
		       response->code);

		/*
		 * max_challenge limits the # of challenges a server can issue
		 * It's a workaround for buggy servers
		 */
		if (config.max_challenge > 0
		    && response->code == PW_ACCESS_CHALLENGE) {
			num_challenge++;
			if (num_challenge >= config.max_challenge) {
				DPRINT(pamh, LOG_DEBUG,
				       "maximum number of challenges (%d)"
				       " reached, failing", num_challenge);
				break;
			}
		}
	}

	/* Whew! Done the pasword checks, look for an authentication acknowledge */
	if (response->code == PW_AUTHENTICATION_ACK) {
		retval = PAM_SUCCESS;
	} else {
		retval = PAM_AUTH_ERR;	/* authentication failure */
	}

 do_next:
	/* If there was a password pass it to the next layer */
	if (password && *password) {
		pam_set_item(pamh, PAM_AUTHTOK, password);
	}

	DPRINT(pamh, LOG_DEBUG, "authentication %s",
	       retval == PAM_SUCCESS ? "succeeded" : "failed");

	close(config.sockfd);
	if (config.sockfd6 >= 0)
		close(config.sockfd6);
	cleanup(config.server);
	_pam_forget(password);
	_pam_forget(resp2challenge);
	{
		int *pret = malloc(sizeof(int));
		*pret = retval;
		pam_set_data(pamh, "rad_setcred_return", (void *)pret,
			     _int_free);
	}
	return retval;
}

/*
 * Return a value matching the return value of pam_sm_authenticate, for
 * greatest compatibility.
 * (Always returning PAM_SUCCESS breaks other authentication modules;
 * always returning PAM_IGNORE breaks PAM when we're the only module.)
 */
PAM_EXTERN int pam_sm_setcred(pam_handle_t * pamh, int flags, int argc,
			      CONST char **argv)
{
	int retval, *pret;

	retval = PAM_SUCCESS;
	pret = &retval;
	pam_get_data(pamh, "rad_setcred_return", (CONST void **)&pret);
	return *pret;
}

#undef PAM_FAIL_CHECK
#define PAM_FAIL_CHECK if (retval != PAM_SUCCESS) { return PAM_SESSION_ERR; }

static int pam_private_session(pam_handle_t * pamh, int flags, int argc,
			       CONST char **argv, int status)
{
	CONST char *user;
	CONST char *rhost;
	int retval = PAM_AUTH_ERR;

	char recv_buffer[4096];
	char send_buffer[4096];
	AUTH_HDR *request = (AUTH_HDR *) send_buffer;
	AUTH_HDR *response = (AUTH_HDR *) recv_buffer;
	radius_conf_t config;

	(void)_pam_parse(pamh, argc, argv, &config);

	/* grab the user name */
	retval = pam_get_user(pamh, &user, NULL);
	PAM_FAIL_CHECK;

	/* check that they've entered something, and not too long, either */
	if ((user == NULL) || (strlen(user) > MAXPWNAM)) {
		return PAM_USER_UNKNOWN;
	}

	/*
	 * Get the IP address of the authentication server
	 * Then, open a socket, and bind it to a port
	 */
	retval = initialize(pamh, &config, TRUE);
	PAM_FAIL_CHECK;

	/*
	 * If there's no client id specified, use the service type, to help
	 * keep track of which service is doing the authentication.
	 */
	if (!config.client_id) {
		retval =
		    pam_get_item(pamh, PAM_SERVICE,
				 (CONST void **)&config.client_id);
		PAM_FAIL_CHECK;
	}

	/* now we've got a socket open, so we've got to clean it up on error */
#undef PAM_FAIL_CHECK
#define PAM_FAIL_CHECK if (retval != PAM_SUCCESS) {goto error; }

	/* build and initialize the RADIUS packet */
	request->code = PW_ACCOUNTING_REQUEST;
	get_random_vector(request->vector);
	request->id = request->vector[0];	/* this should be evenly distributed */

	build_radius_packet(request, user, NULL, &config);

	add_int_attribute(request, PW_ACCT_STATUS_TYPE, status);

	sprintf(recv_buffer, "%08d", (int)getpid());
	add_attribute(request, PW_ACCT_SESSION_ID, (unsigned char *)recv_buffer,
		      strlen(recv_buffer));

	add_int_attribute(request, PW_ACCT_AUTHENTIC, PW_AUTH_RADIUS);

	if (status == PW_STATUS_START) {
		time_t *session_time = malloc(sizeof(time_t));
		time(session_time);
		pam_set_data(pamh, "rad_session_time", (void *)session_time,
			     _int_free);
	} else {
		time_t *session_time;
		retval =
		    pam_get_data(pamh, "rad_session_time",
				 (CONST void **)&session_time);
		PAM_FAIL_CHECK;

		add_int_attribute(request, PW_ACCT_SESSION_TIME,
				  time(NULL) - *session_time);
	}

	/*
	 *      Tell the server which host the user is coming from.
	 *
	 *      Note that this is NOT the IP address of the machine running PAM!
	 *      It's the IP address of the client.
	 */
	retval = pam_get_item(pamh, PAM_RHOST, (CONST void **)&rhost);
	PAM_FAIL_CHECK;
	if (rhost) {
		add_attribute(request, PW_CALLING_STATION_ID,
			      (unsigned char *)rhost, strlen(rhost));
	}

	retval = talk_radius(&config, request, response, NULL, NULL, 1, pamh);
	PAM_FAIL_CHECK;

	/* oops! They don't have the right password.    Complain and die. */
	if (response->code != PW_ACCOUNTING_RESPONSE) {
		retval = PAM_PERM_DENIED;
		goto error;
	}

	retval = PAM_SUCCESS;

 error:

	close(config.sockfd);
	if (config.sockfd6 >= 0)
		close(config.sockfd6);
	cleanup(config.server);

	return retval;
}

PAM_EXTERN int pam_sm_open_session(pam_handle_t * pamh, int flags, int argc,
				   CONST char **argv)
{
	return pam_private_session(pamh, flags, argc, argv, PW_STATUS_START);
}

PAM_EXTERN int pam_sm_close_session(pam_handle_t * pamh, int flags, int argc,
				    CONST char **argv)
{
	return pam_private_session(pamh, flags, argc, argv, PW_STATUS_STOP);
}

#undef PAM_FAIL_CHECK
#define PAM_FAIL_CHECK if (retval != PAM_SUCCESS) {return retval; }
#define MAX_PASSWD_TRIES 3

PAM_EXTERN int pam_sm_chauthtok(pam_handle_t * pamh, int flags, int argc,
				CONST char **argv)
{
	CONST char *user;
	char *password = NULL;
	char *new_password = NULL;
	char *check_password = NULL;
	int ctrl;
	int retval = PAM_AUTHTOK_ERR;
	int attempts;

	char recv_buffer[4096];
	char send_buffer[4096];
	AUTH_HDR *request = (AUTH_HDR *) send_buffer;
	AUTH_HDR *response = (AUTH_HDR *) recv_buffer;
	radius_conf_t config;

	ctrl = _pam_parse(pamh, argc, argv, &config);

	/* grab the user name */
	retval = pam_get_user(pamh, &user, NULL);
	PAM_FAIL_CHECK;

	/* check that they've entered something, and not too long, either */
	if ((user == NULL) || (strlen(user) > MAXPWNAM)) {
		return PAM_USER_UNKNOWN;
	}

	/*
	 * Get the IP address of the authentication server
	 * Then, open a socket, and bind it to a port
	 */
	retval = initialize(pamh, &config, FALSE);
	PAM_FAIL_CHECK;

	/*
	 * If there's no client id specified, use the service type, to help
	 * keep track of which service is doing the authentication.
	 */
	if (!config.client_id) {
		retval =
		    pam_get_item(pamh, PAM_SERVICE,
				 (CONST void **)&config.client_id);
		PAM_FAIL_CHECK;
	}

	/* now we've got a socket open, so we've got to clean it up on error */
#undef PAM_FAIL_CHECK
#define PAM_FAIL_CHECK if (retval != PAM_SUCCESS) {goto error; }

	/* grab the old password (if any) from the previous password layer */
	retval = pam_get_item(pamh, PAM_OLDAUTHTOK, (CONST void **)&password);
	PAM_FAIL_CHECK;
	if (password)
		password = strdup(password);

	/* grab the new password (if any) from the previous password layer */
	retval = pam_get_item(pamh, PAM_AUTHTOK, (CONST void **)&new_password);
	PAM_FAIL_CHECK;
	if (new_password)
		new_password = strdup(new_password);

	/* preliminary password change checks. */
	if (flags & PAM_PRELIM_CHECK) {
		if (!password) {	/* no previous password: ask for one */
			retval =
			    rad_converse(pamh, PAM_PROMPT_ECHO_OFF,
					 config.prompt, &password);
			PAM_FAIL_CHECK;
		}

		/*
		 * We now check the password to see if it's the right one.
		 * If it isn't, we let the user try again.
		 * Note that RADIUS doesn't have any concept of 'root'. The only way
		 * that root can change someone's password is to log into the RADIUS
		 * server, and and change it there.
		 */

		/* build and initialize the access request RADIUS packet */
		request->code = PW_AUTHENTICATION_REQUEST;
		get_random_vector(request->vector);
		request->id = request->vector[0];	/* this should be evenly distributed */

		build_radius_packet(request, user, password, &config);
		add_int_attribute(request, PW_USER_SERVICE_TYPE,
				  PW_AUTHENTICATE_ONLY);

		retval =
		    talk_radius(&config, request, response, password, NULL, 1,
				pamh);
		PAM_FAIL_CHECK;

		/* oops! They don't have the right password.    Complain and die. */
		if (response->code != PW_AUTHENTICATION_ACK) {
			_pam_forget(password);
			retval = PAM_PERM_DENIED;
			goto error;
		}

		/*
		 * We're now sure it's the right user.
		 * Ask for their new password, if appropriate
		 */

		if (!new_password) {	/* not found yet: ask for it */
			int new_attempts;
			attempts = 0;

			/* loop, trying to get matching new passwords */
			while (attempts++ < 3) {

				/* loop, trying to get a new password */
				new_attempts = 0;
				while (new_attempts++ < 3) {
					retval =
					    rad_converse(pamh,
							 PAM_PROMPT_ECHO_OFF,
							 "New password: ",
							 &new_password);
					PAM_FAIL_CHECK;

					/* the old password may be short.       Check it, first. */
					if (strcmp(password, new_password) == 0) {	/* are they the same? */
						rad_converse(pamh,
							     PAM_ERROR_MSG,
							     "You must choose a new password.",
							     NULL);
						_pam_forget(new_password);
						continue;
					} else if (strlen(new_password) < 6) {
						rad_converse(pamh,
							     PAM_ERROR_MSG,
							     "it's WAY too short",
							     NULL);
						_pam_forget(new_password);
						continue;
					}

					/* insert crypt password checking here */

					break;	/* the new password is OK */
				}

				if (new_attempts >= 3) {	/* too many new password attempts: die */
					retval = PAM_AUTHTOK_ERR;
					goto error;
				}

				/* make sure of the password by asking for verification */
				retval = rad_converse(pamh, PAM_PROMPT_ECHO_OFF,
						      "New password (again): ",
						      &check_password);
				PAM_FAIL_CHECK;

				retval = strcmp(new_password, check_password);
				_pam_forget(check_password);

				/* if they don't match, don't pass them to the next module */
				if (retval != 0) {
					_pam_forget(new_password);
					rad_converse(pamh, PAM_ERROR_MSG,
						     "You must enter the same password twice.",
						     NULL);
					retval = PAM_AUTHTOK_ERR;
					goto error;	/* ??? maybe this should be a 'continue' ??? */
				}

				break;	/* everything's fine */
			}	/* loop, trying to get matching new passwords */

			if (attempts >= 3) {	/* too many new password attempts: die */
				retval = PAM_AUTHTOK_ERR;
				goto error;
			}
		}

		/* now we have a new password which passes all of our tests */
		/*
		 * Solaris 2.6 calls pam_sm_chauthtok only ONCE, with PAM_PRELIM_CHECK
		 * set.
		 */
#ifndef sun
		/* If told to update the authentication token, do so. */
	} else if (flags & PAM_UPDATE_AUTHTOK) {
#endif

		if (!password || !new_password) {	/* ensure we've got passwords */
			retval = PAM_AUTHTOK_ERR;
			goto error;
		}

		/* build and initialize the password change request RADIUS packet */
		request->code = PW_PASSWORD_REQUEST;
		get_random_vector(request->vector);
		request->id = request->vector[0];	/* this should be evenly distributed */

		/* the secret here can not be know to the user, so it's the new password */
		_pam_forget(config.server->secret);
		config.server->secret = strdup(password);	/* it's free'd later */

		build_radius_packet(request, user, new_password, &config);
		add_password(request, PW_OLD_PASSWORD, password, password);

		retval =
		    talk_radius(&config, request, response, new_password,
				password, 1, pamh);
		PAM_FAIL_CHECK;

		/* Whew! Done password changing, check for password acknowledge */
		if (response->code != PW_PASSWORD_ACK) {
			retval = PAM_AUTHTOK_ERR;
			goto error;
		}
	}

	/*
	 * Send the passwords to the next stage if preliminary checks fail,
	 * or if the password change request fails.
	 */
	if ((flags & PAM_PRELIM_CHECK) || (retval != PAM_SUCCESS)) {
 error:

		/* If there was a password pass it to the next layer */
		if (password && *password) {
			pam_set_item(pamh, PAM_OLDAUTHTOK, password);
		}

		if (new_password && *new_password) {
			pam_set_item(pamh, PAM_AUTHTOK, new_password);
		}
	}

	if (ctrl & PAM_DEBUG_ARG) {
		_pam_log(pamh, LOG_DEBUG, "password change %s",
			 retval == PAM_SUCCESS ? "succeeded" : "failed");
	}

	close(config.sockfd);
	if (config.sockfd6 >= 0)
		close(config.sockfd6);
	cleanup(config.server);

	_pam_forget(password);
	_pam_forget(new_password);
	return retval;
}

/*
 *	Do nothing for account management. This is apparently needed by
 *	some programs.
 */
PAM_EXTERN int pam_sm_acct_mgmt(pam_handle_t * pamh, int flags, int argc,
				CONST char **argv)
{
	int retval = PAM_SUCCESS;
	CONST char *user;
	radius_conf_t config;

	(void)_pam_parse(pamh, argc, argv, &config);

	/* grab the user name */
	retval = pam_get_user(pamh, &user, NULL);
	if (retval != PAM_SUCCESS || user == NULL || strlen(user) > MAXPWNAM) {
		return PAM_USER_UNKNOWN;
	}

	/*
	 * parse the config file.  We don't make any connections here, so ignore
	 * any failures.  For consistency only.
	 */
	retval = initialize(pamh, &config, FALSE);

	/*
	 * set SUDO_PROMPT in env so that it prompts as the login user, not the mapped
	 * user, unless (unlikely) the prompt has already been set.
	 * It won't hurt to do this if the user wasn't mapped.
	 */
	if (!pam_getenv(pamh, "SUDO_PROMPT")) {
		char nprompt[strlen("SUDO_PROMPT=[sudo] password for ") + strlen(user) + 3];	/* + 3 for ": " and the \0 */
		snprintf(nprompt, sizeof nprompt,
			 "SUDO_PROMPT=[sudo] password for %s: ", user);
		if (pam_putenv(pamh, nprompt) != PAM_SUCCESS)
			_pam_log(pamh, LOG_NOTICE, "failed to set PAM sudo"
				 " prompt (%s)", nprompt);
	}

	return retval;
}

#ifdef PAM_STATIC

/* static module data */

struct pam_module _pam_radius_modstruct = {
	"pam_radius_auth",
	pam_sm_authenticate,
	pam_sm_setcred,
	pam_sm_acct_mgmt,
	pam_sm_open_session,
	pam_sm_close_session,
	pam_sm_chauthtok,
};
#endif
