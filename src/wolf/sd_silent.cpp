// Takes the place of id_sd.cpp. The Jornada plays no sound, so nothing here does.
#include "wl_def.h"
#include <SDL_mixer.h>

globalsoundpos channelSoundPos[MIX_CHANNELS];
boolean AdLibPresent, SoundBlasterPresent, SoundPositioned;
SDMode SoundMode;
SDSMode DigiMode;
SMMode MusicMode;
int DigiMap[LASTSOUND];
int DigiChannel[STARTMUSIC - STARTDIGISOUNDS];

void SD_Startup(void) {}
void SD_Shutdown(void) {}
int SD_GetChannelForDigi(int) { return 0; }
void SD_PositionSound(int, int) {}
boolean SD_PlaySound(soundnames) { return false; }
void SD_SetPosition(int, int, int) {}
void SD_StopSound(void) {}
void SD_WaitSoundDone(void) {}
void SD_StartMusic(int) {}
void SD_ContinueMusic(int, int) {}
void SD_MusicOn(void) {}
void SD_FadeOutMusic(void) {}
int SD_MusicOff(void) { return 0; }
boolean SD_MusicPlaying(void) { return false; }
boolean SD_SetSoundMode(SDMode) { return false; }
boolean SD_SetMusicMode(SMMode) { return false; }
word SD_SoundPlaying(void) { return 0; }
void SD_SetDigiDevice(SDSMode) {}
void SD_PrepareSound(int) {}
int SD_PlayDigitized(word, int, int) { return 0; }
void SD_StopDigitized(void) {}
