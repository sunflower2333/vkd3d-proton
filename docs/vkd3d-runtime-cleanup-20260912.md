# Shared-runtime ordinary device cleanup

The actuala2cc999 shared GPU probe with matching Mesadfbe4f3 completed all8
rounds and preserved every sentinel, but Mesa logged
`retaining failed WDDM teardown owner graph`. The earlier final PASS only
checked the runtime's final KMT close, so it missed the retained Vulkan owner.
Host work retired, no GPU fault was observed and the desktop stayed healthy;
these observations do not make cleanup pass.

The ordinary native device destructor retired runtime callbacks before final
backend destruction. Mesa's device destructor still needed completion/status
callbacks for its last submitted fence. Those callbacks correctly refused an
already retired owner, causing the retained graph.

## Change and lifetime boundary

An ordinary last-owner destroy now enters a closing phase that rejects new
allocation/map/submit work but permits existing context/completion/status and
unmap/release operations. It drains the backend before retiring runtime
callbacks. It suspends the recursive callback lock during backend teardown so
workers can complete; one explicit Context lease survives that call.

A recursive destroy during the drain immediately retires callbacks and
invalidates its runtime slot. The outer call detects this through retained
Context state and never touches the expired slot again. Child owners or
in-flight backend/kernel calls preserve the earlier deferred retirement path;
no callback is permitted after the runtime destroy returns. This does not prove
Microsoft's ordinary runtime destruction serialization or enable admission.

The coordinated private protocol and matching Mesadfbe4f3 are unchanged. The
frozena2cc999 source and parent KMD/Mesa pins remain untouched.

## Verification

The production-entry lifecycle fixture now checks successful final completion
and status from a backend worker, releases a real fixture allocation/map before
context close, and rejects new work during closing. A second case recursively
destroys the device from its backend destructor, poisons expired private-slot
memory, and verifies that later completion/status are refused and cleanup does
not call retired kernel callbacks. Existing mapped/reentrant lifetime cases
remain enabled.

The negative-control invocation adds an extra Context owner to select the real
deferred backend path. The ordinary-destructor oracle must then fail on retired
completion/status; this tests the oracle, rather than claiming that the
deferred safety path should be used for ordinary cleanup. Windows tests have
90-second process deadlines. Personal CI compiles all3 architectures and runs
both ordinary and negative fixtures on x86/x64 plus a native ARM64 runner.

The real GPU probe now requires a successful completed-fence callback during
native teardown, zero remaining tracked allocations and a closed native
context before final runtime KMT device close. Parent still inspects stderr
for retained-owner errors and validates unchanged desktop/event/host health.
No unchanged probe rerun, target operations or stress run occurred here.

Source/CI outcomes and artifact identities are handed off separately after
validation. Native admission remains closed, and controlled callbacks plus
real KMT/GPU execution are not system D3D12CreateDevice acceptance. Latest user
steering prioritizes bounded functional tests and defers costly stress.
