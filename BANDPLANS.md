# Writing a band plan

A band plan is one JSON file. Drop it in the band plans folder, restart, and it
appears in the Band Plan menu. There is no registration step and no code to touch.

`root/res/bandplans/south-africa.json` is a short one worth copying the shape of.

## The file

```json
{
    "name": "South Africa",
    "country_name": "South Africa",
    "country_code": "ZA",
    "author_name": "Your name",
    "author_url": "https://example.com",
    "bands": [
        { "name": "80m", "type": "amateur", "start": 3500000, "end": 3800000 }
    ]
}
```

All six top level keys are required, and every band needs all four of its keys.
The loader fetches them by name and reports the file as broken if any is missing —
it will not fill in a default.

`name` is what the Band Plan menu lists, and it has to be unique across every plan
in the folder. A second plan called "South Africa" is refused with a message in the
log rather than replacing the first.

## Frequencies are in hertz

`start` and `end` are plain numbers in Hz. Not kHz, not MHz, no units string, no
scientific notation.

| You mean | You write |
| --- | --- |
| 137.8 kHz | `137800` |
| 3.5 MHz | `3500000` |
| 145.8 MHz | `145800000` |
| 1.2 GHz | `1200000000` |

`start` must be lower than `end`. A band written backwards is not rejected — it
just never draws, because the code clamps both ends to the visible range and ends
up with nothing between them.

## Types and colours

`type` picks the colour the band is drawn in. These have a colour out of the box:

| type | default colour |
| --- | --- |
| `amateur` | red |
| `aviation` | green |
| `broadcast` | blue |
| `marine` | cyan |
| `military` | yellow |
| `utility` | magenta |
| `navigation` | orange |
| `satellite` | purple |
| `cellular` | rose |

Any other value — `astronomy`, `radiolocation`, `ism`, whatever you like — is
accepted and drawn in the theme's fallback colour, which is the same grey-white for
all of them. That is fine if you only want the band marked, but if you use ten
types and only nine have colours, the tenth will not look like its own category.

To give a type its own colour, add it to `bandColors` in `config.json`, next to the
five that are already there. The format is `#RRGGBBAA`:

```json
"bandColors": {
    "amateur": "#FF0000FF",
    "aviation": "#00FF00FF",
    "broadcast": "#0000FFFF",
    "marine": "#00FFFFFF",
    "military": "#FFFF00FF",
    "utility": "#FF00FFFF",
    "navigation": "#FF8000FF",
    "satellite": "#8000FFFF",
    "cellular": "#FF0080FF",
    "astronomy": "#40C0A0FF"
}
```

**Existing configs are never merged with new defaults.** ConfigManager takes the
defaults only when it creates the file. So if you already have a `config.json` — and
you do — the four colours added most recently are not in it, and neither will
anything you add here be, until you paste it in yourself. A fresh install gets them
automatically.

## What draws and what doesn't

- Bands are drawn in the order they appear in the file. Two bands that overlap are
  both drawn, the later one over the earlier one. Nothing merges or splits them.
- A band narrower than one pixel at the current zoom is skipped entirely.
- The **name is only drawn if it fits inside the band at the current zoom**. Long
  names on narrow bands are invisible until you zoom right in. "70cm" survives where
  "70 Centimetre Amateur Band" never appears. Keep names short; the detail belongs in
  your notes, not on the waterfall.
- Sorting by `start` is not required, but it makes the file far easier to check and
  to edit later.

## Where the file goes

While working on it, put it in `root/res/bandplans/` in this repo — that whole
directory is installed wholesale, so nothing else needs changing to ship it.

For a build you have already installed, it goes in the `bandplans` folder inside the
resources directory, which is whatever `resourcesDirectory` points at in your
`config.json`.

Either way the file is read once at startup. Restart to see changes.

## If it doesn't appear

The band plan loader no longer takes the application down when a file is malformed,
but it does skip the file. Check the log window: a bad plan prints as

```
Could not load band plan <path>: <what was wrong>
```

The usual causes are a trailing comma after the last entry in a list, a missing
`"type"` on one band out of two hundred, or frequencies written as `3.5` instead of
`3500000`.

## Checking your work

Nothing validates a plan beyond the format, so these are worth a read through
before you ship one:

- A type you used that has no colour, leaving those bands indistinguishable from
  each other.
- Bands written backwards, which draw as nothing at all.
- Overlapping bands. Some overlaps are real — ISM 433 sits inside 70cm — but most
  are a copied line someone forgot to edit.
- Names too long to ever draw at the width of their band.

- Frequencies in the wrong unit somewhere in the middle of the file. A band 1000×
  too small is easy to miss when the rest are right.
- Gaps and overlaps at the boundaries between adjacent allocations.
- Bands copied from a different region. South Africa is ITU Region 1, so a plan
  copied from a US source will be wrong in places that look plausible.

Use your national allocation table as the source — for South Africa that is ICASA's
national radio frequency plan, with the SARL band plan for the amateur segments —
rather than another SDR program's file, which may be someone else's guess.
