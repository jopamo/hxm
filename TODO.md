# TODO

## Planned Features

- [ ] Add timerfd/signalfd (and optional IPC fd) to the main event loop for unified poll/epoll handling.
- [ ] Coalesce RandR notify bursts and apply a single settled-tick layout pass.

## Bugs

- [ ] TBD

## Refactoring

- [ ] Remove or async-ify synchronous XCB replies in hot paths (e.g. `_NET_REQUEST_FRAME_EXTENTS` in `src/wm.c`).

## Documentation

- [ ] Document an explicit "sync boundary" policy and enforce it via a simple CI guard/test.
