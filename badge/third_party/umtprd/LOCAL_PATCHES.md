# Local patches to uMTP-Responder 1.8.1

1. `src/umtprd.c`: only treat `flock()` failure as "already running" when the
   error is `EWOULDBLOCK`/`EAGAIN`. Some uClibc/kernel combinations do not
   support `flock()` on POSIX message-queue descriptors; the original code then
   wrongly exits with "Already running". The init script remains responsible
   for single-instance management.
