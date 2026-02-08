# (HPC) High-performance computing
A high-performance computing library focused on developing useful code blocks and intrusive containers with reduced memory fetches, continuous memory, and improved locality to minimize cache misses. It employs a branchless and lockless approach whenever possible, along with efficient synchronization that scales linearly with CPUs, ensuring efficient execution in userland.

## Build 
```
git clone git@github.com:n13l/hpc.git
cd hpc/
./bootstrap.sh          # init submodules (kbuild, liburcu, catch2, bats)
make menuconfig         # or: make defconfig
make -j$(nproc)
make check
```

`bootstrap.sh` initializes only what a standalone build needs: `vendor/kbuild`
(the build system — `scripts/`, `arch/` and `os/` are committed symlinks into
it), `vendor/userspace-rcu` and `vendor/catch2`, plus the bats framework that
kbuild owns. Both in-tree and out-of-tree (`make O=<dir>`) builds are supported.

## Configuration: sections, attributes and getopt_long

`<hpc/conf.h>` lets a program declare its knobs once, as attributes grouped
into sections, and generates three things that otherwise drift apart by hand:

```c
DEFINE_CONF_SECTION(net_conf, "net", "Networking",
	CONF_STRING('l', "listen",  listen,  0, "ADDR", "Address to bind to"),
	CONF_INT   (0,   "backlog", backlog, 0, "N",    "Listen backlog"),
	CONF_INC   ('v', "verbose", verbose, 0, NULL,   "Increase verbosity"),
	CONF_HELP_OPTION
);

struct conf_ctx *ctx = conf_alloc(NULL);	/* NULL: use mm_libc() */
conf_add(ctx, &net_conf);
int rest = conf_getopt(ctx, argc, argv);	/* or drive the loop yourself */
```

- the **getopt_long() tables** — `conf_longopts()` and `conf_shortopts()` give
  back exactly the `struct option[]` and short-option string you would have
  written, so a program can keep its own `getopt_long()` loop and call
  `conf_dispatch()` for the options this owns (`CONF_USER` declares one it
  handles itself);
- the **`--help` text**, grouped under each section's headline;
- the **`net.listen` namespace** — `-S net.listen=…`, `conf_set()`, a
  configuration file behind `-C`, and `--dumpconfig`.

Option spellings are declared verbatim; a section is a namespace and a help
grouping, never a prefix on the command line.

Memory comes from hpc's allocator interface (`struct mm`, `<mem/alloc.h>`):
the context tracks what it allocated and `conf_free()` releases it, so any mm
works — `mm_libc()` by default, an arena or pool if one is passed in.

**CONFIG_SECTION** (*Library options*) decides how much of this a build pays
for. With it off, the section names and the attribute metavars and descriptions
stop being struct fields at all — the declaration macros never name the string
literals, so they are never linked — and `-S`, `-C` and `--dumpconfig` go with
the machinery behind them. What is left is the long name, the short letter, the
target and its type: the option parsing is unchanged, the command line is
identical, and source that compiled with sections on compiles with them off
(`conf_set()`, `conf_load()` and `conf_dump()` become inline stubs and `--help`
lists the options without descriptions).
