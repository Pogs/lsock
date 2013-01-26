/* compile: gcc -o core.{so,c} -shared -fPIC -pedantic -ansi -std=c89 -W -Wall -llua -lm -ldl -flto -fstack-protector-all -O1 -g -nostdlib */
/* release: gcc -o core.{so,c} -shared -fPIC -pedantic -ansi -std=c89 -W -Wall -llua -lm -ldl -flto -fstack-protector-all -Os -s -nostdlib */

#ifndef _POSIX_SOURCE
#	define _POSIX_SOURCE
#endif

#ifndef _GNU_SOURCE
#	define _GNU_SOURCE
#endif

#ifdef _WIN32
#	pragma comment(lib, "Ws2_32.lib")
#	define WIN32_LEAN_AND_MEAN /* avoid MVC stuffs */
#	include <windows.h>
#	include <winsock2.h>
#	include <ws2tcpip.h>
#	include <io.h>             /* _open_osfhandle() / _get_osfhandle() */
#else
#	include <arpa/inet.h>
#	include <sys/socket.h>
#	include <netdb.h>
#	include <netinet/in.h>
#	include <net/if.h>
#	include <sys/types.h>
#	include <sys/un.h>
#	include <string.h>
#	include <unistd.h>
#	include <errno.h>
#	include <sys/time.h>
#	include <sys/sendfile.h>
#	include <fcntl.h>
#	include <stropts.h>
#	include <sys/ioctl.h>
#	include <sys/select.h>
#endif

#include <lauxlib.h>
#include <lualib.h>

#define NUL '\0'
#define BZERO(buf, sz) memset(buf, NUL, sz)

#define LSOCK_FDOPEN_MODE "r+b"

#ifdef _WIN32
typedef SOCKET lsocket;
#else
typedef int    lsocket;
#endif

#define LSOCK_NEWUDATA(L, sz) BZERO(lua_newuserdata(L, sz), sz)

#define LSOCK_CHECKSTREAM(L, index) ((luaL_Stream *) luaL_checkudata(L, index, LUA_FILEHANDLE))
#define LSOCK_CHECKSOCKET(L, index) stream_to_lsocket(L, LSOCK_CHECKSTREAM(L, index)->f)
#define     LSOCK_CHECKFD(L, index) stream_to_fd(L, LSOCK_CHECKSTREAM(L, index)->f)

#define LSOCK_STRERROR(L, fname) lsock_error(L, LSOCK_ERRNO, (char * (*)(int)) &strerror,     fname)
#define LSOCK_GAIERROR(L, err  ) lsock_error(L, err,         (char * (*)(int)) &gai_strerror, NULL )
#define LSOCK_STRFATAL(L, fname) lsock_fatal(L, LSOCK_ERRNO, (char * (*)(int)) &strerror,     fname)

#ifdef _WIN32
#	define SOCKFAIL(s)   (SOCKET_ERROR == (s))
#	define INVALID_SOCKET(s) (INVALID_SOCKET == (s))
#	define LSOCK_ERRNO       WSAGetLastError()
#else
#	define SOCKFAIL(s)       (s < 0)
#	define INVALID_SOCKET(s) (s < 0)
#	define LSOCK_ERRNO       errno
#endif

#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* including sockaddr_un in this only increases the byte count by 18 */
typedef union
{
	struct sockaddr_storage ss;
	struct sockaddr         sa;
	struct sockaddr_in      in;
	struct sockaddr_in6     in6;
#ifndef _WIN32
	struct sockaddr_un      un;
#endif
} lsockaddr;

/*
** DONE:
**			- htons()
**			- htonl()
**			- ntohs()
**			- ntohl()
**
**			- pack_sockaddr()
**			- unpack_sockaddr()
**
**			- socket()
**			- bind()
**			- blocking() -- for setting nonblocking mode
**			- connect()
**			- listen()
**			- shutdown()
**
**			- getsockopt() -- only SOL_SOCKET options supported right now
**			- setsockopt()
**
**			- select() -- I might make an fd_set constructor + helper methods later on
**
**			- strerror()
**			- gai_strerror()
**
**			- sockaddr() -- for bind()/connect()/accept()/getaddrinfo()
**			- linger()   -- for get/setsockopt()
**			- timeval()  -- for get/setsockopt()
**
**			- sendfile()   -- only on Linux
**			- socketpair() -- only on Linux
**
**			- getsockname()
**			- getpeername()
**
**			- getaddrinfo()
**			- getnameinfo()
**
** ALMOST:
**			- send()/sendto()   -- (sendto())
**			- recv()/recvfrom() -- (recvfrom())
**
** TODO:
**			- sendmsg() -- maybe send()+sendmsg() can be wrappers?
**			- recvmsg() -- maybe recv()+recvmsg() can be wrappers?
**
**			- ioctl()       -- unnecessary?
**
**			- htond()       -- only on Windows
**			- htonf()       -- only on Windows
**			- htonll()      -- only on Windows
**			- ntohd()       -- only on Windows
**			- ntohf()       -- only on Windows
**			- ntohll()      -- only on Windows
*/

/*  {{{ - error stuff - */

/* {{{ lsock_error() */

static int
lsock_error(lua_State * L, int err, char * (*errfunc)(int), char * fname)
{
	char * msg = errfunc(err);

	lua_pushnil(L);

	if (NULL == fname)
		lua_pushstring(L, msg); /* even if err is not a valid errno, strerror() will return a pointer to "" */
	else
		lua_pushfstring(L, "%s: %s", fname, msg);

	lua_pushinteger(L, err);

	return 3;
}

/* }}} */

/* {{{ lsock_fatal() */

static int
lsock_fatal(lua_State * L, int err, char * (*errfunc)(int), char * fname)
{
	char * msg = errfunc(err);

	if (NULL == fname)
		return luaL_error(L, msg);
	else
		return luaL_error(L, "%s: %s", fname, msg);
}

/* }}} */

/* {{{ lsock_strerror() */

static int
lsock_strerror(lua_State * L)
{
	lua_pushstring(L, strerror(luaL_checknumber(L, 1)));

	return 1;
}

/* }}} */

/* {{{ lsock_gai_strerror() */

static int
lsock_gai_strerror(lua_State * L)
{
	lua_pushstring(L, gai_strerror(luaL_checknumber(L, 1)));

	return 1;
}

/* }}} */

/* }}} */

/* {{{ - FILE * <-> fd <-> SOCKET stuff - */

/* going to and from SOCKET and fd in Windows: _open_osfhandle()/_get_osfhandle() */

/* {{{ stream_to_fd() */

static int
stream_to_fd(lua_State * L, FILE * stream)
{
	int fd = -1;

#ifdef _WIN32
	fd = _fileno(stream);
#else
	fd = fileno(stream);
#endif

	if (-1 == fd)
#ifdef _WIN32
		LSOCK_STRFATAL(L, "_fileno()");
#else
		LSOCK_STRFATAL(L, "fileno()");
#endif

	return fd;
}

