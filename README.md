# Symbiote-active

<img width="1024" height="559" alt="logo" src="https://github.com/user-attachments/assets/808c960e-6db9-469d-93ed-fd51f01e92cf" />  

<img width="1290" height="850" alt="0.1.1" src="https://github.com/user-attachments/assets/0e444b29-62c9-4ae6-91dd-2dd370c977dd" />  


Continuation of the symbiote kernel in an isolated repo, this version will be active and tweaked on a regular base.   

**0.1.1** - Overhaul Core & Environment Layer

<img width="1004" height="547" alt="Environment" src="https://github.com/user-attachments/assets/4343e103-5507-4315-a907-386609a3aab7" />  

- All interior `core/*` modules have been refactored and aligned, and have added safety logic.
  
- `core/timer.h`, `core/timer.c`:  
  added helpers for milliseconds to timer ticks conversions (`ms_to_ticks_floor`, `ms_to_ticks_ceil`,`ms_to_ticks_round`).
  Rounding policy is now explicit at every call site, and the conversions are overflow-safe.
  `timer_sleep_ms` and `timer_after` were updated to use the ceil variant, which is what both have always implicitly wanted.

- `core/env.c`, `core/env.h`, `ext/env/env.c`:  
  added the `environment` device and its shell toolkit. A bounded, fixed-capacity store of `(namespace, name) -> value` entries,
  with per-entry access budgets, time budgets, importance levels, and free-form classification masks.
  Reclamation follows OOM-model principles: entries are reclaimed synchronously when their access budget is exhausted,
  asynchronously by a timer-driven sweeper when their time budget passes, and under space pressure by an importance-ordered eviction policy.
  The device is registered on the bus as `BUS_CLASS_ENV` and reached through `bus_env()`.

- `ext/envtool/`: added the `envtool` using the `TUI` engine to browse and inspect the `environment` variable store.  
- `ext/btool/`: added the `btool` extension app for inspecting the bus registry.
  
## Environment Layers

The `environment` module is basically a storage device shaped as a driver on the bus registry,  
it stores and shares variables with a lifecycle:  

    A bounded, fixed-capacity key-value store where each entry is a string addressed by a `(namespace, name)` pair,  
    or with the `MASK` field for arbitrary bitmask matching, the table is statically bounded, cannot grow without limit.  

<img width="736" height="223" alt="env" src="https://github.com/user-attachments/assets/1be556f2-ad66-405b-9e16-07c058339d41" />  


Entries carry explicit lifetimes:  

    An access budget that is consumed by reads, a time budget that is enforced by a periodic sweeper,  
    and an importance value that orders which entries are sacrificed when space runs short.  
    All three mechanisms converge on a single reclamation path,  
    so the rules for when an entry dies are stated once and applied uniformly.  

<img width="750" height="455" alt="envtool" src="https://github.com/user-attachments/assets/bcff0efc-0d77-46ae-b09a-bd3af6b9e34e" />  

*using `envtool` extension to inspect all variables*  
 
It serves three platforms, in order of appearance:  

**The kernel and extensions scope**  

which use it as a place to record runtime state  data that would otherwise need to be threaded through call chains as function arguments.  
The `shell.cwd`, `shell.last_exit`, and comparable keys are the first instance of this pattern;  
future devices (System, in particular) will register their own namespaces and expose their values the same way.  

**Interpreters and Script engines**  

which reach the device through interpreters or script engines and use it to write/read/store between script invocations.  
A script that runs, deposits a result under a well-known key, and exits leaves a durable artifact for the next script to pick up.  
The access budget model makes the one-shot handoff, write once, read once, gone, the natural default,  
rather than a pattern that has to be enforced by convention.  

**The shell**  

which uses the device's shell command as a live inspection tool.  
The `env stat` view, the `env list` view and the device-level counters in `env stats`,  
together turn the environment from an opaque store into a piece of kernel state that can be examined,  
while the system is running without adding any diagnostic code to the device itself.  

---
