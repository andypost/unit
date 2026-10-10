# maintainer-from-telegram.md

## Confirmed ideas from Telegram voice transcripts

This document contains only ideas that are directly supported by the provided Telegram voice-message transcripts.

It is intentionally separate from earlier maintainer notes that were based on the roadmap and public repository docs.

---

## 1. Core strategic tension: Unit vs Docker-era packaging

A central theme in the voice messages is that the original Unit idea — one server hosting many applications and many languages together — conflicts with the way software is packaged today.

The maintainer explicitly says this multi-app model is, in practice, contrary to the dominant Docker/microservice packaging model, where each service is usually put into its own container. Even when using Unit today, the described real-world pattern is often “one Unit per service/container”, for example one Unit instance just to run one PHP project.

That creates a strategic product question:

- Is FreeUnit trying to be a real multi-service runtime platform?
- Or is it mostly a per-service wrapper/runtime inside individual containers?
- If it is not replacing that packaging model, what distinct value does it add for Go, Rust, or Node, which already run well on their own?

---

## 2. The strongest original idea: hidden async PHP at the runtime level

The most important technical thesis in the transcripts is not “async PHP syntax”, but **runtime-level hidden asynchrony**.

The idea is:

- developers continue writing mostly synchronous PHP code,
- no need to force explicit async/await-style language constructs,
- blocking operations like DB access and socket I/O are handled asynchronously by the runtime,
- execution is coordinated through an event loop,
- the goal is **not** to make a single request much faster,
- the goal is to handle far more concurrent requests with much better hardware utilization.

This is framed as a way to get very high concurrency (“process more, not faster”) while preserving the developer ergonomics that made PHP popular.

The explicit comparison is that RoadRunner / OpenSwoole-style models remain niche because the ecosystem still expects classic synchronous PHP semantics.

---

## 3. WASM as the “ultimate runtime” direction

The transcripts repeatedly connect this async-runtime idea to **WASM**.

The vision is roughly:

- many modules could run inside one common runtime,
- the runtime could provide a shared event loop,
- work could be spread efficiently across cores/threads,
- the model could be attractive for serverless or multi-tenant execution platforms,
- WASM gives strong isolation and operational control.

This is presented almost as the “ultimate” version of the idea:
a shared runtime, strong isolation, efficient scheduling, and good packing density.

At the same time, the maintainer is very aware that this vision may fit **serverless/platform providers** better than ordinary project developers.

---

## 4. Why PHP-in-WASM is hard

The transcripts are very concrete about implementation difficulties.

### Problem with “just run PHP in WASM”
A naive PHP-in-WASM approach is described as effectively embedding PHP and running scripts in a CLI-like mode.

That leads to major losses:

- no proper opcache behavior,
- no JIT benefit,
- each request effectively becomes a fresh PHP start,
- overall this is considered a bad tradeoff.

### Alternative idea: FPM inside WASM
A more interesting idea mentioned is to run something like PHP-FPM inside WASM and simulate requests into it, for example via FastCGI-like calls.

That would preserve more of PHP’s native behavior and maybe allow a pool-style processing model.

But the transcript is also skeptical:
- implementation is unclear,
- the gains may still be small versus native execution,
- it sounds attractive, but remains highly experimental.

---

## 5. Throughput over latency

A repeated point in the voice messages:

- the async-runtime model may not make a single request finish faster,
- but it should allow much larger concurrency and much better throughput,
- especially while requests wait on external resources like databases.

The described mental model is:

- from the programmer’s perspective the code stays synchronous,
- from the server/runtime perspective the work is asynchronous,
- while one request is waiting on I/O, the runtime keeps processing other work.

This is described as the real win:
not lower per-request computation time, but much higher total request handling capacity.

---

## 6. Memory safety and reset semantics as a major advantage

Another strongly emphasized idea is the memory model.

The transcripts frame the WASM/request model as very attractive because:

- each invocation can be treated like a fresh isolated execution,
- memory can be reset after handling,
- there is less risk of leaks accumulating across requests,
- there is less dependence on complex GC behavior or reference cleanup logic,
- operational safety improves, especially if the host/runtime is implemented in Rust.

This is presented as one of the most compelling advantages of the architecture, not merely a side effect.

---

## 7. The likely market: serverless or platform runtimes, not typical Docker projects

The transcripts are quite skeptical that mainstream developers will change their workflow to fit a new “pack everything into Unit/WASM” model.

The likely audience described is closer to:

- serverless platforms,
- hosted execution providers,
- runtime platforms that accept uploaded modules/functions and schedule them internally,
- multi-tenant systems optimizing density and isolation.

By contrast, for ordinary project teams using Docker and one-container-per-service, the maintainer sounds much less convinced that this approach is compelling enough to change existing habits.

---

## 8. Language relevance question: why FreeUnit for Go/Rust/Node?

There is a direct strategic doubt in the transcripts:

- Go, Rust, Node already solve their runtime/server story reasonably well,
- adding another runtime layer in front of them may not be compelling,
- so what is FreeUnit really for?

This leads to the implicit conclusion that the strongest unique value may be around **PHP**, because PHP’s traditional runtime model leaves more room for this kind of innovation.

That does not mean other languages are impossible, but it suggests the maintainer sees PHP as the most meaningful reason to pursue this direction.

---

## 9. Product-direction uncertainty is itself a major finding

One of the most useful results from the transcripts is not a concrete feature, but a genuine open question:

> Where should Unit / FreeUnit actually go?

The voice messages do not present a fully settled roadmap. They reveal a tension between:

- FreeUnit as a polyglot “everything runtime”,
- FreeUnit as a per-service runtime wrapper,
- FreeUnit as a WASM/serverless execution substrate,
- FreeUnit as a PHP-focused innovation platform.

That uncertainty is real and should be preserved, not polished away.

---

## 10. Best concise summary of the maintainer’s actual idea

If reduced to one line, the most distinctive idea from the Telegram messages is:

> Make PHP effectively asynchronous at the runtime level, ideally inside a high-isolation shared WASM-style runtime, so developers keep synchronous code while the platform handles massively concurrent I/O-bound workloads efficiently.

That is the clearest technical and strategic through-line across all four transcripts.

---

## 11. What should be treated as confirmed vs inferred

### Confirmed by transcripts
- conflict with Docker-era multi-container packaging
- skepticism toward the original “many apps in one Unit” concept
- hidden async PHP at runtime level
- focus on throughput/concurrency rather than single-request speed
- strong interest in WASM as runtime/isolation substrate
- skepticism about naive PHP-in-WASM via CLI
- speculative idea of FPM-inside-WASM
- memory reset / leak resistance as a major advantage
- belief that this could fit serverless/platform vendors better than regular projects
- doubt about the value proposition for Go/Rust/Node
- uncertainty about FreeUnit’s final direction

### Not yet to be presented as fully confirmed
- any precise roadmap or delivery timeline
- any claim that these ideas already match the official public roadmap
- any statement that the maintainer fully committed to WASM as the primary direction
- any claim that this replaces the broader multi-language roadmap already in the repo

---

## 12. Editorial recommendation

Any future maintainer/developer document should clearly separate:

1. **Telegram-confirmed ideas**
2. **Repo roadmap / public docs**
3. **Additional editorial recommendations**

Mixing these together makes the project look more settled than it actually is.
