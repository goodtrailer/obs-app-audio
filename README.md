# obs-app-audio
Windows OBS plugin for capturing audio from just one application.
Mostly a hobby project to learn some OS-level stuffs.

This plugin is just a full-C++ rewrite of a modification to the official `win-capture` plugin that I made over the past year in this [fork](https://github.com/goodtrailer/obs-studio/tree/audio-hook-multi/plugins/win-capture).

[bozbez](https://github.com/bozbez/win-capture-audio) recently made a similar plugin for OBS, except it works better! He did in 1 week what took me months, so I'd highly recommend using his.
[bozbez/win-capture-audio](https://github.com/bozbez/win-capture-audio)

## Issues
* Fluctuating static/flicker
* Occasional crash when switching applications (related to how SwrContext\* is being handled)
* Sometimes doesn't pick up Discord voice channel audio
* Sometimes runs into encoding errors with Discord voice channel audio
* Weird crashes that I think are related to working with memory that is simultaneously being worked on by another thread
* Multiple Firefox tabs sound super bad
