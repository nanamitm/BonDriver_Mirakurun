# Driver safety regression tests

Run from PowerShell on Windows with LLVM clang++ available on PATH:

    ./tests/run-driver-safety.ps1

Initialize the dantto4k submodule first. The runner compiles a standalone executable
under work/driver-safety and runs it; it does not require TSDuck libraries, a
Mirakurun installation, a tuner, or a smart card.

The executable includes the production driver implementation. It exercises HTTP
parsing over loopback sockets, IPv4-only localhost channel selection, delivery of
initial stream bytes, partially started worker shutdown, conversion worker
shutdown after a delay longer than one second, and a deterministic race between
MMT input dequeue and purge. A delayed synthetic OVERLAPPED completion checks
that shutdown retains buffers beyond the former one-second timeout.

The MMT converter and recording exports are test doubles. These tests validate
thread and buffer ownership in the driver, not MMT demuxing, CAS cancellation,
recording, or playback with TVTest. Build the solution separately to validate the
real converter and recorder linkage.
