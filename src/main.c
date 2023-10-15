/***************************************************************************
 * This file is part of Nincfhg       .                                    *
 * Copyright (c) 2023 V10lator <v10lator@myway.de>                         *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify    *
 * it under the terms of the GNU General Public License as published by    *
 * the Free Software Foundation; either version 3 of the License, or       *
 * (at your option) any later version.                                     *
 *                                                                         *
 * This program is distributed in the hope that it will be useful,         *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License along *
 * with this program; if not, If not, see <http://www.gnu.org/licenses/>.  *
 ***************************************************************************/

#include <CommonConfig.h>
#include <CommonConfigStrings.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <coreinit/filesystem_fsa.h>
#include <coreinit/foreground.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memory.h>
#include <coreinit/thread.h>
#include <coreinit/title.h>
#include <proc_ui/procui.h>
#include <sysapp/launch.h>
#include <vpad/input.h>
#include <whb/log.h>
#include <whb/log_console.h>
#include <mocha/mocha.h>

#define COLOR_BACKGROUND 0x000033FF
#define COLOR_RED        0x990000FF

#define FS_ALIGN(x)      ((x + 0x3F) & ~(0x3F))
#define WRITE_BUFSIZE    (1024 * 1024) // 1 MB
#define MAX_LINES        16
#define NINCFG_PATH      "/vol/external01/nincfg.bin"

static FSAClientHandle fsaClient;
static int mcpHandle;

static FSAFileHandle fileHandle;
static uint8_t *writeBuffer;
static size_t writeBufferFill = 0;

static size_t arg0;
static size_t arg1;
static bool error = false;

static char sizeStr[64];

static const char *infoTexts[13] = {
    "Emulate memory card (you want this to be \"Single\")",
    "Size of the emulated memory card",
    "Force 16:9 widescreen for 4:3 games",
    "Force progressive for inerlaced games",
    "Allows to read faster than a GCN disc drive",
    "Move the C stick to insert coins",
    "Rumble the wiimote with classic or pro controller",
    "Skip loading the IPL",
    "Game language (only for PAL)",
    "The video mode the game renders",
    "Video scaling. Set to \"Auto\" or \"104\"",
    "The offset. You want this to be 0",
    "The controller the gamepad replaces",
};

static const char *languages[7] = {
    "English",
    "German",
    "French",
    "Spanish",
    "Italian",
    "Dutch",
    "Auto",
};

static char *getVidMode(uint32_t vidMode)
{
    uint32_t vidMask = vidMode;
    vidMask >>= 16;
    --vidMask;

    if(vidMask & (NIN_VID_INDEX_FORCE | NIN_VID_INDEX_FORCE_DF))
    {
        if(vidMask & NIN_VID_INDEX_FORCE_DF)
            vidMask &= ~(NIN_VID_INDEX_FORCE);

        uint32_t forceMask = vidMode & NIN_VID_FORCE_MASK;
        snprintf(sizeStr, 31, "%s %s", VideoStrings[vidMask], VideoModeStrings[--forceMask]);
    }
    else
        snprintf(sizeStr, 31, "%s", VideoStrings[vidMask]);

    return sizeStr;
}

static void clearScreen()
{
    for(int i = 0; i < MAX_LINES; ++i)
        WHBLogPrint("");
}

static char *getSizeStr(uint32_t size)
{
    const char *suffix;
    if(size >= 1024)
    {
        size >>= 10;
        if(size >= 1024)
        {
            size >>= 10;
            suffix = "MB";
        }
        else
            suffix = "KB";
    }
    else
        suffix = "B";

    snprintf(sizeStr, 63, "%u%s", size, suffix);
    return sizeStr;
}

static size_t readFile(const char *path, void **buffer)
{
    FSAFileHandle handle;
    FSError err = FSAOpenFileEx(fsaClient, path, "r", 0x000, FS_OPEN_FLAG_NONE, 0, &handle);
    if(err == FS_ERROR_OK)
    {
        FSStat stat;
        err = FSAGetStatFile(fsaClient, handle, &stat);
        if(err == FS_ERROR_OK)
        {
            *buffer = MEMAllocFromDefaultHeapEx(FS_ALIGN(stat.size), 0x40);
            if(*buffer != NULL)
            {
                err = FSAReadFile(fsaClient, *buffer, stat.size, 1, handle, 0);
                if(err == 1)
                {
                    FSACloseFile(fsaClient, handle);
                    return stat.size;
                }

                WHBLogPrintf("Error reading %s: %s", path, FSAGetStatusStr(err));
                MEMFreeToDefaultHeap(*buffer);
            }
            else
                WHBLogPrint("EOM!");
        }
        else
            WHBLogPrintf("Error getting stats for %s: %s", path, FSAGetStatusStr(err));

        FSACloseFile(fsaClient, handle);
    }
    else
        WHBLogPrintf("Error opening %s: %s", path, FSAGetStatusStr(err));

    *buffer = NULL;
    return 0;
}

