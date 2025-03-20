# Gust Tools

[![Windows Build](https://img.shields.io/github/actions/workflow/status/VitaSmith/gust_tools/windows.yml?style=flat-square&label=Windows%20Build)](https://github.com/VitaSmith/gust_tools/actions/workflows/windows.yml)
[![Linux Build](https://img.shields.io/github/actions/workflow/status/VitaSmith/gust_tools/linux.yml?style=flat-square&label=Linux%20Build)](https://github.com/VitaSmith/gust_tools/actions/workflows/linux.yml)
[![Github stats](https://img.shields.io/github/downloads/VitaSmith/gust_tools/total.svg?style=flat-square&label=Downloads)](https://github.com/VitaSmith/gust_tools/releases)
[![Latest release](https://img.shields.io/github/release-pre/VitaSmith/gust_tools?style=flat-square&label=Latest%20Release)](https://github.com/VitaSmith/gust_tools/releases)

A set of commandline utilities designed to work with Gust (Koei/Tecmo) PC game assets such as the ones from
[_Atelier series_](https://store.steampowered.com/search/?sort_by=Name_ASC&term=atelier&tags=122&category1=998),
[_Nights of Azure series_](https://store.steampowered.com/search/?term=%22nights%20of%20azure%22&category1=998),
[_Blue Reflection_](https://store.steampowered.com/search/?term=%22blue%20reflection%22&category1=998),
[_Fairy Tail_](https://store.steampowered.com/search/?term=%22fairy%20tail%22&category1=998),
[_Fatal Frame_](https://store.steampowered.com/search/?term=%22fatal%20frame%22&category1=998) ...

Utilities
=========

* `gust_pak`: Unpack or repack a Gust `.pak` archive.
* `gust_elixir`: Unpack or repack a Gust `.elixir[.gz]` archive.
* `gust_gmpk`: Unpack or repack a Gust `.gmpk` archive.
* `gust_g1t`: Unpack or repack a Gust `.g1t` texture archive.
* `gust_enc`: Encode or decode a Gust `.e` archive.
* `gust_ebm`: Convert a `.ebm` message file to or from an editable JSON file.

Notes
-----

`gust_pak` is designed to replace both `A17_Decrypt` and `A18_Decrypt`, as it automatically detects "A17" (32-bit) and "A18" (64-bit) formats.
It should therefore works with all of the Atelier PC ports, _Fairy Tail_, as well as _Blue Reflection_ archives.

`gust_enc` only works on the games where for which the scrambling seeds are known. See `gust_enc.json` for details.
You can find a primer on the `.e` format, as well as what `gust_enc` does [here](https://gist.github.com/VitaSmith/ab384400bd992413ee0da401457abee1).

In most cases, the repacking of an archive relies on a corresponding `.json` to have been created during unpacking.
You will not be able to recreate an archive if a `.json` file does not exist for it, either in the directory (`.elixir`, `.g1t`)
or at the root level (`.pak`).

Building
========

If you have Visual Studio 2022 installed, just open the `.sln` file or run `build.cmd`.

Otherwise (Linux, MinGW) just issue `make`.

Usage
=====

On Windows, you can just drop the file or directory you want to unpack/repack or decode/encode on top of the executable.

Otherwise, you can invoke: `<gust_utility> <file or directory>`.

When invoking `gust_enc`, you may specify the game ID to use for the encryption seeds (e.g. `-BR` for _Blue Reflection_,
`-FT` for _Fairy Tail_ or _Atelier Yumia_). If not specified, then the default ID from `gust_enc.json` is used.

For recreating a `.pak`, you must pass the `.json` that was created during extraction to `gust_pak` rather than the directory.

Modding games
=============

**IMPORTANT: YOU SHOULD BACK UP ALL GAME ARCHIVES AND FOLDERS BEFORE RUNNING THE UNPACKER**

Most Gust game executables are designed to use either packed assets, if a `.pak` archive is present, or the extracted assets, if
a matching directory bearing the same name as the `.pak` is found. For that to work, you must however make sure that the `.pak`
is not seen, as it has precedence over the directory.

For instance, if you want to alter character assets (textures, models, ...) for the game _Blue Reflection_:
* Go to `<GAME_DIR>\DATA\`and copy `gust_pak.exe` there.
* Drop `PACK00_02.pak` on top of `gust_pak.exe`. This will extract all the content into a `data\` subdirectory.
* Move the content from `data\x64\` to `x64\` (in this case, that should only be one folder named `character`). This is needed
  because in this case `<GAME_DIR>\DATA\x64` is the location where _Blue Reflection_ expects extracted game assets, not
  `<GAME_DIR>\DATA\data\x64`.
* Rename `PACK00_02.pak` to `PACK00_02.old` so that the game assets you just extracted are used.

Happy modding! :smile:

License
=======

[GPLv3](https://www.gnu.org/licenses/gpl-3.0.html) or later.

Thanks
======

* _Yuri Hime_/_Lily_/_shizukachan_ and everyone who helped with `A17_Decrypt`/`A18_Decrypt`.
* _Admiral Curtiss_ for [HyoutaTools](https://github.com/AdmiralCurtiss/HyoutaTools/) and _Semory_ for
  [Steven's Gas Machine](http://sticklove.com/xnalara.org/viewtopic.php?f=17&t=1001) (a.k.a. "xentax"), where we picked some
  inspiration on how to unpack the `.elixir` and `.g1t` formats.
* _Rich Geldreich_ and others for the [miniz](https://github.com/richgel999/miniz) inflate/deflate library.
* _Krzysztof Gabis_ for the [parson](http://kgabis.github.com/parson/) JSON parsing library.
* _Gust_, for making games that are interesting enough to make one want to crack their custom compression and encryption schemes. :grin:
