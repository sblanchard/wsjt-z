#ifndef W7PP_FLEX_SOCKET_COMPAT_HPP
#define W7PP_FLEX_SOCKET_COMPAT_HPP

/*
 * Portability shim for the W7PP Native FLEX VITA receiver.
 *
 * FlexVitaReceiver.cpp was written against Winsock. This header supplies
 * the same vocabulary on POSIX so that file stays diffable against future
 * W7PP releases.
 *
 * Three differences are real behaviour, not spelling, and are handled by
 * the helper functions rather than by macros:
 *
 *   1. SIGPIPE. On POSIX, send() to a socket the peer closed raises
 *      SIGPIPE, which by default terminates the process. Windows has no
 *      equivalent. Use flexSuppressSigPipe() plus flexSendFlags().
 *
 *   2. EINTR. Blocking recv()/recvfrom() on POSIX can return EINTR when a
 *      signal arrives. Treat it exactly like a timeout, or a stray signal
 *      drops the VITA stream. Use flexIsRetryableReceiveError().
 *
 *   3. SO_RCVTIMEO takes DWORD milliseconds on Windows and a struct
 *      timeval on POSIX; SO_RCVBUF is doubled and clamped by Linux. Use
 *      flexSetReceiveTimeout() and flexSetReceiveBuffer().
 */

#include <QtGlobal>

#if defined (Q_OS_WIN)

# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif

# include <winsock2.h>
# include <ws2tcpip.h>

#else

# include <arpa/inet.h>
# include <errno.h>
# include <netinet/in.h>
# include <sys/select.h>
# include <sys/socket.h>
# include <sys/time.h>
# include <sys/types.h>
# include <unistd.h>

typedef int SOCKET;

# ifndef INVALID_SOCKET
#  define INVALID_SOCKET (-1)
# endif

# ifndef SOCKET_ERROR
#  define SOCKET_ERROR (-1)
# endif

# ifndef SD_BOTH
#  define SD_BOTH SHUT_RDWR
# endif

# ifndef WSAETIMEDOUT
#  define WSAETIMEDOUT ETIMEDOUT
# endif

# ifndef WSAEWOULDBLOCK
#  define WSAEWOULDBLOCK EWOULDBLOCK
# endif

# ifndef MAKEWORD
#  define MAKEWORD(low, high) \
    (static_cast<unsigned short> ( \
        (static_cast<unsigned char> (low)) | \
        (static_cast<unsigned short> (static_cast<unsigned char> (high)) << 8)))
# endif

struct WSADATA
{
  int unused;
};

inline int closesocket (SOCKET socket)
{
  return ::close (socket);
}

inline int WSAStartup (unsigned short, WSADATA *)
{
  return 0;
}

inline int WSACleanup ()
{
  return 0;
}

inline int WSAGetLastError ()
{
  return errno;
}

#endif  // Q_OS_WIN

/*
 * Set the blocking receive timeout, in milliseconds.
 */
inline bool flexSetReceiveTimeout (SOCKET socket, int milliseconds)
{
#if defined (Q_OS_WIN)
  DWORD timeout = static_cast<DWORD> (milliseconds);

  return SOCKET_ERROR != setsockopt (
      socket,
      SOL_SOCKET,
      SO_RCVTIMEO,
      reinterpret_cast<char const *> (&timeout),
      sizeof (timeout));
#else
  timeval timeout {};

  timeout.tv_sec  = milliseconds / 1000;
  timeout.tv_usec = (milliseconds % 1000) * 1000;

  return SOCKET_ERROR != setsockopt (
      socket,
      SOL_SOCKET,
      SO_RCVTIMEO,
      &timeout,
      sizeof (timeout));
#endif
}

/*
 * Request a receive buffer and report what was actually granted.
 *
 * Linux doubles the request for bookkeeping and clamps it against
 * net.core.rmem_max, so the requested value is not what you get. An
 * undersized buffer shows up as dropped VITA packets under load, which
 * looks like decode loss rather than an error - the return value lets a
 * caller that cares about that distinction inspect what it actually got.
 * A caller that does not need that distinction may ignore the return
 * value; a smaller-than-requested buffer is not itself a failure.
 */
inline int flexSetReceiveBuffer (SOCKET socket, int bytes)
{
  if (SOCKET_ERROR == setsockopt (
          socket,
          SOL_SOCKET,
          SO_RCVBUF,
          reinterpret_cast<char const *> (&bytes),
          sizeof (bytes)))
    {
      return -1;
    }

  int obtained = 0;

#if defined (Q_OS_WIN)
  int length = sizeof (obtained);
#else
  socklen_t length = sizeof (obtained);
#endif

  if (SOCKET_ERROR == getsockopt (
          socket,
          SOL_SOCKET,
          SO_RCVBUF,
          reinterpret_cast<char *> (&obtained),
          &length))
    {
      return -1;
    }

  return obtained;
}

/*
 * Stop a write to a closed socket from killing the process.
 *
 * macOS and the BSDs use the SO_NOSIGPIPE socket option; Linux has no
 * such option and uses the MSG_NOSIGNAL send flag instead - see
 * flexSendFlags(). Windows needs neither.
 */
inline void flexSuppressSigPipe (SOCKET socket)
{
#if defined (SO_NOSIGPIPE)
  int on = 1;

  setsockopt (
      socket,
      SOL_SOCKET,
      SO_NOSIGPIPE,
      reinterpret_cast<char const *> (&on),
      sizeof (on));
#else
  Q_UNUSED (socket);
#endif
}

inline int flexSendFlags ()
{
#if defined (MSG_NOSIGNAL)
  return MSG_NOSIGNAL;
#else
  return 0;
#endif
}

/*
 * True when a failed blocking receive should simply be retried.
 */
inline bool flexIsRetryableReceiveError (int error)
{
  if (WSAETIMEDOUT == error || WSAEWOULDBLOCK == error)
    return true;

#if !defined (Q_OS_WIN)
  if (EINTR == error || EAGAIN == error)
    return true;
#endif

  return false;
}

#endif