/* }}} */

/* {{{ fd_to_stream() */

static FILE *
fd_to_stream(lua_State * L, int fd)
{
	FILE * stream = NULL;

#ifdef _WIN32
	stream = _fdopen(fd, LSOCK_FDOPEN_MODE);
#else
	stream = fdopen(fd, LSOCK_FDOPEN_MODE);
#endif

	if (NULL == stream)
#ifdef _WIN32
		LSOCK_STRFATAL(L, "_fdopen()");
#else
		LSOCK_STRFATAL(L, "fdopen()");
#endif

	return stream;
}

/* }}} */

/* {{{ lsocket_to_fd() */

static int
lsocket_to_fd(lua_State * L, lsocket sock)
{

#ifdef _WIN32
	int fd = -1;

	fd = _open_osfhandle(handle, 0); /* no _O_APPEND, _O_RDONLY, _O_TEXT */

	if (-1 == fd)
		LSOCK_STRFATAL(L, "_open_osfhandle()");

	return fd;
#else
	(void) L;

	return sock;
#endif

}

/* }}} */

/* {{{ fd_to_lsocket() */

static lsocket
fd_to_lsocket(lua_State * L, int fd)
{

#ifdef _WIN32
	lsocket l = _get_osfhandle(fd);

	if (INVALID_HANDLE_VALUE == l)
		return LSOCK_STRFATAL(L, "_get_osfhandle()")

	return l;
#else
	(void) L;

	return fd; /* hah */
#endif

}

/* }}} */

/* {{{ stream_to_lsocket() */

static lsocket
stream_to_lsocket(lua_State * L, FILE * stream)
{
	/* wheee shortness. */
	return fd_to_lsocket(L, stream_to_fd(L, stream));
}

/* }}} */

/* {{{ lsocket_to_stream() */

static FILE *
lsocket_to_stream(lua_State * L, lsocket sock)
{
	/* \o/ \o| |o/ \o/ \o| |o/ \o/ */
	return fd_to_stream(L, lsocket_to_fd(L, sock));
}

/* }}} */

/* }}} */

/* {{{ - table <-> structure stuff - */

/* {{{ timeval_to_table() */

static void
timeval_to_table(lua_State * L, struct timeval * t)
{
	lua_createtable(L, 0, 2);

	lua_pushnumber(L, t->tv_sec);
	lua_setfield(L, -2, "tv_sec");

	lua_pushnumber(L, t->tv_usec);
	lua_setfield(L, -2, "tv_usec");
}

/* }}} */

/* {{{ table_to_timeval() */

static struct timeval *
table_to_timeval(lua_State * L, int index)
{
	struct timeval * t = LSOCK_NEWUDATA(L, sizeof(struct timeval));

	lua_getfield(L, index, "tv_sec");

	if (!lua_isnil(L, -1))
		t->tv_sec = lua_tonumber(L, -1);

	lua_pop(L, 1);

	lua_getfield(L, index, "tv_usec");

	if (!lua_isnil(L, -1))
		t->tv_usec = lua_tonumber(L, -1);

	lua_pop(L, 1);

	return t;
}

/* }}} */

/* {{{ linger_to_table() */

static void
linger_to_table(lua_State * L, struct linger * l)
{
	lua_createtable(L, 0, 2);

	lua_pushnumber(L, l->l_onoff);
	lua_setfield(L, -2, "l_onoff");

	lua_pushnumber(L, l->l_linger);
	lua_setfield(L, -2, "l_linger");
}

/* }}} */

/* {{{ table_to_linger() */

static struct linger *
table_to_linger(lua_State * L, int index)
{
	struct linger * l = LSOCK_NEWUDATA(L, sizeof(struct linger));

	lua_getfield(L, index, "l_onoff");

	if (!lua_isnil(L, -1))
		l->l_onoff = lua_tonumber(L, -1);

	lua_pop(L, 1);

	lua_getfield(L, index, "l_linger");

	if (!lua_isnil(L, -1))
		l->l_linger = lua_tonumber(L, -1);

	lua_pop(L, 1);

	return l;
}

/* }}} */

/* {{{ sockaddr_to_table() */

static int
sockaddr_to_table(lua_State * L, const char * sa, size_t lsa_sz)
{
	lsockaddr * lsa = (lsockaddr *) sa;

	if (lsa_sz < sizeof(((struct sockaddr *) NULL)->sa_family))
		return 0;

	switch (lsa->sa.sa_family)
	{
#ifndef _WIN32
		case AF_UNIX:  if (lsa_sz < sizeof(struct sockaddr_un))  goto invalid_sockaddr; break;
#endif
		case AF_INET:  if (lsa_sz < sizeof(struct sockaddr_in))  goto invalid_sockaddr; break;
		case AF_INET6: if (lsa_sz < sizeof(struct sockaddr_in6)) goto invalid_sockaddr; break;
		default:       if (lsa_sz < sizeof(lsockaddr))           goto invalid_sockaddr; break;
invalid_sockaddr:
		return 0;
	}

	lua_createtable(L, 0, 7); /* 5 fields in sockaddr_in6 + 2 in sockaddr_storage */

	lua_pushnumber(L, lsa->ss.ss_family);
	lua_setfield(L, -2, "ss_family");

	lua_pushnumber(L, lsa->sa.sa_family);
	lua_setfield(L, -2, "sa_family");

	lua_pushlstring(L, lsa->sa.sa_data, sizeof(((lsockaddr *) NULL)->sa.sa_data));
	lua_setfield(L, -2, "sa_data");

	switch (lsa->ss.ss_family)
	{
		case AF_INET:
			{
				char dst[INET_ADDRSTRLEN];

				BZERO(dst, sizeof(dst));

				lua_pushnumber(L, lsa->in.sin_family);
				lua_setfield(L, -2, "sin_family");

				lua_pushnumber(L, ntohs(lsa->in.sin_port));
				lua_setfield(L, -2, "sin_port");

#ifdef _WIN32
				if (NULL == InetNtop(AF_INET, &lsa->in.sin_addr, dst, sizeof(dst)))
					LSOCK_STRFATAL(L, "InetNtop()");
#else
				if (NULL == inet_ntop(AF_INET, &lsa->in.sin_addr, dst, sizeof(dst)))
					LSOCK_STRFATAL(L, "inet_ntop()");
#endif

				lua_pushstring(L, dst);
				lua_setfield(L, -2, "sin_addr");
			}
			
			break;

		case AF_INET6:
			{
				char dst[INET6_ADDRSTRLEN];

				BZERO(dst, sizeof(dst));

				lua_pushnumber(L, lsa->in6.sin6_family);
				lua_setfield(L, -2, "sin6_family");

				lua_pushnumber(L, ntohs(lsa->in6.sin6_port));
				lua_setfield(L, -2, "sin6_port");

				lua_pushnumber(L, ntohl(lsa->in6.sin6_flowinfo));
				lua_setfield(L, -2, "sin6_flowinfo");

				lua_pushnumber(L, ntohl(lsa->in6.sin6_scope_id));
				lua_setfield(L, -2, "sin6_scope_id");

#ifdef _WIN32
				if (NULL == InetNtop(AF_INET6, (char *) &lsa->in6.sin6_addr, dst, sizeof(dst)))
					LSOCK_STRFATAL(L, "InetNtop()");
#else
				if (NULL == inet_ntop(AF_INET6, (char *) &lsa->in6.sin6_addr, dst, sizeof(dst)))
					LSOCK_STRFATAL(L, "inet_ntop()");
#endif

				lua_pushstring(L, dst);
				lua_setfield(L, -2, "sin6_addr");
			}

			break;

#ifndef _WIN32
		case AF_UNIX:
			lua_pushnumber(L, lsa->un.sun_family);
			lua_setfield(L, -2, "sun_family");

			lua_pushlstring(L, (char *) &lsa->un.sun_path, sizeof(((struct sockaddr_un *) NULL)->sun_path));
			lua_setfield(L, -2, "sun_path");

			break;

#endif
	}

	return 1;
}

