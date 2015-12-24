#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "common.h"
#include "darm.h"
#include "r5.h"
#include "fsredir.h"
#include "nimpatch.h"

// it's time
// for
// regioooooonFIIIIIIIIIIVE

Result getTitleInformation(u8* mediatype, u64* tid);
Result gspwn(void* dst, void* src, u32 size);

function_s findCfgSecureInfoGetRegion(u8* code_data, u32 code_size)
{
    if(!code_data || !code_size)return (function_s){0,0};

    u32* code_data32 = (u32*)code_data;
    u32 code_size32 = code_size / 4;
    int i, j, k;

    static function_s candidates[32];
    int num_candidates = 0;

    for(i=0; i<code_size32; i++)
    {
        // search for "mov r0, #0x20000"
        if(code_data32[i] == 0xE3A00802)
        {
            function_s c = findFunction(code_data32, code_size32, i);

            // start at i because svc 0x32 should always be after the mov
            for(j=i; j<=c.end; j++)
            {
                if(code_data32[j] == 0xEF000032)
                {
                    candidates[num_candidates++] = c;
                    break;
                }
            }
        }
    }

    // look for error code which is known to be stored near cfg:u handle
    // this way we can find the right candidate
    // (handle should also be stored right after end of candidate function)
    for(i=0; i<code_size32; i++)
    {
        if(code_data32[i] == 0xD8A103F9)
        {
            for(j=i-4; j<i+4; j++)
            {
                for(k=0; k<num_candidates; k++)
                {
                    if(code_data32[j] == code_data32[candidates[k].end + 1])
                    {
                        return candidates[k];
                    }
                }
            }
        }
    }

    return (function_s){0,0};
}

function_s findCfgCtrGetLanguage(u8* code_data, u32 code_size)
{
    if(!code_data || !code_size)return (function_s){0,0};

    u32* code_data32 = (u32*)code_data;
    u32 code_size32 = code_size / 4;
    int i, j;

    for(i=0; i<code_size32; i++)
    {
        if(code_data32[i] == 0x000A0002)
        {
            function_s c = findFunction(code_data32, code_size32, i-4);

            for(j=c.start; j<=c.end; j++)
            {
                darm_t d;
                if(!darm_armv7_disasm(&d, code_data32[j]) && (d.instr == I_LDR && d.Rn == PC && (i-j-2)*4 == d.imm))
                {
                    return c;
                }
            }
        }
    }

    return (function_s){0,0};
}

void patchCfgSecureInfoGetRegion(u8* code_data, u32 code_size, function_s c, u8 region_code)
{
    if(!code_data || !code_size || c.start == c.end || c.end == 0)return;

    u32* code_data32 = (u32*)code_data;
    int i;

    for(i = c.start; i<c.end; i++)
    {
        darm_t d;
        if(!darm_armv7_disasm(&d, code_data32[i]) && d.instr == I_LDRB)
        {
            printf("%08X %d\n", i * 4 + 0x00100000, d.Rt);
            code_data32[i] = 0xE3A00000 | (d.Rt << 12) | region_code;
            break;
        }
    }
}

void patchCfgCtrGetLanguage(u8* code_data, u32 code_size, function_s c, u8 language_code)
{
    if(!code_data || !code_size || c.start == c.end || c.end == 0)return;

    u32* code_data32 = (u32*)code_data;
    int i;

    for(i = c.end; i>c.start; i--)
    {
        darm_t d;
        if(!darm_armv7_disasm(&d, code_data32[i]) && (d.instr == I_LDRB && d.Rt == 0))
        {
            printf("%08X\n", i * 4 + 0x00100000);
            code_data32[i] = 0xE3A00000 | (d.Rt << 12) | language_code;
            break;
        }
    }
}