static FSError writeFile(const char *path, void *buffer, size_t size)
{
    FSAFileHandle handle;
    FSError err = FSAOpenFileEx(fsaClient, path, "w", 0x660, FS_OPEN_FLAG_NONE, 0, &handle);
    if(err == FS_ERROR_OK)
    {
        err = FSAWriteFile(fsaClient, buffer, size, 1, handle, 0);
        FSACloseFile(fsaClient, handle);
    }

    return err;
}

static uint32_t homeCallback(void *ctx)
{
    uint64_t tid = OSGetTitleID();
    if(tid == 0x0005000013374842 || (tid & 0xFFFFFFFFFFFFFCFF) == 0x000500101004A000) // HBL
        SYSRelaunchTitle(0, NULL);
    else
        SYSLaunchMenu();

// TODO: This causes a blackscreen in the Wii U menu
//    WHBLogConsoleFree();
    return 0;
}
int readInput()
{
    VPADReadError vError;
    VPADStatus vpad;
    VPADRead(VPAD_CHAN_0, &vpad, 1, &vError);
    if(vError == VPAD_READ_SUCCESS && vpad.trigger)
    {
        vpad.trigger &= ~(VPAD_STICK_R_EMULATION_LEFT | VPAD_STICK_R_EMULATION_RIGHT | VPAD_STICK_R_EMULATION_UP | VPAD_STICK_R_EMULATION_DOWN | VPAD_BUTTON_HOME);
        return vpad.trigger;
    }

    return 0;
}