/* }}} */

/* {{{ table_to_sockaddr() */

static char * members[] =
{
	"ss_family",
	"sa_family",   "sa_data",
	"sin_family",  "sin_port",  "sin_addr",
	"sin6_family", "sin6_port", "sin6_flowinfo", "sin6_addr", "sin6_scope_id",
	"sun_family",  "sun_path"
};

enum SOCKADDR_FIELDS
{
	SS_FAMILY,
	SA_FAMILY,   SA_DATA,
	SIN_FAMILY,  SIN_PORT,  SIN_ADDR,
	SIN6_FAMILY, SIN6_PORT, SIN6_ADDR, SIN6_FLOWINFO, SIN6_SCOPE_ID,
	SUN_FAMILY,  SUN_PATH
};

static const char *
table_to_sockaddr(lua_State * L, int index)
{
	unsigned int i;

	size_t out_sz = 0;
	lsockaddr lsa;

	BZERO(&lsa, sizeof(lsa));

	for (i = 0; i < 13; i++)
	{
		lua_getfield(L, index, members[i]);

		if (lua_isnil(L, -1))
			goto next_member;

		switch (i)
		{
			case   SS_FAMILY:
			case   SA_FAMILY:
			case  SIN_FAMILY:
			case SIN6_FAMILY:
			case  SUN_FAMILY:
				lsa.ss.ss_family = luaL_checknumber(L, -1);
				break;

			case  SIN_PORT:   lsa.in.sin_port = ntohs(luaL_checknumber(L, -1)); break;
			case SIN6_PORT: lsa.in6.sin6_port = ntohs(luaL_checknumber(L, -1)); break;

			case SIN6_FLOWINFO: lsa.in6.sin6_flowinfo = ntohl(luaL_checknumber(L, -1)); break;
			case SIN6_SCOPE_ID: lsa.in6.sin6_scope_id = ntohl(luaL_checknumber(L, -1)); break;

			case  SA_DATA:
			case SUN_PATH:
				{
					size_t sz = 0;
					void       * dst = NULL;
					const char * src = NULL;

					src = luaL_checklstring(L, -1, &sz);

					if (i == SA_DATA)
					{
						dst = &lsa.sa.sa_data;
						sz  = MAX(sizeof(((struct sockaddr *) NULL)->sa_data), sz); /* should be MAX(14, l) */
					}
					else
					{
						dst = &lsa.un.sun_path;
						sz  = MAX(sizeof(((struct sockaddr_un *) NULL)->sun_path), sz); /* should be MAX(108, l) */
					}

					memcpy(dst, src, sz);
				}

				break;

			case  SIN_ADDR:
			case SIN6_ADDR:
				{
					int stat, af;
					void       * dst = NULL;
					const char * src = NULL;

					src = luaL_checkstring(L, -1);

					if (i == SIN_ADDR)
					{
						af  = AF_INET;
						dst = &lsa.in.sin_addr;
					}
					else
					{
						af  = AF_INET6;
						dst = &lsa.in6.sin6_addr;
					}

#ifdef _WIN32
					stat = InetPton(af, src, dst);
#else
					stat = inet_pton(af, src, dst);
#endif

					if (1 != stat) /* success is 1, funnily enough */
					{
						if (0 == stat)
							luaL_error(L, "invalid address for family (AF_INET%s)", i == SIN_ADDR ? "" : "6");
						else
#ifdef _WIN32
							LSOCK_STRFATAL(L, "InetPton()");
#else
							LSOCK_STRFATAL(L, "inet_pton()");
#endif
					}
				}
				
				break;
		}

next_member:
		lua_pop(L, 1);
	}

	switch (lsa.ss.ss_family)
	{
		case AF_UNIX:  out_sz = sizeof(struct sockaddr_un);  break;
		case AF_INET:  out_sz = sizeof(struct sockaddr_in);  break;
		case AF_INET6: out_sz = sizeof(struct sockaddr_in6); break;
		default:       out_sz = sizeof(lsockaddr);
	}

	return lua_pushlstring(L, (char *) &lsa, out_sz);
}

/* }}} */

/* {{{ lsock_pack_sockaddr() */

static int
lsock_pack_sockaddr(lua_State * L)
{
	/* I intentionally *don't* check the type */
	(void) table_to_sockaddr(L, 1);

	return 1;
}

/* }}} */

/* {{{ lsock_unpack_sockaddr() */

static int
lsock_unpack_sockaddr(lua_State * L)
{
	size_t       l = 0;
	const char * s = luaL_checklstring(L, 1, &l);

	return sockaddr_to_table(L, s, l);
}

/* }}} */

/* }}} */

/* {{{ lsock_newudata() */

static void *
lsock_newudata(lua_State * L, size_t sz, char * registry_type)
{
	void * p = lua_newuserdata(L, sz);

	BZERO(p, sizeof(sz));

	luaL_setmetatable(L, registry_type);

	return p;
}

/* }}} */

/* {{{ lsock_newstream(): based directly on: newprefile() & newfile() from Lua 5.2 sources */

static int
close_stream(lua_State * L)
{
	luaL_Stream * p = LSOCK_CHECKSTREAM(L, 1);

	return luaL_fileresult(L, (0 == fclose(p->f)), NULL);
}

static luaL_Stream *
newstream(lua_State * L)
{
	luaL_Stream * p = lsock_newudata(L, sizeof(luaL_Stream), LUA_FILEHANDLE);

	p->f      = NULL;
	p->closef = &close_stream;

	return p;
}

/* }}} */

/* {{{ network <-> host byte order stuff */

/* {{{ lsock_htons() */

