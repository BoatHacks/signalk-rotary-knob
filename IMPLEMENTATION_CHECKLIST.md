# Implementation checklist

How a change gets made in this repo, from reading to commit. Short on purpose: work through it in order, skip what does not
apply to a small change, and write down why you skipped something big.

## 1. Explore
- [ ] Read the issue or the plan in `docs/plans/` in full.
- [ ] Read the sections it touches in [SPEC.md](SPEC.md), [ARCHITECTURE.md](ARCHITECTURE.md) and
      [docs/ui-design.md](docs/ui-design.md). The UI contract (`marine_ui.h`: snapshot in, actions out) is the boundary between
      the screens and the rest.
- [ ] Read the code before writing any. Note where the change crosses components (marine_ui, knob_model, knob_actions,
      knob_config, knob_power).
- [ ] Treat SignalK API and espOS behaviour as unverified until checked against the source or the live server. SPEC.md marks
      those with **(verify)**.

## 2. Plan
- [ ] For anything bigger than a small fix, write or update a plan in `docs/plans/` (see [docs/plans/README.md](docs/plans/README.md)).
- [ ] For a screen change, draw it first (mockup in the conversation or the prototype in `ui/`) and agree on it before coding.
- [ ] Decide how it will be checked: a host test in `test/host`, an item on [docs/hardware-checklist.md](docs/hardware-checklist.md),
      or both.
- [ ] Check the safety rules in SPEC section 2 still hold: touch never changes autopilot or anchor state, engage needs the
      confirm turn, losing the knob never changes the autopilot or the anchor, autopilot and anchor calls need live control.

## 3. Implement
- [ ] Pure logic (parsing, thresholds, timing rules) goes in `knob_logic` so it can be host tested. Add or adjust the host test
      with it.
- [ ] Keep SignalK paths, endpoints and bodies in `knob_actions` and `knob_model`, never in `marine_ui`.
- [ ] Mind the RAM budget: internal RAM is about 25 to 32 KB free at steady state and espOS reboots below 12 KB. Large buffers
      go to PSRAM. Check the boot log's low-water mark after a change that allocates.
- [ ] The FreeRTOS tick is 10 ms. `pdMS_TO_TICKS(5)` is 0 and `vTaskDelay(0)` does not sleep.
- [ ] Do not run the full test suite or build after every small edit. Build when the change is complete, or when asked.

## 4. Verify
- [ ] The build is clean (`-Werror` is on).
- [ ] Host tests pass when `knob_logic` changed (`make -C test/host test`, needs a host gcc and Unity).
- [ ] Flash only when asked. Then go through the matching items on the hardware checklist and fill in the date.
- [ ] Try the edge cases, not just the happy path: stale data, link lost, live control off, night palette, an alarm on top.
- [ ] Compare the result with SPEC.md and ui-design.md, not with memory of the task.

## 5. Document and commit
- [ ] Update SPEC.md, ARCHITECTURE.md, ui-design.md and docs/manual.md where the change made them wrong.
- [ ] Tick the plan's steps, or delete the plan when the work is done and the docs say everything it said.
- [ ] Comments describe the code as it is. No "new", "now", "recently".
- [ ] No private data in the diff: no addresses, hostnames, device ids, ports, tokens, keys or Wi-Fi details. The repo may
      become public.
- [ ] Commit with a message that says why, and reference the issue (`#3`). Push only after asking.
- [ ] Releases and version bumps are separate, explicit steps.

## Common mistakes
- Coding before reading SPEC and the UI contract.
- Putting a SignalK path into the UI, or a UI decision into an adapter.
- Loosening a test or a stale timeout to make a symptom go away instead of finding the cause (the heading stale errors were a
  squelch plugin on the server, not a wrong timeout).
- Leaving the docs describing the old behaviour.
