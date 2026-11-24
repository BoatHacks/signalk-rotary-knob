# Registry entry drafts

Drafts of the two files this firmware adds to the [espOS firmware registry](https://github.com/signalk-espOS/registry) once
the first signed release exists (docs/plans/release.md): `boards/elecrow-crowpanel-rotary-1-28.json` and
`projects/rotary-knob.json`.

`signingKeyId` 4e0a983dc96231e6 is the production key's (created 2026-11-24). Before the pull request: check the board's product name and add its `buyUrl` and a
photo we may publish, then run `node scripts/validate.mjs` in a registry checkout.