void mainLoop()
{
    WHBLogConsoleSetColor(COLOR_BACKGROUND);

    bool redraw = true;
    uint32_t buttons;
    uint32_t cursor = 0;

    NIN_CFG *cfg;
    buttons = readFile(NINCFG_PATH, (void **)&cfg);
    if(buttons != sizeof(NIN_CFG))
    {
        WHBLogPrintf("%u vs %u", buttons, sizeof(NIN_CFG));
        error = true;
        return;
    }

    if(cfg->Magicbytes != 0x01070CF6)
    {
        WHBLogPrint("Magic bytes wrong!");
        error = true;
        return;
    }

    if(cfg->Version != NIN_CFG_VERSION)
    {
        WHBLogPrintf("Wrong version (got %u but we support %u only)", cfg->Version, NIN_CFG_VERSION);
        error = true;
        return;
    }

    // Apply unchangeable (Wii U specific) things, copied from https://github.com/FIX94/Nintendont/blob/master/kernel/Config.c
    cfg->MaxPads = 0; // Wii U mode
    cfg-> Config &= ~(NIN_CFG_DEBUGGER | NIN_CFG_DEBUGWAIT | NIN_CFG_LED); // Disables debugging and the drive access LED

    // Disable cheats
    cfg->Config &= ~(NIN_CFG_CHEATS);
    OSBlockSet(cfg->CheatPath, 0, sizeof(char) * 255);

    // Disable autoboot
    cfg->Config &= ~(NIN_CFG_AUTO_BOOT);
    OSBlockSet(cfg->GamePath, 0, sizeof(char) * 255);
    cfg->GameID = 0;

    // Make sure widescreen is enabled correctly
    if(cfg->Config & (NIN_CFG_FORCE_WIDE | NIN_CFG_WIIU_WIDE))
        cfg->Config |= NIN_CFG_FORCE_WIDE | NIN_CFG_WIIU_WIDE;

    // Transform NIN_LAN_AUTO to enum compatible format
    if(cfg->Language == NIN_LAN_AUTO)
        cfg->Language = NIN_LAN_LAST;

    // Disable MC multi in case of no memcard emulation
    if((cfg->Config & NIN_CFG_MC_MULTI) && !(cfg->Config & NIN_CFG_MEMCARDEMU))
        cfg->Config &= ~(NIN_CFG_MC_MULTI);

    // Set sane defaults for things not fitting on the screen
    cfg->Config &= ~(NIN_CFG_OSREPORT | NIN_CFG_LOG | NIN_CFG_USB | NIN_CFG_BBA_EMU);

    // Things not used on the Wii U
    cfg->Config &= ~(NIN_CFG_NATIVE_SI);
    cfg->NetworkProfile = 0;

    // Fix progressive setting
    if(cfg->Config & NIN_CFG_FORCE_PROG)
        cfg->VideoMode &= ~(NIN_VID_PROG);
    else
        cfg->VideoMode |= NIN_VID_PROG;

    // Fix video mode
    if(cfg->VideoMode & (NIN_VID_FORCE | NIN_VID_FORCE_DF) == (NIN_VID_FORCE | NIN_VID_FORCE_DF))
        cfg->VideoMode &= ~(NIN_VID_FORCE);

    while(1)
    {
        switch(ProcUIProcessMessages(true))
        {
            case PROCUI_STATUS_EXITING:
                return;
            case PROCUI_STATUS_RELEASE_FOREGROUND:
                ProcUIDrawDoneRelease();
                goto nextRound;
        }

        buttons = readInput();
        if(buttons & VPAD_BUTTON_PLUS)
        {
            if(cfg->Language == NIN_LAN_LAST)
                cfg->Language = NIN_LAN_AUTO;

            writeFile(NINCFG_PATH, cfg, sizeof(NIN_CFG));
            homeCallback(NULL);
            goto nextRound;
        }
        else if(buttons & VPAD_BUTTON_MINUS)
        {
            homeCallback(NULL);
            goto nextRound;
        }
        else if(buttons & VPAD_BUTTON_DOWN)
        {
            if(++cursor == 13)
                cursor = 0;

            redraw = true;
        }
        else if(buttons & VPAD_BUTTON_UP)
        {
            if(--cursor == (uint32_t)-1)
                cursor = 12;

            redraw = true;
        }
        else if(buttons & (VPAD_BUTTON_RIGHT | VPAD_BUTTON_LEFT))
        {
            uint32_t mask;

            switch(cursor)
            {
                case 0:
                    mask = 0;
                    if(buttons & VPAD_BUTTON_RIGHT)
                    {
                        if(cfg->Config & NIN_CFG_MEMCARDEMU)
                        {
                            if(cfg->Config & NIN_CFG_MC_MULTI)
                                cfg->Config &= ~(NIN_CFG_MEMCARDEMU | NIN_CFG_MC_MULTI);
                            else
                                cfg->Config |= NIN_CFG_MC_MULTI;
                        }
                        else
                            cfg->Config |= NIN_CFG_MEMCARDEMU;
                    }
                    else
                    {
                        if(cfg->Config & NIN_CFG_MEMCARDEMU)
                        {
                            if(cfg->Config & NIN_CFG_MC_MULTI)
                                cfg->Config &= ~(NIN_CFG_MC_MULTI);
                            else
                                cfg->Config &= ~(NIN_CFG_MEMCARDEMU | NIN_CFG_MC_MULTI);
                        }
                        else
                            cfg->Config |= NIN_CFG_MEMCARDEMU | NIN_CFG_MC_MULTI;
                    }

                    break;
                case 1:
                    mask = 0;
                    if(buttons & VPAD_BUTTON_RIGHT)
                    {
                        if(++cfg->MemCardBlocks == MEM_CARD_MAX + 1)
                            cfg->MemCardBlocks = 0;
                    }
                    else
                    {
                        if(--cfg->MemCardBlocks == (unsigned char)-1)
                            cfg->MemCardBlocks = MEM_CARD_MAX;
                    }
                    break;
                case 2:
                    mask = NIN_CFG_FORCE_WIDE | NIN_CFG_WIIU_WIDE;
                    break;
                case 3:
                    mask = NIN_CFG_FORCE_PROG;
                    if(cfg->Config & NIN_CFG_FORCE_PROG)
                        cfg->VideoMode &= ~(NIN_VID_PROG);
                    else
                        cfg->VideoMode |= NIN_VID_PROG;

                    break;
                case 4:
                    mask = NIN_CFG_REMLIMIT;
                    break;
                case 5:
                    mask = NIN_CFG_ARCADE_MODE;
                    break;
                case 6:
                    mask = NIN_CFG_CC_RUMBLE;
                    break;
                case 7:
                    mask = NIN_CFG_SKIP_IPL;
                    break;
                case 8:
                    mask = 0;
                    if(buttons & VPAD_BUTTON_RIGHT)
                    {
                        if(++cfg->Language == NIN_LAN_LAST + 1)
                            cfg->Language = 0;
                    }
                    else
                    {
                        if(--cfg->Language == (unsigned int)-1)
                            cfg->Language = NIN_LAN_LAST;
                    }

                    break;
                case 9:
                    mask = 0;
                    uint32_t vidMask = cfg->VideoMode;
                    vidMask >>= 16;
                    --vidMask;

                    uint32_t forceMask = cfg->VideoMode & NIN_VID_FORCE_MASK;
                    --forceMask;

                    if(buttons & VPAD_BUTTON_RIGHT)
                    {
                        if(vidMask & (NIN_VID_INDEX_FORCE | NIN_VID_INDEX_FORCE_DF))
                        {
                            if(++forceMask == NIN_VID_INDEX_FORCE_MPAL + 1)
                            {
                                forceMask = NIN_VID_INDEX_FORCE_PAL50;
                                goto switchVidMaskRight;
                            }
                        }
                        else
                        {
switchVidMaskRight:
                            switch(vidMask)
                            {
                                case NIN_VID_INDEX_AUTO:
                                    vidMask = NIN_VID_INDEX_FORCE;
                                    break;
                                case NIN_VID_INDEX_FORCE:
                                    vidMask = NIN_VID_INDEX_NONE;
                                    break;
                                case NIN_VID_INDEX_NONE:
                                    vidMask = NIN_VID_INDEX_FORCE_DF;
                                    break;
                                case NIN_VID_INDEX_FORCE_DF:
                                    vidMask = NIN_VID_INDEX_AUTO;
                                    break;
                            }
                        }
                    }
                    else
                    {
                        if(vidMask & (NIN_VID_INDEX_FORCE | NIN_VID_INDEX_FORCE_DF))
                        {
                            if(--forceMask == (uint32_t)-1)
                            {
                                forceMask = NIN_VID_INDEX_FORCE_MPAL;
                                goto switchVidMaskLeft;
                            }
                        }
                        else
                        {
switchVidMaskLeft:
                            switch(vidMask)
                            {
                                case NIN_VID_INDEX_AUTO:
                                    vidMask = NIN_VID_INDEX_FORCE_DF;
                                    break;
                                case NIN_VID_INDEX_FORCE_DF:
                                    vidMask = NIN_VID_INDEX_NONE;
                                    break;
                                case NIN_VID_INDEX_NONE:
                                    vidMask = NIN_VID_INDEX_FORCE;
                                    break;
                                case NIN_VID_INDEX_FORCE:
                                    vidMask = NIN_VID_INDEX_AUTO;
                                    break;
                            }
                        }
                    }

                    ++vidMask;
                    vidMask <<= 16;
                    cfg->VideoMode = vidMask | ++forceMask;

                    break;
                case 10:
                    mask = 0;

                    if(buttons & VPAD_BUTTON_RIGHT)
                    {
                        if(cfg->VideoScale == 0)
                            cfg->VideoScale = 40;
                        else {
                            cfg->VideoScale += 2;
                            if(cfg->VideoScale > 120)
                                cfg->VideoScale = 0; //auto
                        }
                    }
                    else
                    {
                        if(cfg->VideoScale == 0)
                            cfg->VideoScale = 120;
                        else {
                            cfg->VideoScale -= 2;
                            if(cfg->VideoScale < 40)
                                cfg->VideoScale = 0; //auto
                        }
                    }

                    break;
                case 11:
                    mask = 0;

                    if(buttons & VPAD_BUTTON_RIGHT)
                    {
                        if(++cfg->VideoOffset == 21)
                            cfg->VideoOffset = -20;
                    }
                    else
                    {
                        if(--cfg->VideoOffset == -21)
                            cfg->VideoOffset = 20;
                    }

                    break;
                case 12:
                    mask = 0;

                    if(buttons & VPAD_BUTTON_RIGHT)
                    {
                        if(++cfg->WiiUGamepadSlot == NIN_CFG_MAXPAD + 1)
                            cfg->WiiUGamepadSlot = 0;
                    }
                    else
                    {
                        if(--cfg->WiiUGamepadSlot == (unsigned int)-1)
                            cfg->WiiUGamepadSlot = NIN_CFG_MAXPAD;
                    }

                    break;
                default:
                    //TODO
                    break;
            }

            if(mask)
            {
                if(cfg->Config & mask)
                    cfg->Config &= ~(mask);
                else
                    cfg->Config |= mask;
            }

            redraw = true;
        }

        if(redraw)
        {
            redraw = false;
            clearScreen();

            WHBLogPrintf("%s Memcard emulation:      <%s>", (cursor == 0  ? "->" : "  "), (cfg->Config & NIN_CFG_MEMCARDEMU) ? ((cfg->Config & NIN_CFG_MC_MULTI) ? "Multi" : "Single") : "Off");
            WHBLogPrintf("%s Memcard size:           <%s (%u blocks)>", (cursor == 1 ? "->" : "  "), getSizeStr(MEM_CARD_SIZE(cfg->MemCardBlocks)), MEM_CARD_BLOCKS(cfg->MemCardBlocks));
            WHBLogPrintf("%s Force widescreen:       <%s>", (cursor == 2  ? "->" : "  "), (cfg->Config & NIN_CFG_FORCE_WIDE) ? "On" : "Off"); // Keep in sync with NIN_CFG_WIIU_WIDE
            WHBLogPrintf("%s Force progressive:      <%s>", (cursor == 3  ? "->" : "  "), (cfg->Config & NIN_CFG_FORCE_PROG) ? "On" : "Off");
            WHBLogPrintf("%s Remove read limit:      <%s>", (cursor == 4  ? "->" : "  "), (cfg->Config & NIN_CFG_REMLIMIT) ? "On" : "Off");
            WHBLogPrintf("%s Arcade mode:            <%s>", (cursor == 5  ? "->" : "  "), (cfg->Config & NIN_CFG_ARCADE_MODE) ? "On" : "Off");
            WHBLogPrintf("%s Wiimote CC rumble:      <%s>", (cursor == 6  ? "->" : "  "), (cfg->Config & NIN_CFG_CC_RUMBLE) ? "On" : "Off");
            WHBLogPrintf("%s Skip IPL:               <%s>", (cursor == 7  ? "->" : "  "), (cfg->Config & NIN_CFG_SKIP_IPL) ? "On" : "Off");
            WHBLogPrintf("%s Language:               <%s>", (cursor == 8  ? "->" : "  "), languages[cfg->Language]);
            WHBLogPrintf("%s Video mode:             <%s>", (cursor == 9  ? "->" : "  "), getVidMode(cfg->VideoMode));
            WHBLogPrintf("%s Video scale:            <%i>", (cursor == 10 ? "->" : "  "), cfg->VideoScale);
            WHBLogPrintf("%s Video offset:           <%i>", (cursor == 11 ? "->" : "  "), cfg->VideoOffset);
            if(cfg->WiiUGamepadSlot < NIN_CFG_MAXPAD)
                WHBLogPrintf("%s Wii U gamepad slot:     <%i>", (cursor == 12 ? "->" : "  "), cfg->WiiUGamepadSlot + 1);
            else
                WHBLogPrintf("%s Wii U gamepad slot:     <None>", (cursor == 12 ? "->" : "  "));

            WHBLogPrint("");
            WHBLogPrint(infoTexts[cursor]);

            WHBLogConsoleDraw();
        }

nextRound:
        OSSleepTicks(OSMillisecondsToTicks(20));
    }
}

