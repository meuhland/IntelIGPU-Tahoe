# GPU stall and recovery: verified scope

Updated 2026-09-28. This page publishes only conclusions that have been
verified; the current public driver does **not** solve random RCS/CCS stalls
or the shared-lock wait when stopping a queue.

## Verified

- In one real fault, WindowServer's submission thread was waiting on the
  accelerator's shared lock; another thread held that lock while stopping a
  command queue, waiting for a GPU event, and timing out on recovery, which
  finally tripped the WindowServer watchdog. Early in the fault a halt of GPU
  hardware progress was also observed; the existing evidence is not enough to
  single out the first hardware-stall cause.
- A read-only lifetime audit of the native code for the target machine is
  complete: after the queue-stop function returns, the caller still cleans up
  the queue and resources under the same lock. So making only the inner stop
  function return early cannot safely move the wait out of the lock.
- The offline recovery strategy, the resource-hold candidates, and the
  shared-set lifetime ledger have passed host tests; a kernel target-file check
  confirms that the controlled set's `merge(const OSSet*)` override sits in an
  audited call slot. None of them is registered as a running-driver callback,
  and none proves the real resource graph is complete.

## Not yet done

A safe fix also needs to record every set's creation, merge, and release from
the first shared client onward, and to take a persistent reference while the
native reference is still valid. It must cover the RCS/CCS submission and
completion callbacks, page-table updates, event notifications, and display
resource release together, in order to wait outside the lock and decide when to
return resources. A failed task also needs a real error exit; it must not fake
GPU completion or release memory early that could still be accessed.

These candidates are still under independent experimentation. The public main
branch does not contain an unverified stop-entry replacement, a modified Apple
binary, fault-scene logs, or personal diagnostic data. The currently published
one-command startup and the MapKit compatibility fix must not be treated as a
GPU-stall fix.
