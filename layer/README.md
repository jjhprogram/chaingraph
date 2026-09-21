# VK_LAYER_hitchtrace — frame marks for any Vulkan app

An implicit Vulkan layer that wraps `vkQueuePresentKHR` and calls the two
hitchtrace marker functions around it:

```
hitch_frame_mark(frame_id)   present ENTRY  -> closes frame N, opens frame N+1
    <next layer / ICD vkQueuePresentKHR>
hitch_present_end(frame_id)  present RETURN -> closes the present bracket
```

Blocked time between the two is `HT_BLOCK_PRESENT` — the display pacing the app.
The markers are exported (`.symtab` **and** `.dynsym`), `noinline/noclone/used`,
and take the frame id in the ABI's first argument register, the same contract as
`tests/hitchbench.c`.

**Why a layer.** An app that resolves entry points through `vkGetDeviceProcAddr`
— volk, DXVK, vkd3d-proton, winevulkan, and `/usr/bin/vkcube` as shipped — never
enters the loader's exported `vkQueuePresentKHR`, so a uprobe on that symbol
counts zero presents (`nm -D --undefined-only /usr/bin/vkcube` lists no `vk*`
import at all). A layer sits in the device dispatch chain however the app got its
pointers.

`frame_id` is **process-global**, not per device: the BPF side keys frame state by
tgid, so one monotonic sequence per process is what it wants. It counts
*presents*, not rendered images — an app with two swapchains that presents twice
per image produces two ids, exactly like PresentMon's `MsBetweenPresents`. Record
that in the run manifest.

## Build and install

```sh
make layer            # -> build/libVkLayer_hitchtrace.so
make layer-install    # -> ~/.local/share/vulkan/implicit_layer.d/ (no root)
```

`layer-install` rewrites `library_path` to the absolute path of the built `.so`.
For a system-wide install put the same manifest in
`/usr/share/vulkan/implicit_layer.d/` — never use `VK_LAYER_PATH`, which
pressure-vessel clears inside the Steam runtime.

## Run

The layer is inert unless asked for:

| Variable | Effect |
|---|---|
| `HITCHTRACE=1` | `enable_environment` — the loader inserts the layer |
| `HITCHTRACE_DISABLE=1` | `disable_environment` — wins over the above |
| `HITCHTRACE_DEBUG=1` | one line to stderr at `vkCreateInstance` and one at `vkDestroyInstance` with the marker counts. Read once at instance create; never touched in the hot path |

```sh
HITCHTRACE=1 vkcube --c 2000                       # X11 / Xwayland
HITCHTRACE=1 vkcube-wayland --c 600                # native Wayland
HITCHTRACE=1 HITCHTRACE_DEBUG=1 vkcube --c 2000    # + marker counts, presents/s
HITCHTRACE=1 VK_LOADER_DEBUG=layer vulkaninfo 2>&1 | grep hitchtrace
```

Check the loader picked it up — `Insert instance layer "VK_LAYER_hitchtrace"` and
`Inserted device layer "VK_LAYER_hitchtrace"`.

## Attach hitchtrace

The markers live in the layer `.so`, so point `-x` at it, not at the app:

```sh
HITCHTRACE=1 vkcube &
sudo build/hitchtrace -p $! \
     -x /home/asdf/ebpf/build/libVkLayer_hitchtrace.so \
     -m hitch_frame_mark -M hitch_present_end -b 16667
```

Attach by host path even when the app runs inside pressure-vessel: the container's
layer `.so` is a symlink to the same inode, and the tgid comes from
`bpf_get_current_pid_tgid()`, never from a marker argument.

With nothing attached the two markers are two empty calls.

## Limits

* 64-bit only (`"library_arch": "64"`). 32-bit Proton titles without
  `PROTON_USE_WOW64=1` need an i386 build of the same source.
* Static dispatch tables: 16 instances, 32 devices per process. Beyond that
  `vkCreateInstance`/`vkCreateDevice` fail loudly rather than silently returning
  an unhooked chain.
* Only `vkQueuePresentKHR` is bracketed. Review issue 7 also wants
  `vkAcquireNextImageKHR`, `vkWaitForPresentKHR` and fence-wait brackets; on WSI
  stacks that throttle in *acquire* rather than in present, the pacing wait lands
  outside this bracket and is currently charged to the frame, not to
  `HT_BLOCK_PRESENT`.
