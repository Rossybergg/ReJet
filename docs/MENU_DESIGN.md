# The ReJet menu: research and design principles

Phase 3 of the [Alpha 2 plan](ALPHA2_PLAN.md): make the ReJet menu polished, fast and recognisably Jet Set Radio Future. These are the findings it builds on. Mock-ups come next, for the user to choose a direction from.

## What great in-game menus do

**Controller first.** Design for the d-pad and stick from the start, not as an afterthought ([Roblox console guidelines](https://create.roblox.com/docs/production/publishing/console-guidelines)). That means:
- A focus marker that is more than a colour change (shape, size or motion), so it reads at a glance and for colour-blind players.
- Movement in the order the eye reads (top to bottom, left to right), wrapping at the ends.
- Reopening on the last item used.
- Standard buttons: A selects, B goes back, the bumpers switch pages ([game UI notes](https://www.strayspark.studio/blog/game-ui-ux-design-principles)).

**Fast.** Transitions over about 150 ms feel slow by the tenth visit ([common menu mistakes](https://bugnet.io/blog/menu-ux-mistakes-indie-games-make)). Input never waits for an animation: a press during a transition takes effect at once and the animation catches up.

**Feedback.** A sound on every move, select and back, and optionally a short rumble on select (Roblox guidelines above).

**Readable and accessible.** Readable text by default, a larger-text option, and contrast of at least 4.5:1 for text ([Game Accessibility Guidelines](https://gameaccessibilityguidelines.com/basic/), [Xbox Accessibility Guidelines 101](https://devdocs.xbox.com/gaming/accessibility/xbox-accessibility-guidelines/101), [XAG 102 contrast](https://star-hk2.eastasia.cloudapp.azure.com/en-us/gaming/accessibility/xbox-accessibility-guidelines/102)). Button remapping is one of the most-asked-for features.

**Style that doesn't cost readability.** Persona 5 is the reference for stylised menus: graphic shapes, strong colour, motion everywhere ([visual design of Persona 5](https://mechanicsofmagic.com/2022/04/23/visual-design-of-games-persona-5/)). But its director has said the angled, ransom-note text made it hard to read, and that every stylised menu is costly to build ([Kotaku](https://kotaku.com/-metaphor-refantazio-persona-5-menu-ui-hashino-1851667088), [Push Square](https://pushsquare.com/news/2024/10/making-metaphor-refantazio-personas-immaculate-menus-is-really-annoying-says-director)). The lesson: put the attitude in shapes, colour and motion, and keep the words bold, upright and plain.

## Jet Set Radio Future's own menu language

From the game's own screens (the Garage menu, character select and map, captured in ReJet), since the web has little on JSRF's interface:

- **Colour:** big fields of saturated yellow and orange; cyan and teal accents (stat bars, icon tiles); lime and magenta in the world and the graffiti; black everywhere as the frame.
- **Shapes:** thick black outlines; panels and frames tilted a few degrees; square icon tiles in a row; vertical bars and stripes; arrows pointing the way; speech-bubble shapes.
- **Type:** chunky, cut-up stencil lettering for titles ("GARAGE"), white italic text on black strips for descriptions.
- **Attitude:** graffiti and sticker culture, Tokyo street posters, the pirate-radio DJ (Professor K). Loud but tidy: everything sits on a clear grid.

## Principles for ReJet's menu

1. **Instant.** Opens in one frame and feels responsive; no animation longer than 150 ms on a press, and input is never blocked. Measured with the performance tools: the menu stays inside the frame budget.
2. **Controller first, keyboard equal.** The existing button icons; bumpers for pages; remembers position.
3. **JSRF's palette and shapes, ReJet's words.** Tilted panels, black outlines, yellow/orange/cyan, stripes and arrows; titles in a heavy display face, everything else in a clean, upright, highly readable face.
4. **Readable at any size.** Text sized for a TV at 3 metres by default, with a larger-text option; contrast 4.5:1 or better.
5. **Sound.** Move, select, back and toggle sounds, short and punchy. The game's own menu sounds, played from the player's disc, keep it in character without shipping anything.
6. **Fonts we can ship.** An open-licence (OFL) display and text face, embedded in rejet.dll, downloaded only once chosen and approved.
