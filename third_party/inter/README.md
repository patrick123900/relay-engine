# Inter

The built-in font of Relay's game interface (labels, buttons and other UI text when no font file
is chosen) is [Inter](https://github.com/rsms/inter) by Rasmus Andersson and the Inter Project
Authors, under the SIL Open Font License 1.1 (see [`LICENSE.txt`](LICENSE.txt)).

- Version: 4.1, from the release archive
  `https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip`
  (SHA-256 `9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e`).
- Files: `extras/ttf/Inter-Regular.ttf` and `extras/ttf/Inter-Bold.ttf`, unmodified.

The build embeds both files in the engine (`tools/embed_binary_files.cmake`), so games need no font
on disk. The OFL allows bundling and embedding them in software; the fonts themselves must not be
sold on their own.
