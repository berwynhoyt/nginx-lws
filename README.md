# Lua Web Services for NGINX

lws-nginx is a module for the [NGINX](https://nginx.org/) server that supports web services
written in Lua, running directly in the server.

Some central design considerations for lws-nginx are the following:

- **Use [PUC-Lua](https://www.lua.org/).** PUC-Lua is the original implementation of Lua
maintained by the language's creators. While [LuaJIT](https://luajit.org/) is without a doubt
an amazing feat of engineering with impressive performance, it remains based on Lua 5.1 with
select extensions. The latest PUC-Lua release is 5.4. PUC-Lua has added new language features
over the years, including 64-bit integers, bit operators, and variable attributes that are not
directly supported in LuaJIT. Perhaps more worryingly, writing "fast" LuaJIT code promotes using
language idioms that are amenable to its optimization while eschewing language features that are
"slow".[^1] *If* the performance of the Lua VM is deemed insufficient for a particular function
of a web service, there is always the option of implementing the function in C. Furthermore,
the research team at PUC-Rio is working on ahead-of-time compilation through Pallene.[^1]

- **Allow Lua web services to block.** To this end, lws-nginx uses a thread pool that runs Lua
services asynchronously without blocking the NGINX event processing loop. Large parts of the
existing Lua ecosystem are *not* non-blocking. The chosen design allows them be be used as is,
on the condition that their libraries are conditionally thread-safe. There is substantial
discussion and research on the topic of non-blocking event architectures vs. multi-threaded
architectures.[^2][^3][^4][^5][^6][^7] Ultimately, this is a trade-off between resource use and
complexity. An event processing loop is arguably less demanding on resources than threads.
However, the resource demand of threads has been coming down over the years with advances
in operating systems. The complexity in the implementation of web services and libraries is
arguably lower if they can simply block instead of having to implement logic for cooperative
multitasking through yielding and asynchronous continuation. Less complexity generally means
less bugs. The lws-nginx module further avoids complex synchronization logic by keeping the
dispatch logic to the thread pool in the single-threaded event processing loop of NGINX. Given
the existing Lua ecosystem and these considerations, using threads seems a reasonable, pragmatic
approach today.

- **Efficient use of Lua states.** Loading and parsing Lua code can take a signficiant amount of
time. For this reason, lws-nginx can re-use Lua states for subsequent requests. A broad range of
directives allows control over the lifecycle of Lua states.

- **Focus on web services.** The purpose of lws-nginx is implementing web services in Lua. This
focus streamlines the design of lws-nginx. For other extension areas in NGINX (including
rewriting, access, and filters), there are numerous highly configurable modules that address
these areas.


## Documentation

Please refer to the [doc](doc) folder.


## Status

lws-nginx is a work in progress.


## Limitations

lws-nginx has been tested with Ubuntu 20.04.6 LTS, Lua 5.3.3, and NGINX 1.18.0. Your mileage
may vary with other software versions.

Lua libraries used with lws-nginx must be conditionally thread-safe.


## License

lws-nginx is released under the MIT license. See LICENSE for license terms.

[^1]: Hugo Musso Gualandi. The Pallene Programming Language. 2020.
[Link](http://www.lua.inf.puc-rio.br/publications/2020-HugoGualandi-phd-thesis.pdf).

[^2]: Paolo Maresca. Scalable I/O: Events- Vs Multithreading-based. 2016.
[Link](https://thetechsolo.wordpress.com/2016/02/29/scalable-io-events-vs-multithreading-based/)

[^3]: Paul Tyma. Thousands of Threads and Blocking I/O. 2008.
[Link](https://silo.tips/download/thousands-of-threads-and-blocking-i-o).

[^4]: Rob von Behren et al. Why Events Are A Bad Idea (for high-concurrency servers). In
Proceedings of HotOS IX: The 9th Workshop on Hot Topics in Operating Systems. 2003.
[Link](https://www.usenix.org/legacy/events/hotos03/tech/full_papers/vonbehren/vonbehren.pdf)

[^5]: Atul Adya et al. Cooperative Task Management Without Manual Stack Management. In
Proceedings of the 2002 USENIX Annual Technical Conference. 2002.
[Link](https://www.usenix.org/legacy/publications/library/proceedings/usenix02/full_papers/adyahowell/adyahowell.pdf)

[^6]: Matt Welsh et al. SEDA: An Architecture for Well-Conditioned Scalable Internet Services.
In Symposium on Operating Systems. 2001. [Link](http://www.sosp.org/2001/papers/welsh.pdf)

[^7]: John Ousterhout. Why Threads Are A Bad Idea (for most purposes). 1996.
[Link](https://web.stanford.edu/~ouster/cgi-bin/papers/threads.pdf)