void setClockrate(u8 setting)
{
	int j, i;
	u32* patchArea = linearAlloc(0x00100000);

	if(setting != 0) setting = 3;

	// grab waitLoop stub
	GSPGPU_FlushDataCache(NULL, (u8*)patchArea, 0x100);
	gspwn(patchArea, (u32*)(MENU_LOADEDROP_BUFADR-0x100), 0x100);
	svcSleepThread(20*1000*1000);

	// patch it
	for(i=0; i<0x100/4; i++)
	{
		if(patchArea[i] == 0x67666E63) // "cnfg"
		{
			patchArea[i+1] = (patchArea[i+1] & ~0xFF) | setting;
			break;
		}
	}

	// copy it back
	GSPGPU_FlushDataCache(NULL, (u8*)patchArea, 0x100);
	gspwn((u32*)(MENU_LOADEDROP_BUFADR-0x100), patchArea, 0x100);
	svcSleepThread(20*1000*1000);

	// ghetto dcache invalidation
	// don't judge me
	for(j=0; j<4; j++)
		for(i=0; i<0x00100000/0x4; i+=0x4)
			patchArea[i+j]^=0xDEADBABE;

	linearFree(patchArea);
}

u8 smdhGetRegionCode(u8* smdh_data)
{
    if(!smdh_data)return 0xFF;

    u8 flags = smdh_data[0x2018];
    int i;
    for(i=0; i<6; i++)if(flags & (1<<i))return i;

    return 0xFF;
}

u8* loadSmdh(u64 tid, u8 mediatype)
{
	Result ret;
	Handle fileHandle;

	u32 archivePath[] = {tid & 0xFFFFFFFF, (tid >> 32) & 0xFFFFFFFF, mediatype, 0x00000000};
	static const u32 filePath[] = {0x00000000, 0x00000000, 0x00000002, 0x6E6F6369, 0x00000000}; // icon

	ret = FSUSER_OpenFileDirectly(NULL, &fileHandle, (FS_archive){0x2345678a, (FS_path){PATH_BINARY, 0x10, (u8*)archivePath}}, (FS_path){PATH_BINARY, 0x14, (u8*)filePath}, FS_OPEN_READ, FS_ATTRIBUTE_NONE);

	printf("loading smdh : %08X\n", (unsigned int)ret);

	u8* fileBuffer = NULL;
	u64 fileSize = 0;

	{
		u32 bytesRead;

		ret = FSFILE_GetSize(fileHandle, &fileSize);
		if(ret)return NULL;

		fileBuffer = malloc(fileSize);
		if(ret)return NULL;

		ret = FSFILE_Read(fileHandle, &bytesRead, 0x0, fileBuffer, fileSize);
		if(ret)return NULL;

		ret = FSFILE_Close(fileHandle);
		if(ret)return NULL;

		printf("loaded code : %08X\n", (unsigned int)fileSize);
	}

	return fileBuffer;
}

extern PrintConsole topScreen, bottomScreen;

char* regions[] = {"JPN", "USA", "EUR", "AUS", "CHN", "KOR", "TWN", "---"};
char* languages[] = {"JP", "EN", "FR", "DE", "IT", "ES", "ZH", "KO", "NL", "PT", "RU", "TW", "--"};
char* yesno[] = {"YES", "NO"};
char* clocks[] = {"268Mhz", "804Mhz"};

typedef enum
{
	CHOICE_REGION = 0,
	CHOICE_LANGUAGE = 1,
	CHOICE_NIMUPDATE = 2,
	CHOICE_CLOCK = 3,
	CHOICE_CODE = 4,
	CHOICE_ROMFS = 5,
	CHOICE_SAVE = 6,
	CHOICE_OK = 7,
	CHOICE_EXIT,
	CHOICE_NUM
}choices_t;

char *descriptions[] =
{
	"Use this to force the region value used by your game. '--' means your console's actual region value will be used. Choosing the wrong value here may cause some games to crash.",
	"Use this to force the language setting used by your game. '--' means your console's actual language setting will be used. Choosing the wrong setting here may cause some games to crash.",
	"Use this to enable certain apps which check your console's firmware version to work without updating. This can be used to enable eShop use on older firmware versions, for example.",
	"Use this to force the use of a certain CPU clock rate in game. This will only work on N3DS.",
	"Replace the app's code with a file loaded from your SD card. The file should not be compressed.",
	"Redirect the app's romfs reads to a file on your SD card. The file should be a standard ROMFS partition.",
	"Select this if you want to save these settings and not be prompted for them every time you run this game. If you want to change saved settings, just hold L the next time you start HANS.",
	"Select this to confirm your settings and run your game.",
	"Select this to cancel and return to the homebrew launcher.",
	""
};

