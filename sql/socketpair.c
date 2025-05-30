/* socketpair.c
Copyright 2007, 2010 by Nathan C. Myers <ncm@cantrip.org>
Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    Redistributions of source code must retain the above copyright notice, this
    list of conditions and the following disclaimer.

    Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

    The name of the author must not be used to endorse or promote products
    derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Changes:
 * 2023-12-25 Adapted for MariaDB usage
 * 2014-02-12: merge David Woodhouse, Ger Hobbelt improvements
 *     git.infradead.org/users/dwmw2/openconnect.git/commitdiff/bdeefa54
 *     github.com/GerHobbelt/selectable-socketpair
 *   always init the socks[] to -1/INVALID_SOCKET on error, both on Win32/64
 *   and UNIX/other platforms
 * 2013-07-18: Change to BSD 3-clause license
 * 2010-03-31:
 *   set addr to 127.0.0.1 because win32 getsockname does not always set it.
 * 2010-02-25:
 *   set SO_REUSEADDR option to avoid leaking some windows resource.
 *   Windows System Error 10049, "Event ID 4226 TCP/IP has reached
 *   the security limit imposed on the number of concurrent TCP connect
 *   attempts."  Bleah.
 * 2007-04-25:
 *   preserve value of WSAGetLastError() on all error returns.
 * 2007-04-22:  (Thanks to Matthew Gregan <kinetik@flim.org>)
 *   s/EINVAL/WSAEINVAL/ fix trivial compile failure
 *   s/socket/WSASocket/ enable creation of sockets suitable as stdin/stdout
 *     of a child process.
 *   add argument make_overlapped
 */

#include <my_global.h>
#ifdef _WIN32
#include <ws2tcpip.h>  /* socklen_t, et al (MSVC20xx) */
#include <windows.h>
#include <io.h>
#include "socketpair.h"

#define safe_errno (errno != 0) ? errno : -1

/**
  create_socketpair()

  @param socks[2]   Will be filled by 2 SOCKET entries (similar to pipe())
                    socks[0] for reading
                    socks[1] for writing

  @return: 0  ok
          #  System error code. -1 if unknown
 */

int create_socketpair(SOCKET socks[2])
{
  union
  {
    struct sockaddr_in inaddr;
    struct sockaddr addr;
  } a;
  SOCKET listener= -1;
  int reuse = 1;
  int last_error;
  socklen_t addrlen = sizeof(a.inaddr);

  socks[0]= socks[1]= -1;

  if ((listener= socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1)
    return safe_errno;

  memset(&a, 0, sizeof(a));
  a.inaddr.sin_family = AF_INET;
  a.inaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.inaddr.sin_port = 0;

  for (;;)                                      /* Avoid using goto */
  {
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   (char*) &reuse, (socklen_t) sizeof(reuse)) == -1)
      break;
    if  (bind(listener, &a.addr, sizeof(a.inaddr)) == SOCKET_ERROR)
      break;

    memset(&a, 0, sizeof(a));
    if  (getsockname(listener, &a.addr, &addrlen) == SOCKET_ERROR)
      break;
    // win32 getsockname may only set the port number, p=0.0005.
    // ( http://msdn.microsoft.com/library/ms738543.aspx ):
    a.inaddr.sin_addr.s_addr= htonl(INADDR_LOOPBACK);
    a.inaddr.sin_family= AF_INET;

    if (listen(listener, 1) == SOCKET_ERROR)
      break;

    socks[1]= socket(AF_INET, SOCK_STREAM, 0);
    if (socks[1] == -1)
      break;
    if (connect(socks[1], &a.addr, sizeof(a.inaddr)) == SOCKET_ERROR)
      break;

    socks[0]= accept(listener, NULL, NULL);
    if (socks[0] == -1)
      break;

    closesocket(listener);

    {
      /* Make both sockets non blocking */
      ulong arg= 1;
      ioctlsocket(socks[0], FIONBIO,(void*) &arg);
      ioctlsocket(socks[1], FIONBIO,(void*) &arg);
    }
    return 0;
  }
  /* Error handling */
  last_error= WSAGetLastError();
  if (listener != -1)
    closesocket(listener);
  close_socketpair(socks);
  WSASetLastError(last_error);

  return last_error;
}

/*
  Free socketpair
*/

void close_socketpair(SOCKET socks[2])
{
  if (socks[0] != -1)
    closesocket(socks[0]);
  if (socks[1] != -1)
    closesocket(socks[1]);
  socks[0]= socks[1]= -1;
}

#endif /*_WIN32 */
