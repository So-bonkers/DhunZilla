# Preliminary trademark screen — "OpenPattern"

**Date:** 2026-11-18 · **Status:** desk research only — *not* legal advice and *not* a formal
clearance search. Registers were checked through public web/aggregator sources; USPTO, EUIPO and
INPI bulk/API access needs credentials or a paid search, so confidence is stated per item below.

## Bottom line
- **No registered trademark found in the software classes (Nice 9 / 42)** for "OPENPATTERN" or
  "OPEN PATTERN" in the US or EU in this preliminary pass.
- The exact name is nonetheless **in active commercial use in at least four unrelated fields**,
  including a US aircraft marketplace with a class-35 filing, and a German garment-pattern brand
  that owns `openpattern.io` and uses "Open+Pattern".
- **Risk for a music-software launch: low-to-moderate overall** (different goods/services), but the
  name is **descriptive and weak** — "pattern" is a core DAW term, so "OpenPattern" describes the
  product. Expect difficulty registering and enforcing it, and unavoidable search-result collision.
- **Recommendation:** keep it as the codename for now (already public), but decide on a distinctive
  product name before spending on branding, domains, or filings. If keeping it, budget a formal
  clearance search + class 9/42 filings rather than assuming exclusivity.

## Who else uses it

| User | Field | Evidence | Trademark status seen |
|---|---|---|---|
| **Open+Pattern** — Hugh Clarke / TSOM, Aurachtal, Germany | Open-source garment patterns (CC BY 4.0), since 2023 | [openpattern.io](https://openpattern.io/impressum/), owns `openpattern.io` | No ® claim found; likely unregistered |
| **OPEN PATTERN INC** — US | Online marketplace for buying/selling aircraft ("A clearer way to buy and sell aircraft") | [openpattern.com](https://openpattern.com), USPTO Official Gazette [2019-08-20](https://www.uspto.gov/web/trademarks/tmog/20190820_OG.pdf) shows a class-35 online-marketplace filing | US filing in class 35 (marketplace), not software |
| **OPENPATTERN SARL** — Maisons-Laffitte, France, since 2008 | R&D in physical/natural sciences | [Manageo company record](https://www.manageo.fr/entreprises/503025256.html) | Owns only `macaveprivee.com` (INPI), **not** OPENPATTERN |
| **OpenPattern (OMRP)** | Open-source modular routing platform / hardware | [OpenCores project](https://w.opencores.org/websvn/filedetails?repname=omrpv2) | None seen |
| GitHub projects | Python repo `fmetivier/OpenPattern` (2022), `jstanton617/OpenPatterns` (video calibration patterns), `OpenPatternsInc`, textile tool | GitHub API search | — |

No `openpattern` package on npm or PyPI, and no matching app in the iOS App Store search.

## Register search result (per jurisdiction)

| Register | Method | Result | Confidence |
|---|---|---|---|
| USPTO (US) | Trademarkia full-text search ("openpattern" and "open pattern"); Justia (blocked, HTTP 403); USPTO API (needs key) | No software-class hit; class-35 marketplace publication found via OG | Medium — **not a substitute for a USPTO TESS/TSDR clearance search** |
| EUIPO (EU) | Automated access blocked (TMview WAF, eSearch is JS-only) | No EUTM surfaced indirectly; both EU users appear unregistered | Low–medium |
| INPI (FR) | Via Manageo: the SARL's only recorded mark is `macaveprivee.com` | No OPENPATTERN mark at INPI | Medium |
| UK / India | Not searched | — | None |

## Name-strength analysis (independent of conflicts)
- **Descriptive:** "open" + "pattern" (a pattern is a first-class DAW concept). A mark that
  describes the goods is refused registration absent acquired distinctiveness (USPTO §2(e)(1);
  EUIPO Art. 7(1)(c)). This is the bigger long-term issue than any of the users above.
- **Weak enforcement:** even if registered narrowly, scope would be thin and hard to police.
- **Phonetic identity:** "OpenPattern" vs "Open Pattern" vs "Open+Pattern" are the same mark
  commercially; spacing/styling does not create distance.
- **Upside:** none of the users above sell music software, and none appears to hold class 9/42.
  A DAW launch is unlikely to be blocked; the risk is branding clutter, not litigation.

## Domains and handles
| Name | Status |
|---|---|
| `openpattern.com` / `.org` / `.net` / `.dev` / `.io` | **Registered** (RDAP) |
| `openpattern.app` / `.audio` / `.music` / `.studio` / `.co` | No RDAP record — **likely available** |
| GitHub org `openpattern` | **Taken** (exists, 0 public repos). Current repo is `So-bonkers/OpenPattern` |
| npm / PyPI `openpattern` | Free |

## Recommended next steps
1. **Keep "OpenPattern" as the internal codename** — it is already public and no software-class
   registration surfaced. Do not invest in merchandise/logo/domain buys yet.
2. **Before the public beta**, choose between:
   - a **distinctive product name** (a coined word is registrable and searchable), or
   - keeping OpenPattern with a **distinctive secondary element** in the brand lockup — though the
     descriptive weakness remains.
3. If keeping the name: order a **formal clearance search** (US + EU + IN; a few hundred dollars
   from a trademark attorney) and file in **class 9 (software) + 42 (SaaS/development)**.
   Indicative official fees: USPTO ~$250–350/class, EUIPO ~€850 for one class, India ~₹4,500/class
   (verify current fees).
4. If a web presence matters, register `openpattern.music` / `.app` (cheap, available) but only
   after the name decision; `openpattern.com`/`.io` are not obtainable cheaply.
5. Record the decision in ROADMAP "Open human decisions" once made.

---
*Prepared by an AI agent as desk research. Not a legal opinion. A trademark attorney's clearance
search is the only way to rely on this for a filing or a launch decision.*

## Candidate shortlist (same-day follow-up, 2026-11-18)

Screened for domain (RDAP), GitHub/npm/PyPI handles and an obvious web collision. **This is not a
trademark clearance** — each finalist still needs a proper register search.

| Candidate | Meaning | Namespace | Collisions found | Verdict |
|---|---|---|---|---|
| **Loopfold** | loop + fold (folding loops into a song) | `.io` free; GitHub/npm free; `.com` parked | Only an arXiv ML paper named LoopFold | **Best pick** |
| **Beatfold** | beat + fold | `.io` free; GitHub/npm free; `.com` parked | None found | Strong alternate |
| **Loopweft** | weaving loops; "weft" = the cross-thread that makes a pattern | `.io` free; GitHub/npm free | A knitting shop uses "LOOP&WEFT" (different field) | Good, more obscure |
| **Motifold** | motif + fold | `.io` free; GitHub/npm free | None found | Abstract; weaker recall |
| ~~OpenLoop Studio~~ | user suggestion | `.com/.io/.studio` taken; GitHub + npm taken | OpenLoop Health (telehealth), OpenLoop Systems (software), generic control-systems term | Rejected — more crowded than OpenPattern; "Studio" adds no distinctiveness |
| ~~FreeLoop Desktop~~ | user suggestion | `.com/.io` taken | FreeLoop routing concept (ACM paper), FreeLoop Android app; "Free" is descriptive/laudatory, "Desktop" generic | Rejected — describes a feature, not a brand |
| ~~Beatweave~~ | beat + weave | `.io/.dev` free | **Liquid Rhythm BeatWeaver** (Steam, a beat-generation tool) — direct audio-software collision; plus a Kotlin library | Rejected |
| ~~Songloom~~ | weaving songs | `.io` free | songloom.co.uk — personalised-song gift business (music-adjacent) | Rejected |
| ~~Motifloom~~ | motif + loom | `.io` free | motifloom.com — live book-mapping product | Rejected |
| ~~Waveloom~~ | wave + loom | — | waveloom.ai — AI content company | Rejected |
| ~~Neume~~ | medieval music notation | — | Neume — AI song generator app (neume.io) | Rejected |

**Naming rule learned:** "Open/Free + generic noun" is the most crowded and least registrable pattern
in software; single dictionary words are all taken. Coined two-word compounds with a music/weaving
root (fold, weft, loom) are the workable seam. Every candidate's `.com` is already registered
(parked) — plan for `.io`/`.dev` and a later purchase, or accept the `.com` is not coming cheap.
