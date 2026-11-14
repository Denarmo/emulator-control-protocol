# System ids

`systems` in a descriptor and `system` in `launch` use these ids. They are lower case, stable,
and never reused. The extensions are the usual ones, as a hint: an engine lists its own in its
descriptor.

Ask for a new id with a pull request that adds a row here.

| Id | System | Usual extensions |
|---|---|---|
| `switch2` | Nintendo Switch 2 | nsp, xci, nup |
| `nes` | NES | nes, unf, unif |
| `snes` | Super Nintendo | sfc, smc, fig, swc, bs |
| `gb` | Game Boy | gb |
| `gbc` | Game Boy Color | gbc |
| `gba` | Game Boy Advance | gba |
| `virtualboy` | Virtual Boy | vb, vboy |
| `pokemini` | Pokémon mini | min |
| `n64` | Nintendo 64 | n64, z64, v64 |
| `nds` | Nintendo DS | nds |
| `gamecube` | GameCube | gcm, rvz, gcz, iso |
| `wii` | Wii | wbfs, rvz, wad, iso |
| `sg1000` | SG-1000 | sg |
| `sms` | Master System | sms |
| `gg` | Game Gear | gg |
| `genesis` | Genesis / Mega Drive | md, gen, smd, bin |
| `32x` | Sega 32X | 32x |
| `saturn` | Saturn | cue, chd, m3u, iso |
| `dreamcast` | Dreamcast | gdi, cdi, chd, cue, m3u |
| `psx` | PlayStation | cue, chd, pbp, m3u, iso, img |
| `ps2` | PlayStation 2 | iso, chd, cso |
| `psp` | PSP | iso, cso, pbp |
| `pico8` | PICO-8 | p8 |
| `atari2600` | Atari 2600 | a26, bin |
| `atari5200` | Atari 5200 | a52, bin |
| `atari7800` | Atari 7800 | a78, bin |
| `lynx` | Atari Lynx | lnx, o |
| `jaguar` | Atari Jaguar | j64, jag, rom, abs, cof |
| `pce` | PC Engine / TurboGrafx-16 | pce, sgx |
| `ngp` | Neo Geo Pocket | ngp |
| `ngpc` | Neo Geo Pocket Color | ngc |
| `wonderswan` | WonderSwan | ws |
| `wsc` | WonderSwan Color | wsc |
| `vectrex` | Vectrex | vec, bin, gam |
| `msx` | MSX | mx1, mx2, rom |
| `arcade` | Arcade | zip, 7z |

More ids, for systems that engines are likely to need:

| Id | System | Usual extensions |
|---|---|---|
| `switch` | Nintendo Switch | nsp, xci, nca, nro |
| `3ds` | Nintendo 3DS | 3ds, cci, cia, 3dsx |
| `wiiu` | Wii U | wua, wux, rpx |
| `ps3` | PlayStation 3 | pkg, iso |
| `psvita` | PlayStation Vita | vpk |
| `xbox` | Xbox | iso, xiso |
| `xbox360` | Xbox 360 | iso, xex |
| `segacd` | Sega CD / Mega-CD | cue, chd, iso |
| `pcfx` | PC-FX | cue, chd |
| `neogeo` | Neo Geo AES/MVS | zip |
| `3do` | 3DO | iso, cue, chd |
| `dos` | MS-DOS | exe, com, bat, conf |

`arcade` covers MAME and FBNeo sets; an engine names the romset version it expects in its own
documentation.
