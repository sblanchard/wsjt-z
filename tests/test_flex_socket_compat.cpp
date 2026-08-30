#include <QtTest>
#include <QElapsedTimer>

#include "Transceiver/FlexSocketCompat.hpp"

class TestFlexSocketCompat : public QObject
{
  Q_OBJECT

private slots:
  void socket_lifecycle ();
  void startup_and_cleanup_succeed ();
  void receive_timeout_is_honoured ();
  void receive_buffer_is_reported ();
  void retryable_errors_classified ();
  void sigpipe_suppression_does_not_fail ();
};

void TestFlexSocketCompat::startup_and_cleanup_succeed ()
{
  WSADATA data {};
  QCOMPARE (WSAStartup (MAKEWORD (2, 2), &data), 0);
  QCOMPARE (WSACleanup (), 0);
}

void TestFlexSocketCompat::socket_lifecycle ()
{
  SOCKET const s = ::socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  QVERIFY (INVALID_SOCKET != s);

  sockaddr_in local {};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
  local.sin_port = htons (0);           // let the OS choose

  QCOMPARE (::bind (s, reinterpret_cast<sockaddr *> (&local), sizeof (local)), 0);

  shutdown (s, SD_BOTH);                // must compile and not crash
  QCOMPARE (closesocket (s), 0);
}

void TestFlexSocketCompat::receive_timeout_is_honoured ()
{
  SOCKET const s = ::socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  QVERIFY (INVALID_SOCKET != s);

  sockaddr_in local {};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
  local.sin_port = htons (0);
  QCOMPARE (::bind (s, reinterpret_cast<sockaddr *> (&local), sizeof (local)), 0);

  QVERIFY (flexSetReceiveTimeout (s, 200));

  char buffer [64];
  QElapsedTimer clock;
  clock.start ();

  int const received = ::recv (s, buffer, sizeof (buffer), 0);
  qint64 const elapsed = clock.elapsed ();

  QCOMPARE (received, -1);
  QVERIFY2 (flexIsRetryableReceiveError (WSAGetLastError ()),
            "a timed-out recv must classify as retryable, not fatal");

  // Generous bounds: we are asserting the timeout took effect at all,
  // not measuring scheduler precision.
  QVERIFY2 (elapsed >= 150, qPrintable (QString ("returned after only %1 ms").arg (elapsed)));
  QVERIFY2 (elapsed <  2000, qPrintable (QString ("blocked for %1 ms").arg (elapsed)));

  closesocket (s);
}

void TestFlexSocketCompat::receive_buffer_is_reported ()
{
  SOCKET const s = ::socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  QVERIFY (INVALID_SOCKET != s);

  int const obtained = flexSetReceiveBuffer (s, 1024 * 1024);

  // Linux doubles the request and clamps to net.core.rmem_max; macOS may
  // clamp too. We require only that we learn the real value and that it is
  // large enough to matter.
  QVERIFY2 (obtained > 0, "must report the buffer size actually obtained");
  QVERIFY2 (obtained >= 64 * 1024,
            qPrintable (QString ("receive buffer only %1 bytes").arg (obtained)));

  closesocket (s);
}

void TestFlexSocketCompat::retryable_errors_classified ()
{
  QVERIFY (flexIsRetryableReceiveError (WSAETIMEDOUT));
  QVERIFY (flexIsRetryableReceiveError (WSAEWOULDBLOCK));
#ifndef Q_OS_WIN
  QVERIFY2 (flexIsRetryableReceiveError (EINTR),
            "EINTR must be retryable or a signal will kill the VITA stream");
#endif
  QVERIFY (!flexIsRetryableReceiveError (ECONNRESET));
}

void TestFlexSocketCompat::sigpipe_suppression_does_not_fail ()
{
  SOCKET const s = ::socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
  QVERIFY (INVALID_SOCKET != s);
  flexSuppressSigPipe (s);              // must be safe on every platform
  closesocket (s);
}

QTEST_MAIN (TestFlexSocketCompat)
#include "test_flex_socket_compat.moc"
