# Boost.Coroutine to C++20 Standard Coroutines Migration Plan

**Project**: rippled (XRP Ledger node)
**Branch**: `pratik/Switch-to-std-coroutines`
**Date**: 2026-02-25
**Status**: Planning

---

## Table of Contents

1. [Research & Analysis](#1-research--analysis)
2. [Current State Assessment](#2-current-state-assessment)
3. [Migration Strategy](#3-migration-strategy)
4. [Implementation Plan](#4-implementation-plan)
5. [Testing & Validation Strategy](#5-testing--validation-strategy)
6. [Risks & Mitigation](#6-risks--mitigation)
7. [Timeline & Milestones](#7-timeline--milestones)
8. [Standards & Guidelines](#8-standards--guidelines)
9. [Task List](#9-task-list)

---

## 1. Research & Analysis

### 1.1 Stackful (Boost.Coroutine) vs Stackless (C++20) Architecture

```mermaid
graph TD
    subgraph Boost["Boost.Coroutine2 (Stackful)"]
        direction TB
        B1["Coroutine Created"]
        B2["1 MB Stack Allocated"]
        B3["Full Call Stack Available"]
        B4["yield&lpar;&rpar; from ANY<br/>nesting depth"]
        B5["Context Switch:<br/>save/restore registers<br/>+ stack pointer<br/>~40-100 CPU cycles"]
        B1 --> B2 --> B3 --> B4 --> B5
    end

    subgraph Std["C++20 Coroutines (Stackless)"]
        direction TB
        S1["Coroutine Created"]
        S2["200-500 B Frame on Heap"]
        S3["No Dedicated Stack"]
        S4["co_await ONLY at<br/>explicit suspension points"]
        S5["Context Switch:<br/>resume via function call<br/>symmetric transfer / tail-call<br/>~20-50 CPU cycles"]
        S1 --> S2 --> S3 --> S4 --> S5
    end
```

**Boost (right)**: Each coroutine gets a full 1 MB stack. Suspension saves the
entire register set and stack pointer, so `yield()` can be called from any
nesting depth — the whole call chain is preserved. The cost is high per-coroutine
memory and a heavier context switch (~40-100 cycles for `fcontext` save/restore).

**C++20 (left)**: The compiler allocates a small heap frame (200-500 bytes)
holding only the local variables that live across suspension points. There is no
dedicated stack — suspension is only allowed at explicit `co_await` expressions
in the immediate coroutine function. Resumption is a normal function call
(symmetric transfer makes it a tail-call), costing ~20-50 cycles. The trade-off
is that nested functions that need to suspend must themselves be coroutines.

### 1.2 API & Programming Model Comparison

| Aspect                 | Boost.Coroutine2 (Current)                                                                                                                                                                                                                  | C++20 Coroutines (Target)                                                                                                                                                                                                                                                                                                                                |
| ---------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Type**               | **Stackful, asymmetric.** Each coroutine carries its own call stack, and control transfers between a parent (caller) and a child (coroutine) — never between two siblings directly.                                                         | **Stackless, asymmetric.** The compiler transforms the coroutine function into a state machine allocated on the heap. The same parent/child asymmetry applies, but there is no separate stack.                                                                                                                                                           |
| **Stack Model**        | **Dedicated 1 MB stack per coroutine.** Allocated at construction via `boost::context::fixedsize_stack`. The full stack is reserved even if the coroutine only uses a few hundred bytes, leading to high memory overhead under concurrency. | **Heap frame of ~200-500 bytes.** The compiler allocates only the local variables that live across suspension points into a coroutine frame on the heap. The frame may be elided entirely if the compiler can prove the coroutine's lifetime is bounded by its caller.                                                                                   |
| **Suspension**         | **`(*yield_)()` — can yield from any call depth.** Because the coroutine has its own stack, a call chain `fn_a() → fn_b() → yield()` suspends the entire stack. The `yield_` pointer is a `push_type*` provided by Boost.                   | **`co_await expr` — only at explicit suspension points.** Suspension is only possible in the immediate coroutine function body. If a nested regular function needs to suspend, it must itself be refactored into a coroutine returning an awaitable.                                                                                                     |
| **Resumption**         | **`coro_()` — resumes from last yield.** Calling the `pull_type` object switches back to the coroutine's stack and continues execution right after the last `yield()` call.                                                                 | **`handle.resume()` — resumes from last co_await.** The `std::coroutine_handle<>` is a lightweight pointer to the coroutine frame. Calling `.resume()` jumps to the suspension point via a function-call dispatch (no stack switch).                                                                                                                     |
| **Creation**           | **`pull_type` constructor auto-starts the coroutine.** When a `pull_type` is constructed, it immediately transfers control into the coroutine body, which runs until its first `yield()`. The caller must account for this eager start.     | **Calling a coroutine function returns a suspended handle.** The function body does NOT execute until `handle.resume()` is called (when `initial_suspend()` returns `suspend_always`). This lazy-start model gives the caller full control over when execution begins.                                                                                   |
| **Completion Check**   | **`static_cast<bool>(coro_)` returns false when done.** The `pull_type` is contextually convertible to `bool`; it becomes false after the coroutine body returns.                                                                           | **`handle.done()` returns true when done.** A direct query on the coroutine handle. Calling `resume()` after `done()` is true is undefined behavior.                                                                                                                                                                                                     |
| **Value Passing**      | **Typed via `pull_type<T>` / `push_type<T>`.** Values are exchanged through the coroutine's type parameter — `pull_type<T>` pulls values out, `push_type<T>` pushes values in. rippled uses `<void>` (no values exchanged).                 | **Via `promise_type::return_value(T)` or `co_return`.** Values are stored in the promise object inside the coroutine frame. The caller retrieves them through `await_resume()`. For void coroutines, `return_void()` is used instead.                                                                                                                    |
| **Exception Handling** | **Natural stack-based propagation.** An exception thrown inside the coroutine unwinds its stack normally and propagates to the caller at the `pull_type` call site (i.e., whoever called `coro_()`).                                        | **Explicit capture via `promise_type::unhandled_exception()`.** Exceptions thrown in the coroutine body are caught by the promise and stored (typically as `std::exception_ptr`). They are rethrown in `await_resume()` when the caller co_awaits the result.                                                                                            |
| **Cancellation**       | **Application-managed (poll a flag).** There is no built-in cancellation. rippled uses `expectEarlyExit()` to mark a coroutine as abandoned during shutdown, then decrements `nSuspend_` so `JobQueue::stop()` can proceed.                 | **Via `await_ready()` or cancellation tokens.** An awaiter can check a cancellation flag in `await_ready()` and return true to skip suspension. Alternatively, `std::stop_token` patterns (C++20) can be threaded through. Our `JobQueueAwaiter` returns false from `await_suspend()` when the JobQueue is stopping, effectively cancelling the suspend. |
| **Keywords**           | **None (library-only).** All coroutine machinery is expressed through library types (`pull_type`, `push_type`) and regular function calls. No special language syntax required.                                                             | **`co_await`, `co_yield`, `co_return`.** The presence of any of these keywords in a function body makes it a coroutine. The compiler generates the state machine, frame allocation, and suspension/resumption code automatically.                                                                                                                        |
| **Standard**           | **Boost library (not ISO C++).** `Boost.Coroutine` is deprecated in favor of `Boost.Coroutine2`, which itself has no active development. Depends on `Boost.Context` for platform-specific assembly-level stack switching.                   | **ISO C++20 standard.** Part of the language specification. Supported by all major compilers (GCC 11+, Clang 14+, MSVC 19.28+). Tooling, debugger support, and static analysis are steadily improving across the ecosystem.                                                                                                                              |

### 1.3 Performance Characteristics

| Metric                         | Boost.Coroutine2                           | C++20 Coroutines                     |
| ------------------------------ | ------------------------------------------ | ------------------------------------ |
| **Memory per coroutine**       | ~1MB (fixed stack)                         | ~200-500 bytes (frame only)          |
| **1000 concurrent coroutines** | ~1 GB                                      | ~0.5 MB                              |
| **Context switch cost**        | ~40-100 CPU cycles (fcontext save/restore) | ~20-50 CPU cycles (function call)    |
| **Allocation**                 | Stack allocated at creation                | Heap allocation (compiler may elide) |
| **Cache behavior**             | Poor (large stack rarely fully used)       | Good (small frame, hot data close)   |
| **Compiler optimization**      | Opaque to compiler                         | Inlinable, optimizable               |

### 1.4 Feature Parity Analysis

#### Suspension Points

- **Boost**: Can yield from any nesting level — `fn_a()` calls `fn_b()` calls `yield()`. The entire call stack is preserved.
- **C++20**: Suspension only at `co_await` expressions in the immediate coroutine function. Nested functions that need to suspend must themselves be coroutines returning awaitables.
- **Impact**: (Assumption, needs confirmation from people who know the code better) Rippled's usage is **shallow** — `yield()` is called directly from the RPC handler lambda, never from deeply nested code. This makes migration straightforward.

**Boost** — yield from coroutine body, resume later via `post()`:

```cpp
jq.postCoro(jtCLIENT, "Handler", [&](auto const& coro) {
    auto result = doFirstHalf();
    coro->yield();          // suspend — entire stack preserved
    // resumes here when coro->post() is called externally
    doSecondHalf(result);
});
```

**C++20** — `co_await` suspends, `JobQueueAwaiter` combines yield + auto-repost:

```cpp
jq.postCoroTask(jtCLIENT, "Handler", [&](auto runner) -> CoroTask<void> {
    auto result = doFirstHalf();
    co_await JobQueueAwaiter{runner};  // suspend + auto-repost
    // resumes on a worker thread when the job is picked up
    doSecondHalf(result);
    co_return;
});
```

**Key difference** — Boost can yield from nested calls; C++20 cannot:

```cpp
// Boost — works: yield from inside a helper function
void helper(std::shared_ptr<JobQueue::Coro> coro) {
    coro->yield();  // OK — stackful, entire call stack is preserved
}
jq.postCoro(jtCLIENT, "Deep", [](auto coro) { helper(coro); });

// C++20 — does NOT work: regular functions cannot co_await
void helper(std::shared_ptr<CoroTaskRunner> runner) {
    co_await runner->suspend();  // COMPILE ERROR — not a coroutine
}
// FIX: helper must itself be a coroutine returning CoroTask<void>
CoroTask<void> helper(std::shared_ptr<CoroTaskRunner> runner) {
    co_await runner->suspend();  // OK — this is a coroutine
    co_return;
}
```

#### Exception Handling

- **Boost**: Exceptions propagate naturally up the call stack across yield points.
- **C++20**: Exceptions in coroutine body are caught by `promise_type::unhandled_exception()`. Must be explicitly stored and rethrown.
- **Impact**: Need to implement `unhandled_exception()` in promise type. Pattern is well-established.

**Boost** — exceptions propagate naturally through `yield()`:

```cpp
jq.postCoro(jtCLIENT, "Risky", [](auto coro) {
    coro->yield();
    throw std::runtime_error("oops");
    // Exception propagates up the coroutine stack naturally.
    // The Coro::resume() caller sees it when the coroutine unwinds.
});
```

**C++20** — exceptions are captured by `promise_type` and rethrown on `co_await`:

```cpp
// Inner coroutine throws
CoroTask<int> failingOp() {
    throw std::runtime_error("oops");
    co_return 0;  // never reached
}

// Outer coroutine catches — exception crosses coroutine boundary via promise
jq.postCoroTask(jtCLIENT, "Caller", [](auto runner) -> CoroTask<void> {
    try {
        int v = co_await failingOp();  // rethrows here
    } catch (std::runtime_error const& e) {
        // e.what() == "oops" — caught across coroutine boundary
    }
    co_return;
});
```

**Key difference** — C++20 requires explicit plumbing, but it's already wired up:

```cpp
// Inside CoroTask<void>::promise_type (already implemented):
void unhandled_exception() {
    exception_ = std::current_exception();  // capture
}
// Inside CoroTask<void>::await_resume() (already implemented):
void await_resume() {
    if (auto& ep = handle_.promise().exception_)
        std::rethrow_exception(ep);          // rethrow to caller
}
```

#### Cancellation

- **Boost**: rippled uses `expectEarlyExit()` for graceful shutdown — not a general cancellation mechanism.
- **C++20**: Can check cancellation in `await_ready()` before suspension, or via `stop_token` patterns.
- **Impact**: C++20 provides strictly better cancellation support.

**Boost** — `expectEarlyExit()` for cleanup when coroutine never ran:

```cpp
auto coro = std::make_shared<Coro>(create, jq, t, name, fn);
if (!coro->post()) {
    // JobQueue is stopping — coroutine will never run.
    // Must manually decrement nSuspend_ so shutdown doesn't hang.
    coro->expectEarlyExit();
    coro.reset();
}
```

No cooperative in-body cancellation — coroutine just runs to completion or gets abandoned.

**C++20** — `expectEarlyExit()` for the same case, plus cooperative in-body checking:

```cpp
// Same early-exit pattern when post() fails:
auto runner = CoroTaskRunner::create(jq, t, name);
runner->init(fn);
++nSuspend_;
if (!runner->post()) {
    runner->expectEarlyExit();  // decrements nSuspend_, destroys frame
    runner.reset();
}

// Cooperative cancellation — coroutine checks jq.isStopping() after each yield:
jq.postCoroTask(jtCLIENT, "Long", [jqp = &jq](auto runner) -> CoroTask<void> {
    while (hasWork()) {
        co_await JobQueueAwaiter{runner};
        if (jqp->isStopping())
            co_return;              // graceful exit
        doNextChunk();
    }
    co_return;
});
```

**C++20 bonus** — `JobQueueAwaiter::await_suspend()` handles shutdown automatically:

```cpp
bool await_suspend(std::coroutine_handle<>) {
    runner->onSuspend();
    if (!runner->post()) {
        // JQ stopping — undo suspend, return false so coroutine
        // continues immediately (can fall through to co_return)
        runner->onUndoSuspend();
        return false;
    }
    return true;  // actually suspend
}
```

### 1.5 Compiler Support

| Compiler  | rippled Minimum | C++20 Coroutine Support  | Status |
| --------- | --------------- | ------------------------ | ------ |
| **GCC**   | 12.0+           | Full (since GCC 11)      | Ready  |
| **Clang** | 16.0+           | Full (since Clang 14)    | Ready  |
| **MSVC**  | 19.28+          | Full (since VS2019 16.8) | Ready  |

rippled already requires C++20 (`CMAKE_CXX_STANDARD 20` in `CMakeLists.txt`). All supported compilers have mature C++20 coroutine support. **No compiler upgrades required.**

### 1.6 Viability Analysis — Addressing Stackless Concerns

C++20 stackless coroutines have well-known limitations compared to stackful coroutines. This section analyzes each concern against rippled's **actual codebase** to determine viability.

#### Concern 1: Cannot Suspend from Nested Call Stacks

**Claim**: Stackless coroutines cannot yield from arbitrary stack depths. If `fn_a()` calls `fn_b()` calls `yield()`, only stackful coroutines can suspend the entire chain.

**Analysis**: An exhaustive codebase audit found:

- **1 production yield() call**: `RipplePathFind.cpp:131` — directly in the handler function body
- **All test yield() calls**: directly in `postCoro` lambda bodies (Coroutine_test.cpp, JobQueue_test.cpp)
- **The `push_type*` architecture** makes deep-nested yield() structurally impossible — the `yield_` pointer is only available inside the `postCoro` lambda via the `shared_ptr<Coro>`, and handlers call `context.coro->yield()` at the top level

```mermaid
graph LR
    subgraph Stackful["Stackful &lpar;Boost&rpar; — can yield anywhere"]
        direction TB
        A1["postCoro lambda"] --> A2["handlerFn&lpar;&rpar;"]
        A2 --> A3["helperFn&lpar;&rpar;"]
        A3 --> A4["coro→yield&lpar;&rpar; ✅"]
    end

    subgraph Stackless["Stackless &lpar;C++20&rpar; — co_await at top only"]
        direction TB
        B1["postCoroTask lambda"] --> B2["co_await ✅"]
        B1 --> B3["regularFn&lpar;&rpar;"]
        B3 -.-> B4["co_await ❌"]
    end

    subgraph Rippled["rippled actual usage — all shallow"]
        direction TB
        C1["postCoro lambda"] --> C2["context.coro→yield&lpar;&rpar;<br/>&lpar;direct, no nesting&rpar;"]
    end

    style A4 fill:#f96,stroke:#333,color:#000
    style B4 fill:#f66,stroke:#333,color:#fff
    style C2 fill:#3d8,stroke:#333,color:#000
```

**Verdict**: This concern does NOT apply. All suspension is shallow.

#### Concern 2: Colored Function Problem (Viral co_await)

**Claim**: Once a function needs to suspend, every caller up the chain must also be a coroutine. This "infects" the call chain.

**Analysis**: In rippled's case, the coloring is minimal:

- `postCoroTask()` launches a coroutine — this is the "root" colored function
- The `postCoro` lambda itself becomes the coroutine function (returns `CoroTask<void>`)
- `doRipplePathFind()` is the only handler that calls `co_await`
- No other handler in the chain needs to become a coroutine — they continue to be regular functions dispatched through `doCommand()`

The "coloring" stops at the entry point lambda and the one handler that suspends. No deep infection.

```mermaid
graph TD
    subgraph Feared["Feared: deep coloring infection"]
        direction TB
        F1["main&lpar;&rpar;"] -->|"must become<br/>coroutine"| F2["Server::run&lpar;&rpar;"]
        F2 -->|"must become<br/>coroutine"| F3["dispatch&lpar;&rpar;"]
        F3 -->|"must become<br/>coroutine"| F4["doCommand&lpar;&rpar;"]
        F4 -->|"must become<br/>coroutine"| F5["handler&lpar;&rpar;"]
        F5 --> F6["co_await"]
        style F1 fill:#f66,stroke:#333,color:#fff
        style F2 fill:#f66,stroke:#333,color:#fff
        style F3 fill:#f66,stroke:#333,color:#fff
        style F4 fill:#f66,stroke:#333,color:#fff
        style F5 fill:#f66,stroke:#333,color:#fff
    end

    subgraph Actual["Actual: coloring stops at entry point"]
        direction TB
        A1["main&lpar;&rpar;"] --- A2["Server::run&lpar;&rpar;"]
        A2 --- A3["dispatch&lpar;&rpar;"]
        A3 --- A4["doCommand&lpar;&rpar;"]
        A4 -->|"only this<br/>is a coroutine"| A5["postCoroTask lambda<br/>→ CoroTask&lt;void&gt;"]
        A5 --> A6["co_await"]
        style A1 fill:#eee,stroke:#333,color:#000
        style A2 fill:#eee,stroke:#333,color:#000
        style A3 fill:#eee,stroke:#333,color:#000
        style A4 fill:#eee,stroke:#333,color:#000
        style A5 fill:#f96,stroke:#333,color:#000
        style A6 fill:#3d8,stroke:#333,color:#000
    end
```

**Verdict**: Minimal impact. Only 4 lambdas (3 entry points + 1 handler) need `co_await`.

#### Concern 3: No Standard Library Support for Common Patterns

**Claim**: C++20 provides the language primitives but no standard task type, executor integration, or composition utilities.

**Analysis**: This is accurate — we need to write custom types:

- `CoroTask<T>` (task/return type) — well-established pattern, ~80 lines
- `JobQueueAwaiter` (executor integration) — ~20 lines
- `FinalAwaiter` (continuation chaining) — ~10 lines

However, these types are small, well-understood, and have extensive reference implementations (cppcoro, folly::coro, libunifex). The total boilerplate is approximately 150-200 lines of header code.

```mermaid
graph TD
    subgraph StdLib["C++20 standard provides"]
        direction LR
        S1["coroutine_handle&lt;P&gt;"]
        S2["suspend_always /<br/>suspend_never"]
        S3["noop_coroutine&lpar;&rpar;"]
        S4["co_await / co_return"]
    end

    subgraph Custom["Custom types we wrote &lpar;~150 lines total&rpar;"]
        direction TB
        C1["CoroTask&lt;T&gt;<br/>~80 lines<br/>&lpar;task + promise_type&rpar;"]
        C2["JobQueueAwaiter<br/>~20 lines<br/>&lpar;suspend + repost&rpar;"]
        C3["FinalAwaiter<br/>~10 lines<br/>&lpar;symmetric transfer&rpar;"]
        C4["CoroTaskRunner<br/>~40 lines decl<br/>&lpar;lifecycle manager&rpar;"]
    end

    subgraph Ref["Reference implementations"]
        direction LR
        R1["cppcoro"]
        R2["folly::coro"]
        R3["libunifex"]
    end

    S1 --> C1
    S2 --> C1
    S3 --> C3
    S4 --> C2
    Ref -.->|"patterns<br/>borrowed from"| Custom
```

**Verdict**: Manageable. Custom types are small and well-documented in C++ community.

#### Concern 4: Stack Overflow from Synchronous Resumption Chains

**Claim**: If coroutine A `co_await`s coroutine B, and B completes synchronously, B's `final_suspend` resumes A on the same stack, potentially building up unbounded stack depth.

**Analysis**: This is addressed by **symmetric transfer** via `FinalAwaiter::await_suspend()` returning a `coroutine_handle<>` instead of `void`. The compiler transforms this into a tail-call, preventing stack growth. This is the standard solution used by all major coroutine libraries and is implemented in our `FinalAwaiter` design (Section 4.1).

```mermaid
graph TD
    subgraph Problem["Without symmetric transfer — stack grows"]
        direction TB
        P1["A resumes B"] --> P2["B::resume&lpar;&rpar;<br/>stack frame +1"]
        P2 --> P3["B completes<br/>final_suspend resumes A"]
        P3 --> P4["A::resume&lpar;&rpar;<br/>stack frame +2"]
        P4 --> P5["A resumes C"]
        P5 --> P6["C::resume&lpar;&rpar;<br/>stack frame +3"]
        P6 --> P7["... stack overflow ❌"]
        style P7 fill:#f66,stroke:#333,color:#fff
    end

    subgraph Solution["With symmetric transfer — tail call, no growth"]
        direction TB
        S1["A resumes B"] --> S2["B::resume&lpar;&rpar;<br/>stack frame 1"]
        S2 --> S3["B completes"]
        S3 -->|"FinalAwaiter returns<br/>handle → tail call"| S4["A::resume&lpar;&rpar;<br/>stack frame 1 &lpar;reused&rpar;"]
        S4 --> S5["A resumes C"]
        S5 -->|"tail call"| S6["C::resume&lpar;&rpar;<br/>stack frame 1 &lpar;reused&rpar;"]
        S6 --> S7["... bounded ✅"]
        style S7 fill:#3d8,stroke:#333,color:#000
    end
```

**Verdict**: Solved by symmetric transfer (already in our design).

#### Concern 5: Dangling Reference Risk

**Claim**: Coroutine frames are heap-allocated and outlive the calling scope, making references to locals dangerous.

**Analysis**: This is a real concern that requires engineering discipline:

- Coroutine parameters are copied into the frame (safe by default)
- References passed to coroutine functions can dangle if the referent's scope ends before the coroutine completes
- Our design mitigates this: `RPC::Context` is passed by reference but its lifetime is managed by `shared_ptr<Coro>` / the entry point lambda's scope, which outlives the coroutine

```mermaid
graph TD
    subgraph Danger["Dangling reference — ❌ use-after-free"]
        direction TB
        D1["caller&lpar;&rpar; scope"] --> D2["int local = 42"]
        D2 --> D3["launch coroutine<br/>with &amp;local"]
        D3 --> D4["caller returns<br/>local destroyed"]
        D4 --> D5["coroutine resumes<br/>reads &amp;local 💥"]
        style D5 fill:#f66,stroke:#333,color:#fff
    end

    subgraph Safe["rippled pattern — ✅ lifetime managed"]
        direction TB
        S1["postCoroTask&lpar;&rpar;"] --> S2["shared_ptr&lt;Runner&gt;<br/>owns coroutine frame"]
        S2 --> S3["lambda captures<br/>by value or<br/>shared_ptr"]
        S3 --> S4["FuncStore keeps<br/>lambda alive on heap"]
        S4 --> S5["coroutine resumes<br/>captures still valid ✅"]
        style S5 fill:#3d8,stroke:#333,color:#000
    end
```

**Verdict**: Real risk, but manageable with RAII patterns and ASAN testing.

#### Concern 6: yield_to.h / boost::asio::spawn

**Claim**: `yield_to.h:111` uses `boost::asio::spawn`, suggesting broader coroutine usage.

**Analysis**: `yield_to.h` uses `boost::asio::spawn` with `boost::context::fixedsize_stack(2 * 1024 * 1024)` — this is a **completely separate** coroutine system:

- Different type: `boost::asio::yield_context` (not `push_type*`)
- Different purpose: test infrastructure for async I/O tests
- Different mechanism: Boost.Asio stackful coroutines (not Boost.Coroutine2)
- **Not part of this migration scope** — used only in tests and unrelated to `JobQueue::Coro`

```mermaid
graph TD
    subgraph ThisMigration["This migration &lpar;JobQueue::Coro&rpar;"]
        direction TB
        M1["Boost.Coroutine2<br/>push_type / pull_type"] -->|"replace with"| M2["C++20 coroutines<br/>CoroTask + co_await"]
        M3["JobQueue::Coro"] -->|"replace with"| M4["CoroTaskRunner"]
        M5["coro→yield&lpar;&rpar; + post&lpar;&rpar;"] -->|"replace with"| M6["JobQueueAwaiter"]
    end

    subgraph OutOfScope["Out of scope &lpar;yield_to.h&rpar;"]
        direction TB
        O1["boost::asio::spawn"]
        O2["yield_context"]
        O3["fixedsize_stack&lpar;2MB&rpar;"]
        O1 --- O2
        O1 --- O3
    end

    ThisMigration ~~~ OutOfScope
    style OutOfScope fill:#eee,stroke:#999
```

**Verdict**: Separate system. Out of scope for this migration.

#### Overall Viability Conclusion

The migration IS viable because:

1. rippled's coroutine usage is **shallow** (no deep-nested yield)
2. The **colored function infection** is limited to 4 call sites
3. Custom types are **small and well-understood**
4. **Symmetric transfer** solves the stack overflow concern
5. **ASAN/TSAN** testing catches lifetime and race bugs
6. The alternative (ASAN annotations for Boost.Context) only addresses sanitizer false positives — it does not provide memory savings, standard compliance, or the dependency elimination that C++20 migration delivers

### 1.7 Merits & Demerits Summary

#### Merits of C++20 Migration

1. **2000x memory reduction** per coroutine (1MB → ~500 bytes)
2. **Faster context switching** (~2x improvement)
3. **Remove external dependency** on Boost.Coroutine (and transitively Boost.Context)
4. **Language-native** — better tooling, debugger support, static analysis
5. **Future-proof** — ISO standard, not a deprecated library
6. **Compiler-optimizable** — suspension points can be inlined/elided
7. **ASAN compatibility** — eliminates Boost context-switching false positives (see `docs/build/sanitizers.md`)

#### Demerits / Challenges

1. **Stackless limitation** — cannot yield from nested calls (verified: not an issue for rippled's shallow usage)
2. **Explicit lifetime management** — `coroutine_handle::destroy()` must be called (mitigated by RAII CoroTask)
3. **Verbose boilerplate** — promise_type, awaiter interfaces (~150-200 lines of infrastructure code)
4. **Debugging** — no visible coroutine stack in debugger (improving with tooling)
5. **Learning curve** — team needs familiarity with C++20 coroutine machinery
6. **Dangling reference risk** — coroutine frames outlive calling scope (mitigated by ASAN + careful design)
7. **No standard library task type** — must write custom CoroTask, awaiters (well-established patterns exist)

#### Alternative Considered: ASAN Annotations Only

Instead of full migration, one could keep Boost.Coroutine and add `__sanitizer_start_switch_fiber` / `__sanitizer_finish_switch_fiber` annotations to Coro.ipp to suppress ASAN false positives. This was evaluated and rejected because:

- It only fixes sanitizer false positives — does NOT reduce 1MB/coroutine memory usage
- Does NOT remove the deprecated Boost.Coroutine dependency
- Does NOT provide standard compliance or future-proofing
- The full migration is feasible given shallow yield usage and delivers all the above benefits

---

## 2. Current State Assessment

### 2.1 Architecture Overview

```mermaid
graph TD
    subgraph "Request Entry Points"
        HTTP["HTTP Request<br/>ServerHandler::onRequest()"]
        WS["WebSocket Message<br/>ServerHandler::onWSMessage()"]
        GRPC["gRPC Request<br/>CallData::process()"]
    end

    subgraph "Coroutine Layer"
        POST["JobQueue::postCoro()<br/>Creates Coro + schedules job"]
        CORO["JobQueue::Coro<br/>boost::coroutines::pull_type<br/>1MB stack per instance"]
    end

    subgraph "JobQueue Thread Pool"
        W1["Worker Thread 1"]
        W2["Worker Thread 2"]
        WN["Worker Thread N"]
    end

    subgraph "RPC Handlers"
        CTX["RPC::Context<br/>holds shared_ptr#lt;Coro#gt;"]
        RPC["RPC Handler<br/>e.g. doRipplePathFind"]
        YIELD["coro.yield()<br/>Suspends execution"]
        RESUME["coro.post()<br/>Reschedules on JobQueue"]
    end

    HTTP --> POST
    WS --> POST
    GRPC --> POST
    POST --> CORO
    CORO --> W1
    CORO --> W2
    CORO --> WN
    W1 --> CTX
    W2 --> CTX
    CTX --> RPC
    RPC --> YIELD
    YIELD -.->|"event completes"| RESUME
    RESUME --> W1
```

### 2.2 `JobQueue::Coro` Implementation Audit

**File**: `include/xrpl/core/JobQueue.h` (lines 40-120) + `include/xrpl/core/Coro.ipp`

#### Class Members

```cpp
class Coro : public std::enable_shared_from_this<Coro> {
    detail::LocalValues lvs_;                                         // Per-coroutine thread-local storage
    JobQueue& jq_;                                                    // Parent JobQueue reference
    JobType type_;                                                    // Job type (jtCLIENT_RPC, etc.)
    std::string name_;                                                // Name for logging
    bool running_;                                                    // Is currently executing
    std::mutex mutex_;                                                // Prevents concurrent resume
    std::mutex mutex_run_;                                            // Guards running_ flag
    std::condition_variable cv_;                                      // For join() blocking
    boost::coroutines::asymmetric_coroutine<void>::pull_type coro_;   // THE BOOST COROUTINE
    boost::coroutines::asymmetric_coroutine<void>::push_type* yield_; // Yield function pointer
    bool finished_;                                                   // Debug assertion flag
};
```

#### Boost.Coroutine APIs Used

| API                                           | Location        | Purpose                     |
| --------------------------------------------- | --------------- | --------------------------- |
| `asymmetric_coroutine<void>::pull_type`       | `JobQueue.h:51` | The coroutine object itself |
| `asymmetric_coroutine<void>::push_type`       | `JobQueue.h:52` | Yield function type         |
| `boost::coroutines::attributes(megabytes(1))` | `Coro.ipp:23`   | Stack size configuration    |
| `#include <boost/coroutine/all.hpp>`          | `JobQueue.h:10` | Header inclusion            |

#### Method Behaviors

| Method                  | Behavior                                                                                                                          |
| ----------------------- | --------------------------------------------------------------------------------------------------------------------------------- |
| **Constructor**         | Creates `pull_type` with 1MB stack. Lambda captures user function. Auto-runs to first `yield()`.                                  |
| **`yield()`**           | Increments `jq_.nSuspend_`, calls `(*yield_)()` to suspend. Returns control to caller.                                            |
| **`post()`**            | Sets `running_=true`, calls `jq_.addJob()` with a lambda that calls `resume()`. Returns false if JobQueue is stopping.            |
| **`resume()`**          | Swaps `LocalValues`, acquires `mutex_`, calls `coro_()` to resume. Restores `LocalValues`. Sets `running_=false`, notifies `cv_`. |
| **`runnable()`**        | Returns `static_cast<bool>(coro_)` — true if coroutine hasn't returned.                                                           |
| **`expectEarlyExit()`** | Decrements `nSuspend_`, sets `finished_=true`. Used during shutdown.                                                              |
| **`join()`**            | Blocks on `cv_` until `running_==false`.                                                                                          |

### 2.3 Coroutine Execution Lifecycle

```mermaid
sequenceDiagram
    participant HT as Handler Thread
    participant JQ as JobQueue
    participant WT as Worker Thread
    participant C as Coro
    participant UF as User Function

    HT->>JQ: postCoro(type, name, fn)
    JQ->>C: Coro::Coro() constructor
    Note over C: pull_type auto-starts lambda
    C->>C: yield_ = #amp;do_yield
    C->>C: yield() [initial suspension]
    C-->>JQ: Returns to constructor
    JQ->>JQ: coro->post()
    JQ->>JQ: addJob(type, name, resume_lambda)
    JQ-->>HT: Returns shared_ptr#lt;Coro#gt;
    Note over HT: Handler thread is FREE

    WT->>C: resume() [job executes]
    Note over C: Swap LocalValues
    C->>C: coro_() [resume boost coroutine]
    C->>UF: fn(shared_from_this())
    UF->>UF: Do work...

    UF->>C: coro->yield() [suspend]
    Note over C: ++nSuspend_, invoke yield_()
    C-->>WT: Returns from resume()
    Note over WT: Worker thread is FREE

    Note over UF: External event completes
    UF->>C: coro->post() [reschedule]
    C->>JQ: addJob(resume_lambda)

    WT->>C: resume() [job executes]
    C->>C: coro_() [resume]
    C->>UF: Continues after yield()
    UF->>UF: Finish work
    UF-->>C: Return [coroutine complete]
    Note over C: running_=false, cv_.notify_all()
```

### 2.4 All Coroutine Touchpoints

#### Core Infrastructure (Must Change)

| File                               | Role                                     | Lines of Interest         |
| ---------------------------------- | ---------------------------------------- | ------------------------- |
| `include/xrpl/core/JobQueue.h`     | Coro class definition, postCoro template | Lines 10, 40-120, 385-402 |
| `include/xrpl/core/Coro.ipp`       | Coro method implementations              | All (122 lines)           |
| `include/xrpl/basics/LocalValue.h` | Per-coroutine thread-local storage       | Lines 12-59 (LocalValues) |
| `cmake/deps/Boost.cmake`           | Boost.Coroutine dependency               | Lines 7, 24               |

#### Entry Points (postCoro Callers)

| File                                         | Entry Point                  | Job Type             |
| -------------------------------------------- | ---------------------------- | -------------------- |
| `src/xrpld/rpc/detail/ServerHandler.cpp:287` | `onRequest()` — HTTP RPC     | `jtCLIENT_RPC`       |
| `src/xrpld/rpc/detail/ServerHandler.cpp:325` | `onWSMessage()` — WebSocket  | `jtCLIENT_WEBSOCKET` |
| `src/xrpld/app/main/GRPCServer.cpp:102`      | `CallData::process()` — gRPC | `jtRPC`              |

#### Context Propagation

| File                                    | Role                                                   |
| --------------------------------------- | ------------------------------------------------------ |
| `src/xrpld/rpc/Context.h:27`            | `RPC::Context` holds `shared_ptr<JobQueue::Coro> coro` |
| `src/xrpld/rpc/ServerHandler.h:174-188` | `processSession/processRequest` pass coro through      |

#### Active Coroutine Consumer (yield/post)

| File                                                | Usage                                                 |
| --------------------------------------------------- | ----------------------------------------------------- |
| `src/xrpld/rpc/handlers/RipplePathFind.cpp:131`     | `context.coro->yield()` — suspends for path-finding   |
| `src/xrpld/rpc/handlers/RipplePathFind.cpp:116-123` | Continuation calls `coro->post()` or `coro->resume()` |

#### Test Files

| File                               | Tests                                                         |
| ---------------------------------- | ------------------------------------------------------------- |
| `src/test/core/Coroutine_test.cpp` | `correct_order`, `incorrect_order`, `thread_specific_storage` |
| `src/test/core/JobQueue_test.cpp`  | `testPostCoro` (post/resume cycles, shutdown behavior)        |
| `src/test/app/Path_test.cpp`       | Path-finding RPC via postCoro                                 |
| `src/test/jtx/impl/AMMTest.cpp`    | AMM RPC via postCoro                                          |

### 2.5 Suspension/Continuation Model

The current model documented in `src/xrpld/rpc/README.md` defines four functional types:

```
Callback     = std::function<void()>           — generic 0-arg function
Continuation = std::function<void(Callback)>   — calls Callback later
Suspend      = std::function<void(Continuation)> — runs Continuation, suspends
Coroutine    = std::function<void(Suspend)>    — given a Suspend, starts work
```

In practice, `JobQueue::Coro` simplifies this to:

- **Suspend** = `coro->yield()`
- **Continue** = `coro->post()` (async on JobQueue) or `coro->resume()` (sync on current thread)

### 2.6 CMake Dependency

In `cmake/deps/Boost.cmake`:

```cmake
find_package(Boost REQUIRED COMPONENTS ... coroutine ...)
target_link_libraries(xrpl_boost INTERFACE ... Boost::coroutine ...)
```

Additionally in `cmake/XrplInterface.cmake`:

```cpp
BOOST_COROUTINES_NO_DEPRECATION_WARNING  // Suppresses Boost.Coroutine deprecation warnings
```

### 2.7 Existing C++20 Coroutine Usage

rippled **already uses C++20 coroutines** in test code:

- `src/tests/libxrpl/net/HTTPClient.cpp` uses `co_await` with `boost::asio::use_awaitable`
- Demonstrates team familiarity with C++20 coroutine syntax
- Proves compiler toolchain supports C++20 coroutines

---

## 3. Migration Strategy

### 3.1 Incremental vs Atomic Migration

**Decision: Incremental (multi-phase) migration.**

Rationale:

- Only **one RPC handler** (`RipplePathFind`) actively uses `yield()/post()` suspension
- The **three entry points** (HTTP, WS, gRPC) all funnel through `postCoro()`
- The `RPC::Context.coro` field is the sole propagation mechanism
- We can introduce a new C++20 coroutine system alongside the existing one and migrate callsites incrementally

### 3.2 Migration Phases

```mermaid
graph TD
    subgraph "Phase 1: Foundation"
        P1A["Create CoroTask#lt;T#gt; type<br/>(promise_type, awaiter)"]
        P1B["Create JobQueueAwaiter<br/>(schedules resume on JobQueue)"]
        P1C["Add postCoroTask() to JobQueue<br/>(parallel to postCoro)"]
        P1D["Unit tests for new primitives"]
        P1A --> P1B --> P1C --> P1D
    end

    subgraph "Phase 2: Entry Point Migration"
        P2A["Migrate ServerHandler::onRequest()"]
        P2B["Migrate ServerHandler::onWSMessage()"]
        P2C["Migrate GRPCServer::CallData::process()"]
        P2D["Update RPC::Context to use new type"]
        P2A --> P2D
        P2B --> P2D
        P2C --> P2D
    end

    subgraph "Phase 3: Handler Migration"
        P3A["Migrate RipplePathFind handler"]
        P3B["Verify all other handlers<br/>(no active yield usage)"]
    end

    subgraph "Phase 4: Cleanup"
        P4A["Remove old Coro class"]
        P4B["Remove Boost.Coroutine from CMake"]
        P4C["Remove deprecation warning suppression"]
        P4D["Final benchmarks & validation"]
    end

    P1D --> P2A
    P2D --> P3A
    P3B --> P4A
    P3A --> P4A
    P4A --> P4B --> P4C --> P4D
```

### 3.3 Coexistence Strategy

During migration, both implementations will coexist:

```mermaid
graph LR
    subgraph "Transition Period"
        OLD["JobQueue::Coro<br/>(Boost, existing)"]
        NEW["JobQueue::CoroTask<br/>(C++20, new)"]
        CTX["RPC::Context"]
    end

    CTX -->|"phase 1-2"| OLD
    CTX -->|"phase 2-3"| NEW

    style OLD fill:#fdd,stroke:#c00,color:#000
    style NEW fill:#dfd,stroke:#0a0,color:#000
```

- `RPC::Context` will temporarily hold both `shared_ptr<Coro>` (old) and the new coroutine handle
- Entry points will be migrated one at a time
- Each migration is independently testable
- Once all entry points and handlers are migrated, old code is removed

### 3.4 Breaking Changes & Compatibility

| Concern                          | Impact                                    | Mitigation                                               |
| -------------------------------- | ----------------------------------------- | -------------------------------------------------------- |
| `RPC::Context::coro` type change | All RPC handlers receive context          | Migrate context field last, after all consumers updated  |
| `postCoro()` removal             | 3 callers                                 | Replace with `postCoroTask()`, remove old API in Phase 4 |
| `LocalValue` integration         | Thread-local storage must work            | New implementation must swap LocalValues identically     |
| Shutdown behavior                | `expectEarlyExit()`, `nSuspend_` tracking | Replicate in new CoroTask                                |

---

## 4. Implementation Plan

### 4.1 New Type Design

#### `CoroTask<T>` — Coroutine Return Type

```mermaid
classDiagram
    class CoroTask~T~ {
        +Handle handle_
        +CoroTask(Handle h)
        +destroy()
        +bool done() const
        +T get() const
        +bool await_ready() const
        +void await_suspend(coroutine_handle h) const
        +T await_resume() const
    }

    class promise_type {
        -result_ : variant~T, exception_ptr~
        -continuation_ : coroutine_handle
        +CoroTask get_return_object()
        +suspend_always initial_suspend()
        +FinalAwaiter final_suspend()
        +void return_value(T)
        +void return_void()
        +void unhandled_exception()
    }

    class FinalAwaiter {
        +bool await_ready()
        +coroutine_handle await_suspend(coroutine_handle~promise_type~)
        +void await_resume()
    }

    class JobQueueAwaiter {
        -jq_ : JobQueue
        -type_ : JobType
        -name_ : string
        +bool await_ready()
        +void await_suspend(coroutine_handle h)
        +void await_resume()
    }

    CoroTask --> promise_type : contains
    promise_type --> FinalAwaiter : returns from final_suspend
    CoroTask ..> JobQueueAwaiter : used with co_await
```

#### `JobQueueAwaiter` — Schedules Resumption on JobQueue

```cpp
// Conceptual design — actual implementation may vary
struct JobQueueAwaiter {
    JobQueue& jq;
    JobType type;
    std::string name;

    bool await_ready() { return false; }  // Always suspend

    void await_suspend(std::coroutine_handle<> h) {
        // Schedule coroutine resumption as a job
        jq.addJob(type, name, [h]() { h.resume(); });
    }

    void await_resume() {}
};
```

### 4.2 Mapping: Old API → New API

```mermaid
graph LR
    subgraph "Current (Boost)"
        A1["postCoro(type, name, fn)"]
        A2["coro->yield()"]
        A3["coro->post()"]
        A4["coro->resume()"]
        A5["coro->join()"]
        A6["coro->runnable()"]
        A7["coro->expectEarlyExit()"]
    end

    subgraph "New (C++20)"
        B1["postCoroTask(type, name, fn)<br/>fn returns CoroTask&lt;void&gt;"]
        B2["co_await JobQueueAwaiter{jq, type, name}"]
        B3["Built into await_suspend()<br/>(automatic scheduling)"]
        B4["handle.resume()<br/>(direct call)"]
        B5["co_await task<br/>(continuation-based)"]
        B6["handle.done()"]
        B7["handle.destroy() + cleanup"]
    end

    A1 --> B1
    A2 --> B2
    A3 --> B3
    A4 --> B4
    A5 --> B5
    A6 --> B6
    A7 --> B7
```

### 4.3 File Changes Required

#### Phase 1: New Coroutine Primitives

| File                                  | Action     | Description                                                   |
| ------------------------------------- | ---------- | ------------------------------------------------------------- |
| `include/xrpl/core/CoroTask.h`        | **CREATE** | `CoroTask<T>` return type with `promise_type`, `FinalAwaiter` |
| `include/xrpl/core/JobQueueAwaiter.h` | **CREATE** | Awaiter that schedules resume on JobQueue                     |
| `include/xrpl/core/JobQueue.h`        | **MODIFY** | Add `postCoroTask()` template alongside existing `postCoro()` |
| `src/test/core/CoroTask_test.cpp`     | **CREATE** | Unit tests for `CoroTask<T>` and `JobQueueAwaiter`            |

#### Phase 2: Entry Point Migration

| File                                     | Action     | Description                                                            |
| ---------------------------------------- | ---------- | ---------------------------------------------------------------------- |
| `src/xrpld/rpc/detail/ServerHandler.cpp` | **MODIFY** | `onRequest()` and `onWSMessage()`: replace `postCoro` → `postCoroTask` |
| `src/xrpld/rpc/ServerHandler.h`          | **MODIFY** | Update `processSession`/`processRequest` signatures                    |
| `src/xrpld/app/main/GRPCServer.cpp`      | **MODIFY** | `CallData::process()`: replace `postCoro` → `postCoroTask`             |
| `src/xrpld/app/main/GRPCServer.h`        | **MODIFY** | Update `process()` method signature                                    |
| `src/xrpld/rpc/Context.h`                | **MODIFY** | Change `shared_ptr<JobQueue::Coro>` to new coroutine handle type       |

#### Phase 3: Handler Migration

| File                                        | Action     | Description                                                      |
| ------------------------------------------- | ---------- | ---------------------------------------------------------------- |
| `src/xrpld/rpc/handlers/RipplePathFind.cpp` | **MODIFY** | Replace `context.coro->yield()` / `coro->post()` with `co_await` |
| `src/test/app/Path_test.cpp`                | **MODIFY** | Update test to use new coroutine API                             |
| `src/test/jtx/impl/AMMTest.cpp`             | **MODIFY** | Update test to use new coroutine API                             |

#### Phase 4: Cleanup

| File                               | Action     | Description                                                        |
| ---------------------------------- | ---------- | ------------------------------------------------------------------ |
| `include/xrpl/core/Coro.ipp`       | **DELETE** | Remove old Boost.Coroutine implementation                          |
| `include/xrpl/core/JobQueue.h`     | **MODIFY** | Remove `Coro` class, `postCoro()`, `Coro_create_t`, Boost includes |
| `cmake/deps/Boost.cmake`           | **MODIFY** | Remove `coroutine` from `find_package` and `target_link_libraries` |
| `cmake/XrplInterface.cmake`        | **MODIFY** | Remove `BOOST_COROUTINES_NO_DEPRECATION_WARNING`                   |
| `src/test/core/Coroutine_test.cpp` | **MODIFY** | Rewrite tests for new CoroTask                                     |
| `src/test/core/JobQueue_test.cpp`  | **MODIFY** | Update `testPostCoro` to use new API                               |
| `include/xrpl/basics/LocalValue.h` | **MODIFY** | Update LocalValues integration for C++20 coroutines                |

### 4.4 LocalValue Integration Design

The current `LocalValue` system swaps per-coroutine storage on resume/yield:

```mermaid
sequenceDiagram
    participant WT as Worker Thread
    participant LV as LocalValues
    participant C as Coroutine

    Note over WT: Thread has its own LocalValues

    WT->>LV: saved = getLocalValues().release()
    WT->>LV: getLocalValues().reset(#amp;coro.lvs_)
    Note over LV: Now pointing to coroutine's storage

    WT->>C: coro_() / handle.resume()
    Note over C: User code sees coroutine's LocalValues

    C-->>WT: yield / co_await returns

    WT->>LV: getLocalValues().release()
    WT->>LV: getLocalValues().reset(saved)
    Note over LV: Restored to thread's storage
```

**For C++20**: The same swap pattern must be implemented in the awaiter's `await_suspend()` and `await_resume()`, or in a wrapper that calls `handle.resume()`.

### 4.5 RipplePathFind Migration Design

Current pattern:

```cpp
// Continuation callback
auto callback = [&context]() {
    std::shared_ptr<JobQueue::Coro> coroCopy{context.coro};
    if (!coroCopy->post()) {
        coroCopy->resume();  // Fallback: run on current thread
    }
};

// Start async work, then suspend
jvResult = makeLegacyPathRequest(request, callback, ...);
if (request) {
    context.coro->yield();       // ← SUSPEND HERE
    jvResult = request->doStatus(context.params);  // ← RESUME HERE
}
```

Target pattern:

```cpp
// Start async work, suspend via co_await
jvResult = makeLegacyPathRequest(request, /* awaiter-based callback */, ...);
if (request) {
    co_await PathFindAwaiter{context};  // ← SUSPEND + RESUME via awaiter
    jvResult = request->doStatus(context.params);
}
```

The `PathFindAwaiter` will encapsulate the scheduling logic currently in the lambda continuation.

---

## 5. Testing & Validation Strategy

### 5.1 Test Architecture

```mermaid
graph TD
    subgraph "Unit Tests"
        UT1["CoroTask_test<br/>- Construction/destruction<br/>- co_return values<br/>- Exception propagation<br/>- Lifetime management"]
        UT2["JobQueueAwaiter_test<br/>- Schedule on correct JobType<br/>- Resume on worker thread<br/>- Shutdown handling"]
        UT3["LocalValue integration<br/>- Per-coroutine isolation<br/>- Multi-coroutine concurrent<br/>- Cross-thread consistency"]
    end

    subgraph "Migration Tests"
        MT1["Coroutine_test rewrite<br/>- correct_order<br/>- incorrect_order<br/>- thread_specific_storage"]
        MT2["PostCoro migration<br/>- Post/resume cycles<br/>- Shutdown rejection<br/>- Early exit"]
    end

    subgraph "Integration Tests"
        IT1["RPC Path Finding<br/>- Suspend/resume flow<br/>- Shutdown during suspend<br/>- Concurrent requests"]
        IT2["Full --unittest suite<br/>- All existing tests pass<br/>- No regressions"]
    end

    subgraph "Performance Tests"
        PT1["Memory benchmarks"]
        PT2["Context switch benchmarks"]
        PT3["RPC throughput under load"]
    end

    subgraph "Sanitizer Tests"
        ST1["ASAN<br/>(memory errors)"]
        ST2["TSAN<br/>(data races)"]
        ST3["UBSan<br/>(undefined behavior)"]
    end

    UT1 --> MT1
    UT2 --> MT2
    MT1 --> IT1
    MT2 --> IT2
    IT1 --> PT1
    IT2 --> PT2
    PT1 --> ST1
    PT2 --> ST2
    PT3 --> ST3
```

### 5.2 Benchmarking Tests

#### Memory Usage Benchmark

```
Test: Create N coroutines, measure RSS
- N = 100, 1000, 10000
- Measure: peak RSS, per-coroutine overhead
- Compare: Boost (N * 1MB + overhead) vs C++20 (N * ~500B + overhead)
- Tool: /proc/self/status (VmRSS), or getrusage()
```

#### Context Switch Benchmark

```
Test: Yield/resume M times across N coroutines
- M = 100,000 iterations
- N = 1, 10, 100 concurrent coroutines
- Measure: total time, per-switch latency (ns)
- Compare: Boost yield/resume cycle vs C++20 co_await/resume cycle
- Tool: std::chrono::high_resolution_clock
```

#### RPC Throughput Benchmark

```
Test: Concurrent ripple_path_find requests
- Load: 10, 50, 100 concurrent requests
- Measure: requests/second, p50/p95/p99 latency
- Compare: before vs after migration
- Tool: Custom load generator or existing perf infrastructure
```

### 5.3 Unit Test Coverage

| Test                           | What It Validates                             |
| ------------------------------ | --------------------------------------------- |
| `CoroTask<void>` basic         | Coroutine runs to completion, handle cleanup  |
| `CoroTask<int>` with value     | `co_return` value correctly retrieved         |
| `CoroTask` exception           | `unhandled_exception()` captures and rethrows |
| `CoroTask` cancellation        | Destruction before completion cleans up       |
| `JobQueueAwaiter` basic        | `co_await` suspends, resumes on worker thread |
| `JobQueueAwaiter` shutdown     | Returns false / throws when JobQueue stopping |
| `PostCoroTask` lifecycle       | Create → suspend → resume → complete          |
| `PostCoroTask` multiple yields | Multiple co_await points in sequence          |
| `LocalValue` isolation         | 4 coroutines, each sees own LocalValue        |
| `LocalValue` cross-thread      | Resume on different thread, values preserved  |

### 5.4 Integration Testing

- **All existing `--unittest` tests must pass unchanged** (except coroutine-specific tests that are rewritten)
- **Path_test** must pass with identical behavior
- **AMMTest** RPC tests must pass
- **ServerHandler** HTTP/WS handling must work end-to-end

### 5.5 Sanitizer Testing

Per `docs/build/sanitizers.md`:

```bash
# ASAN (memory errors — especially important for coroutine frame lifetime)
export SANITIZERS=address,undefinedbehavior
# Build + test

# TSAN (data races — critical for concurrent coroutine resume)
export SANITIZERS=thread
# Build + test (separate build — cannot mix with ASAN)
```

**Key benefit**: Removing Boost.Coroutine eliminates the `__asan_handle_no_return` false positives caused by Boost context switching (documented in `docs/build/sanitizers.md` line 184).

### 5.6 Regression Testing Methodology

```mermaid
graph LR
    subgraph "Before Migration (Baseline)"
        B1["Build on develop branch"]
        B2["Run --unittest (record pass/fail)"]
        B3["Run memory benchmark (record RSS)"]
        B4["Run context switch benchmark (record ns/switch)"]
    end

    subgraph "After Migration"
        A1["Build on feature branch"]
        A2["Run --unittest (compare pass/fail)"]
        A3["Run memory benchmark (compare RSS)"]
        A4["Run context switch benchmark (compare ns/switch)"]
    end

    subgraph "Acceptance Criteria"
        C1["Zero test regressions"]
        C2["Memory: ≤ baseline"]
        C3["Context switch: ≤ baseline"]
        C4["ASAN/TSAN clean"]
    end

    B1 --> B2 --> B3 --> B4
    A1 --> A2 --> A3 --> A4
    B2 -.->|compare| C1
    A2 -.->|compare| C1
    B3 -.->|compare| C2
    A3 -.->|compare| C2
    B4 -.->|compare| C3
    A4 -.->|compare| C3
    A2 -.-> C4
```

---

## 6. Risks & Mitigation

### 6.1 Risk Matrix

| Risk                                                  | Probability | Impact | Mitigation                                                                  |
| ----------------------------------------------------- | ----------- | ------ | --------------------------------------------------------------------------- |
| **Performance regression** in context switching       | Low         | High   | Benchmark before/after; C++20 should be faster                              |
| **Coroutine frame lifetime bugs** (use-after-destroy) | Medium      | High   | ASAN testing, RAII wrapper for handle, code review                          |
| **Data races on resume**                              | Medium      | High   | TSAN testing, careful await_suspend() implementation                        |
| **LocalValue corruption** across threads              | Low         | High   | Dedicated test with 4+ concurrent coroutines                                |
| **Shutdown race conditions**                          | Medium      | Medium | Replicate existing mutex/cv pattern in new design                           |
| **Missed coroutine consumer** during migration        | Low         | Medium | Exhaustive grep audit (Section 2.4 is complete)                             |
| **Compiler bugs** in coroutine codegen                | Low         | Medium | Test on all three compilers (GCC, Clang, MSVC)                              |
| **Exception loss** across suspension points           | Medium      | Medium | Test exception propagation in every phase                                   |
| **Third-party code depending on Boost.Coroutine**     | Very Low    | Low    | Grep confirms only internal usage                                           |
| **Dangling references in coroutine frames**           | Medium      | High   | ASAN testing, avoid reference params in coroutine functions, use shared_ptr |
| **Colored function infection spreading**              | Low         | Medium | Only 4 call sites need co_await; no nested handlers suspend                 |
| **Symmetric transfer not available**                  | Very Low    | High   | All target compilers (GCC 12+, Clang 16+) support symmetric transfer        |
| **Future handler adding deep yield**                  | Low         | Medium | Code review + CI: static analysis flag any yield from nested depth          |

### 6.2 Rollback Strategy

```mermaid
graph TD
    START["Migration In Progress"]
    CHECK{"Critical Issue<br/>Discovered?"}
    PHASE{"Which Phase?"}

    P1["Phase 1: Delete new files<br/>No production code changed"]
    P2["Phase 2: Revert entry point changes<br/>Old postCoro still present"]
    P3["Phase 3: Revert handler changes<br/>Old Coro still present"]
    P4["Phase 4: Cannot easily rollback<br/>Old code deleted"]

    PREVENT["Prevention:<br/>Do NOT delete old code<br/>until Phase 4 is fully validated"]

    START --> CHECK
    CHECK -->|Yes| PHASE
    CHECK -->|No| DONE["Continue Migration"]
    PHASE -->|1| P1
    PHASE -->|2| P2
    PHASE -->|3| P3
    PHASE -->|4| P4
    P4 --> PREVENT
```

**Key principle**: Old `Coro` class and `postCoro()` remain in the codebase through Phases 1-3. They are only removed in Phase 4, after all migration is validated. Each phase is independently revertible via `git revert`.

### 6.3 Specific Risk: Stackful → Stackless Limitation

**The Big Question**: Can all current `yield()` call sites work with stackless `co_await`?

**Analysis**:

```mermaid
graph TD
    Q["Does yield() get called from<br/>a deeply nested function?"]
    Q -->|Yes| PROBLEM["PROBLEM: co_await can't<br/>suspend from nested calls"]
    Q -->|No| OK["OK: Direct co_await<br/>in coroutine function"]

    CHECK1["RipplePathFind.cpp:131<br/>context.coro.yield()"]
    CHECK1 -->|"Called directly in handler"| OK

    CHECK2["Coroutine_test.cpp<br/>c.yield()"]
    CHECK2 -->|"Called directly in lambda"| OK

    CHECK3["JobQueue_test.cpp<br/>c.yield()"]
    CHECK3 -->|"Called directly in lambda"| OK

    style OK fill:#dfd,stroke:#0a0,color:#000
    style PROBLEM fill:#fdd,stroke:#c00,color:#000
```

**Result**: All `yield()` calls are in the direct body of the postCoro lambda or RPC handler function. **No deep nesting exists.** Migration to stackless `co_await` is fully feasible without architectural redesign.

---

## 7. Timeline & Milestones

### 7.1 Milestone Overview

```mermaid
gantt
    title Migration Timeline
    dateFormat  YYYY-MM-DD
    axisFormat  %b %d

    section Phase 1 - Foundation
    CoroTask + JobQueueAwaiter design     :p1a, 2026-02-26, 3d
    CoroTask implementation               :p1b, after p1a, 3d
    Unit tests for primitives             :p1c, after p1b, 2d
    PR 1 - New coroutine primitives       :milestone, p1m, after p1c, 0d

    section Phase 2 - Entry Points
    Migrate ServerHandler (HTTP + WS)     :p2a, after p1m, 3d
    Migrate GRPCServer                    :p2b, after p2a, 2d
    Update RPC Context                     :p2c, after p2b, 1d
    PR 2 - Entry point migration          :milestone, p2m, after p2c, 0d

    section Phase 3 - Handlers
    Migrate RipplePathFind                :p3a, after p2m, 3d
    Update test infrastructure            :p3b, after p3a, 2d
    PR 3 - Handler migration              :milestone, p3m, after p3b, 0d

    section Phase 4 - Cleanup
    Remove old Coro and update CMake      :p4a, after p3m, 2d
    Performance benchmarks                :p4b, after p4a, 2d
    Sanitizer validation                  :p4c, after p4b, 1d
    PR 4 - Cleanup + validation           :milestone, p4m, after p4c, 0d
```

### 7.2 Milestone Details

#### Milestone 1: New Coroutine Primitives (PR #1)

**Deliverables**:

- `CoroTask<T>` with `promise_type`, `FinalAwaiter`
- `CoroTask<void>` specialization
- `JobQueueAwaiter` for scheduling on JobQueue
- `postCoroTask()` on `JobQueue`
- LocalValue integration in new coroutine type
- Unit test suite: `CoroTask_test.cpp`

**Acceptance Criteria**:

- All new unit tests pass
- Existing `--unittest` suite passes (no regressions from new code)
- ASAN + TSAN clean on new tests
- Code compiles on GCC 12+, Clang 16+

#### Milestone 2: Entry Point Migration (PR #2)

**Deliverables**:

- `ServerHandler::onRequest()` uses `postCoroTask()`
- `ServerHandler::onWSMessage()` uses `postCoroTask()`
- `GRPCServer::CallData::process()` uses `postCoroTask()`
- `RPC::Context` updated to carry new coroutine type
- `processSession`/`processRequest` signatures updated

**Acceptance Criteria**:

- HTTP, WebSocket, and gRPC RPC requests work end-to-end
- Full `--unittest` suite passes
- Manual smoke test: `ripple_path_find` via HTTP/WS

#### Milestone 3: Handler Migration (PR #3)

**Deliverables**:

- `RipplePathFind` uses `co_await` instead of `yield()/post()`
- Path_test and AMMTest updated
- Coroutine_test and JobQueue_test updated for new API

**Acceptance Criteria**:

- Path-finding suspension/continuation works correctly
- All `--unittest` tests pass
- Shutdown-during-pathfind scenario tested

#### Milestone 4: Cleanup & Validation (PR #4)

**Deliverables**:

- Old `Coro` class and `Coro.ipp` removed
- `postCoro()` removed from `JobQueue`
- `Boost::coroutine` removed from CMake
- `BOOST_COROUTINES_NO_DEPRECATION_WARNING` removed
- Performance benchmark results documented
- Sanitizer test results documented

**Acceptance Criteria**:

- Build succeeds without Boost.Coroutine
- Full `--unittest` suite passes
- Memory per coroutine ≤ 10KB (down from 1MB)
- Context switch time ≤ baseline
- ASAN, TSAN, UBSan all clean

---

## 8. Standards & Guidelines

### 8.1 Coroutine Design Standards

#### Rule 1: All coroutine return types must use RAII for handle lifetime

```cpp
// GOOD: Handle destroyed in destructor
~CoroTask() {
    if (handle_) handle_.destroy();
}

// BAD: Manual destroy calls scattered in code
void cleanup() { handle_.destroy(); } // Easy to forget
```

#### Rule 2: Never resume a coroutine from within `await_suspend()`

```cpp
// GOOD: Schedule resume on executor
void await_suspend(std::coroutine_handle<> h) {
    jq_.addJob(type_, name_, [h]() { h.resume(); });
}

// BAD: Direct resume in await_suspend (blocks caller)
void await_suspend(std::coroutine_handle<> h) {
    h.resume(); // Defeats the purpose of suspension
}
```

#### Rule 3: Use `suspend_always` for `initial_suspend()` (lazy start)

```cpp
// GOOD: Lazy start — coroutine doesn't run until explicitly resumed
std::suspend_always initial_suspend() { return {}; }

// BAD for our use case: Eager start — runs immediately on creation
std::suspend_never initial_suspend() { return {}; }
```

Rationale: Matches existing Boost behavior where `postCoro()` schedules execution, not the constructor.

#### Rule 4: Always handle `unhandled_exception()` explicitly

```cpp
void unhandled_exception() {
    exception_ = std::current_exception();
    // NEVER: just swallow the exception
    // NEVER: std::terminate() without logging
}
```

#### Rule 5: Use `suspend_always` for `final_suspend()` to enable continuation

```cpp
// GOOD: Suspend at end to allow cleanup and value retrieval
auto final_suspend() noexcept {
    struct FinalAwaiter {
        bool await_ready() noexcept { return false; }
        std::coroutine_handle<> await_suspend(
            std::coroutine_handle<promise_type> h) noexcept {
            if (h.promise().continuation_)
                return h.promise().continuation_;  // Resume waiter
            return std::noop_coroutine();
        }
        void await_resume() noexcept {}
    };
    return FinalAwaiter{};
}
```

#### Rule 6: Coroutine functions must be clearly marked

```cpp
// GOOD: Return type makes it obvious this is a coroutine
CoroTask<Json::Value> doRipplePathFind(RPC::JsonContext& context) {
    co_await ...;
    co_return result;
}

// BAD: Coroutine hidden behind auto or unclear return type
auto doSomething() { co_return; }
```

### 8.2 Coding Guidelines

#### Thread Safety

1. **Never resume a coroutine concurrently from two threads.** Use the same mutex pattern as existing `Coro::mutex_` to prevent races.
2. **`await_suspend()` is the synchronization point.** All state visible before `await_suspend()` must be visible after `await_resume()`.
3. **Use `std::atomic` or mutexes for shared state** between coroutine and continuation callback.

#### Memory Management

1. **`CoroTask<T>` owns its `coroutine_handle`**. It is move-only, non-copyable.
2. **Never store raw `coroutine_handle<>`** in long-lived data structures without clear ownership.
3. **Prefer `shared_ptr<CoroTask<T>>`** when multiple parties need to observe/wait on a coroutine, mirroring the existing `shared_ptr<Coro>` pattern.

#### Error Handling

1. **Exceptions thrown in coroutine body** are captured by `promise_type::unhandled_exception()` and rethrown in `await_resume()`.
2. **Never let exceptions escape `final_suspend()`** — it's `noexcept`.
3. **Shutdown path**: When `JobQueue` is stopping and `addJob()` returns false, the awaiter must resume the coroutine with an error (throw or return error state) rather than leaving it suspended forever.

#### Naming Conventions

| Entity                | Convention                | Example                                   |
| --------------------- | ------------------------- | ----------------------------------------- |
| Coroutine return type | `CoroTask<T>`             | `CoroTask<void>`, `CoroTask<Json::Value>` |
| Awaiter types         | `*Awaiter` suffix         | `JobQueueAwaiter`, `PathFindAwaiter`      |
| Coroutine functions   | Same as regular functions | `doRipplePathFind(...)`                   |
| Promise types         | Nested `promise_type`     | `CoroTask<T>::promise_type`               |
| JobQueue method       | `postCoroTask()`          | `jq.postCoroTask(jtCLIENT, "name", fn)`   |

#### Code Organization

1. **Coroutine primitives** go in `include/xrpl/core/` (header-only where possible)
2. **Application-specific awaiters** go alongside their consumers
3. **Tests** mirror source structure: `src/test/core/CoroTask_test.cpp`
4. **No conditional compilation** (`#ifdef`) for old vs new coroutine code — migration is clean phases

#### Documentation

1. **Each awaiter must document**: what it waits for, which thread resumes, and what `await_resume()` returns.
2. **Promise type must document**: exception handling behavior and suspension points.
3. **Migration commits must reference this plan** in commit messages.

### 8.3 Branch Strategy

Each milestone is developed on a **sub-branch** of the main feature branch. This keeps PRs focused and independently reviewable.

```
develop
  └── pratik/Switch-to-std-coroutines                       (main feature branch)
        ├── pratik/std-coro/add-coroutine-primitives        (CoroTask, CoroTaskRunner, JobQueueAwaiter, postCoroTask)
        ├── pratik/std-coro/migrate-entry-points            (ServerHandler, GRPCServer, RPC::Context)
        ├── pratik/std-coro/migrate-handlers                (doRipplePathFind, PathFindAwaiter, tests)
        └── pratik/std-coro/cleanup-boost-coroutine         (delete Coro.ipp, remove Boost dep, benchmarks)
```

**Workflow**:

1. Create sub-branch from `pratik/Switch-to-std-coroutines` for each milestone
2. Develop and test on the sub-branch
3. Create PR from sub-branch → `pratik/Switch-to-std-coroutines`
4. After review + merge, start next milestone sub-branch from the updated feature branch
5. Final PR from `pratik/Switch-to-std-coroutines` → `develop`

**Rules**:

- Never push directly to the main feature branch — always via sub-branch PR
- Each sub-branch must pass `--unittest` and sanitizers before PR
- Sub-branch names follow the pattern: `pratik/std-coro/<descriptive-action>` (e.g., `add-coroutine-primitives`, `migrate-entry-points`)
- Milestone PRs must reference this plan document in the description

### 8.4 Code Review Checklist

For every PR in this migration:

- [ ] `coroutine_handle::destroy()` called exactly once per coroutine
- [ ] No concurrent `handle.resume()` calls possible
- [ ] `unhandled_exception()` stores the exception (doesn't discard it)
- [ ] `final_suspend()` is `noexcept`
- [ ] Awaiter `await_suspend()` doesn't block (schedules, not runs)
- [ ] `LocalValues` correctly swapped on suspend/resume boundaries
- [ ] Shutdown path tested (JobQueue stopping during coroutine execution)
- [ ] ASAN clean (no use-after-free on coroutine frame)
- [ ] TSAN clean (no data races on resume)
- [ ] All existing `--unittest` tests still pass

---

## 9. Task List

### Milestone 1: New Coroutine Primitives

- [ ] **1.1** Design `CoroTask<T>` class with `promise_type`
  - Define `promise_type` with `initial_suspend`, `final_suspend`, `unhandled_exception`, `return_value`/`return_void`
  - Implement `FinalAwaiter` for continuation support
  - Implement move-only RAII handle wrapper
  - Support both `CoroTask<T>` and `CoroTask<void>`

- [ ] **1.2** Design and implement `JobQueueAwaiter`
  - `await_suspend()` calls `jq_.addJob(type, name, [h]{ h.resume(); })`
  - Handle `addJob()` failure (shutdown) — resume with error flag or throw
  - Integrate `nSuspend_` counter increment/decrement

- [ ] **1.3** Implement `LocalValues` swap in new coroutine resume path
  - Before `handle.resume()`: save thread-local, install coroutine-local
  - After `handle.resume()` returns: restore thread-local
  - Ensure this works when coroutine migrates between threads

- [ ] **1.4** Add `postCoroTask()` template to `JobQueue`
  - Accept callable returning `CoroTask<void>`
  - Schedule initial execution on JobQueue (mirror `postCoro()` behavior)
  - Return a handle/shared_ptr for join/cancel

- [ ] **1.5** Write unit tests (`src/test/core/CoroTask_test.cpp`)
  - Test `CoroTask<void>` runs to completion
  - Test `CoroTask<int>` returns value
  - Test exception propagation across co_await
  - Test coroutine destruction before completion
  - Test `JobQueueAwaiter` schedules on correct thread
  - Test `LocalValue` isolation across 4+ coroutines
  - Test shutdown rejection (addJob returns false)
  - Test `correct_order` equivalent (yield → join → post → complete)
  - Test `incorrect_order` equivalent (post → yield → complete)
  - Test multiple sequential co_await points

- [ ] **1.6** Verify build on GCC 12+, Clang 16+
- [ ] **1.7** Run ASAN + TSAN on new tests
- [ ] **1.8** Run full `--unittest` suite (no regressions)
- [ ] **1.9** Self-review and create PR #1

### Milestone 2: Entry Point Migration

- [ ] **2.1** Migrate `ServerHandler::onRequest()` (`ServerHandler.cpp:287`)
  - Replace `m_jobQueue.postCoro(jtCLIENT_RPC, ...)` with `postCoroTask()`
  - Update lambda to return `CoroTask<void>` (add `co_return`)
  - Update `processSession` to accept new coroutine type

- [ ] **2.2** Migrate `ServerHandler::onWSMessage()` (`ServerHandler.cpp:325`)
  - Replace `m_jobQueue.postCoro(jtCLIENT_WEBSOCKET, ...)` with `postCoroTask()`
  - Update lambda signature

- [ ] **2.3** Migrate `GRPCServer::CallData::process()` (`GRPCServer.cpp:102`)
  - Replace `app_.getJobQueue().postCoro(JobType::jtRPC, ...)` with `postCoroTask()`
  - Update `process(shared_ptr<Coro> coro)` overload signature

- [ ] **2.4** Update `RPC::Context` (`Context.h:27`)
  - Replace `std::shared_ptr<JobQueue::Coro> coro{}` with new coroutine wrapper type
  - Ensure all code that accesses `context.coro` compiles

- [ ] **2.5** Update `ServerHandler.h` signatures
  - `processSession()` and `processRequest()` parameter types

- [ ] **2.6** Update `GRPCServer.h` signatures
  - `process()` method parameter types

- [ ] **2.7** Run full `--unittest` suite
- [ ] **2.8** Manual smoke test: HTTP + WS + gRPC RPC requests
- [ ] **2.9** Run ASAN + TSAN
- [ ] **2.10** Self-review and create PR #2

### Milestone 3: Handler Migration

- [ ] **3.1** Migrate `doRipplePathFind()` (`RipplePathFind.cpp`)
  - Replace `context.coro->yield()` with `co_await PathFindAwaiter{...}`
  - Replace continuation lambda's `coro->post()` / `coro->resume()` with awaiter scheduling
  - Handle shutdown case (post failure) in awaiter

- [ ] **3.2** Create `PathFindAwaiter` (or use generic `JobQueueAwaiter`)
  - Encapsulate the continuation + yield pattern from `RipplePathFind.cpp` lines 108-132

- [ ] **3.3** Update `Path_test.cpp`
  - Replace `postCoro` usage with `postCoroTask`
  - Ensure `context.coro` usage matches new type

- [ ] **3.4** Update `AMMTest.cpp`
  - Replace `postCoro` usage with `postCoroTask`

- [ ] **3.5** Rewrite `Coroutine_test.cpp` for new API
  - `correct_order`: postCoroTask → co_await → join → resume → complete
  - `incorrect_order`: post before yield equivalent
  - `thread_specific_storage`: 4 coroutines with LocalValue isolation

- [ ] **3.6** Update `JobQueue_test.cpp` `testPostCoro`
  - Migrate to `postCoroTask` API

- [ ] **3.7** Verify `ripple_path_find` works end-to-end with new coroutines
- [ ] **3.8** Test shutdown-during-pathfind scenario
- [ ] **3.9** Run full `--unittest` suite
- [ ] **3.10** Run ASAN + TSAN
- [ ] **3.11** Self-review and create PR #3

### Milestone 4: Cleanup & Validation

- [ ] **4.1** Delete `include/xrpl/core/Coro.ipp`
- [ ] **4.2** Remove from `JobQueue.h`:
  - `#include <boost/coroutine/all.hpp>`
  - `struct Coro_create_t`
  - `class Coro` (entire class)
  - `postCoro()` template
  - Comment block (lines 322-377) describing old race condition
- [ ] **4.3** Update `cmake/deps/Boost.cmake`:
  - Remove `coroutine` from `find_package(Boost REQUIRED COMPONENTS ...)`
  - Remove `Boost::coroutine` from `target_link_libraries`
- [ ] **4.4** Update `cmake/XrplInterface.cmake`:
  - Remove `BOOST_COROUTINES_NO_DEPRECATION_WARNING`
- [ ] **4.5** Run memory benchmark
  - Create N=1000 coroutines, compare RSS: before vs after
  - Document results
- [ ] **4.6** Run context switch benchmark
  - 100K yield/resume cycles, compare latency: before vs after
  - Document results
- [ ] **4.7** Run RPC throughput benchmark
  - Concurrent `ripple_path_find` requests, compare throughput
  - Document results
- [ ] **4.8** Run full `--unittest` suite
- [ ] **4.9** Run ASAN, TSAN, UBSan
  - Confirm `__asan_handle_no_return` warnings are gone
- [ ] **4.10** Verify build on all supported compilers
- [ ] **4.11** Self-review and create PR #4
- [ ] **4.12** Document final benchmark results in PR description

---

## Appendix A: File Inventory

Complete list of files that reference coroutines (for audit tracking):

| #   | File                                        | Must Change | Phase                      |
| --- | ------------------------------------------- | ----------- | -------------------------- |
| 1   | `include/xrpl/core/JobQueue.h`              | Yes         | 1 (add), 4 (remove old)    |
| 2   | `include/xrpl/core/Coro.ipp`                | Yes         | 4 (delete)                 |
| 3   | `include/xrpl/basics/LocalValue.h`          | Maybe       | 1 (if integration changes) |
| 4   | `cmake/deps/Boost.cmake`                    | Yes         | 4                          |
| 5   | `cmake/XrplInterface.cmake`                 | Yes         | 4                          |
| 6   | `src/xrpld/rpc/Context.h`                   | Yes         | 2                          |
| 7   | `src/xrpld/rpc/detail/ServerHandler.cpp`    | Yes         | 2                          |
| 8   | `src/xrpld/rpc/ServerHandler.h`             | Yes         | 2                          |
| 9   | `src/xrpld/app/main/GRPCServer.cpp`         | Yes         | 2                          |
| 10  | `src/xrpld/app/main/GRPCServer.h`           | Yes         | 2                          |
| 11  | `src/xrpld/rpc/handlers/RipplePathFind.cpp` | Yes         | 3                          |
| 12  | `src/test/core/Coroutine_test.cpp`          | Yes         | 3                          |
| 13  | `src/test/core/JobQueue_test.cpp`           | Yes         | 3                          |
| 14  | `src/test/app/Path_test.cpp`                | Yes         | 3                          |
| 15  | `src/test/jtx/impl/AMMTest.cpp`             | Yes         | 3                          |
| 16  | `src/xrpld/rpc/README.md`                   | Yes         | 4 (update docs)            |

## Appendix B: New Files to Create

| #   | File                                  | Phase | Purpose                                  |
| --- | ------------------------------------- | ----- | ---------------------------------------- |
| 1   | `include/xrpl/core/CoroTask.h`        | 1     | `CoroTask<T>` return type + promise_type |
| 2   | `include/xrpl/core/JobQueueAwaiter.h` | 1     | Awaiter for scheduling on JobQueue       |
| 3   | `src/test/core/CoroTask_test.cpp`     | 1     | Unit tests for new primitives            |
