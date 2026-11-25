/* The firmware versions this tree builds, for the firmware itself and for the host side: the driver
   warns about a dongle or radio older than these (the installer ships these same builds), and
   scripts/stage-firmwares.py reads DONGLE_FW_VERSION from here. */
#pragma once

#define DONGLE_FW_VERSION  0x0031
#define DML_VERSION_IDX    0x6A000029u        /* Valve nRF52 radios: SteamVR reads it as a unix time; keep it */
                                              /* newer than Valve's (0x5D27B2A9) so nothing "updates" it back */
#define DML_VERSION_HTC    0x0A00001Bu        /* HTC radios (Tracker 3.0 etc.) */
