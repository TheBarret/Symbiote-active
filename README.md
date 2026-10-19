# Symbiote-active
<img width="1024" height="559" alt="logo" src="https://github.com/user-attachments/assets/808c960e-6db9-469d-93ed-fd51f01e92cf" />  

Continuation of the symbiote kernel in an isolated repo, this version will be active and tweaked on a regular base.   

**0.1.1** - Major overhaul of the core

<img width="1290" height="850" alt="image" src="https://github.com/user-attachments/assets/abc68e05-4359-46e1-9728-7f0f8246e40e" />  

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

- `ext/envtool/`: added the `envtool` using the `TUI` engine to browse inspect and stress test the `environment` variable store.

- `mask.h`: added contracts and functions that can encode and decode the `MASK` bit field used in the `env.h`;  
  Bit layout:
  ```bash
        | 31    24 | 23  20 | 19  16 | 15              0 |
        +----------+--------+--------+-------------------+
        |  OWNER   |  TYPE  | FLAGS  |     PRIVATE       |
        | (8 bits) | (4 bit)| (4 bit)|    (16 bits)      |
        +----------+--------+--------+-------------------+
   ````

- `bus.c`: added `manifest` function, catalogues the entire bus registry, pushes information on the `environment` store.  
  
  <img width="755" height="429" alt="image" src="https://github.com/user-attachments/assets/b4157208-0a1d-435f-99a8-2322d71aa797" />   

- `tui.c`: added `tui_fetch_resolution()`, uses the `env` store to obtain framebuffer width and height, there is a known bug where full resolution creates incorrect behavior.  
  *(a temporary make-shift fix by scaling the `col / row` down by 15% seems to work for now)*  


# Active & Planned Features

**Environment Broker ("ring collector")**  

A bounded, fixed-capacity table of `(namespace, name) -> value` entries.  
Intended as the kernel's read-mostly information surface: system, session, shell state, flags, paths.   
Ring collector is classified as a virtual device, it can be discovered by extension or scripted objects, through `bus_env()`.  

Mechanical control points (owned by env):  
- access budget : 0 = unlimited; N > 0 = entry is reclaimed after N get()s.  
                  peek() and stat() never consume budget.   

- Ttl ticks     : `0 = no expiry`; otherwise entry expires ttl ticks after set.  

- Importance    : `0 = persistent` Under space pressure the `5-10` importance objects are evicted first, then the least recently accessed.  

- Mask          : `mask.h` defined classification field, searchable through `list_by_mask()` / `count_by_mask()`.  
                  Holds universal and arbitrary metrics about what it is and who owns it, plus device specific event metrics.  

**Interpreter**  

A custom syntax, very basic, inspired from (MS) QBasic, that lets you address devices within the kernel.  

Pseudo `Hello, World!`:  
```lua
    USE: console;

    LET $message, "Hello, World\n";
    LET &console_write, $message;
```

Pseudo `Timer`-event:  
```lua
    USE: console, timer;

    ON TIMER_TICK(10000) {             // milliseconds
      LET $message, "Hello, World\n";
      LET &console_write, $message;
    }
```
  
---
