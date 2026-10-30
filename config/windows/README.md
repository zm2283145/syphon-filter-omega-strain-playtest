# Windows application icon

`omega.ico` is the promotional artwork supplied by the project owner on
2026-10-30 for the executable, taskbar, and title bar. It is an explicit exception
to the repository's source-only policy. It is not extracted from the game ISO.
The original artwork remains the property of its respective copyright holders;
the repository's code license does not license this artwork.

The full portrait is fitted into a transparent square without cropping or
stretching. The icon includes 16, 20, 24, 32, 40, 48, 64, 96, 128, and 256 pixel
images. Windows builds embed it as resource 101 in both `OmegaStrain.exe` and
`omega_play.exe`; no external icon file is required at runtime.

Source PNG SHA-256:
`2611ee91c13c91dc7a0d1ae69c0f27cf08635e0f2425bc898f699c730a23480e`.
Conversion: Pillow 12.3 `ImageOps.pad` with Lanczos sampling, transparent padding,
256-square output, saved as a multiple-resolution ICO.