static int
lsock_htons(lua_State * L)
{
	lua_Number n = luaL_checknumber(L, 1);
	uint16_t   s = n;

	if (s != n) /* type promotion back to lua_Number */
	{
		lua_pushnil(L);
		lua_pushfstring(L, "number cannot be represented as [network] short (%s)", s > n ? "underflow" : "overflow");
		return 2;
	}

	s = htons(s);

	lua_pushlstring(L, (char *) &s, sizeof(uint16_t));

	return 1;
}

/* }}} */

/* {{{ lsock_ntohs() */

static int
lsock_ntohs(lua_State * L)
{
	uint16_t     h = 0;
	size_t       l = 0;
	const char * s = luaL_checklstring(L, 1, &l);

	if (sizeof(uint16_t) != l) /* obviously 2 bytes... */
	{
		lua_pushnil(L);
		lua_pushliteral(L, "string length must be sizeof(uint16_t) (2 bytes)");
		return 2;
	}

	h = ntohs(*((uint16_t *) s));

	lua_pushnumber(L, h);

	return 1;
}

/* }}} */

/* {{{ lsock_htonl() */

static int
lsock_htonl(lua_State * L)
{
	lua_Number n = luaL_checknumber(L, 1);
	uint32_t   l = n;

	if (l != n) /* type promotion back to lua_Number */
	{
		lua_pushnil(L);
		lua_pushfstring(L, "number cannot be represented as [network] long (%s)", l > n ? "underflow" : "overflow");
		return 2;
	}

	l = htonl(l);

	lua_pushlstring(L, (char *) &l, sizeof(uint32_t));

	return 1;
}

/* }}} */

/* {{{ lsock_ntohl() */

static int
lsock_ntohl(lua_State * L)
{
	uint32_t     h = 0;
	size_t       l = 0;
	const char * s = luaL_checklstring(L, 1, &l);

	if (sizeof(uint32_t) != l) /* 4 bytes */
	{
		lua_pushnil(L);
		lua_pushliteral(L, "string length must be sizeof(uint32_t) (4 bytes)");
		return 2;
	}

	h = ntohl(*((uint32_t *) s));

	lua_pushnumber(L, h);

	return 1;
}

/* }}} */

/* }}} */

/* {{{ - socket operations - */

/* {{{ lsock_accept() */

static int
lsock_accept(lua_State * L)
{
	lsocket       serv    = LSOCK_CHECKSOCKET(L, 1);
	luaL_Stream * newfh   = NULL;
	socklen_t     sz      = sizeof(lsockaddr);
	lsockaddr     info;

	lsocket       newsock = accept(serv, (struct sockaddr *) &info, &sz);

	if (INVALID_SOCKET(newsock))
		return LSOCK_STRERROR(L, NULL);

	newfh    = newstream(L);
	newfh->f = lsocket_to_stream(L, newsock);

	lua_pushlstring(L, (char *) &info, sz);
	sockaddr_to_table(L, (char *) &info, sz);

	return 3;
}

/* }}} */

/* {{{ lsock_listen(sock, backlog_number) */

static int
lsock_listen(lua_State * L)
{
	lsocket serv = LSOCK_CHECKSOCKET(L, 1);
	int  backlog = lua_isnone(L, 2) ? 0 : luaL_checknumber(L, 2);

	if (SOCKFAIL(listen(serv, backlog)))
		return LSOCK_STRERROR(L, NULL);

	lua_pushboolean(L, 1);

	return 1;
}

/* }}} */

/* {{{ lsock_bind() */

/* bind(sock, sockaddr_userdata) -> true -or- nil, 'error message', error_constant */

static int
lsock_bind(lua_State * L)
{
	size_t       sz   = 0;
	lsocket      serv = LSOCK_CHECKSOCKET(L, 1);
	const char * addr = luaL_checklstring(L, 2, &sz);

	if (SOCKFAIL(bind(serv, (struct sockaddr *) addr, sz)))
		return LSOCK_STRERROR(L, NULL);

	lua_pushboolean(L, 1);
	
	return 1;
}

/* }}} */

/* {{{ lsock_connect(sock, sockaddr_userdata) -> true -or- nil, 'error message', error_constant */

/* identical to lsock_bind(), pretty much */

static int
lsock_connect(lua_State * L)
{
	lsocket      client = LSOCK_CHECKSOCKET(L, 1);
	const char * sa     = NULL;
	size_t       sz     = 0;

	sa = luaL_checklstring(L, 2, &sz);

	if (SOCKFAIL(connect(client, (struct sockaddr *) sa, sz))) /* rawlen() for safety */
		return LSOCK_STRERROR(L, NULL);

	lua_pushboolean(L, 1);

	return 1;
}

/* }}} */

/* {{{ lsock_getsockname() */

static int
lsock_getsockname(lua_State * L)
{
	lsockaddr addr;

	lsocket    s = LSOCK_CHECKSOCKET(L, 1);
	socklen_t sz = sizeof(addr);

	BZERO(&addr, sizeof(addr));

	if (SOCKFAIL(getsockname(s, (struct sockaddr *) &addr, &sz)))
		return LSOCK_STRERROR(L, NULL);

	sockaddr_to_table(L, (char * const) &addr, sz);

	return 1;
}

/* }}} */

/* {{{ lsock_getpeername() */

static int
lsock_getpeername(lua_State * L)
{
	lsockaddr addr;

	lsocket    s = LSOCK_CHECKSOCKET(L, 1);
	socklen_t sz = sizeof(addr);

	BZERO(&addr, sizeof(addr));

	if (SOCKFAIL(getpeername(s, (struct sockaddr *) &addr, &sz)))
		return LSOCK_STRERROR(L, NULL);

	sockaddr_to_table(L, (char * ) &addr, sz);

	return 1;
}

/* }}} */

/* {{{ lsock_sendto() */

static int
lsock_sendto(lua_State * L)
{
	ssize_t sent;

	size_t s_len = 0;

	lsocket      sock   = LSOCK_CHECKSOCKET(L, 1);
	const char * s      = luaL_checklstring(L, 2, &s_len);
	size_t       i      = luaL_checknumber(L, 3);
	size_t       j      = luaL_checknumber(L, 4);
	int          flags  = lua_isnoneornil(L, 5) ? 0 : luaL_checknumber(L, 5);

	const char * to     = NULL;
	size_t       to_len = 0;

	if (!lua_isnone(L, 6))
		to = luaL_checklstring(L, 6, &to_len);

	if (i < 1 || j > s_len)
		luaL_error(L, "out of bounds [%d,%d]: data is %d bytes", i, j, s_len);

	sent = sendto(sock, (s - 1) + i, (j - i) + 1, flags, (struct sockaddr *) to, to_len);

	if (SOCKFAIL(sent))
		return LSOCK_STRERROR(L, NULL);

	lua_pushnumber(L, sent);

	return 1; /* success! */
}

/* }}} */

/* {{{ lsock_send() */