Result checkRomfs(char* _path)
{
	if(!_path)return -1;

	static char path[256];
	snprintf(path, 255, "sdmc:%s", _path);

	FILE* f = fopen(path, "rb");
	if(!f)return -2;

	fclose(f);

	return 0;
}

Result configureTitle(char* cfg_path, u8* region_code, u8* language_code, u8* clock, char** romfs, char** code, u8* nim, int* nimversion)
{
	u8 mediatype = 0;
	u64 tid = 0;

	getTitleInformation(&mediatype, &tid);

	if(!tid)return -1;

	static char _fn[256];
	char* fn = _fn;
	if(cfg_path) fn = cfg_path;
	else sprintf(fn, "titles/%08X.txt", (unsigned int)(tid & 0xffffffff));

	mkdir("titles", 777);

	u8 numChoices[] = {sizeof(regions) / sizeof(regions[0]), sizeof(languages) / sizeof(languages[0]), sizeof(yesno) / sizeof(yesno[0]), sizeof(clocks) / sizeof(clocks[0]), sizeof(yesno) / sizeof(yesno[0]), sizeof(yesno) / sizeof(yesno[0]), sizeof(yesno) / sizeof(yesno[0]), 0, 0};
	int choice[] = {numChoices[0]-1, numChoices[1]-1, 1, 0, 1, 1, 1, 0, 0};

	static char romfs_path[128];
	static char code_path[128];
	static char name[16];

	// grab config name from path
	int i, j;
	int n = strlen(fn);
	for(j=n-1; j>=0; j--) if(fn[j] == '/') break;
	j++; for(i=j; i<n && fn[i] != '.' && (i-j)<10; i++) name[i-j] = fn[i];
	name[i-j] = '\0';

	snprintf(romfs_path, 128, "/hans/%s.romfs", name);
	snprintf(code_path, 128, "/hans/%s.code", name);

	Result romfsValid = checkRomfs(romfs_path);

	if(nimversion) *nimversion = 0;

	hidScanInput();

	{
		FILE* f = fopen(fn, "r");
		if(f)
		{
			static char l[256];
			while(fgets(l, sizeof(l), f))
			{
				if(sscanf(l, "region : %d", &choice[CHOICE_REGION]) != 1)
				if(sscanf(l, "language : %d", &choice[CHOICE_LANGUAGE]) != 1);
				if(sscanf(l, "nim_checkupdate : %d", &choice[CHOICE_NIMUPDATE]) != 1);
				if(nimversion && sscanf(l, "nimversion : %d", nimversion) != 1);
				if(sscanf(l, "clock : %d", &choice[CHOICE_CLOCK]) != 1);
				if(sscanf(l, "romfs : %d", &choice[CHOICE_ROMFS]) != 1);
				if(sscanf(l, "code : %d", &choice[CHOICE_CODE]) != 1);
			}
			choice[CHOICE_SAVE] = 0;

			fclose(f);

			if(!(hidKeysHeld() & KEY_L))goto end;
		}else{
			u8* smdh_data = loadSmdh(tid, mediatype);
			if(smdh_data)
			{
				choice[CHOICE_REGION] = smdhGetRegionCode(smdh_data);
				free(smdh_data);
			}
		}
	}

	consoleClear();

	int field = 0;
	while(1)
	{
		hidScanInput();

		u32 kDown = hidKeysDown();

		if((kDown & KEY_START) || ((kDown & KEY_A) && (field == CHOICE_OK || field == CHOICE_EXIT)))break;

		if(kDown & KEY_UP)field--;
		if(kDown & KEY_DOWN)field++;

		if(field < 0)field = CHOICE_NUM - 1;
		if(field >= CHOICE_NUM)field = 0;

		if(kDown & KEY_LEFT)choice[field]--;
		if(kDown & KEY_RIGHT)choice[field]++;

		if(choice[field] < 0) choice[field] = numChoices[field] - 1;
		if(choice[field] >= numChoices[field]) choice[field] = 0;

		consoleSelect(&topScreen);

		printf("\x1b[0;0H\n");
		printf(                            "               HANS             \n");
		printf("\n");
		printf(field == CHOICE_REGION ?    "  Region             : < %s > \n" : "  Region             :   %s   \n", regions[choice[CHOICE_REGION]]);
		printf(field == CHOICE_LANGUAGE ?  "  Language           : < %s > \n" : "  Language           :   %s   \n", languages[choice[CHOICE_LANGUAGE]]);
		printf(field == CHOICE_NIMUPDATE ? "  FW Version Spoof   : < %s > \n" : "  FW Version Spoof   :   %s   \n", yesno[choice[CHOICE_NIMUPDATE]]);
		printf(field == CHOICE_CLOCK ?     "  N3DS CPU clock     : < %s > \n" : "  N3DS CPU clock     :   %s   \n", clocks[choice[CHOICE_CLOCK]]);
		printf(field == CHOICE_CODE ?      "  Code  -> SD        : < %s > \n" : "  Code  -> SD        :   %s   \n", yesno[choice[CHOICE_CODE]]);
		printf(field == CHOICE_ROMFS ?     "  Romfs -> SD        : < %s > \n" : "  Romfs -> SD        :   %s   \n", yesno[choice[CHOICE_ROMFS]]);
		printf(field == CHOICE_SAVE ?      "  Save configuration : < %s > \n" : "  Save configuration :   %s   \n", yesno[choice[CHOICE_SAVE]]);
		printf(                            "                                               \n");
		printf(                            "  Current title        : %08X%08X        \n", (unsigned int)(tid >> 32), (unsigned int)(tid & 0xFFFFFFFF));
		if(!choice[CHOICE_CODE])
			printf(                        "  Code path            : sd:%s \n", code_path);
		if(!choice[CHOICE_ROMFS])
			printf(!romfsValid ?           "  Romfs path           : sd:%s\n" : "  Romfs path (INVALID) : sd:%s\n", romfs_path);
		printf(                            "                                                  ");
		printf(                            "                                                  ");
		printf(field == CHOICE_OK ?        "             > OK  \n"              : "               OK    \n");
		printf(field == CHOICE_EXIT ?      "             > EXIT\n"              : "               EXIT\n");
		printf(                            "                                               \n");
		printf(                            "                                               \n");

		consoleSelect(&bottomScreen);
		printf("\x1b[0;0H\n");
		printf("Description :\n");
		printf("    %s%*s", descriptions[field], 240 - strlen(descriptions[field]), " ");

		gfxFlushBuffers();
		gfxSwapBuffers();

		gspWaitForVBlank();
	}

	if(field == CHOICE_EXIT)return -1;

	if(choice[CHOICE_REGION] >= numChoices[CHOICE_REGION] - 1) choice[CHOICE_REGION] = -1;
	if(choice[CHOICE_LANGUAGE] >= numChoices[CHOICE_LANGUAGE] - 1) choice[CHOICE_LANGUAGE] = -1;

	if(choice[CHOICE_SAVE] == 0)
	{
		FILE* f = fopen(fn, "w");
		if(f)
		{
			fprintf(f, "region : %d\nlanguage : %d\nclock : %d\nromfs : %d\ncode : %d\nnim_checkupdate : %d\n", choice[CHOICE_REGION], choice[CHOICE_LANGUAGE], choice[CHOICE_CLOCK], choice[CHOICE_ROMFS], choice[CHOICE_CODE], choice[CHOICE_NIMUPDATE]);

			fclose(f);
		}
	}

	end:
	if(region_code)*region_code = choice[CHOICE_REGION];
	if(language_code)*language_code = choice[CHOICE_LANGUAGE];
	if(nim)*nim = choice[CHOICE_NIMUPDATE] == 0;
	if(clock)*clock = choice[CHOICE_CLOCK];
	// romfs_path and code_path are static so as long as we're not idiots this is totally fine
	if(romfs)*romfs = (choice[CHOICE_ROMFS] == 0) ? romfs_path : NULL;
	if(code)*code = (choice[CHOICE_CODE] == 0) ? code_path : NULL;

	return 0;
}

