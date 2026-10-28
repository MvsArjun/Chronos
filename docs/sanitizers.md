# Sanitizer verification

AddressSanitizer plus UndefinedBehaviorSanitizer and ThreadSanitizer run in
separate Linux builds. Python and benchmarks are disabled in sanitizer builds;
both GCC and Clang Release builds exercise the Python API and full workflows.
`tools/verify_linux.py` runs all six configurations, saves command logs and XML,
and records a hash of the source/configuration before and after verification.

```sh
python tools/verify_linux.py
```

The original verification host was Windows. A disposable Ubuntu 24.04.5 guest
under the already installed VMware runtime was used to close the Linux coverage
gap. It had two virtual CPUs, 4 GiB RAM, GCC 13.3, Clang 18.1.3 and kernel
6.8.0-147. Compiler/runtimes and Python dependencies came from verified package
repositories. No Windows features were installed or enabled.

## ThreadSanitizer address-space precondition

With the guest's default `vm.mmap_rnd_bits=32`, GCC 13 ThreadSanitizer aborted
with `unexpected memory mapping` even for a correct, separate atomic/thread
canary program. This was a runtime initialization failure, not a Chronos test
success. The disposable guest was adjusted to:

```sh
sudo sysctl -w vm.mmap_rnd_bits=28
```

Both GCC and Clang then ran the correct canary with exit zero. A separate program
containing an intentional unsynchronized write race produced
`WARNING: ThreadSanitizer: data race` and exit 66 under both compilers. These
positive/negative controls establish that the runtime can initialize and report
a race. They are not project tests, and their expected warnings must not be
interpreted as Chronos findings.

The same ephemeral Linux CI setting is applied only to the TSan jobs. It changes
the test guest/runner's address randomization; it does not disable sanitizer
instrumentation or suppress race diagnostics. The Windows host is unchanged.
Do not copy test-environment settings blindly onto a shared production host.

Actual project outcomes and source hashes are recorded in
[the pre-upload evidence](results/pre-upload/), including runtime controls,
per-configuration XML and the complete matrix summary. A missing runtime,
timeout, crash or failed test remains a failed verification step.
