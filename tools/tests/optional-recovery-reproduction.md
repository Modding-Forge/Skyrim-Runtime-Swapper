# BoAW-Clean optional-selection recovery regression

The defect was confirmed on Windows against the 1.2.2 production core (base
commit `7b30103`). This harness now asserts successful recovery of those cases
with the fix, safe failure at injected write boundaries, and continued rejection
of pre-existing inconsistent state. A zero exit code means every expectation
passed. The depot-based probe is not added to ordinary CTest because it requires
local, legally obtained Bethesda source files. Synthetic selection and guard
coverage runs automatically as `recovery_selection_tests` on Windows and Linux.

## Run

Configure a separate or existing BoAW-Clean build with `BUILD_TESTING=ON` and
`SKYRIM_RUNTIME_PATCH_MANIFEST` pointing to
`assets/runtime/1.7.104-to-1.6.1170-clean/manifest.json`, then:

```powershell
cmake --build build/1.6.1170/boaw-clean --config Release --target optional_recovery_probe
./tools/tests/reproduce-optional-recovery.ps1 `
  -Probe build/1.6.1170/boaw-clean/Release/optional_recovery_probe.exe `
  -SourceCore '<1.7.104 core depot root>' `
  -SourceExe '<1.7.104 executable depot root>' `
  -OriginalBeafarmer '<supported original Beafarmer file>' `
  -CleanBeafarmer '<supported cleaned Beafarmer file>' `
  -TestParent '<existing short writable directory>'
```

The runner checks the source hashes against the release manifest and creates
fresh plain-file copies beneath a unique sandbox. It never launches Skyrim,
changes Defender settings, or writes to the input depot files or installed game.
It retains sandbox files and per-case logs, including failed recovery journals.
Allow disk space for multiple copies of the managed files and control backups.

## Before and after (Windows, 2026-09-03)

| Initial state | 1.2.2 baseline | Fixed core |
| --- | --- | --- |
| All ten files original; no manifest | Success, unchanged | Success, unchanged |
| Nine original files; Beafarmer absent; no manifest | Success, unchanged | Success, unchanged |
| Nine original files, exact clean Beafarmer; verified backups and manifest | Beafarmer restored | Beafarmer restored |
| Same mixed files and verified backups, but no manifest | Error 35; live hashes unchanged | Beafarmer restored |
| Same mixed files with neither backups nor manifest, three fresh fixtures | Error 35 in all three; live hashes unchanged | Beafarmer restored in all three |

Every reproduced baseline failure leaves one valid recovery-journal record but no
manifest. Layout detection then becomes invalid and
`inspect_persistent_runtime` returns `invalid`. The application maps that state
to error 34, "The persistent recovery markers are inconsistent."
The probe's direct recovery retry instead returns error 35, reporting an
unavailable vault because the saved optional-file selection is missing.

The fixed run also checks these four cases (11 total):

- A valid prior recovery journal without its selection manifest remains blocked,
  with every live hash and the journal hash unchanged on both attempts.
- Failure before the manifest write leaves neither a manifest nor a journal.
- Failure after the verified atomic manifest write leaves a valid selection,
  without creating a journal.
- Failure after the recovery journal's directory sync leaves a valid selection
  and one readable journal record.

Each injected failure leaves all live hashes unchanged. The runner clears the
fault and starts a new probe process for each of the three interrupted cases;
all resume successfully. Every successful recovery verifies the source runtime
and an additional no-change retry. These are deterministic I/O fault simulations,
not physical power-loss tests. The validated fixed runs are
`srs-bee-repro-6ffbf93e` and `srs-bee-repro-c737a3ce` (the latter also re-hashes
all live files after the rejected retry). The original baseline with backup control is
`srs-bee-repro-5b2929d4`. Per-case logs remain under the supplied test parent.

All five profile builds also passed their complete CTest suites with the fix:
80 Windows tests (16 per profile) and 45 native Linux tests (9 per profile).
No tests were excluded. These checks do not launch Skyrim or simulate antivirus
quarantine; they establish the recovery defect and the behavior of this fix.

## Cause established by the tests

1. The initial optional-file selection succeeds by checking Beafarmer presence
   while there is no manifest or journal.
2. `recover_to_source` recognizes nine source files and one exact target file.
   The existing reverse patch passes preflight.
3. It writes `recovery_started` without first saving the optional-file manifest.
4. Its next `runtime_layout_matches` call re-reads the selection.
5. `beafarmer_selected` correctly rejects a journal without its selection
   manifest, so recovery rejects its own newly created state before modifying
   any live file. The error text misleadingly says the managed path changed.

Verified source backups alone do not prevent this. Persisting the manifest
before journal creation is the differentiating control, not a proposed
relaxation of missing-manifest checks.

The defect requires no repeated launches, external file deletion or antivirus
interference to reproduce. This establishes an SRS defect matching the report,
but does not establish what originally put Ixion's files into that state.

## Fix and safety boundary

After all recovery inputs pass preflight, fresh optional-file selections are
written atomically and verified before the transaction workspace/journal is
created. Existing selections are not overwritten. The manifest records the plan;
it does not assert that all source objects exist. Recovery still verifies a
rollback, reverse patch or source backup independently before using it.

The missing-selection guard is unchanged. This prevents a new self-created
inconsistency; it does not automatically repair inconsistent journals left by
older builds, infer missing choices, or remove recovery evidence. No release
archive or publication is performed by this regression runner.
