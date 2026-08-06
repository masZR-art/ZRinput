# Candidate concurrency contract

`CandidatePipeline` owns one permanent `std::jthread`. It never creates a
thread per key. Its queue has exactly one executing request and one pending
slot; submitting a key replaces the pending slot and requests cooperative
cancellation of the active decode.

Each job carries two independent identities:

- `submission_id`, assigned monotonically inside the pipeline, prevents a
  caller that accidentally reuses a composition version from reviving work;
- `composition_version`, assigned by the composition engine, is preserved in
  the decode result and must also match current TSF/UI state at consumption.

The decoder checks its `stop_token` before candidate work, between parse paths,
and inside sentence-beam expansion. A result is publishable only when its job
is the latest submission, its token is not cancelled, its returned version
matches its request, and shutdown has not started. The publication callback is
outside all pipeline locks. The eventual TSF window-message consumer performs
the final composition-version check because a new key can arrive after a
callback has been posted but before the UI thread processes it.

`Stop` rejects new work, clears pending work, cancels the active token, and is
idempotent. Normal destruction joins the worker. Destruction from inside the
publication callback detaches only the worker handle while the worker retains
its reference-counted state, preventing self-join and use-after-free.