Result doRegionFive(u8* code_data, u32 code_size, char* cfg_path)
{
    u8 region_code = 2;
    u8 language_code = 2;
    u8 clock = 0;
    u8 nim = 0;
    int nimversion = -1;
    char* romfs = NULL;
    char* code = NULL;

    Result ret = configureTitle(cfg_path, &region_code, &language_code, &clock, &romfs, &code, &nim, &nimversion);

    if(ret)return ret;
    
	u64 tid = 0;
	getTitleInformation(NULL, &tid);

	if(code)
	{
		char path[32];
		sprintf(path, "sdmc:%s", code);

		FILE* f = fopen(path, "rb");
		if(!f)return -1;

		fread(code_data, 1, code_size, f);

		fclose(f);
	}

    printf("region %X\n", region_code);
    printf("language %X\n", language_code);

    if(nimversion >= 0)
    {
    	patchNimTitleVersion(code_data, code_size, nimversion);
    }

    if(region_code != 0xFF)
    {
	    function_s cfgSecureInfoGetRegion = findCfgSecureInfoGetRegion(code_data, code_size);

	    printf("cfgSecureInfoGetRegion : %08X - %08X\n", (unsigned int)(cfgSecureInfoGetRegion.start * 4 + 0x00100000), (unsigned int)(cfgSecureInfoGetRegion.end * 4 + 0x00100000));

	    patchCfgSecureInfoGetRegion(code_data, code_size, cfgSecureInfoGetRegion, region_code);
    }

    if(language_code != 0xFF)
	{
	    function_s cfgCtrGetLanguage = findCfgCtrGetLanguage(code_data, code_size);
	    
	    printf("cfgCtrGetLanguage : %08X - %08X\n", (unsigned int)(cfgCtrGetLanguage.start * 4 + 0x00100000), (unsigned int)(cfgCtrGetLanguage.end * 4 + 0x00100000));

	    patchCfgCtrGetLanguage(code_data, code_size, cfgCtrGetLanguage, language_code);
	}

	if(clock != 0xFF)
	{
		setClockrate(clock);
	}

	if(nim)
	{
		patchNimCheckSysupdateAvailableSOAP(code_data, code_size);

		const static char target[] = "%s/samurai/ws/%s/title/%llu/other_purchased?shop_id=1&lang=%s&_t";
		const static char url[] = "http://smealum.github.io/ninjhax2/samurai.json?%s%s%llu%s";

		int i;
		int cursor = 0;
		int l = strlen(target);
		for(i=0; i<code_size; i++)
		{
			if(cursor == l)
			{
				strcpy((char*)&code_data[i - l], url);
				break;
			}

			if(target[cursor] == code_data[i]) cursor++;
			else cursor = 0;
		}
	}

	if(romfs)
	{
		Handle fsHandle, fileHandle;
		FS_archive sdmcArchive = (FS_archive){0x00000009, (FS_path){PATH_EMPTY, 1, (u8*)""}};
		srvGetServiceHandle(&fsHandle, "fs:USER");

		FSUSER_OpenFileDirectly(&fsHandle, &fileHandle, sdmcArchive, FS_makePath(PATH_CHAR, romfs), FS_OPEN_READ, FS_ATTRIBUTE_NONE);

		// patchFsOpenRom(code_data, code_size, fsHandle, romfs);
		patchFsOpenRom(code_data, code_size, fileHandle);
	}

	// {
	// 	Handle fsHandle;
	// 	FS_archive sdmcArchive = (FS_archive){0x00000009, (FS_path){PATH_EMPTY, 1, (u8*)""}};
	// 	srvGetServiceHandle(&fsHandle, "fs:USER");

	// 	FSUSER_OpenArchive(&fsHandle, &sdmcArchive);

	// 	patchFsSavegame(code_data, code_size, fsHandle, (u64)sdmcArchive.handleLow | (((u64)sdmcArchive.handleHigh) << 32));
	// }

	return 0;
}
