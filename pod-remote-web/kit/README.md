# VOTOL POD — Alternative UI Asset Kit

This kit is intended to implement the alternative cyber-industrial VOTOL POD UI concept.

## Included

- `mockups/`
  - `control_disarmed.png`
  - `control_armed.png`
  - `configuration.png`
  - `pairing.png`
- `icons/`
  - lightweight stroke-based SVG icons, using `currentColor`
- `tokens/design-tokens.json`
- `tokens/tokens.css`

## Recommended implementation

Use the SVG icons as masks/currentColor assets so the same icon can inherit state color.

Core UI mapping:

- Jade/green → connected / disarmed / safe
- Red → armed / panic / destructive
- Amber → primary brand / navigation active / pairing CTA
- Cyan → battery telemetry
- Slate → neutral/offline surfaces

## Important note about hero imagery

The motorcycle/pod imagery visible in the concept mockups is part of the generated presentation image, not a separately layered production asset. For implementation, use:
1. your own bike/product photography, or
2. a dedicated rendered hero background exported separately.

Do not crop the mockup and use that embedded vehicle imagery as a production UI asset because it includes lighting/composition baked into the screen design.

## Suggested app assets

- App icon: VOTOL bolt mark on obsidian background
- Optional bike/pod hero image: 9:16 or 4:5 dark product render, transparent PNG/WebP preferred
- Optional display pod image for pairing: transparent PNG/WebP
- Use CSS/Flutter/Compose gradients and glows instead of rasterizing card backgrounds.