int main()
{
    ProcUIInit(OSSavesDone_ReadyToRelease);
    ProcUIRegisterCallback(PROCUI_CALLBACK_HOME_BUTTON_DENIED, homeCallback, NULL, 100);
    OSEnableHomeButtonMenu(false);
    WHBLogConsoleInit();
    writeBuffer = MEMAllocFromDefaultHeapEx(FS_ALIGN(WRITE_BUFSIZE), 0x40);
    if(writeBuffer != NULL)
    {
        FSAInit();
        fsaClient = FSAAddClient(NULL);
        if(fsaClient)
        {
            MochaUtilsStatus ret = Mocha_InitLibrary();
            if(ret == MOCHA_RESULT_SUCCESS)
            {
                ret = Mocha_UnlockFSClientEx(fsaClient);
                if(ret == MOCHA_RESULT_SUCCESS)
                    mainLoop();
                else
                {
                    WHBLogPrint("Error unlocking FSA client!");
                    error = true;
                }

                Mocha_DeInitLibrary();
            }
            else
            {
                WHBLogPrint("Libmocha error!");
                error = true;
            }

            FSADelClient(fsaClient);
        }
        else
        {
            WHBLogPrint("No FSA client!");
            error = true;
        }

        FSAShutdown();
        MEMFreeToDefaultHeap(writeBuffer);
    }
    else
    {
        WHBLogPrint("EOM!");
        error = true;
    }

    if(error)
    {
        WHBLogPrint("");
        WHBLogPrint("Press HOME to exit");
        WHBLogConsoleSetColor(COLOR_RED);
        WHBLogConsoleDraw();

        while(ProcUIProcessMessages(true) != PROCUI_STATUS_EXITING)
            OSSleepTicks(OSMillisecondsToTicks(1000 / 60));
    }

    return 0;
}
