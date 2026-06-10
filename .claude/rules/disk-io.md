---
paths:
  - "include/libtorrent/disk_interface.hpp"
  - "src/mmap_disk_io*"
  - "src/posix_disk_io*"
  - "src/pread_disk_io*"
  - "src/disk_job_fence*"
---

## `disk_interface` contract

`disk_interface` (`include/libtorrent/disk_interface.hpp`) is the session-level
customization point for disk I/O, selected via
`session_params::disk_io_constructor`. Built-in implementations: `mmap_disk_io`,
`posix_disk_io`, `pread_disk_io`.

**Threading.** Every `async_*` method is called from libtorrent's single network
thread. The work may be performed asynchronously on the implementation's own
threads, but each completion handler **must be posted back to the network
thread** via the `io_context` given to the constructor; handlers run on the
network thread. An implementation may use any number of threads and service
queued jobs concurrently and out of order, except where the contract below
requires ordering.

**`async_hash` reflects all prior `async_write`s.** For a given piece, the
engine posts all of the piece's blocks via `async_write`, then posts
`async_hash` for that piece. The hash returned **must be computed over the data
of every `async_write` for that piece that was posted before it.** This is the
key constraint for a multi-threaded back end that services jobs out of order: it
must order a piece's hash after that piece's writes and must never hash stale
or missing data. The piece picker treats the returned hash as authoritative for
the data it handed over. `async_hash2` (single v2 block hash) has the same
requirement for the block it covers.

**Fence jobs.** Several operations must run with exclusive access to a storage,
synchronized against all other I/O on it. These are *fence jobs*. A fence job:

- waits for all jobs currently outstanding **on that storage** to finish, then
  executes -- so it runs alone, with no other operation on the storage in
  flight;
- *fences* everything posted after it: any job posted to the same storage while
  the fence is raised is queued and not executed until the fence job completes.

Fences are per-storage -- a fence on one storage does not block jobs on another.
The fence operations are:

- `async_clear_piece` -- synchronize with all outstanding I/O to the piece
  before the engine re-requests its blocks
- `async_move_storage`
- `async_release_files`
- `async_stop_torrent`
- `async_rename_file`
- `async_delete_files`
- `async_set_file_priority`
- `async_check_files`

`async_read`, `async_write`, `async_hash`, and `async_hash2` are ordinary
(non-fence) jobs. The built-in back ends implement the fence semantics with
`disk_job_fence` (`src/disk_job_fence.cpp`).

**Back-pressure.** `async_write` returns `true` when the write queue is over the
high watermark; the caller then stops issuing writes for that peer until the
`disk_observer` passed in is notified that the queue has drained. `false` means
the caller may keep writing.

**Batching.** `async_*` calls only enqueue work; `submit_jobs()` is called once
after a batch to wake the disk thread(s). An implementation may also wake on each
`async_*`, but should tolerate the batched notification.
