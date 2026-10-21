# Symbiote-active
<img width="1024" height="559" alt="logo" src="https://github.com/user-attachments/assets/808c960e-6db9-469d-93ed-fd51f01e92cf" />  

Continuation of the symbiote kernel in an isolated repo, this version will be active and tweaked on a regular base.   

**Current goal is to implement layers that accommodate a custom interpreter**  

**0.1.1** - Major overhaul of the core

<img width="1286" height="863" alt="boot" src="https://github.com/user-attachments/assets/9e86190d-187f-4c27-af27-4faec2393479" />  


<img width="1024" height="558" alt="image" src="https://github.com/user-attachments/assets/101c2799-9d71-4560-8abe-40289e9da28b" />  


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
  *(Flag bits are not enforced as of yet!)*

- `bus.c`: added `manifest` function, catalogues the entire bus registry, pushes information on the `environment` store.  
- `ext/fbcon/fbcon.c` : writes `column, row, width, height` manifest to `env` store.
- `tui.c`: added `tui_fetch_resolution()`, uses the `env` store to obtain framebuffer width and height.
- `tui.c`: added `CP437` charsets, for primitive panels.
- `ext/envtool/envtool.c`: added `CP437` support.

<img width="1286" height="863" alt="envtool" src="https://github.com/user-attachments/assets/867f6399-6500-4c16-807b-9f23b271023e" />  

- `ext/tokenizer/tokenizer.c`: added a token parser, first stage of the interpreter.  
  data-types: `string, int, hex`
  structures: `symbolics, identifiers = (functions), keywords = (use, let, while, if, else)`  

- `ext/proxy/proxy.h`, `ext/proxy/proxy.h`: added proxy module, inspired by Microsoft's [MarshalByRefobject](https://learn.microsoft.com/en-us/dotnet/api/system.marshalbyrefobject?view=net-10.0).  

  Example of writing `Hello, World!` to console device using safe call mechanism:  
  ```c
  const struct proxy_ops *p = bus_proxy();
  uint32_t h;
  /* lease  */
  if (p->bind("console", &h) == PROXY_OK) {
      const char msg[] = "Hello, World!\0";
      p->call(h, PROXY_CONSOLE_WRITE, (const uint8_t *)msg, sizeof msg, NULL, 0, NULL);
      p->unbind(h);
  }
  ```
---
