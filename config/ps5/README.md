# PS5 home screen media

`omega_ps5_package` copies the four files in `sce_sys/` into the PS5 folder title. They are presentation files supplied for this project, and are separate from the game disc assets loaded at runtime.

| File | Format | Role |
| --- | --- | --- |
| `sce_sys/icon0.png` | 512 × 512 RGBA PNG | Home screen tile |
| `sce_sys/pic0.dds` | 3840 × 2160, DX10 BC7_UNORM, one mip | Selection background |
| `sce_sys/pic1.dds` | Same image and format as `pic0.dds` | Launch background |
| `sce_sys/snd0.at9` | RIFF ATRAC9, 48 kHz stereo, 192 kb/s, 36.06 s | Selection music |

The two user supplied 1000 × 750 images were fitted into the target frames with neutral dark padding, preserving their complete artwork and embedded credits. The user supplied “Main Theme (short)” MP3 was attenuated by 8.6 dB before encoding; a decoded loudness check measured −28.0 LUFS, matching `cmake/ps5/param.json`.

Conversion used the [standalone PS5 image and ATRAC9 tools](https://github.com/Nazky/UnleashedRecomp-Prospero/tree/Prospero/tools) outside this repository. No encoder binary or SDK asset is included here.

These exact files are an exception explicitly requested by the project owner on
2026-10-30. The artwork and music remain the property of their respective rights
holders and are not licensed by this repository's code license. This exception
does not allow ISO-extracted game assets into either repository. CI copies these
ready-to-package files; it does not need an encoder or the original MP3.
