# jobs

App-owned worker execution for CPU ranges and background callbacks. `mir::jobs`
depends privately
on `core_base` and the platform thread library; it knows no World, physics,
animation, terrain or Vulkan types. `JobExecutor` owns its state and worker
lifetime, so this is a service module rather than a stateless helper library.

App constructs the executor before its World/modules and destroys it after them.
Worker entry points arm the engine crash handler. Thread-start failure unwinds
already-created workers and reports a System SetupError with a serial fallback.

## API and ownership

```cpp
JobExecutor jobs(2); // workers in addition to the participating caller
jobs.ParallelFor(count, 32, [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; ++i)
        outputs[i] = Evaluate(immutableInputs[i]);
}); // every range finished; inputs/results can be modified or destroyed
```

One synchronous dispatch runs at a time. A batch index under the queue mutex distributes
ranges without an unbounded task queue or per-entity function allocations. The
caller helps. Workers publish completion under the group mutex before return;
borrowed callback storage is cleared before the next dispatch.

Zero workers executes inline. Empty ranges do nothing; grain must be positive.
Nested calls on this executor execute inline, preventing pool starvation.
Concurrent external callers serialize. Callbacks must not create synchronous
dependency cycles involving other callers/executors.

Exceptions are captured, other ranges drain, and an exception is rethrown on the
caller. Serial execution has the same draining contract. If a callback throws
partway through a range, remaining indices within that callback are its
responsibility; other range callbacks still finish. Close background consumers
before destroying the executor, and destroy only outside range dispatch.
Shutdown joins workers before synchronization state dies, including partial-
construction unwinding.

`BackgroundJobQueue(executor, concurrency)` supplies a consumer-owned background
lane on the same workers. `Post` adds callbacks; its concurrency cap is clamped
to the engine pool size. The lane resubmits between callbacks, allowing ready
frame ranges to take priority. `CloseAndWait` cancels callbacks not yet started,
releases their captures, and waits for this lane's running callbacks and pending
submissions. It does not drain unrelated work or stop the executor. The executor
must outlive its lanes. Queued no-op wrappers own their state after close.

Range joins track completed ranges, not every worker: a worker doing unrelated
background IO does not hold up a completed frame batch. Running IO is not
preempted. Background callbacks must not throw, close their own lane, or wait on
other callbacks in that lane. A thrown callback terminates through the worker
crash handler. Zero workers runs posts inline; callers must allow immediate
completion. `JobExecutor::Post` is the lower-level API without consumer cleanup.

There is no dependency graph or cancellation of a running callback/range. World
access, structural commands, renderer calls and lifecycle transitions stay on main.

## Configuration

`--workers N` accepts 0–32 workers and survives project relaunches. Normal launches
use `DefaultWorkerCount()`: hardware threads minus one, capped at four, or zero
if only one hardware thread is available/known. `--workers 0` explicitly runs
animation, physics readback, terrain background callbacks and Jolt simulation on the calling thread.
PhysicsModule applies the same count to Jolt's own pool. These are separate
pools: Jolt simulation joins before engine jobs read poses, and animation runs
later. Terrain background work can overlap these phases on the engine pool;
Jolt and audio retain separate pools.

Animation selects a grain of 1–32 targeting four ranges per participating thread
at 64 or more instances when workers are available.
Small scenes and editor preview Worlds evaluate the same kernel inline. Inputs
copy instance state and borrow immutable model resources for the joined call.
Each result owns its deformation buffers, which commit moves into frame-owned
storage before rendering. Asset loading, error reporting and pose history stay
on main; workers never receive World or renderer references.

Physics uses the executor to read body positions/rotations in ranges of 16 for
128 or more moving bodies. Workers use Jolt body read locks and private output
slots. Entity queries, body changes and parent-first transform commits remain
on main. Readback joins before any commit or lifecycle transition.

Terrain uses `TerrainJobExecutor` to adapt reads, decoding, mesh/collision builds
and release checks to a shared background lane. `io.workerThreads` caps that
lane's concurrency. Terrain keeps its internal demand queues and per-frame
installation budgets. Its runtime closes the lane before collaborators die;
scene unload uses existing cancellation tickets to discard stale results.
