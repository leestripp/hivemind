# Hive-Garden — Cartoon SVG Avatars

A set of 20 hand-authored cartoon creatures on hexagon badges — the HiveMind
avatar set (240×240 viewBox, crisp at any size). Five bees (the brand) plus
fifteen hive-garden friends.

## Style

Bold modern cartoon style, in the appeal family of classic animation
character design (sparkle-highlight eyes, squashy rounded shapes, saturated
color stories) — no specific studio's characters. Every avatar sits on a
pointy-top hexagon (honeycomb cell) with a unique two-tone gradient, a soft
top-left glow, and a faint honeycomb texture. Thick dark outlines (`#2E2A38`,
round caps/joins), big white eyes with highlight sparkles, blush cheeks.

## The cast

Bees: `queen-bee` (crown, royal purple/gold), `worker-bee` (sky blue),
`drone-bee` (sunglasses, teal), `honey-bee` (honey pot, gold),
`bumblebee` (chubby, amber).

Garden friends: `ladybug`, `butterfly`, `blue-butterfly`, `dragonfly`,
`firefly` (glowing tail, night badge), `caterpillar` (rainbow spots),
`snail`, `ant` (carrying a leaf), `beetle` (jewel shine), `spider`
(four-eyed jumper), `moth`, `grasshopper`, `worm` (round glasses),
`mantis`, `cricket` (music notes, midnight badge).

`user.svg` (human silhouette) is kept from the previous set for the user's
own avatar fallback.

## Usage

Referenced in-app as GTK resources at
`/com/hivemind/client/avatars/<name>.svg`; `auto_avatar()` assigns agents a
stable face by name-hash, and the settings avatar pickers show the grid
(bees first).