static int
lsock_send(lua_State * L)
{
	int n = lua_gettop(L);

	if (n > 5)
		lua_pop(L, n - 5);
	
	return lsock_sendto(L);
}

/* }}} */

/* {{{ lsock_recvfrom() */

static int
lsock_recvfrom(lua_State * L)
{
	ssize_t gotten = 0;

	const char * from     = NULL;
	socklen_t    from_len = 0;

	char * buf;
	luaL_Buffer B;

	lsocket sock  = LSOCK_CHECKSOCKET(L, 1);
	size_t length = luaL_checknumber(L, 2);
	int    flags  = lua_isnoneornil(L, 3) ? 0 : luaL_checknumber(L, 3);

	if (!lua_isnone(L, 4))
		from = luaL_checklstring(L, 4, (size_t *) &from_len);

	buf = luaL_buffinitsize(L, &B, length);

	BZERO(buf, length); /* a must! */

	gotten = recvfrom(sock, buf, length, flags, (struct sockaddr *) from, &from_len);
	
	if (SOCKFAIL(gotten))
		return LSOCK_STRERROR(L, NULL);

	luaL_pushresultsize(&B, gotten);

	return 1; /* success! */
}

/* }}} */

/* {{{ lsock_recv() */

static int
lsock_recv(lua_State * L)
{
	int n = lua_gettop(L);

	if (n > 3)
		lua_pop(L, n - 3);

	return lsock_recvfrom(L);
}

/* }}} */

/* {{{ lsock_shutdown() */

/* shutdown(sock, how) -> true  -or-  nil, errno */
/* lsock/init.lua wraps this and sets it as the __gc for sockets */

static int
lsock_shutdown(lua_State * L)
{
	lsocket sock = LSOCK_CHECKSOCKET(L, 1);
	int     how  = lua_isnone(L, 2) ? SHUT_RDWR : luaL_checknumber(L, 2);

	if (SOCKFAIL(shutdown(sock, how)))
		return LSOCK_STRERROR(L, NULL);
	
	lua_pushboolean(L, 1);

	return 1;
}

/* }}} */

/* {{{ lsock_socket() */

/* socket('AF_INET', 'SOCK_STREAM', 0) -> 29 */

static int
lsock_socket(lua_State * L)
{
	int domain   = luaL_checknumber(L, 1),
		type     = luaL_checknumber(L, 2),
		protocol = luaL_checknumber(L, 3);

	lsocket       sock   = socket(domain, type, protocol);
	luaL_Stream * stream = NULL;

	if (INVALID_SOCKET(sock))
		return LSOCK_STRERROR(L, NULL);

	stream    = newstream(L);
	stream->f = lsocket_to_stream(L, sock);

	return 1;
}

/* }}} */

/* {{{ lsock_blocking() */

static int
lsock_blocking(lua_State * L)
{
	lsocket s = LSOCK_CHECKSOCKET(L, 1);
	int block =    !lua_toboolean(L, 2); /* sock:blocking() would set blocking, sock:blocking(false) would unblock, etc.. */

#ifndef _WIN32
	int flags = fcntl(s, F_GETFL);

	if (-1 == flags)
		return LSOCK_STRERROR(L, "fcntl()");

	flags |= (block ? O_NONBLOCK : 0);

	flags = fcntl(s, F_SETFL, flags);

	if (-1 == flags)
		return LSOCK_STRERROR(L, "fcntl()");

	flags = block ? 1 : 0;

	flags = ioctl(s, FIONBIO, &flags);

	if (-1 == flags)
		return LSOCK_STRERROR(L, "ioctl()");
#else
	if (SOCKET_ERROR == ioctlsocket(s, FIONBIO, block))
		return LSOCK_ERROR(L, "ioctlsocket()");
#endif

	lua_pushboolean(L, block);
	
	return 1;
}

/* }}} */

/* {{{ lsock_close() */

/* you can also use io.close()... */

