# pingStation 3 JSON fixtures

Used by `test_pingstation` (ctest `pingstation`).

## Provenance

`icd-traffic-1090.json`, `icd-status.json` and `icd-status-verbatim.json` are the JSON
examples of the the vendor interface document, document the vendor interface document-001, pages 12–13,
transcribed from the PDF (the PDF itself is not committed; it stays git-ignored).
These files are the repository's copy of those examples. The coordinates, addresses, GUIDs
and times in them are the vendor's example values, not real observations.

- `icd-traffic-1090.json`: the traffic example (the vendor interface document), byte for byte as printed,
  including its indentation, the trailing space in `"CJT340 "` and `timeOfReceptionNS`
  written as a string.
- `icd-status-verbatim.json`: the status example (the vendor interface document) exactly as printed. The PDF puts
  typographic curly quotes (U+201C/U+201D) around the key `status`, which makes the text
  invalid JSON. This is a typesetting error in the document; a real device sends plain JSON.
  The parser must reject this file (no lenient parsing), and the test checks that it is
  rejected and counted.
- `icd-status.json`: the same status example with those two curly quotes replaced by ASCII
  `"`; nothing else changed.

## Synthetic

- `uat-detail.json`: **synthetic**, not from the ICD (the ICD gives no UAT example). Built
  from the UAT field list of the vendor interface document: a UAT target (`trafficSource` 1) with a geometric
  primary altitude of 5000 ft, a barometric secondary altitude of 5250 ft, on the ground,
  minimum-fuel emergency, emitter type 14 (B6), squawk 0007 and an 18-byte `rawData` whose
  content is made up (only its length matters).
