# Known bugs

Seen but parked until the main features work. Newest first; move an entry to `PROGRESS.md` when it is fixed.

| Seen | Where | What | Notes |
|---|---|---|---|
| 2026-10-28 | Intro fly-through, grind rails | Shimmering: see-through black patches flicker over the rails as the camera moves | User report. Possibly the stencil shadow volumes (the shadow quad darkens where the stencil count is off: z-fighting between volume and rail, or volume caps). Capture with F11 when it happens. |
| 2026-10-28 | Intro fly-through (frame ~2400) | A large flat white shape covers the lower part of the screen | Not a regression (same with the 10-27 build). Check against the original: may be foreground geometry missing its texture or lighting. |
| 2026-10-27 | Garage | No ground shadow under the player | No character shadow volume is drawn there; find how the character shadow is made. |