static int
lsock_close(lua_State * L)
{
	lsocket s = LSOCK_CHECKSOCKET(L, 1);

#ifdef _WIN32
	if (SOCKFAIL(closesocket(s))
		return LSOCK_STRERROR(L, "closesocket()");
#else
	if (SOCKFAIL(close(s)))
		return LSOCK_STRERROR(L, NULL);
#endif

	lua_pushboolean(L, 1);

	return 1;
}

/* }}} */

/* {{{ sockopt() */

static int
sockopt(lua_State * L)
{
	lsocket s = LSOCK_CHECKSOCKET(L, 1);
	int level = luaL_checknumber(L, 2);
	int oname  = luaL_checknumber(L, 3);

	int get = lua_isnone(L, 4) ? 1 : 0;

	if (SOL_SOCKET != level)
	{
		lua_pushnil(L);
		lua_pushliteral(L, "currently `level' (arg 2) may only be SOL_SOCKET");
		return 2;
	}

	switch (oname)
	{
		/* booleans */
		case SO_ACCEPTCONN:
		case SO_BROADCAST:
		case SO_REUSEADDR:
		case SO_KEEPALIVE:
		case SO_OOBINLINE:
		case SO_DONTROUTE:
		case SO_TIMESTAMP:
		case SO_NO_CHECK:
		case SO_DEBUG:
			{
				int opt = 0;
				socklen_t sz = sizeof(opt);

				if (get)
				{
					if (SOCKFAIL(getsockopt(s, level, oname, &opt, &sz)))
						goto sockopt_failure;

					lua_pushboolean(L, opt);
				}
				else
				{
					luaL_checktype(L, 4, LUA_TBOOLEAN);

					opt = lua_toboolean(L, 4);

					if (SOCKFAIL(setsockopt(s, level, oname, &opt, sz)))
						goto sockopt_failure;
				}
			}
			break;

		/* numbers */
		case SO_RCVBUFFORCE:
		case SO_SNDBUFFORCE:
		case SO_RCVLOWAT:
		case SO_RCVTIMEO:
		case SO_SNDLOWAT:
		case SO_SNDTIMEO:
		case SO_PRIORITY:
		case SO_PROTOCOL:
		case SO_DOMAIN:
		case SO_SNDBUF:
		case SO_RCVBUF:
		case SO_ERROR:
		case SO_TYPE:
			{
				int opt = 0;
				socklen_t sz = sizeof(opt);

				if (get)
				{
					if (SOCKFAIL(getsockopt(s, level, oname, &opt, &sz)))
						goto sockopt_failure;

					lua_pushnumber(L, opt);
				}
				else
				{
					opt = luaL_checknumber(L, 4);

					if (SOCKFAIL(setsockopt(s, level, oname, &opt, sz)))
						goto sockopt_failure;
				}
			}
			break;

		/* linger */
		case SO_LINGER:
			{
				if (get)
				{
					struct linger l;
					socklen_t sz = sizeof(l);

					BZERO(&l, sz);

					if (SOCKFAIL(getsockopt(s, level, oname, &l, &sz)))
						goto sockopt_failure;

					linger_to_table(L, &l);
				}
				else
				{
					struct linger * l = NULL; 
					socklen_t sz = 0;

					luaL_checktype(L, 4, LUA_TTABLE);

					l  = table_to_linger(L, 4);
					sz = lua_rawlen(L, -1);

					if (SOCKFAIL(setsockopt(s, level, oname, l, sz)))
						goto sockopt_failure;
				}
			}
			break;

		case SO_BINDTODEVICE:
			{
				if (get)
				{
					char buf[IFNAMSIZ + 1]; /* +1 for safety (this is like ~17 bytes anyway) */
					socklen_t sz = sizeof(buf);

					BZERO(buf, sizeof(buf));

					if (SOCKFAIL(getsockopt(s, level, oname, buf, &sz)))
						goto sockopt_failure;

					lua_pushlstring(L, buf, sz);
				}
				else
				{
					socklen_t sz = 0;

					const char * opt = luaL_checklstring(L, 4, (size_t *) &sz);

					if (SOCKFAIL(setsockopt(s, level, oname, opt, sz)))
						goto sockopt_failure;
				}
			}
			break;

		default:
			lua_pushnil(L);
			lua_pushfstring(L, "could not %s socket option", lua_isnone(L, 4) ? "get" : "set");

			return 2;

sockopt_failure:
		return LSOCK_STRERROR(L, NULL);
	}

	return get;
}

/* }}} */

/* {{{ lsock_getsockopt() */

static int
lsock_getsockopt(lua_State * L)
{
	int n = lua_gettop(L);

	if (n > 3)
		lua_pop(L, n - 3);

	return sockopt(L);
}

/* }}} */

/* {{{ lsock_setsockopt() */

static int
lsock_setsockopt(lua_State * L)
{
	return sockopt(L);
}

/* }}} */

/* }}} */

/* {{{ lsock_getaddrinfo() */

static int
lsock_getaddrinfo(lua_State * L)
{
	int ret;
	int i = 1;

	struct addrinfo hints, * info, * p;

	/* node and service both cannot be NULL, getaddrinfo() will spout EAI_NONAME */
	const char * nname = lua_isnil(L, 1) ? NULL : luaL_checkstring(L, 1);
	const char * sname = lua_isnil(L, 2) ? NULL : luaL_checkstring(L, 2);

	BZERO(&hints, sizeof(struct addrinfo));

	if (!lua_isnone(L, 3))
	{
		luaL_checktype(L, 3, LUA_TTABLE); /* getaddrinfo('www.google.com', 'http', { ... }) */

		lua_getfield(L, 3, "ai_flags");

		if (!lua_isnil(L, -1))
			hints.ai_flags = lua_tonumber(L, -1);

		lua_pop(L, 1);

		lua_getfield(L, 3, "ai_family");

		if (!lua_isnil(L, -1))
			hints.ai_family = lua_tonumber(L, -1);

		lua_pop(L, 1);

		lua_getfield(L, 3, "ai_socktype");

		if (!lua_isnil(L, -1))
			hints.ai_socktype = lua_tonumber(L, -1);

		lua_pop(L, 1);

		lua_getfield(L, 3, "ai_protocol");

		if (!lua_isnil(L, -1))
			hints.ai_protocol = lua_tonumber(L, -1);

		lua_pop(L, 1);
	}

	info = NULL;

	ret = getaddrinfo(nname, sname, &hints, &info);

	if (0 != ret)
		return LSOCK_GAIERROR(L, ret);

	ret = 1; /* to reflect how many returns */

	lua_newtable(L);

	for (p = info; p != NULL; p = (i++, p->ai_next))
	{
		lua_pushnumber(L, i);

		lua_newtable(L);

		lua_pushnumber(L, p->ai_flags);
		lua_setfield(L, -2, "ai_flags");

		lua_pushnumber(L, p->ai_family);
		lua_setfield(L, -2, "ai_family");

		lua_pushnumber(L, p->ai_socktype);
		lua_setfield(L, -2, "ai_socktype");

		lua_pushnumber(L, p->ai_protocol);
		lua_setfield(L, -2, "ai_protocol");

		lua_pushnumber(L, p->ai_addrlen);
		lua_setfield(L, -2, "ai_addrlen");

		lua_pushlstring(L, (char *) p->ai_addr, p->ai_addrlen);
		lua_setfield(L, -2, "ai_addr");

		if (NULL != p->ai_canonname)
		{
			lua_pushstring(L, p->ai_canonname);
			lua_setfield(L, -2, "ai_canonname");
		}

		lua_settable(L, -3);
	}

	if (NULL != info->ai_canonname)
	{
		lua_pushstring(L, info->ai_canonname);
		ret++;
	}

	freeaddrinfo(info);

	return ret;
}

/* }}} */

/* {{{ lsock_getnameinfo() */

static int
lsock_getnameinfo(lua_State * L)
{
	int stat;

	char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

	size_t       sz    = 0;
	const char * sa    = luaL_checklstring(L, 1, &sz);
	int          flags = 0;

	flags = lua_isnone(L, 2) ? 0 : luaL_checknumber(L, 2);

	BZERO(hbuf, NI_MAXHOST);
	BZERO(sbuf, NI_MAXSERV);

	stat = getnameinfo((struct sockaddr *) sa, sz, hbuf, sizeof(hbuf), sbuf, sizeof(sbuf), flags);

	if (0 != stat)
		return LSOCK_GAIERROR(L, stat);

	lua_pushstring(L, hbuf);
	lua_pushstring(L, sbuf);
	
	return 2;
}

/* }}} */

/* {{{ lsock_select() */

enum { R_SET, W_SET, E_SET };

static int
lsock_select(lua_State * L)
{
	fd_set r, w, e;
	int x, y, stat;

	int highsock = 1;
	struct timeval * t = NULL;

	luaL_checktype(L, 1, LUA_TTABLE); /*  readfds */
	luaL_checktype(L, 2, LUA_TTABLE); /* writefds */
	luaL_checktype(L, 3, LUA_TTABLE); /* errorfds */

	if (!lua_isnoneornil(L, 4))
	{
		luaL_checktype(L, 4, LUA_TTABLE);
		t = table_to_timeval(L, 4);
	}

	FD_ZERO(&r);
	FD_ZERO(&w);
	FD_ZERO(&e);

	highsock = 0;

	/* parse tables into fd_sets */
	for (x = 1; x <= 3; x++)
	{
		int how_many = 0;

		lua_len(L, x);

		how_many = lua_tonumber(L, -1);

		lua_pop(L, 1);

		for (y = 1; y <= how_many; y++)
		{
			int fd = -1;

			lua_pushnumber(L, y);
			lua_gettable(L, x);

			fd = LSOCK_CHECKFD(L, -1);

			highsock = MAX(highsock, fd);

			switch (x)
			{
				case R_SET: FD_SET(fd, &r); break;
				case W_SET: FD_SET(fd, &w); break;
				case E_SET: FD_SET(fd, &e); break;
			}
		}
	}

	stat = select(highsock + 1, &r, &w, &e, t);

	if (-1 == stat)
		LSOCK_STRERROR(L, NULL);

	/* parse fd_sets back into tables */
	for (x = 1; x <= 3; x++)
	{
		int how_many = 0;

		lua_len(L, x);

		how_many = lua_tonumber(L, -1);

		lua_pop(L, 1);

		/* reuse stat for how many array elems we might have */
		lua_createtable(L, stat, 0);

		for (y = 1; y <= how_many; y++)
		{
			int isset =  0;
			int fd    = -1;

			/* push the file handle (socket) userdata */
			lua_pushnumber(L, y);
			lua_gettable(L, x);

			fd = LSOCK_CHECKFD(L, -1);

			switch (x)
			{
				case R_SET: isset = FD_ISSET(fd, &r); break;
				case W_SET: isset = FD_ISSET(fd, &w); break;
				case E_SET: isset = FD_ISSET(fd, &e); break;
			}

			if (isset)
			{
				lua_pushnumber(L, y); /* the numeric index */
				lua_pushvalue(L, -2); /* the file handle */
				lua_settable(L, -4);  /* the new outgoing table */
			}

			/* remove the file handle userdata */
			lua_pop(L, 1);
		}
	}

	/* returns the read, write, and exception tables */
	return 3;
}

/* }}} */

#ifndef _WIN32
/* {{{ lsock_socketpair() */

static int
lsock_socketpair(lua_State * L)
{
	lsocket pair[2] = { -1, -1 };
	int     stat    = -1;

	luaL_Stream * one = NULL;
	luaL_Stream * two = NULL;

	int domain   = luaL_checknumber(L, 1),
		type     = luaL_checknumber(L, 2),
		protocol = lua_isnone(L, 3) ? 0 : luaL_checknumber(L, 3);

	stat = socketpair(domain, type, protocol, pair);

	if (-1 == stat)
		return LSOCK_STRERROR(L, NULL);

	one    = newstream(L);
	one->f = lsocket_to_stream(L, pair[0]);

	two    = newstream(L);
	two->f = lsocket_to_stream(L, pair[1]);

	return 2;
}

/* }}} */

/* {{{ lsock_sendfile() */

/* USERS SHOULD SET THE NEW OFFSET OF `in' AFTER SENDFILE()'ING */

static int
lsock_sendfile(lua_State * L)
{
	int    out    =    LSOCK_CHECKFD(L, 1);
	int    in     =    LSOCK_CHECKFD(L, 2);
	off_t  offset = luaL_checknumber(L, 3);
	size_t count  = luaL_checknumber(L, 4);

	/* I know sendfile() acts different when offset is NULL,
	** but you can get the current offset of `in' from Lua with :seek() */
	ssize_t sent = sendfile(out, in, &offset, count);

	if (-1 == sent)
		return LSOCK_STRERROR(L, NULL);

	lua_pushnumber(L, sent);
	lua_pushnumber(L, offset); /* just for aqua */

	return 2;
}

/* }}} */
#endif

/* {{{ lsock_getfd() */

/* for working with (some) luasocket stuff */

static int
lsock_getfd(lua_State * L)
{
	luaL_Stream * file = LSOCK_CHECKSTREAM(L, 1);

	lua_pushnumber(L, stream_to_fd(L, file->f));

	return 1;
}

/* }}} */

/* {{{ lsock_getfh() */

static int
lsock_getfh(lua_State * L)
{
	luaL_Stream * stream = LSOCK_CHECKSTREAM(L, 1);

	lua_pushnumber(L, stream_to_lsocket(L, stream->f));

	return 1;
}

/* }}} */

/* {{{ lsock_getstream() */

static int
lsock_getstream(lua_State * L)
{
	lua_pushlightuserdata(L, (LSOCK_CHECKSTREAM(L, 1))->f);

	return 1;
}

/* }}} */

#ifdef _WIN32
/* {{{ lsock_cleanup() */

static void
lsock_cleanup(lua_State * L)
{
	((void) L);

	WSACleanup();
}

/* }}} */
#endif

/* {{{ luaopen_lsock_core() */

#define LUA_REG(x) { #x, lsock_##x }

static luaL_Reg lsocklib[] =
{
	/* alphabetical */
	LUA_REG(accept),
	LUA_REG(bind),
	LUA_REG(blocking),
	LUA_REG(close),
	LUA_REG(connect),
	LUA_REG(gai_strerror),
	LUA_REG(getaddrinfo),
	LUA_REG(getfd),
	LUA_REG(getfh),
	LUA_REG(getpeername),
	LUA_REG(getnameinfo),
	LUA_REG(getsockname),
	LUA_REG(getsockopt),
	LUA_REG(getstream),
	LUA_REG(htons),
	LUA_REG(ntohs),
	LUA_REG(htonl),
	LUA_REG(ntohl),
	LUA_REG(listen),
	LUA_REG(pack_sockaddr),
	LUA_REG(recv),
	LUA_REG(recvfrom),
	LUA_REG(select),
#ifndef _WIN32
	LUA_REG(sendfile),
#endif
	LUA_REG(send),
	LUA_REG(sendto),
	LUA_REG(setsockopt),
	LUA_REG(shutdown),
	LUA_REG(socket),
#ifndef _WIN32
	LUA_REG(socketpair),
#endif
	LUA_REG(strerror),
	LUA_REG(unpack_sockaddr),
	{ NULL, NULL }
};

#undef LUA_REG


LUALIB_API int
luaopen_lsock_core(lua_State * L)
{
#ifdef _WIN32
	WSADATA wsaData;

	int stat = WSAStartup(MAKEWORD(2, 2), &wsaData);

	if (stat != 0)
	{
		WSACleanup();
		luaL_error(L, "WSAStartup() failed [%d]: %s\n", stat, strerror(stat));
	}

	if
	(
		2 != LOBYTE(wsaData.wVersion) ||
		2 != HIBYTE(wsaData.wVersion)
	)
	{
		WSACleanup();
		luaL_error(L, "Could not find a usable version of Winsock.dll");
	}
#endif

	luaL_newlib(L, lsocklib);

#define LSOCK_CONST(C) \
    lua_pushinteger(L, C);  \
    lua_setfield(L, -2, #C)

	/* protocol family constants */
	LSOCK_CONST(PF_APPLETALK);
	LSOCK_CONST(PF_INET     );
	LSOCK_CONST(PF_INET6    );
	LSOCK_CONST(PF_IPX      );
	LSOCK_CONST(PF_IRDA     );
	LSOCK_CONST(PF_LOCAL    );
	LSOCK_CONST(PF_UNIX     );
	LSOCK_CONST(PF_UNSPEC   );

	/* address family constants */
	LSOCK_CONST(AF_APPLETALK);
	LSOCK_CONST(AF_INET     );
	LSOCK_CONST(AF_INET6    );
	LSOCK_CONST(AF_IPX      );
	LSOCK_CONST(AF_IRDA     );
	LSOCK_CONST(AF_UNIX     );
	LSOCK_CONST(AF_UNSPEC   );

	/* socket type constants */
	LSOCK_CONST(SOCK_DGRAM    );
	LSOCK_CONST(SOCK_RAW      );
	LSOCK_CONST(SOCK_RDM      );
	LSOCK_CONST(SOCK_SEQPACKET);
	LSOCK_CONST(SOCK_STREAM   );

	/* protocol constants */
	LSOCK_CONST(IPPROTO_ICMP  );
	LSOCK_CONST(IPPROTO_ICMPV6);
	LSOCK_CONST(IPPROTO_IGMP  );
	LSOCK_CONST(IPPROTO_TCP   );
	LSOCK_CONST(IPPROTO_UDP   );

	/* errno's, alphabetical */
	LSOCK_CONST(EACCES         );
	LSOCK_CONST(EADDRINUSE     );
	LSOCK_CONST(EADDRNOTAVAIL  );
	LSOCK_CONST(EAFNOSUPPORT   );
	LSOCK_CONST(EAGAIN         );
	LSOCK_CONST(EBADF          );
	LSOCK_CONST(ECONNABORTED   );
	LSOCK_CONST(EDESTADDRREQ   );
	LSOCK_CONST(EINVAL         );
	LSOCK_CONST(EINTR          );
	LSOCK_CONST(EIO            );
	LSOCK_CONST(EISDIR         );
	LSOCK_CONST(ELOOP          );
	LSOCK_CONST(EMFILE         );
	LSOCK_CONST(ENAMETOOLONG   );
	LSOCK_CONST(ENFILE         );
	LSOCK_CONST(ENOBUFS        );
	LSOCK_CONST(ENOENT         );
	LSOCK_CONST(ENOTDIR        );
	LSOCK_CONST(ENOTSOCK       );
	LSOCK_CONST(ENOBUFS        );
	LSOCK_CONST(ENOMEM         );
	LSOCK_CONST(ENOTCONN       );
	LSOCK_CONST(ENOTSOCK       );
	LSOCK_CONST(EOPNOTSUPP     );
	LSOCK_CONST(EPROTO         );
	LSOCK_CONST(EPROTONOSUPPORT);
	LSOCK_CONST(EPROTOTYPE     );
	LSOCK_CONST(EROFS          );
	LSOCK_CONST(EWOULDBLOCK    );

	/* getaddrinfo() constants */
	LSOCK_CONST(AI_ADDRCONFIG              );
	LSOCK_CONST(AI_ALL                     );
	LSOCK_CONST(AI_CANONIDN                );
	LSOCK_CONST(AI_CANONNAME               );
	LSOCK_CONST(AI_IDN                     );
	LSOCK_CONST(AI_IDN_ALLOW_UNASSIGNED    );
	LSOCK_CONST(AI_IDN_USE_STD3_ASCII_RULES);
	LSOCK_CONST(AI_NUMERICHOST             );
	LSOCK_CONST(AI_NUMERICSERV             );
	LSOCK_CONST(AI_PASSIVE                 );
	LSOCK_CONST(AI_V4MAPPED                );

	/* getnameinfo() constants */
	LSOCK_CONST(NI_DGRAM                   );
	LSOCK_CONST(NI_IDN                     );
	LSOCK_CONST(NI_IDN_ALLOW_UNASSIGNED    );
	LSOCK_CONST(NI_IDN_USE_STD3_ASCII_RULES);
	LSOCK_CONST(NI_NAMEREQD                );
	LSOCK_CONST(NI_NOFQDN                  );
	LSOCK_CONST(NI_NUMERICHOST             );
	LSOCK_CONST(NI_NUMERICSERV             );

	/* getaddrinfo() errors */
	LSOCK_CONST(EAI_ADDRFAMILY);
	LSOCK_CONST(EAI_AGAIN     );
	LSOCK_CONST(EAI_BADFLAGS  );
	LSOCK_CONST(EAI_FAIL      );
	LSOCK_CONST(EAI_FAMILY    );
	LSOCK_CONST(EAI_MEMORY    );
	LSOCK_CONST(EAI_NODATA    );
	LSOCK_CONST(EAI_NONAME    );
	LSOCK_CONST(EAI_OVERFLOW  );
	LSOCK_CONST(EAI_SERVICE   );
	LSOCK_CONST(EAI_SOCKTYPE  );
	LSOCK_CONST(EAI_SYSTEM    );

	/* send() & recv() flag constants */
	LSOCK_CONST(MSG_EOR    );
	LSOCK_CONST(MSG_OOB    );
	LSOCK_CONST(MSG_PEEK   );
	LSOCK_CONST(MSG_WAITALL);

	LSOCK_CONST(SHUT_RD  );
	LSOCK_CONST(SHUT_RDWR);
	LSOCK_CONST(SHUT_WR  );

	/* getsockopt()/setsockopt() constants */
	LSOCK_CONST(SOL_SOCKET     );
	LSOCK_CONST(SO_TYPE        );
	LSOCK_CONST(SO_DEBUG       );
	LSOCK_CONST(SO_ACCEPTCONN  );
	LSOCK_CONST(SO_REUSEADDR   );
	LSOCK_CONST(SO_KEEPALIVE   );
	LSOCK_CONST(SO_LINGER      );
	LSOCK_CONST(SO_OOBINLINE   );
	LSOCK_CONST(SO_SNDBUF      );
	LSOCK_CONST(SO_RCVBUF      );
	LSOCK_CONST(SO_ERROR       );
	LSOCK_CONST(SO_DONTROUTE   );
	LSOCK_CONST(SO_RCVLOWAT    );
	LSOCK_CONST(SO_RCVTIMEO    );
	LSOCK_CONST(SO_SNDLOWAT    );
	LSOCK_CONST(SO_SNDTIMEO    );
	LSOCK_CONST(SO_TIMESTAMP   );
	LSOCK_CONST(SO_PROTOCOL    );
	LSOCK_CONST(SO_PRIORITY    );
	LSOCK_CONST(SO_DOMAIN      );
	LSOCK_CONST(SO_BROADCAST   );
	LSOCK_CONST(SO_BINDTODEVICE);
	LSOCK_CONST(SO_RCVBUFFORCE );
	LSOCK_CONST(SO_SNDBUFFORCE );

#undef LSOCK_CONST

#ifdef _WIN32
	lua_puchcfunction(L, &lsock_cleanup);
	lua_setfield(L, -2, "__gc");
	lua_pushvalue(L, -1)
	lua_setmetatable(L, -1) /* it is its own metatable */
#endif

	return 1;
}

/* }}} */
